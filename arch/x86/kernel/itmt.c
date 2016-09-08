/*
 * itmt.c: Functions and data structures for enabling
 *	   scheduler to favor scheduling on cores that
 *	   can be boosted to a higher frequency using
 *	   Intel Turbo Boost Max Technology 3.0
 *
 * (C) Copyright 2016 Intel Corporation
 * Author: Tim Chen <tim.c.chen@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/cpuset.h>
#include <asm/mutex.h>
#include <linux/sched.h>
#include <linux/sysctl.h>
#include <linux/nodemask.h>

DEFINE_PER_CPU_READ_MOSTLY(int, sched_core_priority);
static DEFINE_MUTEX(itmt_update_mutex);

static unsigned int zero;
static unsigned int one = 1;

/*
 * Boolean to control whether we want to move processes to cpu capable
 * of higher turbo frequency for cpus supporting Intel Turbo Boost Max
 * Technology 3.0.
 *
 * It can be set via /proc/sys/kernel/sched_itmt_enabled
 */
unsigned int __read_mostly sysctl_sched_itmt_enabled;

/*
 * The pstate_driver calls set_sched_itmt to indicate if the system
 * is ITMT capable.
 */
static bool __read_mostly sched_itmt_capable;

int arch_asym_cpu_priority(int cpu)
{
	return per_cpu(sched_core_priority, cpu);
}

/* Called with itmt_update_mutex lock held */
static void enable_sched_itmt(bool enable_itmt)
{
	sysctl_sched_itmt_enabled = enable_itmt;
	x86_topology_update = true;
	rebuild_sched_domains();
}

static int sched_itmt_update_handler(struct ctl_table *table, int write,
			      void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;

	mutex_lock(&itmt_update_mutex);

	ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);

	if (ret || !write) {
		mutex_unlock(&itmt_update_mutex);
		return ret;
	}

	enable_sched_itmt(sysctl_sched_itmt_enabled);

	mutex_unlock(&itmt_update_mutex);

	return ret;
}

static struct ctl_table itmt_kern_table[] = {
	{
		.procname	= "sched_itmt_enabled",
		.data		= &sysctl_sched_itmt_enabled,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= sched_itmt_update_handler,
		.extra1		= &zero,
		.extra2		= &one,
	},
	{}
};

static struct ctl_table itmt_root_table[] = {
	{
		.procname	= "kernel",
		.mode		= 0555,
		.child		= itmt_kern_table,
	},
	{}
};

static struct ctl_table_header *itmt_sysctl_header;

/*
 * The boot code will find out the max boost frequency
 * and call this function to set a priority proportional
 * to the max boost frequency. CPU with higher boost
 * frequency will receive higher priority.
 */
void sched_set_itmt_core_prio(int prio, int core_cpu)
{
	int cpu, i = 1;

	for_each_cpu(cpu, topology_sibling_cpumask(core_cpu)) {
		int smt_prio;

		/*
		 * Discount the priority of sibling so that we don't
		 * pack all loads to the same core before using other cores.
		 */
		smt_prio = prio * smp_num_siblings / i;
		i++;
		per_cpu(sched_core_priority, cpu) = smt_prio;
	}
}

/*
 * During boot up, boot code will detect if the system
 * is ITMT capable and call set_sched_itmt.
 *
 * This should be called after sched_set_itmt_core_prio
 * has been called to set the cpus' priorities.
 *
 * This function should be called without cpu hot plug lock
 * as we need to acquire the lock to rebuild sched domains
 * later.
 */
void set_sched_itmt(bool itmt_capable)
{
	mutex_lock(&itmt_update_mutex);

	if (itmt_capable != sched_itmt_capable) {

		if (itmt_capable) {
			itmt_sysctl_header =
				register_sysctl_table(itmt_root_table);
			/*
			 * ITMT capability automatically enables ITMT
			 * scheduling for client systems (single node).
			 */
			if (topology_num_packages() == 1)
				sysctl_sched_itmt_enabled = 1;
		} else {
			if (itmt_sysctl_header)
				unregister_sysctl_table(itmt_sysctl_header);
			sysctl_sched_itmt_enabled = 0;
		}

		sched_itmt_capable = itmt_capable;
		x86_topology_update = true;
		rebuild_sched_domains();
	}

	mutex_unlock(&itmt_update_mutex);
}
