// SPDX-License-Identifier: GPL-2.0
/*
 * Basic Node interface support
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/memory.h>
#include <linux/vmstat.h>
#include <linux/notifier.h>
#include <linux/node.h>
#include <linux/hugetlb.h>
#include <linux/compaction.h>
#include <linux/cpumask.h>
#include <linux/topology.h>
#include <linux/nodemask.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/pm_runtime.h>
#include <linux/swap.h>
#include <linux/slab.h>

static struct bus_type node_subsys = {
	.name = "node",
	.dev_name = "node",
};


static ssize_t node_read_cpumap(struct device *dev, bool list, char *buf)
{
	ssize_t n;
	cpumask_var_t mask;
	struct node *node_dev = to_node(dev);

	/* 2008/04/07: buf currently PAGE_SIZE, need 9 chars per 32 bits. */
	BUILD_BUG_ON((NR_CPUS/32 * 9) > (PAGE_SIZE-1));

	if (!alloc_cpumask_var(&mask, GFP_KERNEL))
		return 0;

	cpumask_and(mask, cpumask_of_node(node_dev->dev.id), cpu_online_mask);
	n = cpumap_print_to_pagebuf(list, buf, mask);
	free_cpumask_var(mask);

	return n;
}

static inline ssize_t node_read_cpumask(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return node_read_cpumap(dev, false, buf);
}
static inline ssize_t node_read_cpulist(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return node_read_cpumap(dev, true, buf);
}

static ssize_t node_read_nodemap(nodemask_t *mask, bool list, char *buf)
{
	return list ? scnprintf(buf, PAGE_SIZE - 1, "%*pbl\n",
				nodemask_pr_args(mask)) :
		      scnprintf(buf, PAGE_SIZE - 1, "%*pb\n",
				nodemask_pr_args(mask));
}

static ssize_t primary_mem_nodelist_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct node *n = to_node(dev);
	return node_read_nodemap(&n->primary_mem_nodes, true, buf);
}

static ssize_t primary_mem_nodemask_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct node *n = to_node(dev);
	return node_read_nodemap(&n->primary_mem_nodes, false, buf);
}

static ssize_t primary_cpu_nodelist_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct node *n = to_node(dev);
	return node_read_nodemap(&n->primary_cpu_nodes, true, buf);
}

static ssize_t primary_cpu_nodemask_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct node *n = to_node(dev);
	return node_read_nodemap(&n->primary_cpu_nodes, false, buf);
}

static DEVICE_ATTR_RO(primary_mem_nodelist);
static DEVICE_ATTR_RO(primary_mem_nodemask);
static DEVICE_ATTR_RO(primary_cpu_nodemask);
static DEVICE_ATTR_RO(primary_cpu_nodelist);
static DEVICE_ATTR(cpumap,  S_IRUGO, node_read_cpumask, NULL);
static DEVICE_ATTR(cpulist, S_IRUGO, node_read_cpulist, NULL);

#ifdef CONFIG_HMEM_REPORTING
const struct attribute_group node_access_attrs_group;

#define ACCESS_ATTR(name) 						\
static ssize_t name##_show(struct device *dev,				\
			   struct device_attribute *attr,		\
			   char *buf)					\
{									\
	return sprintf(buf, "%d\n", to_node(dev)->hmem_attrs.name);	\
}									\
static DEVICE_ATTR_RO(name);

ACCESS_ATTR(read_bandwidth)
ACCESS_ATTR(read_latency)
ACCESS_ATTR(write_bandwidth)
ACCESS_ATTR(write_latency)

static struct attribute *access_attrs[] = {
	&dev_attr_read_bandwidth.attr,
	&dev_attr_read_latency.attr,
	&dev_attr_write_bandwidth.attr,
	&dev_attr_write_latency.attr,
	NULL,
};

const struct attribute_group node_access_attrs_group = {
	.name		= "primary_initiator_access",
	.attrs		= access_attrs,
};

void node_set_perf_attrs(unsigned int nid, struct node_hmem_attrs *hmem_attrs)
{
	struct node *node;

	if (WARN_ON_ONCE(!node_online(nid)))
		return;
	node = node_devices[nid];
	node->hmem_attrs = *hmem_attrs;
	if (sysfs_create_group(&node->dev.kobj, &node_access_attrs_group))
		pr_info("failed to add performance attribute group to node %d\n",
			nid);
}

struct node_cache_info {
	struct device dev;
	struct list_head node;
	struct node_cache_attrs cache_attrs;
};
#define to_cache_info(device) container_of(device, struct node_cache_info, dev)

#define CACHE_ATTR(name, fmt) 							\
static ssize_t name##_show(struct device *dev,					\
			   struct device_attribute *attr,			\
			   char *buf)						\
{										\
	return sprintf(buf, fmt "\n", to_cache_info(dev)->cache_attrs.name);	\
}										\
DEVICE_ATTR_RO(name);

CACHE_ATTR(size, "%lld")
CACHE_ATTR(level, "%d")
CACHE_ATTR(line_size, "%d")
CACHE_ATTR(associativity, "%d")
CACHE_ATTR(write_policy, "%d")

static struct attribute *cache_attrs[] = {
	&dev_attr_level.attr,
	&dev_attr_associativity.attr,
	&dev_attr_size.attr,
	&dev_attr_line_size.attr,
	&dev_attr_write_policy.attr,
	NULL,
};

const struct attribute_group node_cache_attrs_group = {
	.attrs = cache_attrs,
};

const struct attribute_group *node_cache_attrs_groups[] = {
	&node_cache_attrs_group,
	NULL,
};

static void node_release(struct device *dev)
{
	kfree(dev);
}

static void node_cache_release(struct device *dev)
{
	struct node_cache_info *info = to_cache_info(dev);
	kfree(info);
}

static void node_init_cache_dev(struct node *node)
{
	struct device *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return;

	dev->parent = &node->dev;
	dev->release = node_release;
	dev_set_name(dev, "side_cache");

	if (device_register(dev)) {
		kfree(dev);
		return;
	}
	pm_runtime_no_callbacks(dev);
	node->cache_dev = dev;
}

void node_add_cache(unsigned int nid, struct node_cache_attrs *cache_attrs)
{
	struct node_cache_info *info;
	struct device *dev;
	struct node *node;

	if (!node_online(nid) || !node_devices[nid])
		return;

	node = node_devices[nid];
	list_for_each_entry(info, &node->cache_attrs, node) {
		if (info->cache_attrs.level == cache_attrs->level) {
			dev_warn(&node->dev,
				"attempt to add duplicate cache level:%d\n",
				cache_attrs->level);
			return;
		}
	}

	if (!node->cache_dev)
		node_init_cache_dev(node);
	if (!node->cache_dev)
		return;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return;

	dev = &info->dev;
	dev->parent = node->cache_dev;
	dev->release = node_cache_release;
	dev->groups = node_cache_attrs_groups;
	dev_set_name(dev, "index%d", cache_attrs->level);
	info->cache_attrs = *cache_attrs;
	if (device_register(dev)) {
		dev_warn(&node->dev, "failed to add cache level:%d\n",
			 cache_attrs->level);
		kfree(info);
		return;
	}
	pm_runtime_no_callbacks(dev);
	list_add_tail(&info->node, &node->cache_attrs);
}

static void node_remove_caches(struct node *node)
{
	struct node_cache_info *info, *next;

	if (!node->cache_dev)
		return;

	list_for_each_entry_safe(info, next, &node->cache_attrs, node) {
		list_del(&info->node);
		device_unregister(&info->dev);
	}
	device_unregister(node->cache_dev);
}

static void node_init_caches(unsigned int nid)
{
	INIT_LIST_HEAD(&node_devices[nid]->cache_attrs);
}
#else
static void node_init_caches(unsigned int nid) { }
static void node_remove_caches(struct node *node) { }
#endif

#define K(x) ((x) << (PAGE_SHIFT - 10))
static ssize_t node_read_meminfo(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int n;
	int nid = dev->id;
	struct pglist_data *pgdat = NODE_DATA(nid);
	struct sysinfo i;
	unsigned long sreclaimable, sunreclaimable;

	si_meminfo_node(&i, nid);
	sreclaimable = node_page_state(pgdat, NR_SLAB_RECLAIMABLE);
	sunreclaimable = node_page_state(pgdat, NR_SLAB_UNRECLAIMABLE);
	n = sprintf(buf,
		       "Node %d MemTotal:       %8lu kB\n"
		       "Node %d MemFree:        %8lu kB\n"
		       "Node %d MemUsed:        %8lu kB\n"
		       "Node %d Active:         %8lu kB\n"
		       "Node %d Inactive:       %8lu kB\n"
		       "Node %d Active(anon):   %8lu kB\n"
		       "Node %d Inactive(anon): %8lu kB\n"
		       "Node %d Active(file):   %8lu kB\n"
		       "Node %d Inactive(file): %8lu kB\n"
		       "Node %d Unevictable:    %8lu kB\n"
		       "Node %d Mlocked:        %8lu kB\n",
		       nid, K(i.totalram),
		       nid, K(i.freeram),
		       nid, K(i.totalram - i.freeram),
		       nid, K(node_page_state(pgdat, NR_ACTIVE_ANON) +
				node_page_state(pgdat, NR_ACTIVE_FILE)),
		       nid, K(node_page_state(pgdat, NR_INACTIVE_ANON) +
				node_page_state(pgdat, NR_INACTIVE_FILE)),
		       nid, K(node_page_state(pgdat, NR_ACTIVE_ANON)),
		       nid, K(node_page_state(pgdat, NR_INACTIVE_ANON)),
		       nid, K(node_page_state(pgdat, NR_ACTIVE_FILE)),
		       nid, K(node_page_state(pgdat, NR_INACTIVE_FILE)),
		       nid, K(node_page_state(pgdat, NR_UNEVICTABLE)),
		       nid, K(sum_zone_node_page_state(nid, NR_MLOCK)));

#ifdef CONFIG_HIGHMEM
	n += sprintf(buf + n,
		       "Node %d HighTotal:      %8lu kB\n"
		       "Node %d HighFree:       %8lu kB\n"
		       "Node %d LowTotal:       %8lu kB\n"
		       "Node %d LowFree:        %8lu kB\n",
		       nid, K(i.totalhigh),
		       nid, K(i.freehigh),
		       nid, K(i.totalram - i.totalhigh),
		       nid, K(i.freeram - i.freehigh));
#endif
	n += sprintf(buf + n,
		       "Node %d Dirty:          %8lu kB\n"
		       "Node %d Writeback:      %8lu kB\n"
		       "Node %d FilePages:      %8lu kB\n"
		       "Node %d Mapped:         %8lu kB\n"
		       "Node %d AnonPages:      %8lu kB\n"
		       "Node %d Shmem:          %8lu kB\n"
		       "Node %d KernelStack:    %8lu kB\n"
		       "Node %d PageTables:     %8lu kB\n"
		       "Node %d NFS_Unstable:   %8lu kB\n"
		       "Node %d Bounce:         %8lu kB\n"
		       "Node %d WritebackTmp:   %8lu kB\n"
		       "Node %d KReclaimable:   %8lu kB\n"
		       "Node %d Slab:           %8lu kB\n"
		       "Node %d SReclaimable:   %8lu kB\n"
		       "Node %d SUnreclaim:     %8lu kB\n"
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
		       "Node %d AnonHugePages:  %8lu kB\n"
		       "Node %d ShmemHugePages: %8lu kB\n"
		       "Node %d ShmemPmdMapped: %8lu kB\n"
#endif
			,
		       nid, K(node_page_state(pgdat, NR_FILE_DIRTY)),
		       nid, K(node_page_state(pgdat, NR_WRITEBACK)),
		       nid, K(node_page_state(pgdat, NR_FILE_PAGES)),
		       nid, K(node_page_state(pgdat, NR_FILE_MAPPED)),
		       nid, K(node_page_state(pgdat, NR_ANON_MAPPED)),
		       nid, K(i.sharedram),
		       nid, sum_zone_node_page_state(nid, NR_KERNEL_STACK_KB),
		       nid, K(sum_zone_node_page_state(nid, NR_PAGETABLE)),
		       nid, K(node_page_state(pgdat, NR_UNSTABLE_NFS)),
		       nid, K(sum_zone_node_page_state(nid, NR_BOUNCE)),
		       nid, K(node_page_state(pgdat, NR_WRITEBACK_TEMP)),
		       nid, K(sreclaimable +
			      node_page_state(pgdat, NR_KERNEL_MISC_RECLAIMABLE)),
		       nid, K(sreclaimable + sunreclaimable),
		       nid, K(sreclaimable),
		       nid, K(sunreclaimable)
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
		       ,
		       nid, K(node_page_state(pgdat, NR_ANON_THPS) *
				       HPAGE_PMD_NR),
		       nid, K(node_page_state(pgdat, NR_SHMEM_THPS) *
				       HPAGE_PMD_NR),
		       nid, K(node_page_state(pgdat, NR_SHMEM_PMDMAPPED) *
				       HPAGE_PMD_NR)
#endif
		       );
	n += hugetlb_report_node_meminfo(nid, buf + n);
	return n;
}

#undef K
static DEVICE_ATTR(meminfo, S_IRUGO, node_read_meminfo, NULL);

static ssize_t node_read_numastat(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sprintf(buf,
		       "numa_hit %lu\n"
		       "numa_miss %lu\n"
		       "numa_foreign %lu\n"
		       "interleave_hit %lu\n"
		       "local_node %lu\n"
		       "other_node %lu\n",
		       sum_zone_numa_state(dev->id, NUMA_HIT),
		       sum_zone_numa_state(dev->id, NUMA_MISS),
		       sum_zone_numa_state(dev->id, NUMA_FOREIGN),
		       sum_zone_numa_state(dev->id, NUMA_INTERLEAVE_HIT),
		       sum_zone_numa_state(dev->id, NUMA_LOCAL),
		       sum_zone_numa_state(dev->id, NUMA_OTHER));
}
static DEVICE_ATTR(numastat, S_IRUGO, node_read_numastat, NULL);

static ssize_t node_read_vmstat(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int nid = dev->id;
	struct pglist_data *pgdat = NODE_DATA(nid);
	int i;
	int n = 0;

	for (i = 0; i < NR_VM_ZONE_STAT_ITEMS; i++)
		n += sprintf(buf+n, "%s %lu\n", vmstat_text[i],
			     sum_zone_node_page_state(nid, i));

#ifdef CONFIG_NUMA
	for (i = 0; i < NR_VM_NUMA_STAT_ITEMS; i++)
		n += sprintf(buf+n, "%s %lu\n",
			     vmstat_text[i + NR_VM_ZONE_STAT_ITEMS],
			     sum_zone_numa_state(nid, i));
#endif

	for (i = 0; i < NR_VM_NODE_STAT_ITEMS; i++)
		n += sprintf(buf+n, "%s %lu\n",
			     vmstat_text[i + NR_VM_ZONE_STAT_ITEMS +
			     NR_VM_NUMA_STAT_ITEMS],
			     node_page_state(pgdat, i));

	return n;
}
static DEVICE_ATTR(vmstat, S_IRUGO, node_read_vmstat, NULL);

static ssize_t node_read_distance(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int nid = dev->id;
	int len = 0;
	int i;

	/*
	 * buf is currently PAGE_SIZE in length and each node needs 4 chars
	 * at the most (distance + space or newline).
	 */
	BUILD_BUG_ON(MAX_NUMNODES * 4 > PAGE_SIZE);

	for_each_online_node(i)
		len += sprintf(buf + len, "%s%d", i ? " " : "", node_distance(nid, i));

	len += sprintf(buf + len, "\n");
	return len;
}
static DEVICE_ATTR(distance, S_IRUGO, node_read_distance, NULL);

static struct attribute *node_dev_attrs[] = {
	&dev_attr_cpumap.attr,
	&dev_attr_cpulist.attr,
	&dev_attr_meminfo.attr,
	&dev_attr_numastat.attr,
	&dev_attr_distance.attr,
	&dev_attr_vmstat.attr,
	&dev_attr_primary_mem_nodelist.attr,
	&dev_attr_primary_mem_nodemask.attr,
	&dev_attr_primary_cpu_nodemask.attr,
	&dev_attr_primary_cpu_nodelist.attr,
	NULL
};
ATTRIBUTE_GROUPS(node_dev);

#ifdef CONFIG_HUGETLBFS
/*
 * hugetlbfs per node attributes registration interface:
 * When/if hugetlb[fs] subsystem initializes [sometime after this module],
 * it will register its per node attributes for all online nodes with
 * memory.  It will also call register_hugetlbfs_with_node(), below, to
 * register its attribute registration functions with this node driver.
 * Once these hooks have been initialized, the node driver will call into
 * the hugetlb module to [un]register attributes for hot-plugged nodes.
 */
static node_registration_func_t __hugetlb_register_node;
static node_registration_func_t __hugetlb_unregister_node;

static inline bool hugetlb_register_node(struct node *node)
{
	if (__hugetlb_register_node &&
			node_state(node->dev.id, N_MEMORY)) {
		__hugetlb_register_node(node);
		return true;
	}
	return false;
}

static inline void hugetlb_unregister_node(struct node *node)
{
	if (__hugetlb_unregister_node)
		__hugetlb_unregister_node(node);
}

void register_hugetlbfs_with_node(node_registration_func_t doregister,
				  node_registration_func_t unregister)
{
	__hugetlb_register_node   = doregister;
	__hugetlb_unregister_node = unregister;
}
#else
static inline void hugetlb_register_node(struct node *node) {}

static inline void hugetlb_unregister_node(struct node *node) {}
#endif

static void node_device_release(struct device *dev)
{
	struct node *node = to_node(dev);

#if defined(CONFIG_MEMORY_HOTPLUG_SPARSE) && defined(CONFIG_HUGETLBFS)
	/*
	 * We schedule the work only when a memory section is
	 * onlined/offlined on this node. When we come here,
	 * all the memory on this node has been offlined,
	 * so we won't enqueue new work to this work.
	 *
	 * The work is using node->node_work, so we should
	 * flush work before freeing the memory.
	 */
	flush_work(&node->node_work);
#endif
	node_remove_caches(node);
	kfree(node);
}

/*
 * register_node - Setup a sysfs device for a node.
 * @num - Node number to use when creating the device.
 *
 * Initialize and register the node device.
 */
static int register_node(struct node *node, int num)
{
	int error;

	node->dev.id = num;
	node->dev.bus = &node_subsys;
	node->dev.release = node_device_release;
	node->dev.groups = node_dev_groups;
	error = device_register(&node->dev);

	if (error)
		put_device(&node->dev);
	else {
		hugetlb_register_node(node);

		compaction_register_node(node);
	}
	return error;
}

/**
 * unregister_node - unregister a node device
 * @node: node going away
 *
 * Unregisters a node device @node.  All the devices on the node must be
 * unregistered before calling this function.
 */
void unregister_node(struct node *node)
{
	hugetlb_unregister_node(node);		/* no-op, if memoryless node */

	device_unregister(&node->dev);
}

struct node *node_devices[MAX_NUMNODES];

/*
 * register cpu under node
 */
int register_cpu_under_node(unsigned int cpu, unsigned int nid)
{
	int ret;
	struct device *obj;

	if (!node_online(nid))
		return 0;

	obj = get_cpu_device(cpu);
	if (!obj)
		return 0;

	ret = sysfs_create_link(&node_devices[nid]->dev.kobj,
				&obj->kobj,
				kobject_name(&obj->kobj));
	if (ret)
		return ret;

	return sysfs_create_link(&obj->kobj,
				 &node_devices[nid]->dev.kobj,
				 kobject_name(&node_devices[nid]->dev.kobj));
}

int register_memory_node_under_compute_node(unsigned int m, unsigned int p)
{
	struct node *init, *targ;
	char initiator[28]; /* "primary_initiator4294967295\0" */
	char target[25]; /* "primary_target4294967295\0" */
	int ret;

	if (!node_online(p) || !node_online(m))
		return -ENODEV;
	if (m == p)
		return 0;

	snprintf(initiator, sizeof(initiator), "primary_initiator%d", p);
	snprintf(target, sizeof(target), "primary_target%d", m);

	init = node_devices[p];
	targ = node_devices[m];

	ret = sysfs_create_link(&init->dev.kobj, &targ->dev.kobj, target);
	if (ret)
		return ret;

	ret = sysfs_create_link(&targ->dev.kobj, &init->dev.kobj, initiator);
	if (ret)
		goto err;

	node_set(m, init->primary_mem_nodes);
	node_set(p, targ->primary_cpu_nodes);

	return 0;
 err:
	sysfs_remove_link(&node_devices[p]->dev.kobj,
			  kobject_name(&node_devices[m]->dev.kobj));
	return ret;
}

int unregister_cpu_under_node(unsigned int cpu, unsigned int nid)
{
	struct device *obj;

	if (!node_online(nid))
		return 0;

	obj = get_cpu_device(cpu);
	if (!obj)
		return 0;

	sysfs_remove_link(&node_devices[nid]->dev.kobj,
			  kobject_name(&obj->kobj));
	sysfs_remove_link(&obj->kobj,
			  kobject_name(&node_devices[nid]->dev.kobj));

	return 0;
}

#ifdef CONFIG_MEMORY_HOTPLUG_SPARSE
static int __ref get_nid_for_pfn(unsigned long pfn)
{
	if (!pfn_valid_within(pfn))
		return -1;
#ifdef CONFIG_DEFERRED_STRUCT_PAGE_INIT
	if (system_state < SYSTEM_RUNNING)
		return early_pfn_to_nid(pfn);
#endif
	return pfn_to_nid(pfn);
}

/* register memory section under specified node if it spans that node */
int register_mem_sect_under_node(struct memory_block *mem_blk, void *arg)
{
	int ret, nid = *(int *)arg;
	unsigned long pfn, sect_start_pfn, sect_end_pfn;

	mem_blk->nid = nid;

	sect_start_pfn = section_nr_to_pfn(mem_blk->start_section_nr);
	sect_end_pfn = section_nr_to_pfn(mem_blk->end_section_nr);
	sect_end_pfn += PAGES_PER_SECTION - 1;
	for (pfn = sect_start_pfn; pfn <= sect_end_pfn; pfn++) {
		int page_nid;

		/*
		 * memory block could have several absent sections from start.
		 * skip pfn range from absent section
		 */
		if (!pfn_present(pfn)) {
			pfn = round_down(pfn + PAGES_PER_SECTION,
					 PAGES_PER_SECTION) - 1;
			continue;
		}

		/*
		 * We need to check if page belongs to nid only for the boot
		 * case, during hotplug we know that all pages in the memory
		 * block belong to the same node.
		 */
		if (system_state == SYSTEM_BOOTING) {
			page_nid = get_nid_for_pfn(pfn);
			if (page_nid < 0)
				continue;
			if (page_nid != nid)
				continue;
		}
		ret = sysfs_create_link_nowarn(&node_devices[nid]->dev.kobj,
					&mem_blk->dev.kobj,
					kobject_name(&mem_blk->dev.kobj));
		if (ret)
			return ret;

		return sysfs_create_link_nowarn(&mem_blk->dev.kobj,
				&node_devices[nid]->dev.kobj,
				kobject_name(&node_devices[nid]->dev.kobj));
	}
	/* mem section does not span the specified node */
	return 0;
}

/* unregister memory section under all nodes that it spans */
int unregister_mem_sect_under_nodes(struct memory_block *mem_blk,
				    unsigned long phys_index)
{
	NODEMASK_ALLOC(nodemask_t, unlinked_nodes, GFP_KERNEL);
	unsigned long pfn, sect_start_pfn, sect_end_pfn;

	if (!mem_blk) {
		NODEMASK_FREE(unlinked_nodes);
		return -EFAULT;
	}
	if (!unlinked_nodes)
		return -ENOMEM;
	nodes_clear(*unlinked_nodes);

	sect_start_pfn = section_nr_to_pfn(phys_index);
	sect_end_pfn = sect_start_pfn + PAGES_PER_SECTION - 1;
	for (pfn = sect_start_pfn; pfn <= sect_end_pfn; pfn++) {
		int nid;

		nid = get_nid_for_pfn(pfn);
		if (nid < 0)
			continue;
		if (!node_online(nid))
			continue;
		if (node_test_and_set(nid, *unlinked_nodes))
			continue;
		sysfs_remove_link(&node_devices[nid]->dev.kobj,
			 kobject_name(&mem_blk->dev.kobj));
		sysfs_remove_link(&mem_blk->dev.kobj,
			 kobject_name(&node_devices[nid]->dev.kobj));
	}
	NODEMASK_FREE(unlinked_nodes);
	return 0;
}

int link_mem_sections(int nid, unsigned long start_pfn, unsigned long end_pfn)
{
	return walk_memory_range(start_pfn, end_pfn, (void *)&nid,
					register_mem_sect_under_node);
}

#ifdef CONFIG_HUGETLBFS
/*
 * Handle per node hstate attribute [un]registration on transistions
 * to/from memoryless state.
 */
static void node_hugetlb_work(struct work_struct *work)
{
	struct node *node = container_of(work, struct node, node_work);

	/*
	 * We only get here when a node transitions to/from memoryless state.
	 * We can detect which transition occurred by examining whether the
	 * node has memory now.  hugetlb_register_node() already check this
	 * so we try to register the attributes.  If that fails, then the
	 * node has transitioned to memoryless, try to unregister the
	 * attributes.
	 */
	if (!hugetlb_register_node(node))
		hugetlb_unregister_node(node);
}

static void init_node_hugetlb_work(int nid)
{
	INIT_WORK(&node_devices[nid]->node_work, node_hugetlb_work);
}

static int node_memory_callback(struct notifier_block *self,
				unsigned long action, void *arg)
{
	struct memory_notify *mnb = arg;
	int nid = mnb->status_change_nid;

	switch (action) {
	case MEM_ONLINE:
	case MEM_OFFLINE:
		/*
		 * offload per node hstate [un]registration to a work thread
		 * when transitioning to/from memoryless state.
		 */
		if (nid != NUMA_NO_NODE)
			schedule_work(&node_devices[nid]->node_work);
		break;

	case MEM_GOING_ONLINE:
	case MEM_GOING_OFFLINE:
	case MEM_CANCEL_ONLINE:
	case MEM_CANCEL_OFFLINE:
	default:
		break;
	}

	return NOTIFY_OK;
}
#endif	/* CONFIG_HUGETLBFS */
#endif /* CONFIG_MEMORY_HOTPLUG_SPARSE */

#if !defined(CONFIG_MEMORY_HOTPLUG_SPARSE) || \
    !defined(CONFIG_HUGETLBFS)
static inline int node_memory_callback(struct notifier_block *self,
				unsigned long action, void *arg)
{
	return NOTIFY_OK;
}

static void init_node_hugetlb_work(int nid) { }

#endif

int __register_one_node(int nid)
{
	int error;
	int cpu;

	node_devices[nid] = kzalloc(sizeof(struct node), GFP_KERNEL);
	if (!node_devices[nid])
		return -ENOMEM;

	error = register_node(node_devices[nid], nid);

	/* link cpu under this node */
	for_each_present_cpu(cpu) {
		if (cpu_to_node(cpu) == nid)
			register_cpu_under_node(cpu, nid);
	}

	if (node_state(nid, N_MEMORY))
		node_set(nid, node_devices[nid]->primary_mem_nodes);
	if (node_state(nid, N_CPU))
		node_set(nid, node_devices[nid]->primary_cpu_nodes);

	/* initialize work queue for memory hot plug */
	init_node_hugetlb_work(nid);
	node_init_caches(nid);

	return error;
}

void unregister_one_node(int nid)
{
	if (!node_devices[nid])
		return;

	unregister_node(node_devices[nid]);
	node_devices[nid] = NULL;
}

/*
 * node states attributes
 */

static ssize_t print_nodes_state(enum node_states state, char *buf)
{
	int n;

	n = scnprintf(buf, PAGE_SIZE - 1, "%*pbl",
		      nodemask_pr_args(&node_states[state]));
	buf[n++] = '\n';
	buf[n] = '\0';
	return n;
}

struct node_attr {
	struct device_attribute attr;
	enum node_states state;
};

static ssize_t show_node_state(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct node_attr *na = container_of(attr, struct node_attr, attr);
	return print_nodes_state(na->state, buf);
}

#define _NODE_ATTR(name, state) \
	{ __ATTR(name, 0444, show_node_state, NULL), state }

static struct node_attr node_state_attr[] = {
	[N_POSSIBLE] = _NODE_ATTR(possible, N_POSSIBLE),
	[N_ONLINE] = _NODE_ATTR(online, N_ONLINE),
	[N_NORMAL_MEMORY] = _NODE_ATTR(has_normal_memory, N_NORMAL_MEMORY),
#ifdef CONFIG_HIGHMEM
	[N_HIGH_MEMORY] = _NODE_ATTR(has_high_memory, N_HIGH_MEMORY),
#endif
	[N_MEMORY] = _NODE_ATTR(has_memory, N_MEMORY),
	[N_CPU] = _NODE_ATTR(has_cpu, N_CPU),
};

static struct attribute *node_state_attrs[] = {
	&node_state_attr[N_POSSIBLE].attr.attr,
	&node_state_attr[N_ONLINE].attr.attr,
	&node_state_attr[N_NORMAL_MEMORY].attr.attr,
#ifdef CONFIG_HIGHMEM
	&node_state_attr[N_HIGH_MEMORY].attr.attr,
#endif
	&node_state_attr[N_MEMORY].attr.attr,
	&node_state_attr[N_CPU].attr.attr,
	NULL
};

static struct attribute_group memory_root_attr_group = {
	.attrs = node_state_attrs,
};

static const struct attribute_group *cpu_root_attr_groups[] = {
	&memory_root_attr_group,
	NULL,
};

#define NODE_CALLBACK_PRI	2	/* lower than SLAB */
static int __init register_node_type(void)
{
	int ret;

 	BUILD_BUG_ON(ARRAY_SIZE(node_state_attr) != NR_NODE_STATES);
 	BUILD_BUG_ON(ARRAY_SIZE(node_state_attrs)-1 != NR_NODE_STATES);

	ret = subsys_system_register(&node_subsys, cpu_root_attr_groups);
	if (!ret) {
		static struct notifier_block node_memory_callback_nb = {
			.notifier_call = node_memory_callback,
			.priority = NODE_CALLBACK_PRI,
		};
		register_hotmemory_notifier(&node_memory_callback_nb);
	}

	/*
	 * Note:  we're not going to unregister the node class if we fail
	 * to register the node state class attribute files.
	 */
	return ret;
}
postcore_initcall(register_node_type);
