/*
 * Copyright (c) 2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __WALT_H
#define __WALT_H

#ifdef CONFIG_SCHED_WALT

void walt_update_task_ravg(struct task_struct *p, struct rq *rq,
			   enum task_event event, u64 wallclock, u64 irqtime);
void walt_prepare_migrate(struct task_struct *p, struct rq *rq, bool locked);
void walt_finish_migrate(struct task_struct *p, struct rq *rq, bool locked);
void walt_init_new_task_load(struct task_struct *p);
void walt_mark_task_starting(struct task_struct *p);
void walt_set_window_start(struct rq *rq);
void walt_migrate_sync_cpu(int cpu);
u64 walt_ktime_clock(void);
void walt_account_irqtime(int cpu, struct task_struct *curr, u64 delta,
			  u64 wallclock);

extern unsigned int sysctl_sched_use_walt_metrics;
extern unsigned int walt_ravg_window;

/* Fold into cpu_util */
static inline unsigned long cpu_walt_util(struct rq *rq)
{
	if (!sysctl_sched_use_walt_metrics)
		return rq->cfs.avg.util_avg;

	return (rq->prev_runnable_sum * rq->cpu_capacity_orig) /
					walt_ravg_window;
}

#else /* CONFIG_SCHED_WALT */

static inline void walt_update_task_ravg(struct task_struct *p, struct rq *rq,
		int event, u64 wallclock, u64 irqtime) { }
static inline void walt_prepare_migrate(struct task_struct *p, struct rq *rq,
					bool locked) { }
static inline void walt_finish_migrate(struct task_struct *p, struct rq *rq,
				       bool locked) { }
static inline void walt_init_new_task_load(struct task_struct *p) { }
static inline void walt_mark_task_starting(struct task_struct *p) { }
static inline void walt_set_window_start(struct rq *rq) { }
static inline void walt_migrate_sync_cpu(int cpu) { }
static inline u64 walt_ktime_clock(void) { return 0; }
static inline void walt_account_irqtime(int cpu, struct task_struct *curr,
				u64 delta, u64 wallclock) { }

static inline unsigned long cpu_walt_util(struct rq *rq)
{
	return rq->cfs.avg.util_avg;
}

#endif /* CONFIG_SCHED_WALT */

extern unsigned int walt_ravg_window;

#endif
