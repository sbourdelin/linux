/*
 * Memory delay metric
 *
 * Copyright (c) 2017 Facebook, Johannes Weiner
 *
 * This code quantifies and reports to userspace the wall-time impact
 * of memory pressure on the system and memory-controlled cgroups.
 */

#include <linux/memdelay.h>
#include <linux/cgroup.h>
#include <linux/sched.h>

#include "sched.h"

/**
 * memdelay_enter - mark the beginning of a memory delay section
 * @flags: flags to handle nested memdelay sections
 *
 * Marks the calling task as being delayed due to a lack of memory,
 * such as waiting for a workingset refault or performing reclaim.
 */
void memdelay_enter(unsigned long *flags)
{
	*flags = current->flags & PF_MEMDELAY;
	if (*flags)
		return;
	/*
	 * PF_MEMDELAY & accounting needs to be atomic wrt changes to
	 * the task's scheduling state (hence IRQ disabling) and its
	 * domain association (hence lock_task_cgroup). Otherwise we
	 * could race with CPU or cgroup migration and misaccount.
	 */
	WARN_ON_ONCE(irqs_disabled());
	local_irq_disable();
	lock_task_cgroup(current);

	current->flags |= PF_MEMDELAY;
	memdelay_task_change(current, MTS_WORKING, MTS_DELAYED_ACTIVE);

	unlock_task_cgroup(current);
	local_irq_enable();
}

/**
 * memdelay_leave - mark the end of a memory delay section
 * @flags: flags to handle nested memdelay sections
 *
 * Marks the calling task as no longer delayed due to memory.
 */
void memdelay_leave(unsigned long *flags)
{
	if (*flags)
		return;
	/*
	 * PF_MEMDELAY & accounting needs to be atomic wrt changes to
	 * the task's scheduling state (hence IRQ disabling) and its
	 * domain association (hence lock_task_cgroup). Otherwise we
	 * could race with CPU or cgroup migration and misaccount.
	 */
	WARN_ON_ONCE(irqs_disabled());
	local_irq_disable();
	lock_task_cgroup(current);

	current->flags &= ~PF_MEMDELAY;
	memdelay_task_change(current, MTS_DELAYED_ACTIVE, MTS_WORKING);

	unlock_task_cgroup(current);
	local_irq_enable();
}

#ifdef CONFIG_CGROUPS
/**
 * cgroup_move_task - move task to a different cgroup
 * @task: the task
 * @to: the target css_set
 *
 * Move task to a new cgroup and safely migrate its associated
 * delayed/working state between the different domains.
 *
 * This function acquires the task's rq lock and lock_task_cgroup() to
 * lock out concurrent changes to the task's scheduling state and - in
 * case the task is running - concurrent changes to its delay state.
 */
void cgroup_move_task(struct task_struct *task, struct css_set *to)
{
	struct rq_flags rf;
	struct rq *rq;
	int state;

	lock_task_cgroup(task);
	rq = task_rq_lock(task, &rf);

	if (task->flags & PF_MEMDELAY)
		state = MTS_DELAYED + task_current(rq, task);
	else if (task_on_rq_queued(task) || task->in_iowait)
		state = MTS_WORKING;
	else
		state = MTS_NONE;

	/*
	 * Lame to do this here, but the scheduler cannot be locked
	 * from the outside, so we move cgroups from inside sched/.
	 */
	memdelay_task_change(task, state, MTS_NONE);
	rcu_assign_pointer(task->cgroups, to);
	memdelay_task_change(task, MTS_NONE, state);

	task_rq_unlock(rq, task, &rf);
	unlock_task_cgroup(task);
}
#endif /* CONFIG_CGROUPS */
