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
 *
 *
 * Window Assisted Load Tracking (WALT) implementation credits:
 * Srivatsa Vaddagiri, Steve Muckle, Syed Rameez Mustafa, Joonwoo Park,
 * Pavan Kumar Kondeti, Olav Haugan
 *
 * 2016-03-06: Integration with EAS/refactoring by Vikram Mulukutla
 *             and Todd Kjos
 * 2016-08-31: Integration with mainline by Srivatsa Vaddagiri
 *             and Vikram Mulukutla
 */

#include <linux/syscore_ops.h>
#include <linux/cpufreq.h>
#include "sched.h"
#include "walt.h"


char *task_event_names[] = {"PUT_PREV_TASK", "PICK_NEXT_TASK",
				"TASK_WAKE", "TASK_MIGRATE", "TASK_UPDATE",
				"IRQ_UPDATE"};

__read_mostly unsigned int sysctl_sched_use_walt_metrics = 1;

static __read_mostly unsigned int walt_freq_account_wait_time;
static __read_mostly unsigned int walt_io_is_busy;

/* 1 -> use PELT based load stats, 0 -> use window-based load stats */
static unsigned int __read_mostly walt_disabled;

/* Window size (in ns) */
__read_mostly unsigned int walt_ravg_window = 20000000;

/* Min window size (in ns) = 5ms */
#define MIN_SCHED_RAVG_WINDOW 5000000

/* Max window size (in ns) = 1s */
#define MAX_SCHED_RAVG_WINDOW 1000000000

static unsigned int sync_cpu;
static ktime_t ktime_last;
static bool walt_ktime_suspended;

u64 walt_ktime_clock(void)
{
	if (unlikely(walt_ktime_suspended))
		return ktime_to_ns(ktime_last);
	return ktime_get_ns();
}

static void walt_resume(void)
{
	walt_ktime_suspended = false;
}

static int walt_suspend(void)
{
	ktime_last = ktime_get();
	walt_ktime_suspended = true;
	return 0;
}

static struct syscore_ops walt_syscore_ops = {
	.resume	= walt_resume,
	.suspend = walt_suspend
};

static int __init walt_init_ops(void)
{
	register_syscore_ops(&walt_syscore_ops);
	return 0;
}
late_initcall(walt_init_ops);

static int __init set_walt_ravg_window(char *str)
{
	get_option(&str, &walt_ravg_window);

	walt_disabled = (walt_ravg_window < MIN_SCHED_RAVG_WINDOW ||
				walt_ravg_window > MAX_SCHED_RAVG_WINDOW);
	return 0;
}

early_param("walt_ravg_window", set_walt_ravg_window);

static void
update_window_start(struct rq *rq, u64 wallclock)
{
	s64 delta;
	int nr_windows;
	u64 prev_sum = 0;

	delta = wallclock - rq->window_start;
	BUG_ON(delta < 0);
	if (delta < walt_ravg_window)
		return;

	nr_windows = div64_u64(delta, walt_ravg_window);
	if (nr_windows == 1)
		prev_sum = rq->curr_runnable_sum;

	rq->prev_runnable_sum = prev_sum;
	rq->curr_runnable_sum = 0;

	rq->window_start += (u64)nr_windows * (u64)walt_ravg_window;
}

static u64 scale_exec_time(u64 delta, struct rq *rq)
{
	unsigned long scale_freq = arch_scale_freq_capacity(NULL, cpu_of(rq));
	unsigned long scale_cpu = arch_scale_cpu_capacity(NULL, cpu_of(rq));
	u64 scaled_delta = cap_scale(delta, scale_freq);

	return cap_scale(scaled_delta, scale_cpu);
}

static int cpu_is_waiting_on_io(struct rq *rq)
{
	if (!walt_io_is_busy)
		return 0;

	return atomic_read(&rq->nr_iowait);
}

static int account_cpu_busy_time(struct rq *rq, struct task_struct *p,
				     u64 irqtime, int event)
{
	if (is_idle_task(p)) {
		/* TASK_WAKE && TASK_MIGRATE is not possible on idle task! */
		if (event == PICK_NEXT_TASK)
			return 0;

		/* PUT_PREV_TASK, TASK_UPDATE && IRQ_UPDATE are left */
		return irqtime || cpu_is_waiting_on_io(rq);
	}

	if (event == TASK_WAKE)
		return 0;

	if (event == PUT_PREV_TASK || event == IRQ_UPDATE ||
					 event == TASK_UPDATE)
		return 1;

	/* Only TASK_MIGRATE && PICK_NEXT_TASK left */
	return walt_freq_account_wait_time;
}

/*
 * Account cpu activity in its busy time counters (rq->curr/prev_runnable_sum)
 */
static void update_cpu_busy_time(struct task_struct *p, struct rq *rq,
	     int event, u64 wallclock, u64 irqtime)
{
	int new_window, nr_full_windows = 0;
	u64 mark_start = p->ravg.mark_start;
	u64 window_start = rq->window_start;
	u32 window_size = walt_ravg_window;
	u64 delta;

	new_window = mark_start < window_start;
	if (new_window)
		nr_full_windows = div64_u64((window_start - mark_start),
								window_size);

	/* Handle window rollover */
	if (new_window) {
		if (!is_idle_task(p)) {
			u32 curr_window = 0;

			if (!nr_full_windows)
				curr_window = p->ravg.curr_window;

			p->ravg.prev_window = curr_window;
			p->ravg.curr_window = 0;
		}
	}

	if (!account_cpu_busy_time(rq, p, irqtime, event))
		return;

	if (!new_window) {
		if (!irqtime || !is_idle_task(p) || cpu_is_waiting_on_io(rq))
			delta = wallclock - mark_start;
		else
			delta = irqtime;
		delta = scale_exec_time(delta, rq);
		rq->curr_runnable_sum += delta;
		if (!is_idle_task(p))
			p->ravg.curr_window += delta;

		return;
	}

	if (!irqtime || !is_idle_task(p) || cpu_is_waiting_on_io(rq)) {
		if (!nr_full_windows) {
			/*
			 * A full window hasn't elapsed, account partial
			 * contribution to previous completed window.
			 */
			delta = scale_exec_time(window_start - mark_start, rq);
			p->ravg.prev_window += delta;
		} else {
			/*
			 * Since at least one full window has elapsed,
			 * the contribution to the previous window is the
			 * full window (window_size).
			 */
			delta = scale_exec_time(window_size, rq);
			p->ravg.prev_window = delta;
		}
		rq->prev_runnable_sum += delta;

		/* Account piece of busy time in the current window. */
		delta = scale_exec_time(wallclock - window_start, rq);
		rq->curr_runnable_sum += delta;
		p->ravg.curr_window = delta;

		return;
	}

	if (irqtime) {
		/* IRQ busy time start = wallclock - irqtime */
		mark_start = wallclock - irqtime;

		if (mark_start > window_start) {
			rq->curr_runnable_sum += scale_exec_time(irqtime, rq);
			return;
		}

		/*
		 * IRQ busy time spanned multiple windows. Process the
		 * busy time preceding the current window first
		 */
		delta = window_start - mark_start;
		if (delta > window_size)
			delta = window_size;
		delta = scale_exec_time(delta, rq);
		rq->prev_runnable_sum += delta;

		/* Process the remaining IRQ busy time in the current window. */
		delta = wallclock - window_start;
		rq->curr_runnable_sum += scale_exec_time(delta, rq);

		return;
	}

	BUG();
}

/* Reflect task activity on its demand and cpu's busy time statistics */
void walt_update_task_ravg(struct task_struct *p, struct rq *rq,
	     enum task_event event, u64 wallclock, u64 irqtime)
{
	if (walt_disabled || !rq->window_start)
		return;

	lockdep_assert_held(&rq->lock);

	update_window_start(rq, wallclock);

	if (!p->ravg.mark_start)
		goto done;

	update_cpu_busy_time(p, rq, event, wallclock, irqtime);

done:
	p->ravg.mark_start = wallclock;
}

void walt_mark_task_starting(struct task_struct *p)
{
	u64 wallclock;
	struct rq *rq = task_rq(p);

	if (!rq->window_start)
		return;

	wallclock = walt_ktime_clock();
	p->ravg.mark_start = wallclock;
}

void walt_set_window_start(struct rq *rq)
{
	int cpu = cpu_of(rq);
	struct rq *sync_rq = cpu_rq(sync_cpu);
	unsigned long flags;

	if (rq->window_start || walt_ktime_clock() < walt_ravg_window)
		return;

	if (cpu == sync_cpu) {
		raw_spin_lock_irqsave(&rq->lock, flags);
		rq->window_start = walt_ktime_clock();
		rq->curr_runnable_sum = rq->prev_runnable_sum = 0;
		raw_spin_unlock_irqrestore(&rq->lock, flags);
	} else {
		local_irq_save(flags);
		double_rq_lock(rq, sync_rq);
		rq->window_start = cpu_rq(sync_cpu)->window_start;
		rq->curr_runnable_sum = rq->prev_runnable_sum = 0;
		double_rq_unlock(rq, sync_rq);
		local_irq_restore(flags);
	}
}

void walt_migrate_sync_cpu(int cpu)
{
	if (cpu == sync_cpu)
		sync_cpu = smp_processor_id();
}

void walt_finish_migrate(struct task_struct *p, struct rq *dest_rq, bool locked)
{
	u64 wallclock;
	unsigned long flags;

	if (!p->on_rq && p->state != TASK_WAKING)
		return;

	if (locked == false)
		raw_spin_lock_irqsave(&dest_rq->lock, flags);

	lockdep_assert_held(&dest_rq->lock);

	wallclock = walt_ktime_clock();

	/* Update counters on destination CPU */
	walt_update_task_ravg(dest_rq->curr, dest_rq,
				TASK_UPDATE, wallclock, 0);

	/* We may be in a new window. Update task counters */
	walt_update_task_ravg(p, dest_rq, TASK_MIGRATE, wallclock, 0);

	if (p->ravg.curr_window) {
		if (!dest_rq->window_start) {
			p->ravg.curr_window = 0;
			p->ravg.mark_start = 0;
		}
		dest_rq->curr_runnable_sum += p->ravg.curr_window;
	}

	if (p->ravg.prev_window) {
		if (!dest_rq->window_start)
			p->ravg.prev_window = 0;
		dest_rq->prev_runnable_sum += p->ravg.prev_window;
	}

	if (locked == false)
		raw_spin_unlock_irqrestore(&dest_rq->lock, flags);
}

void walt_prepare_migrate(struct task_struct *p, struct rq *src_rq, bool locked)
{
	u64 wallclock;
	unsigned long flags;

	if (!p->on_rq && p->state != TASK_WAKING)
		return;

	if (locked == false)
		raw_spin_lock_irqsave(&src_rq->lock, flags);

	lockdep_assert_held(&src_rq->lock);

	/* Note that same wallclock reference is used for all 3 events below */
	wallclock = walt_ktime_clock();

	/* Update counters on source CPU */
	walt_update_task_ravg(task_rq(p)->curr, task_rq(p),
			TASK_UPDATE, wallclock, 0);

	/* Update task's counters */
	walt_update_task_ravg(p, task_rq(p), TASK_MIGRATE, wallclock, 0);

	/* Fixup busy time */
	if (p->ravg.curr_window)
		src_rq->curr_runnable_sum -= p->ravg.curr_window;

	if (p->ravg.prev_window)
		src_rq->prev_runnable_sum -= p->ravg.prev_window;

	if ((s64)src_rq->prev_runnable_sum < 0) {
		src_rq->prev_runnable_sum = 0;
		WARN_ON(1);
	}
	if ((s64)src_rq->curr_runnable_sum < 0) {
		src_rq->curr_runnable_sum = 0;
		WARN_ON(1);
	}

	if (locked == false)
		raw_spin_unlock_irqrestore(&src_rq->lock, flags);
}

void walt_account_irqtime(int cpu, struct task_struct *curr,
				u64 delta, u64 wallclock)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long flags;

	raw_spin_lock_irqsave(&rq->lock, flags);

	/*
	 * cputime (wallclock) uses sched_clock so use the same here for
	 * consistency.
	 */
	delta += sched_clock_cpu(cpu) - wallclock;

	walt_update_task_ravg(curr, rq, IRQ_UPDATE, walt_ktime_clock(), delta);

	raw_spin_unlock_irqrestore(&rq->lock, flags);
}

void walt_init_new_task_load(struct task_struct *p)
{
	memset(&p->ravg, 0, sizeof(struct ravg));
}
