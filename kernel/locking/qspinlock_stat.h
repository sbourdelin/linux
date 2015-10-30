/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Authors: Waiman Long <waiman.long@hpe.com>
 */

/*
 * When queued spinlock statistics is enabled, the following sysfs files
 * will be created to hold the statistics counters:
 *
 * /sys/kernel/qlockstat/
 *   pv_hash_hops	- average # of hops per hashing operation
 *   pv_kick_unlock	- # of vCPU kicks issued at unlock time
 *   pv_kick_wake	- # of vCPU kicks used for computing pv_latency_wake
 *   pv_latency_kick	- average latency (ns) of vCPU kick operation
 *   pv_latency_wake	- average latency (ns) from vCPU kick to wakeup
 *   pv_spurious_wakeup	- # of spurious wakeups
 *   pv_wait_again	- # of vCPU wait's that happened after a vCPU kick
 *   pv_wait_head	- # of vCPU wait's at the queue head
 *   pv_wait_node	- # of vCPU wait's at a non-head queue node
 *
 * Writing to the "reset_counters" file will reset all the above counter
 * values.
 *
 * These statistics counters are implemented as per-cpu variables which are
 * summed and computed whenever the corresponding sysfs files are read. This
 * minimizes added overhead making the counters usable even in a production
 * environment.
 *
 * There may be slight difference between pv_kick_wake and pv_kick_unlock.
 */
enum qlock_stats {
	qstat_pv_hash_hops,
	qstat_pv_kick_unlock,
	qstat_pv_kick_wake,
	qstat_pv_latency_kick,
	qstat_pv_latency_wake,
	qstat_pv_spurious_wakeup,
	qstat_pv_wait_again,
	qstat_pv_wait_head,
	qstat_pv_wait_node,
	qstat_num,	/* Total number of statistics counters */
	qstat_reset_cnts = qstat_num,
};

#ifdef CONFIG_QUEUED_LOCK_STAT
/*
 * Collect pvqspinlock statistics
 */
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/sched.h>

static const char * const qstat_names[qstat_num + 1] = {
	[qstat_pv_hash_hops]	   = "pv_hash_hops",
	[qstat_pv_kick_unlock]     = "pv_kick_unlock",
	[qstat_pv_kick_wake]       = "pv_kick_wake",
	[qstat_pv_spurious_wakeup] = "pv_spurious_wakeup",
	[qstat_pv_latency_kick]	   = "pv_latency_kick",
	[qstat_pv_latency_wake]    = "pv_latency_wake",
	[qstat_pv_wait_again]      = "pv_wait_again",
	[qstat_pv_wait_head]       = "pv_wait_head",
	[qstat_pv_wait_node]       = "pv_wait_node",
	[qstat_reset_cnts]         = "reset_counters",
};

/*
 * Per-cpu counters
 */
static DEFINE_PER_CPU(unsigned long, qstats[qstat_num]);
static DEFINE_PER_CPU(u64, pv_kick_time);

/*
 * Sysfs data structures
 */
static struct kobj_attribute qstat_kobj_attrs[qstat_num + 1];
static struct attribute *attrs[qstat_num + 2];
static struct kobject *qstat_kobj;
static struct attribute_group attr_group = {
	.attrs = attrs,
};

/*
 * Function to show the qlock statistics count
 */
static ssize_t
qstat_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int cpu, idx;
	u64 stat = 0;

	/*
	 * Compute the index of the kobj_attribute in the array and used
	 * it as the same index as the per-cpu variable
	 */
	idx = attr - qstat_kobj_attrs;

	for_each_online_cpu(cpu)
		stat += per_cpu(qstats[idx], cpu);
	return sprintf(buf, "%llu\n", stat);
}

/*
 * Return the average kick latency (ns) = pv_latency_kick/pv_kick_unlock
 */
static ssize_t
kick_latency_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int cpu;
	u64 latencies = 0, kicks = 0;

	for_each_online_cpu(cpu) {
		kicks     += per_cpu(qstats[qstat_pv_kick_unlock],  cpu);
		latencies += per_cpu(qstats[qstat_pv_latency_kick], cpu);
	}

	/* Rounded to the nearest ns */
	return sprintf(buf, "%llu\n", kicks ? (latencies + kicks/2)/kicks : 0);
}

/*
 * Return the average wake latency (ns) = pv_latency_wake/pv_kick_wake
 */
static ssize_t
wake_latency_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int cpu;
	u64 latencies = 0, kicks = 0;

	for_each_online_cpu(cpu) {
		kicks     += per_cpu(qstats[qstat_pv_kick_wake],    cpu);
		latencies += per_cpu(qstats[qstat_pv_latency_wake], cpu);
	}

	/* Rounded to the nearest ns */
	return sprintf(buf, "%llu\n", kicks ? (latencies + kicks/2)/kicks : 0);
}

/*
 * Return the average hops/hash = pv_hash_hops/pv_kick_unlock
 */
static ssize_t
hash_hop_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int cpu;
	u64 hops = 0, kicks = 0;

	for_each_online_cpu(cpu) {
		kicks += per_cpu(qstats[qstat_pv_kick_unlock], cpu);
		hops  += per_cpu(qstats[qstat_pv_hash_hops],   cpu);
	}

	if (!kicks)
		return sprintf(buf, "0\n");

	/*
	 * Return a X.XX decimal number
	 */
	return sprintf(buf, "%llu.%02llu\n", hops/kicks,
		      ((hops%kicks)*100 + kicks/2)/kicks);
}

/*
 * Reset all the counters value
 *
 * Since the counter updates aren't atomic, the resetting is done twice
 * to make sure that the counters are very likely to be all cleared.
 */
static ssize_t
reset_counters_store(struct kobject *kobj, struct kobj_attribute *attr,
		     const char *buf, size_t count)
{
	int cpu;

	for_each_online_cpu(cpu) {
		int i;
		unsigned long *ptr = per_cpu_ptr(qstats, cpu);

		for (i = 0 ; i < qstat_num; i++)
			WRITE_ONCE(ptr[i], 0);
		for (i = 0 ; i < qstat_num; i++)
			WRITE_ONCE(ptr[i], 0);
	}
	return count;
}

/*
 * Initialize sysfs for the qspinlock statistics
 */
static int __init init_qspinlock_stat(void)
{
	int i, retval;

	qstat_kobj = kobject_create_and_add("qlockstat", kernel_kobj);
	if (qstat_kobj == NULL)
		return -ENOMEM;

	/*
	 * Initialize the attribute table
	 *
	 * As reading from and writing to the stat files can be slow, only
	 * root is allowed to do the read/write to limit impact to system
	 * performance.
	 */
	for (i = 0; i <= qstat_num; i++) {
		qstat_kobj_attrs[i].attr.name = qstat_names[i];
		qstat_kobj_attrs[i].attr.mode = 0400;
		qstat_kobj_attrs[i].show      = qstat_show;
		attrs[i]		      = &qstat_kobj_attrs[i].attr;
	}
	qstat_kobj_attrs[qstat_pv_hash_hops].show    = hash_hop_show;
	qstat_kobj_attrs[qstat_pv_latency_kick].show = kick_latency_show;
	qstat_kobj_attrs[qstat_pv_latency_wake].show = wake_latency_show;

	/*
	 * Set attributes for reset_counters
	 */
	qstat_kobj_attrs[qstat_reset_cnts].attr.mode = 0200;
	qstat_kobj_attrs[qstat_reset_cnts].show      = NULL;
	qstat_kobj_attrs[qstat_reset_cnts].store     = reset_counters_store;

	retval = sysfs_create_group(qstat_kobj, &attr_group);
	if (retval)
		kobject_put(qstat_kobj);

	return retval;
}
fs_initcall(init_qspinlock_stat);

/*
 * Increment the PV qspinlock statistics counters
 */
static inline void qstat_inc(enum qlock_stats stat, bool cond)
{
	if (cond)
		this_cpu_inc(qstats[stat]);
}

/*
 * PV hash hop count
 */
static inline void qstat_hop(int hopcnt)
{
	this_cpu_add(qstats[qstat_pv_hash_hops], hopcnt);
}

/*
 * Replacement function for pv_kick()
 */
static inline void __pv_kick(int cpu)
{
	u64 start = sched_clock();

	per_cpu(pv_kick_time, cpu) = start;
	pv_kick(cpu);
	this_cpu_add(qstats[qstat_pv_latency_kick], sched_clock() - start);
}

/*
 * Replacement function for pv_wait()
 */
static inline void __pv_wait(u8 *ptr, u8 val)
{
	u64 *pkick_time = this_cpu_ptr(&pv_kick_time);

	*pkick_time = 0;
	pv_wait(ptr, val);
	if (*pkick_time) {
		this_cpu_add(qstats[qstat_pv_latency_wake],
			     sched_clock() - *pkick_time);
		qstat_inc(qstat_pv_kick_wake, true);
	}
}

#define pv_kick(c)	__pv_kick(c)
#define pv_wait(p, v)	__pv_wait(p, v)

#else /* CONFIG_QUEUED_LOCK_STAT */

static inline void qstat_inc(enum qlock_stats stat, bool cond)	{ }
static inline void qstat_hop(int hopcnt)			{ }

#endif /* CONFIG_QUEUED_LOCK_STAT */
