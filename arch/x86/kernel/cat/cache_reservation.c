
#include <linux/list.h>
#include <linux/bitmap.h>
#include <linux/kernel.h>
#include <linux/cacheinfo.h>
#include <linux/cpumask.h>
#include <linux/topology.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/module.h>
#include "cache_reservation.h"
#include <uapi/asm/cache_reservation.h>
#include <asm/uaccess.h>

/*
 *
 * There are two main data structures: tcrid entries, and tcrid lists.
 * A tcrid entry contains size,type information and is used 
 * to identify a cache allocation reservation.
 * One task should not allocate more than one tcrid per type 
 * unless that tcrid is to be shared with a different task.
 * A tcrid list is a set of tcrid entries, and is mapped to (used by)
 * one or more tasks.
 * Each task is mapped to only one tcrid list.
 * A tcrid entry can be in one or more tcrid lists at the same time.
 *
 * Mapping to Intel CAT:
 * 	* tcrid list maps one-to-one to a COS-ID.
 * 	* tcrid entry represents a range of bits 
 * 	  in a number of (one or more) Cache Capacity Bitmasks,
 * 	  which are specified in HW via IA32_L3_MASK_n MSRs.
 * 	* one tcrid entry can be in different locations 
 * 	  in different sockets.
 * 	* tcrid entries of a tcrid list must be mapped contiguously
 * 	  in hardware.
 *
 */

unsigned long *closmap; 

LIST_HEAD(tcr_global_list);
DEFINE_MUTEX(tcr_list_mutex);

DECLARE_BITMAP(tcrid_used_bitmap, CBM_LEN);
struct tcr_entry *tcrid_table;
static unsigned int total_tcrentry_bits;

static unsigned int l3_cache_size;
//static u32 max_closid;
static u32 max_cbm_len;
static unsigned int kbytes_per_cbm_bit;
static unsigned int l3_nr_cbm_bits;

static unsigned int max_sockets;

struct cache_layout {
	unsigned long *closmap;
	u32 hw_shared_bitmask;
	int id;
	struct list_head link;
	int nr_users;
};

LIST_HEAD(layout_list);

struct per_socket_data {
	/* start, end of shared region with HW */
	u32 hw_shared_bitmask;
	int initialized;
	unsigned long *cosidzeromask;
	struct cache_layout *layout;
	unsigned int occupied_cbm_bits;
};

struct per_socket_data *psd;
static unsigned int psd_size;

/*
 * CDP capable hardware: CDP-on by default.
 * Use intel_cat_mode=cat kernel parameter to switch to cat.
 */
static bool __read_mostly enable_cdp = 1;
module_param_named(ept, enable_cdp, bool, S_IRUGO);

// protects addition to layout_list
static DEFINE_RAW_SPINLOCK(cache_layout_lock);

DECLARE_BITMAP(cache_layout_ids, MAX_LAYOUTS);

struct cache_layout *find_create_layout(u32 hw_shared_bitmask)
{
	struct cache_layout *l;

	raw_spin_lock(&cache_layout_lock);

	list_for_each_entry(l, &layout_list, link) {
		if (l->hw_shared_bitmask == hw_shared_bitmask)
			l->nr_users++;
			raw_spin_unlock(&cache_layout_lock);
			return l;
	}

	l = kzalloc(GFP_ATOMIC, sizeof(struct cache_layout));
	if (!l) {
		panic("%s alloc failed", __func__);
	}
	l->hw_shared_bitmask = hw_shared_bitmask;
	l->id = find_first_zero_bit(cache_layout_ids, MAX_LAYOUTS);
	if (l->id == MAX_LAYOUTS) {
		printk(KERN_ERR "intel_cat: MAX_LAYOUTS exceeded\n");
		/* reuse id 0 */
		l = list_first_entry(&layout_list, struct cache_layout, link);
		l->nr_users++;
		raw_spin_unlock(&cache_layout_lock);
		return l;
	}
	set_bit(l->id, cache_layout_ids);
	l->nr_users++;
	INIT_LIST_HEAD(&l->link);
	list_add(&l->link, &layout_list);
	raw_spin_unlock(&cache_layout_lock);
	return l;
}

u32 maxtcrlist_id;

int alloc_tcrid_table(void)
{
	struct tcr_entry *e;
	struct cpuinfo_x86 *c = &boot_cpu_data;
	int i;

	maxtcrlist_id = c->x86_cache_max_closid;

	tcrid_table = kzalloc(GFP_KERNEL, CBM_LEN);
	if (!tcrid_table)
		return -ENOMEM;

	for (i = 0; i < CBM_LEN; i++) {
		unsigned int size;
		e = &tcrid_table[i];
		e->tcrid = i;
		size = BITS_TO_LONGS(maxtcrlist_id) * 
			sizeof(unsigned long);
		e->tcrlist_bmap = kzalloc(GFP_KERNEL, size);
		if (!e->tcrlist_bmap) {
			goto out_err;
		}
	}

	return 0;
out_err:
	for (i = 0; i < CBM_LEN; i++) {
		e = &tcrid_table[i];
		kfree(e->tcrlist_bmap);
	}
	kfree(tcrid_table);
	return -ENOMEM;
}


#define reserved_cbm_bits 2
int account_cbm_bits(struct cat_reservation_cpumask *crmask,
		     unsigned int cbm_bits)
{
	unsigned int cpu;

	
	// const struct cpumask *cpumask
	for_each_cpu(cpu, crmask->mask) {
		unsigned int socket, free_cbm_bits;
		struct per_socket_data *psd;

		if (!cpu_online(cpu))
			return 1;

		socket = topology_physical_package_id(cpu);
		psd = get_socket_data(socket);
		free_cbm_bits = l3_nr_cbm_bits - psd->occupied_cbm_bits;
		if (cbm_bits > free_cbm_bits)
			return 1;
	}

	for_each_cpu(cpu, crmask->mask) {
		unsigned int socket, free_cbm_bits;
		struct per_socket_data *psd;

		socket = topology_physical_package_id(cpu);
		psd = get_socket_data(socket);
		psd->occupied_cbm_bits += cbm_bits;
	}
	return 0;
}

int deaccount_cbm_bits(struct tcr_entry *e)
{
	unsigned int cpu;

	for_each_cpu(cpu, e->mask) {
		unsigned int socket;
		struct per_socket_data *psd;

		/* FIXME:
 		 * 
 		 *   1) alloc reservation
 		 *   2) cpu offline
 		 *   3) dealloc reservation
 		 *   4) cpu online
 		 */
		if (!cpu_online(cpu))
			return 1;

		socket = topology_physical_package_id(cpu);
		psd = get_socket_data(socket);
		psd->occupied_cbm_bits -= e->cbm_bits;
	}
	return 0;
}

struct tcr_entry *alloc_tcr_entry(struct cat_reservation_cpumask *crmask,
				  unsigned int cbm_bits)
{
	struct tcr_entry *e;
	int i;

	i = find_first_zero_bit(tcrid_used_bitmap, CBM_LEN);
	if (i >= CBM_LEN) {
		return ERR_PTR(-ENOMEM);
	}

	if (account_cbm_bits(cpumask, cbm_bits))
		return ERR_PTR(-ENOMEM);

	set_bit(i, tcrid_used_bitmap);
	e = &tcrid_table[i];

	return e;
}

struct tcr_entry *find_tcr_entry(u32 tcrid)
{
	struct tcr_entry *e;

	if (tcrid >= CBM_LEN) {
		return ERR_PTR(-EINVAL);
	}
	if (!test_bit(tcrid, tcrid_used_bitmap)) {
		return ERR_PTR(-EINVAL);
	}

	e = &tcrid_table[tcrid];
	return e;
}

void free_tcr_entry(struct tcr_entry *e)
{
	clear_bit(e->tcrid, tcrid_used_bitmap);
	WARN_ON(!bitmap_empty(e->tcrlist_bmap, maxtcrlist_id));
	deaccount_cbm_bits(e);
	if (e->cpumask)
		free_cpumask_var(e->cpumask);
	e->cpumask = NULL;
}

int tcrentry_in_tcrlist(struct tcr_entry *e, struct tcr_list *l)
{
	return test_bit(l->id, e->tcrlist_bmap);
}


#if 0
void tcrlist_changed(struct tcr_list *l)
{
	unsigned int size = BITS_TO_LONGS(max_sockets * NR_CPUS) * sizeof(long);
	bitmap_clear(l->synced_to_socket, size);
}
#endif

int add_tcrentry_to_tcrlist(struct tcr_entry *e, struct tcr_list *l)
{
	set_bit(l->id, e->tcrlist_bmap);
	set_bit(e->tcrid, l->tcrentry_bmap);
	return 0;
}

int remove_tcrentry_from_tcrlist(struct tcr_entry *e, struct tcr_list *l)
{
	clear_bit(l->id, e->tcrlist_bmap);
	clear_bit(e->tcrid, l->tcrentry_bmap);
	/* no more tcrlists referencing this tcrentry: undo allocation
 	   on the cache layouts */
	if (bitmap_empty(&e->tcrlist_bmap, maxtcrlist_id))
		dealloc_contiguous_regions(e, l);
	/* no more tcrentries on this tcrlist: unlink it from task */
	if (bitmap_empty(&l->tcrentry_bmap, CBM_LEN))
		unlink_tcrlist_from_tasks(l);
		
	return 0;
}

/*
 * returns -ENOMEM if not enough space, -EPERM if no permission.
 * returns 0 if reservation has been successful, copying actual
 * number of kbytes reserved to "kbytes", type to type, and tcrid.
 *
 */
int __create_cache_reservation(struct cat_reservation_cpumask *crmask,
			       unsigned long argp)
{
	struct tcr_entry *e;
	unsigned int cbm_bits;
	unsigned int kbytes;
	struct cat_reservation *cr = &crmask->res;
	int ret;

	if (cr->type != CACHE_RSVT_TYPE_BOTH && !enable_cdp)
		return -ENOTSUPP;

	if (cr->type & CACHE_RSVT_ROUND_DOWN)
		kbytes = round_down(cr->kbytes, kbytes_per_cbm_bit);
	else
		kbytes = round_up(cr->kbytes, kbytes_per_cbm_bit);

	if (kbytes > l3_cache_size)
		return -ENOSPC;

	cbm_bits = kbytes / kbytes_per_cbm_bit;

	e = alloc_tcr_entry(crmask, cbm_bits);
	if (IS_ERR(e))
		return PTR_ERR(e);

	/* fix up the cr with the info we got and copy to user */
	cr->kbytes = kbytes;
	cr->type = CACHE_RSVT_TYPE_BOTH;
	cr->flags = 0;
	cr->tcrid = e->tcrid;
	ret = -EFAULT;
	if (copy_to_user(argp, cr, sizeof(*cr)))
		goto out_release_tcrid;

	e->user_kbytes = cr->kbytes;
	e->rounded_kbytes = kbytes;
	e->cbm_bits = kbytes / kbytes_per_cbm_bit;
	e->type = cr->type;

	return 0;
out_release_tcrid:
	free_tcr_entry(e);
	return ret;
}

int create_cache_reservation(struct cat_reservation_cpumask *crmask,
			     unsigned long arg)
{
	cpumask_var_t new_mask;
	int ret;
	struct cat_reservation *cr = crmask->cr;

        if (!alloc_cpumask_var(&new_mask, GFP_KERNEL))
                return -ENOMEM;

        ret = get_user_cpu_mask(crmask->mask, crmask->cpusetsize,
				   new_mask);
        if (ret == 0)
                ret = __create_cache_reservation(crmask, arg);

	if (ret == 0) {
		int len = crmask->cpusetsize;

		size_t retlen = min_t(size_t, len, cpumask_size());

		if (copy_to_user(crmask->mask, new_mask, retlen))
			ret = -EFAULT;
		else
			ret = retlen;
	}
	if (ret > 0)
		cr->cpumask = new_mask;
	else
        	free_cpumask_var(new_mask);
        return retval;
}

/*
 * TCRentry -> TCRlist mapping:
 * Each TCRlist is assigned an id from [0, ..., maxclosid]
 * The id_to_tcrlist[maxclosid] structure contains pointers
 * to tcrlist structures.
 * TCRentries contains a bitmap[0, ..., maxclosid]. A bit
 * set in this bitmap represents the fact that particular
 * tcrlist references the tcrentry.
 */
struct tcr_list *id_to_tcrlist;
#define TCRLIST_ID_SZ 128
DECLARE_BITMAP(tcrlist_ids, TCRLIST_ID_SZ);

static unsigned int alloc_tcrlist_id(void)
{
	unsigned int id;
	id = find_first_zero_bit(&tcrlist_ids, TCRLIST_ID_SZ);
	if (id < TCRLIST_ID_SZ)
		set_bit(id, &tcrlist_ids);
	return id;
}

static void free_tcrlist_id(unsigned int id)
{
	clear_bit(id, &tcrlist_ids);
	id_to_tcrlist[id] = NULL;
}


struct tcr_list *alloc_tcrlist(void)
{
	unsigned int cpus_per_socket;
	struct tcr_list *l;
	unsigned int id;
	u32 size;

	l = kzalloc(sizeof(struct tcr_list), GFP_KERNEL);
	if (!l) {
		return ERR_PTR(-ENOMEM);
	}
	INIT_LIST_HEAD(&l->global_link);
	INIT_LIST_HEAD(&l->tcr_list);
	size = BITS_TO_LONGS(max_sockets * NR_CPUS) * sizeof(long);
	l->synced_to_socket = kzalloc(GFP_KERNEL, size);
	if (!l->synced_to_socket) {
		kfree(l);
		return ERR_PTR(-ENOMEM);
	}
	mutex_lock(&tcr_list_mutex);
	id = alloc_tcrlist_id();
	if (id >= TCRLIST_ID_SZ) {
		kfree(l);
		mutex_unlock(&tcr_list_mutex);
		return ERR_PTR(-ENOMEM);
	}
	l->id = id;
	id_to_tcrlist[id] = l;
	list_add(&l->global_link, &tcr_global_list);

	mutex_unlock(&tcr_list_mutex);
	return l;
}

struct tcr_list *find_tcrlist(unsigned long *cmp_bmap)
{
	struct tcrlist *l;

	list_for_each_entry(l, &tcr_global_list, global_link) {
		if (bitmap_equal(l->tcrentry_bmap, &tcrentry_bmap, CBM_LEN)) 
			return l;
	}
	return NULL;
}

void free_tcrlist(struct tcr_list *l)
{
	mutex_lock(&tcr_list_mutex);
	free_tcrlist_id(l->id);
	mutex_unlock(&tcr_list_mutex);
	kfree(l);
}

/*
 * tcrlist is created when attaching a tcrentry to a task.
 * 
 * destroyed when either task count goes to zero, 
 * or tcrentry count goes to zero.
 *
 */
static void inc_use_count(struct tcr_list *l)
{
	l->nr_tasks++;
}

static void dec_use_count(struct tcr_list *l)
{
	l->nr_tasks--;
	if (l->nr_tasks == 0)
		free_tcrlist(l);
}

int link_tcrlist_to_task(struct task_struct *t, struct tcr_list *l)
{
	inc_use_count(l);
	rcu_assign_pointer(t->tcrlist, l); 
#if 0
	#ifdef CONFIG_INTEL_CAT
        	struct list_head tcrlist_link;
	#endif
#endif

	list_add(&t->tcrlist_link, &l->tasks);
}

int unlink_tcrlist_from_task(struct task_struct *t, struct tcr_list *l)
{
	rcu_assign_pointer(t->tcrlist, NULL);
	rcu_synchronize();
	list_del(&t->tcrlist_link);
	dec_use_count(l);
}

void unlink_tcrlist_from_tasks(struct tcr_list *l)
{
	struct task_struct *tsk, *tsk2;

	list_for_each_entry_safe(tsk, tsk2, &l->tasks, tcrlist_link) {
		rcu_assign_pointer(tsk->tcrlist, NULL);
		kick_task(tsk);
	}
	rcu_synchronize();

	list_for_each_entry_safe(tsk, tsk2, &l->tasks, tcrlist_link) {
		list_del(&t->tcrlist_link);
		dec_use_count(l);
	}
}

int delete_cache_reservation(struct cat_tcrid *i)
{
        struct tcr_entry *e;
	int bit;

        e = find_tcr_entry(i->tcrid);
        if (IS_ERR(e)) {
                return PTR_ERR(e);
        }

	for_each_set_bit(bit, &e->tcrlist_bmap, maxtcrlist_id) {
		struct tcr_list *l;

		l = id_to_tcrlist[id];
		if (!l) {
			BUG_ON();
			return 0;
		}
		remove_tcrentry_from_tcrlist(e, l);
		kick_tasks(l);
	}
	free_tcr_entry(e);
	return 0;
}


int check_contiguous_region(struct tcr_entry *e, struct tcr_list *l,
			    struct cache_layout *layout, int *size_p)
{
        unsigned long *temp_closmap;
        u32 size = BITS_TO_LONGS(max_cbm_len) * sizeof(unsigned long);
	struct tcr_list_per_socket *psd = l->psd[layout->id];
	u32 cbm_bits;

        temp_closmap = kzalloc(GFP_KERNEL, size);
        if (!temp_closmap) {
                return -ENOMEM;
        }

        memcpy(temp_closmap, layout->closmap, size);
	/* mark cache ways shared with hw as busy */
	bitmap_or(temp_closmap, &layout->hw_shared_bitmask, min(max_cbm_len, 32));
	cbm_bits = 0;
	if (psd->cbm_end_bit) {
		cbm_bits = psd->cbm_end_bit - psd->cbm_start_bit + 1;
		bitmap_clear(temp_closmap, psd->cbm_start_bit, cbm_bits);
	}

	cbm_bits += e->cbm_bits;
	s = bitmap_find_next_zero_area(temp_closmap, max_cbm_len, 0,
				   cbm_bits, 0);
	if (s >= max_cbm_len) {
        	kfree(temp_closmap);
		return -EBUSY;
	}
	*size_p = cbm_bits;
	return s;
}

int alloc_contiguous_region(struct tcr_entry *e, struct tcr_list *l,
			    struct cache_layout *layout)
{
	int size_p, r;
	struct tcr_list_per_socket *psd = l->psd[layout->id];

	r = check_contiguous_region(e, l, clayout, &size_p);
	if (r < 0)
		return r;
	
	psd->cbm_start_bit = r;
	psd->cbm_end_bit = r + size_p;

	for (bit = psd->cbm_start_bit; bit < psd->cbm_end_bit;
		bit++) {
		__set_bit(bit, layout->closmap);
	}
	return 0;
}

int alloc_contiguous_regions(struct tcr_entry *e, struct tcr_list *l)
{
	struct cache_layout *clayout;

	list_for_each_entry(clayout, &layout_list, link) {
		int size_p, r;

		r = check_contiguous_region(e, l, clayout, &size_p);
		if (r < 0)
			return error;
		r = alloc_contiguous_region(e, l, clayout);
		if (r) {
			WARN_ON(1);
		}
	}
}

int dealloc_contiguous_regions(struct tcr_entry *e, struct tcr_list *l)
{
	struct cache_layout *clayout;

	list_for_each_entry(clayout, &layout_list, link) {
		struct tcr_list_per_socket *psd = l->psd[clayout->id];
		int bit;

		for (bit = psd->cbm_start_bit; bit < psd->cbm_end_bit;
			bit++) {
			__clear_bit(bit, layout->closmap);
		}
	}
}

void kick_task(struct task_struct *tsk)
{
	set_tsk_need_resched(tsk);
	kick_process(tsk);
}

/* When attach returns, any task attached to the tcrlist
 * which has been modified must:
 *	Task Running) sync_to_msr.
 *	Task Not Running) nothing, as long as sync_to_msr is performed
 *	when its scheduled in.
 */
void kick_tasks(struct tcr_list *l)
{
	struct task_struct *tsk;

	list_for_each_entry(tsk, &l->tasks, tcrlist_link) {
		set_tsk_need_resched(tsk);
		kick_process(tsk);
	}
}

int attach_cache_reservation(struct pid_cat_reservation *pcr)
{
	struct pid *pid;
	struct task_struct *task;
	struct tcr_list *l, *undo;
	struct tcr_entry *e;

	e = find_tcr_entry(pcr->tcrid);
	if (IS_ERR(e)) {
		return PTR_ERR(e);
	}

	pid = find_get_pid(pcr);
	if (!pid) {
		return -ENOSYS;
	}

	task = get_pid_task(task);
	if (!task) {
		put_pid(pid;
		return -EINVAL;
	}

	if (!task->tcrlist) {
		u64 b = 1UL << e->tcrid;

		l = find_tcrlist(&b);
		if (l) { 
			link_tcrlist_to_task(task,l);
			return 0;
		}
		l = alloc_tcrlist();
		if (IS_ERR(l)) {
			put_pid(pid);
			put_task_struct(task);
			return PTR_ERR(l);
		}
		undo = l;
	} else {
		l = task->tcrlist;
	}

	if (tcrentry_in_tcrlist(e, l))
		return -EINVAL;

	if (l->nr_tasks > 1) {
		struct tcrlist_entry *lnew;
		u64 b = l->tcrentry_bmap;

		set_bit(e->tcrid, &b);

		lnew = find_tcrlist(&b);
		if (lnew) {
			unlink_tcrlist_from_task(task, l);
			link_tcrlist_to_task(task, lnew);
			goto out;
		}

		lnew = alloc_tcrlist();
		if (IS_ERR(lnew)) {
			put_pid(pid);
			put_task_struct(task);
			return PTR_ERR(lnew);
		}

		if (alloc_contiguous_regions(e, lnew) == -ENOSPC) {
			free_tcrlist(lnew);
			return -ENOSPC;
		}
		for_each_set_bit(bit, &l->tcrentry_bmap, CBM_LEN) {
			struct tcr_entry *et;

			et = &tcrid_table[bit];
			add_tcrentry_to_tcrlist(et, lnew);
		}
		unlink_tcrlist_from_task(task, l);
		link_tcrlist_to_task(task, lnew);
		l = lnew;
	} else {
		if (alloc_contiguous_regions(e, l) == -ENOSPC) {
			if (undo)
				free_tcrlist(undo);
			return -ENOSPC;
		}
	}

	add_tcrentry_to_tcrlist(e, l);
	kick_tasks(l);
out:
	put_pid(pid);
	put_task_struct(task);
	return 0;
}

int detach_cache_reservation(struct pid_cat_reservation *pcr)
{
	struct pid *pid;
	struct task_struct *task;
	struct tcr_list *l, *undo;
	struct tcr_entry *e;
	int err;

	e = find_tcr_entry(pcr->tcrid);
	if (IS_ERR(e)) {
		return PTR_ERR(e);
	}

	pid = find_get_pid(pcr);
	if (!pid) {
		return -ENOSYS;
	}

	task = get_pid_task(task);
	if (!task) {
		put_pid(pid);
		return -EINVAL;
	}

	l = task->tcrlist;
	if (!l) {
		err = -EINVAL;
		goto out;
	}

	if (!tcrentry_in_tcrlist(e, l))
		return -EINVAL;

	if (l->nr_tasks > 1) {
		struct tcrlist_entry *lnew;
		u64 b = l->tcrentry_bmap;

		clear_bit(e->tcrid, &b);

		lnew = find_tcrlist(&b);
		if (lnew) {
			unlink_tcrlist_from_task(task, l);
			link_tcrlist_to_task(task, lnew);
			kick_task(task);
			goto out;
		}

		lnew = alloc_tcrlist();
		if (IS_ERR(lnew)) {
			put_pid(pid);
			put_task_struct(task);
			return PTR_ERR(lnew);
		}
		for_each_set_bit(bit, &l->tcrentry_bmap, CBM_LEN) {
			struct tcr_entry *et;

			if (bit == e->tcrid)
				continue;

			et = &tcrid_table[bit];
			add_tcrentry_to_tcrlist(et, lnew);
		}
		unlink_tcrlist_from_task(task, l);
		link_tcrlist_to_task(task, lnew);
		l = lnew;
		kick_task(task);
	} else {
		remove_tcrentry_from_tcrlist(e, l);
	}
	
	err = 0;
out:
	put_pid(pid);
	put_task_struct(task);
	return err;
}

void sync_to_msr(struct task_struct *task, struct tcr_list *l,
		 unsigned int start, unsigned int end)
{
	u64 msr;
	unsigned long bitmask = -1;
	int len = end - start + 1;

	bitmask = bitmask << (sizeof(unsigned long)*8 - len);
	bitmask = bitmask >> (sizeof(unsigned long)*8 - end -1);

	/* check and enforce cosidzero has [s,e] == 0 */
	rdmsrl(CBM_FROM_INDEX(0), msr);
	if (msr & bitmask)
		wrmsrl(CBM_FROM_INDEX(0), msr & ~bitmask);

	/* check and enforce this cosid has [s,e] == 1. */
	rdmsrl(CBM_FROM_INDEX(l->id), msr);
	if ((msr & bitmask) != bitmask)
		wrmsrl(CBM_FROM_INDEX(l->id), msr | bitmask);

	set_bit(this_socket, task->tcrlist->synced_to_socket);
}		 

void __intel_rdt_sched_in(void)
{
	struct task_struct *task = current;
	unsigned int cpu = smp_processor_id();
	unsigned int this_socket = topology_physical_package_id(cpu);
	unsigned int start, end;
	struct per_socket_data *psd = get_socket_data(this_socket);

	/*
 	 * The CBM bitmask for a particular task is enforced
 	 * on sched-in to a given processor, and only for the 
 	 * range (cbm_start_bit,cbm_end_bit) which the 
 	 * tcr_list (COSid) owns.
 	 * This way we allow COSid0 (global task pool) to use
 	 * reserved L3 cache on sockets where the tasks that
 	 * reserve the cache have not been scheduled.
 	 *
 	 * Since reading the MSRs is slow, it is necessary to
 	 * cache the MSR CBM map on each socket.
 	 *
 	 */

	if (task->tcrlist == NULL) {
		wrmsrl(CBM_FROM_INDEX(0), psd->cosidzeromask);
	}
	else if (test_bit(this_socket,
		     task->tcrlist->synced_to_socket) == 0) {
		spin_lock(&this_socket->msr_cbm_lock);
		unsigned int start;
		struct per_socket_data *psd = get_socket_data(this_socket);
		struct cache_layout *layout = psd->layout;

		start = task->tcrlist->psd[layout->id].cbm_start;
		end = task->tcrlist->psd[layout->id].cbm_end;
		sync_to_msr(task, tcrlist, start, end);
		// barrier
		spin_unlock(&this_socket->msr_cbm_lock);
	}

}

static int get_reservations(struct cat_reservation_list *in,
			    unsigned long arg)
{
	int r, bit;
	struct cat_reservation *cr;
	void *res_user_ptr, *cpumask_user_ptr;
	unsigned int copied_entries;
	unsigned int x, coffset, uoffset;
	size_t cpumasksz;

	cpumasksz = cpumask_size()*bitmap_weight(&tcrid_used_bitmap, CBM_LEN);
	cpumasksz = min_t(size_t, cpumasksz);

	x = sizeof(*cr)*cpumasksz;
	if (x > in->cat_res_size)
		return -ENOSPC;
	if (cpumasksz > in->cpumask_size)
		return -ENOSPC;

	cr = kzalloc(GFP_KERNEL, sizeof(*cr));
	if (!cr)
		return -ENOMEM;

	res_user_ptr = in->list;
	cpumask_user_ptr = in->mask;

	in->cpumask_size = cpumasksz;
	r = -EFAULT;
	if (copy_to_user(argp, &in, sizeof(*in)))
		goto out;

	uoffset = coffset = copied_entries = 0;

	for_each_set_bit(bit, &tcrid_used_bitmap, CBM_LEN) {
		struct tcr_entry *e = &tcrid_table[bit];

		cr->kbytes = e->rounded_kbytes;
		cr->type = e->type;
		cr->flags = 0;
		cr->tcrid = tcrid;

		if (copy_to_user(user_ptr + uoffset, &cr, sizeof(*cr))) {
			r = -EFAULT;
			goto out;
		}
		uoffset += sizeof(*cr);

		if (copy_to_user(cpumask_user_ptr + coffset, e->cpumask, cpumasksz)) {
			r = -EFAULT;
			goto out;
		}
		coffset += cpumasksz;
		copied_entries++;

		memset(cr, 0, sizeof(*cr));
	}

	copied_entries = r;

out:
	kfree(cr);
	return r;
}

static int basic_cr_checks(struct cat_reservation *cr)
{
	int r;

	r = -EINVAL;
	if (cr->type != CACHE_RSVT_TYPE_CODE &&
	    cr->type != CACHE_RSVT_TYPE_DATA &&
	    cr->type != CACHE_RSVT_TYPE_BOTH)
		return r;

	if (cr->flags != 0 && cr->flags != CACHE_RSVT_ROUND_DOWN)
		return r;

	r = 0;
	return r;
}

static long intelcat_ioctl(struct file *filp,
			   unsigned int ioctl, unsigned long arg)
{
        long r = -EINVAL;
	switch (ioctl) {
		case CAT_CREATE_RESERVATION:
			struct cat_reservation_cpumask crmask;

			r = -EFAULT;
			if (copy_from_user(&crmask, argp, sizeof(crmask)))
				goto out;

			r = basic_cr_checks(&crmask.res);
			if (r)
				goto out;

			r = create_cache_reservation(&crmask, arg);
	
			break;
		case CAT_DELETE_RESERVATION:
			struct cat_tcrid tcrid;

			r = -EFAULT;
			if (copy_from_user(&tcrid, argp, sizeof(cr)))
				goto out;

			r = delete_cache_reservation(&tcrid);

			break;
		case CAT_ATTACH_RESERVATION:
			struct pid_cat_reservation pcr;
			r = -EFAULT;
		
			if (copy_from_user(&pcr, argp, sizeof(pcr)))
				goto out;
			r = attach_cache_reservation(&pcr);
			break;
		case CAT_DETACH_RESERVATION:
			struct pid_cat_reservation pcr;
			r = -EFAULT;
		
			if (copy_from_user(&pcr, argp, sizeof(pcr)))
				goto out;
			r = detach_cache_reservation(&pcr);
			break;
		case CAT_GET_RESERVATIONS:
			struct cat_reservation_list *in;
			r = -EFAULT;
		
			if (copy_from_user(&pcr, argp, sizeof(pcr)))
				goto out;

			r = get_reservations(in, argp);
			return r;
		default:
			break;
	}

out:
	return r;
}

static struct file_operations intelcat_chardev_ops = {
	.unlocked_ioctl	= intelcat_ioctl,
	.compat_ioctl	= intelcat_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice intel_cat_misc = 
{
	INTEL_CAT_MINOR,
	"intel_cat",
	&intelcat_chardev_ops,
};

static int get_l3_cache_size(void)
{
	struct cpu_cacheinfo *cinfo;
	struct cacheinfo *ci;

	cinfo = get_cpu_cacheinfo(0);

	if (cinfo && cinfo->num_levels >= 3) {
		ci = cinfo->info_list[3];
		l3_cache_size = ci->size;
		return 0;
	}
	return -EINVAL;
}

static struct per_socket_data *get_socket_data(int socket)
{
	struct per_socket_data *data;

	if (socket >= psd_size) {
		BUG_ON();
		return NULL;
	}
	return &psd[socket];
}

static int __init alloc_init_per_socket_data(void)
{
	psd = kzalloc(max_sockets * sizeof(struct per_socket_data));
	if (!psd)
		return -ENOMEM;
	psd_size = max_sockets;
	return 0;
}

static void percpu_init_hw_shared_zone(void)
{
	unsigned int cpu, this_socket;
	struct cpuinfo_x86 *c;
	uint32_t eax, ebx, ecx, edx;
	struct per_socket_data *psd;
	u32 size;

	cpu = smp_processor_id();
	this_socket = topology_physical_package_id(cpu);
	psd = get_socket_data(this_socket);
	c = &cpu_data(cpu);

	cpuid_count(0x00000010, 1, &eax, &ebx, &ecx, &edx);
	if (atomic_test_and_set(&psd->initialized))
		return 0;
	psd->hw_shared_bitmask = ebx;
	// reserve 10% of cache ways for host
	psd->reserved_for_host = c->x86_cache_max_cbm_len/10;
	psd->reserved_for_host = max(psd->reserved_for_host,
			bitmap_weight(&psd->hw_shared_bitmask));
	psd->layout = find_create_layout(psd->hw_shared_bitmask);
	
        size = BITS_TO_LONGS(c->x86_cache_max_cbm_len) * sizeof(unsigned long);
	if (cdp_enabled)
		size = 2*size;
	psd->cosidzeromask = kzalloc(size, GFP_ATOMIC);
	if (!closmap)
		panic("%s allocation failed\n", __func__);
		
	memset(psd->cosidzeromask, 1, size);
}

static int cat_cpu_notifier(struct notifier_block *nfb,
                            unsigned long action, void *hcpu)
{
        unsigned int cpu = (unsigned long)hcpu;

        switch (action) {
                case CPU_ONLINE:
			percpu_init_hw_shared_zone();
			break;
        }
        return NOTIFY_OK;
}

static struct notifier_block cat_cpu_notifier_block = {
        .notifier_call  = cat_cpu_notifier,
        .priority = -INT_MAX
};

static int init_hw_shared_zone(void)
{
	cpumask_t cpumask;
	int cpu;
	unsigned long *topology_bmap;
	int size = BITS_TO_LONGS(max_sockets * NR_CPUS) * sizeof(long);

	topology_bmap = kzalloc(size, GFP_KERNEL);
	if (!topology_bmap)
		return -ENOMEM;

	cpumask_zero(&cpumask);

	for_each_online_cpu(cpu) {
		phys_id = topology_physical_package_id(cpu);
		if (test_and_set_bit(phys_id, topology_bmap))
			continue;
		cpumask_set_cpu(cpu, &cpumask);
	}
		
	smp_call_function_many(&cpumask,
				percpu_init_hw_shared_zone, 0, 1);

	kfree(topology_bmap);

	return 0;
}


static int __init intel_cat_mem_init(void)
{
	struct cpuinfo_x86 *c = &boot_cpu_data;
	u32 maxid;

	err = -ENOMEM;

	max_cbm_len = c->x86_cache_max_cbm_len;
	maxid = max_closid = c->x86_cache_max_closid;
	//maxid = max_closid = c->x86_cache_max_closid;
	size = BITS_TO_LONGS(maxid) * sizeof(long);
	closmap = kzalloc(size, GFP_KERNEL);
	if (!closmap)
		goto err_out;

	size = maxid * sizeof(struct tcr_list *);
	id_to_tcrlist = kzalloc(size, GFP_KERNEL);
	if (!id_to_tcrlist)
		goto err_out;

	err = alloc_tcrid_table();
	if (err)
		goto err_out;

	err = get_l3_cache_size();
	if (err)
		goto err_out;

	/* kbytes per cbm bit = 
 	 * L3 cache size in kbytes / capacity bitmask length.
 	 */
	kbytes_per_cbm_bit = (l3_cache_size >> 10) / max_cbm_len;

	/* L3 cache size in kbytes / kbytes per cbm bit = 
 	 * cbm bits in L3 cache.
 	 */
	l3_nr_cbm_bits = (l3_cache_size >> 10) / kbytes_per_cbm_bit;

	err = alloc_init_per_socket_data();
	if (err)
		goto err_out;

	init_hw_shared_zone();

	/* bit 0 is reserved for global task pool */
	set_bit(0, &tcrlist_ids);

	return 0;
err_out:
	kfree(id_to_tcrlist);
	kfree(closmap);
	return err;
}

static int __init intel_cat_init(void)
{
	int r;
	int cpu;

	preempt_disable();
	cpu = smp_processor_id();
	cpus_per_socket = cpumask_weight(topology_core_cpumask(cpu));
	max_sockets = NR_CPUS/cpus_per_socket;
	preempt_enable();

	r = misc_register(&intel_cat_misc);
	if (r) {
		printk(KERN_ERR "intel_cat: misc_register error = %d\n",r);
		return r;
	}

	r = intel_cat_mem_init();
	if (r) {
		misc_unregister(&intel_cat_misc);
	}

	cpu_notifier_register_begin();
	__register_hotcpu_notifier(&cat_cpu_notifier_block);
	cpu_notifier_register_done();

	return r;
}

