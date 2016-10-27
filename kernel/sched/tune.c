/*
 * Scheduler Tunability (SchedTune) Extensions for CFS
 *
 * Copyright (C) 2016 ARM Ltd, Patrick Bellasi <patrick.bellasi@arm.com>
 */

#include "sched.h"

unsigned int sysctl_sched_cfs_boost __read_mostly;

int
sysctl_sched_cfs_boost_handler(struct ctl_table *table, int write,
			       void __user *buffer, size_t *lenp,
			       loff_t *ppos)
{
	int ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);

	if (ret || !write)
		return ret;

	return 0;
}

