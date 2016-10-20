/*
 * Cavium Thunder uncore PMU support.
 *
 * Copyright (C) 2015,2016 Cavium Inc.
 * Author: Jan Glauber <jan.glauber@cavium.com>
 */

#include <linux/cpufeature.h>
#include <linux/numa.h>
#include <linux/slab.h>

#include "uncore_cavium.h"

/*
 * Some notes about the various counters supported by this "uncore" PMU
 * and the design:
 *
 * All counters are 64 bit long.
 * There are no overflow interrupts.
 * Counters are summarized per node/socket.
 * Most devices appear as separate PCI devices per socket with the exception
 * of OCX TLK which appears as one PCI device per socket and contains several
 * units with counters that are merged.
 * Some counters are selected via a control register (L2C TAD) and read by
 * a number of counter registers, others (L2C CBC, LMC & OCX TLK) have
 * one dedicated counter per event.
 * Some counters are not stoppable (L2C CBC & LMC).
 * Some counters are read-only (LMC).
 * All counters belong to PCI devices, the devices may have additional
 * drivers but we assume we are the only user of the counter registers.
 * We map the whole PCI BAR so we must be careful to forbid access to
 * addresses that contain neither counters nor counter control registers.
 */

void thunder_uncore_read(struct perf_event *event)
{
	struct thunder_uncore *uncore = to_uncore(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore_node *node;
	struct thunder_uncore_unit *unit;
	u64 prev, delta, new = 0;

	node = get_node(hwc->config, uncore);

	/* read counter values from all units on the node */
	list_for_each_entry(unit, &node->unit_list, entry)
		new += readq(hwc->event_base + unit->map);

	prev = local64_read(&hwc->prev_count);
	local64_set(&hwc->prev_count, new);
	delta = new - prev;
	local64_add(delta, &event->count);
}

int thunder_uncore_add(struct perf_event *event, int flags, u64 config_base,
		       u64 event_base)
{
	struct thunder_uncore *uncore = to_uncore(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore_node *node;
	int id;

	node = get_node(hwc->config, uncore);
	id = get_id(hwc->config);

	if (!cmpxchg(&node->events[id], NULL, event))
		hwc->idx = id;

	if (hwc->idx == -1)
		return -EBUSY;

	hwc->config_base = config_base;
	hwc->event_base = event_base;
	hwc->state = PERF_HES_UPTODATE | PERF_HES_STOPPED;

	if (flags & PERF_EF_START)
		uncore->pmu.start(event, PERF_EF_RELOAD);

	return 0;
}

void thunder_uncore_del(struct perf_event *event, int flags)
{
	struct thunder_uncore *uncore = to_uncore(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore_node *node;
	int i;

	event->pmu->stop(event, PERF_EF_UPDATE);

	/*
	 * For programmable counters we need to check where we installed it.
	 * To keep this function generic always test the more complicated
	 * case (free running counters won't need the loop).
	 */
	node = get_node(hwc->config, uncore);
	for (i = 0; i < node->num_counters; i++) {
		if (cmpxchg(&node->events[i], event, NULL) == event)
			break;
	}
	hwc->idx = -1;
}

void thunder_uncore_start(struct perf_event *event, int flags)
{
	struct thunder_uncore *uncore = to_uncore(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore_node *node;
	struct thunder_uncore_unit *unit;
	u64 new = 0;

	/* read counter values from all units on the node */
	node = get_node(hwc->config, uncore);
	list_for_each_entry(unit, &node->unit_list, entry)
		new += readq(hwc->event_base + unit->map);
	local64_set(&hwc->prev_count, new);

	hwc->state = 0;
	perf_event_update_userpage(event);
}

void thunder_uncore_stop(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;

	hwc->state |= PERF_HES_STOPPED;

	if ((flags & PERF_EF_UPDATE) && !(hwc->state & PERF_HES_UPTODATE)) {
		thunder_uncore_read(event);
		hwc->state |= PERF_HES_UPTODATE;
	}
}

int thunder_uncore_event_init(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore_node *node;
	struct thunder_uncore *uncore;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/* we do not support sampling */
	if (is_sampling_event(event))
		return -EINVAL;

	/* counters do not have these bits */
	if (event->attr.exclude_user	||
	    event->attr.exclude_kernel	||
	    event->attr.exclude_host	||
	    event->attr.exclude_guest	||
	    event->attr.exclude_hv	||
	    event->attr.exclude_idle)
		return -EINVAL;

	uncore = to_uncore(event->pmu);
	if (!uncore)
		return -ENODEV;
	if (!uncore->event_valid(event->attr.config & UNCORE_EVENT_ID_MASK))
		return -EINVAL;

	/* check NUMA node */
	node = get_node(event->attr.config, uncore);
	if (!node) {
		pr_debug("Invalid NUMA node selected\n");
		return -EINVAL;
	}

	hwc->config = event->attr.config;
	hwc->idx = -1;
	return 0;
}

static ssize_t thunder_uncore_attr_show_cpumask(struct device *dev,
						struct device_attribute *attr,
						char *buf)
{
	struct pmu *pmu = dev_get_drvdata(dev);
	struct thunder_uncore *uncore =
		container_of(pmu, struct thunder_uncore, pmu);

	return cpumap_print_to_pagebuf(true, buf, &uncore->active_mask);
}
static DEVICE_ATTR(cpumask, S_IRUGO, thunder_uncore_attr_show_cpumask, NULL);

static struct attribute *thunder_uncore_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

struct attribute_group thunder_uncore_attr_group = {
	.attrs = thunder_uncore_attrs,
};

ssize_t thunder_events_sysfs_show(struct device *dev,
				  struct device_attribute *attr,
				  char *page)
{
	struct perf_pmu_events_attr *pmu_attr =
		container_of(attr, struct perf_pmu_events_attr, attr);

	if (pmu_attr->event_str)
		return sprintf(page, "%s", pmu_attr->event_str);

	return 0;
}

/* node attribute depending on number of NUMA nodes */
static ssize_t node_show(struct device *dev, struct device_attribute *attr,
			 char *page)
{
	if (NODES_SHIFT)
		return sprintf(page, "config:16-%d\n", 16 + NODES_SHIFT - 1);
	else
		return sprintf(page, "config:16\n");
}

struct device_attribute format_attr_node = __ATTR_RO(node);

/*
 * Thunder uncore events are independent from CPUs. Provide a cpumask
 * nevertheless to prevent perf from adding the event per-cpu and just
 * set the mask to one online CPU. Use the same cpumask for all uncore
 * devices.
 *
 * There is a performance penalty for accessing a device from a CPU on
 * another socket, but we do not care (yet).
 */
static int thunder_uncore_offline_cpu(unsigned int old_cpu, struct hlist_node *node)
{
	struct thunder_uncore *uncore = hlist_entry_safe(node, struct thunder_uncore, node);
	int new_cpu;

	if (!cpumask_test_and_clear_cpu(old_cpu, &uncore->active_mask))
		return 0;
	new_cpu = cpumask_any_but(cpu_online_mask, old_cpu);
	if (new_cpu >= nr_cpu_ids)
		return 0;
	perf_pmu_migrate_context(&uncore->pmu, old_cpu, new_cpu);
	cpumask_set_cpu(new_cpu, &uncore->active_mask);
	return 0;
}

static struct thunder_uncore_node * __init alloc_node(struct thunder_uncore *uncore,
						      int node_id, int counters)
{
	struct thunder_uncore_node *node;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return NULL;
	node->num_counters = counters;
	INIT_LIST_HEAD(&node->unit_list);
	return node;
}

int __init thunder_uncore_setup(struct thunder_uncore *uncore, int device_id,
				struct pmu *pmu, int counters)
{
	unsigned int vendor_id = PCI_VENDOR_ID_CAVIUM;
	struct thunder_uncore_unit  *unit, *tmp;
	struct thunder_uncore_node *node;
	struct pci_dev *pdev = NULL;
	int ret, node_id, found = 0;

	/* detect PCI devices */
	while ((pdev = pci_get_device(vendor_id, device_id, pdev))) {
		if (!pdev)
			break;

		node_id = dev_to_node(&pdev->dev);

		/* allocate node if necessary */
		if (!uncore->nodes[node_id])
			uncore->nodes[node_id] = alloc_node(uncore, node_id, counters);

		node = uncore->nodes[node_id];
		if (!node) {
			ret = -ENOMEM;
			goto fail;
		}

		unit = kzalloc(sizeof(*unit), GFP_KERNEL);
		if (!unit) {
			ret = -ENOMEM;
			goto fail;
		}

		unit->pdev = pdev;
		unit->map = ioremap(pci_resource_start(pdev, 0),
				    pci_resource_len(pdev, 0));
		list_add(&unit->entry, &node->unit_list);
		node->nr_units++;
		found++;
	}

	if (!found)
		return -ENODEV;

	cpuhp_state_add_instance_nocalls(CPUHP_AP_UNCORE_CAVIUM_ONLINE,
                                         &uncore->node);

	/*
	 * perf PMU is CPU dependent in difference to our uncore devices.
	 * Just pick a CPU and migrate away if it goes offline.
	 */
	cpumask_set_cpu(smp_processor_id(), &uncore->active_mask);

	uncore->pmu = *pmu;
	ret = perf_pmu_register(&uncore->pmu, uncore->pmu.name, -1);
	if (ret)
		goto fail;

	return 0;

fail:
	node_id = 0;
	while (uncore->nodes[node_id]) {
		node = uncore->nodes[node_id];

		list_for_each_entry_safe(unit, tmp, &node->unit_list, entry) {
			if (unit->pdev) {
				if (unit->map)
					iounmap(unit->map);
				pci_dev_put(unit->pdev);
			}
			kfree(unit);
		}
		kfree(uncore->nodes[node_id]);
		node_id++;
	}
	return ret;
}

static int __init thunder_uncore_init(void)
{
	unsigned long implementor = read_cpuid_implementor();
	int ret;

	if (implementor != ARM_CPU_IMP_CAVIUM)
		return -ENODEV;

	ret = cpuhp_setup_state_multi(CPUHP_AP_UNCORE_CAVIUM_ONLINE,
				      "AP_PERF_UNCORE_CAVIUM_ONLINE", NULL,
				      thunder_uncore_offline_cpu);
	if (ret)
		return ret;

	thunder_uncore_l2c_tad_setup();
	thunder_uncore_l2c_cbc_setup();
	return 0;
}
late_initcall(thunder_uncore_init);
