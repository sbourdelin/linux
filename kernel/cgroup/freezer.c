//SPDX-License-Identifier: GPL-2.0
#include <linux/cgroup.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>

#include "cgroup-internal.h"

/*
 * Propagate the cgroup frozen state upwards by the cgroup tree.
 */
static void cgroup_propagate_frozen(struct cgroup *cgrp, bool frozen)
{
	int desc = 1;

	/*
	 * If the new state is frozen, some freezing ancestor cgroups may change
	 * their state too, depending on if all their descendants are frozen.
	 *
	 * Otherwise, all ancestor cgroups are forced into the non-frozen state.
	 */
	while ((cgrp = cgroup_parent(cgrp))) {
		if (frozen) {
			cgrp->freezer.nr_frozen_descendants += desc;
			if (!test_bit(CGRP_FROZEN, &cgrp->flags) &&
			    test_bit(CGRP_FREEZE, &cgrp->flags) &&
			    cgrp->freezer.nr_frozen_descendants ==
			    cgrp->nr_descendants) {
				set_bit(CGRP_FROZEN, &cgrp->flags);
				cgroup_file_notify(&cgrp->events_file);
				desc++;
			}
		} else {
			cgrp->freezer.nr_frozen_descendants -= desc;
			if (test_bit(CGRP_FROZEN, &cgrp->flags)) {
				clear_bit(CGRP_FROZEN, &cgrp->flags);
				cgroup_file_notify(&cgrp->events_file);
				desc++;
			}
		}
	}
}

/*
 * Revisit the cgroup frozen state.
 * Checks if the cgroup is really frozen and performs
 * all state transitions.
 */
static void cgroup_update_frozen(struct cgroup *cgrp)
{
	bool frozen;

	lockdep_assert_held(&css_set_lock);

	/*
	 * If the cgroup has to be frozen (CGRP_FREEZE bit set),
	 * and all tasks are frozen or stopped, let's consider
	 * the cgroup frozen. Otherwise it's not frozen.
	 */
	frozen = test_bit(CGRP_FREEZE, &cgrp->flags) &&
		cgrp->freezer.nr_frozen_tasks +
		cgrp->freezer.nr_stopped_tasks ==
		cgrp->freezer.nr_tasks_to_freeze;

	if (frozen) {
		/* Already there? */
		if (test_bit(CGRP_FROZEN, &cgrp->flags))
			return;

		set_bit(CGRP_FROZEN, &cgrp->flags);
	} else {
		/* Already there? */
		if (!test_bit(CGRP_FROZEN, &cgrp->flags))
			return;

		clear_bit(CGRP_FROZEN, &cgrp->flags);
	}
	cgroup_file_notify(&cgrp->events_file);

	/* Update the state of ancestor cgroups. */
	cgroup_propagate_frozen(cgrp, frozen);
}

/*
 * Increment cgroup's nr_tasks_to_freeze, nr_frozen_tasks
 * and nr_stopped_tasks counters. Revisit cgroup state if necessary.
 */
static void cgroup_inc_frozen_cnt(struct cgroup *cgrp, bool to_freeze,
				  bool frozen, bool stopped)
{
	if (!to_freeze && !frozen && !stopped)
		return;
	if (to_freeze)
		cgrp->freezer.nr_tasks_to_freeze++;
	if (frozen) {
		cgrp->freezer.nr_frozen_tasks++;
		WARN_ON_ONCE(cgrp->freezer.nr_frozen_tasks >
			     cgrp->freezer.nr_tasks_to_freeze);
	}
	if (stopped)
		cgrp->freezer.nr_stopped_tasks++;

	cgroup_update_frozen(cgrp);
}

/*
 * Decrement cgroup's nr_tasks_to_freeze, nr_frozen_tasks
 * and nr_stopped_tasks counters. Revisit cgroup state if necessary.
 */
static void cgroup_dec_frozen_cnt(struct cgroup *cgrp, bool to_freeze,
				  bool frozen, bool stopped)
{
	if (!to_freeze && !frozen && !stopped)
		return;
	if (frozen) {
		cgrp->freezer.nr_frozen_tasks--;
		WARN_ON_ONCE(cgrp->freezer.nr_frozen_tasks < 0);
	}
	if (to_freeze) {
		cgrp->freezer.nr_tasks_to_freeze--;
		WARN_ON_ONCE(cgrp->freezer.nr_tasks_to_freeze < 0);
		WARN_ON_ONCE(cgrp->freezer.nr_frozen_tasks >
			     cgrp->freezer.nr_tasks_to_freeze);
	}
	if (stopped)
		cgrp->freezer.nr_stopped_tasks--;

	cgroup_update_frozen(cgrp);
}

void cgroup_dec_tasks_to_freeze(struct cgroup *cgrp)
{
	cgroup_dec_frozen_cnt(cgrp, true, false, false);
}

/*
 * Entry path into the frozen state.
 * If the task was not frozen before, counters are updated and the cgroup state
 * is revisited, if necessary.
 */
void cgroup_enter_frozen(void)
{
	spin_lock_irq(&css_set_lock);
	current->frozen = true;
	cgroup_inc_frozen_cnt(task_dfl_cgroup(current), false, true, false);
	spin_unlock_irq(&css_set_lock);
}

/*
 * Exit path from the frozen state.
 * Counters are updated and the cgroup state is revisited, if necessary.
 */
void cgroup_leave_frozen(void)
{
	spin_lock_irq(&css_set_lock);
	cgroup_dec_frozen_cnt(task_dfl_cgroup(current), false, true, false);
	current->frozen = false;
	spin_unlock_irq(&css_set_lock);
}

void cgroup_enter_stopped(void)
{
	spin_lock_irq(&css_set_lock);
	current->stopped = true;
	cgroup_inc_frozen_cnt(task_dfl_cgroup(current), false, false, true);
	spin_unlock_irq(&css_set_lock);
}

void cgroup_leave_stopped(void)
{
	spin_lock_irq(&css_set_lock);
	cgroup_dec_frozen_cnt(task_dfl_cgroup(current), false, false, true);
	current->stopped = false;
	spin_unlock_irq(&css_set_lock);
}

/*
 * Freeze or unfreeze the task by setting or clearing the JOBCTL_TRAP_FREEZE
 * jobctl bit.
 */
static void cgroup_freeze_task(struct task_struct *task, bool freeze)
{
	unsigned long flags;

	/* If the task is about to die, don't bother with freezing it. */
	if (!lock_task_sighand(task, &flags))
		return;

	if (freeze) {
		task->jobctl |= JOBCTL_TRAP_FREEZE;
		signal_wake_up(task, false);
	} else {
		task->jobctl &= ~JOBCTL_TRAP_FREEZE;
		wake_up_process(task);
	}

	unlock_task_sighand(task, &flags);
}

/*
 * Freeze or unfreeze all tasks in the given cgroup.
 */
static void cgroup_do_freeze(struct cgroup *cgrp, bool freeze)
{
	struct css_task_iter it;
	struct task_struct *task;

	lockdep_assert_held(&cgroup_mutex);

	spin_lock_irq(&css_set_lock);
	if (freeze) {
		cgrp->freezer.nr_tasks_to_freeze = __cgroup_task_count(cgrp);
		set_bit(CGRP_FREEZE, &cgrp->flags);
	} else {
		clear_bit(CGRP_FREEZE, &cgrp->flags);
	}
	spin_unlock_irq(&css_set_lock);

	css_task_iter_start(&cgrp->self, 0, &it);
	while ((task = css_task_iter_next(&it))) {
		/*
		 * Ignore kernel threads here. Freezing cgroups containing
		 * kthreads isn't supported.
		 */
		if (task->flags & PF_KTHREAD)
			continue;
		cgroup_freeze_task(task, freeze);
	}
	css_task_iter_end(&it);

	/*
	 * Cgroup state should be revisited here to cover empty leaf cgroups
	 * and cgroups which descendants are already in the desired state.
	 */
	spin_lock_irq(&css_set_lock);
	if (cgrp->nr_descendants == cgrp->freezer.nr_frozen_descendants)
		cgroup_update_frozen(cgrp);
	spin_unlock_irq(&css_set_lock);
}

void cgroup_freezer_migrate_task(struct task_struct *task,
				 struct cgroup *src, struct cgroup *dst)
{
	lockdep_assert_held(&css_set_lock);

	/*
	 * Kernel threads are not supposed to be frozen at all.
	 */
	if (task->flags & PF_KTHREAD)
		return;

	/*
	 * Adjust counters of freezing and frozen tasks.
	 * Note, that if the task is frozen, but the destination cgroup is not
	 * frozen, we bump both counters to keep them balanced.
	 */
	cgroup_dec_frozen_cnt(src, test_bit(CGRP_FREEZE, &src->flags),
			      task->frozen, task->stopped);
	cgroup_inc_frozen_cnt(dst, test_bit(CGRP_FREEZE, &dst->flags) ||
			      task->frozen, task->frozen, task->stopped);

	/*
	 * If the task isn't in the desired state, force it to the state.
	 */
	if (task->frozen != test_bit(CGRP_FREEZE, &dst->flags))
		cgroup_freeze_task(task, test_bit(CGRP_FREEZE, &dst->flags));
}

void cgroup_freeze(struct cgroup *cgrp, bool freeze)
{
	struct cgroup_subsys_state *css;
	struct cgroup *dsct;
	bool applied = false;

	lockdep_assert_held(&cgroup_mutex);

	/*
	 * Nothing changed? Just exit.
	 */
	if (cgrp->freezer.freeze == freeze)
		return;

	cgrp->freezer.freeze = freeze;

	/*
	 * Propagate changes downwards the cgroup tree.
	 */
	css_for_each_descendant_pre(css, &cgrp->self) {
		dsct = css->cgroup;

		if (cgroup_is_dead(dsct))
			continue;

		if (freeze) {
			dsct->freezer.e_freeze++;
			/*
			 * Already frozen because of ancestor's settings?
			 */
			if (dsct->freezer.e_freeze > 1)
				continue;
		} else {
			dsct->freezer.e_freeze--;
			/*
			 * Still frozen because of ancestor's settings?
			 */
			if (dsct->freezer.e_freeze > 0)
				continue;

			WARN_ON_ONCE(dsct->freezer.e_freeze < 0);
		}

		/*
		 * Do change actual state: freeze or unfreeze.
		 */
		cgroup_do_freeze(dsct, freeze);
		applied = true;
	}

	/*
	 * Even if the actual state hasn't changed, let's notify a user.
	 * The state can be enforced by an ancestor cgroup: the cgroup
	 * can already be in the desired state or it can be locked in the
	 * opposite state, so that the transition will never happen.
	 * In both cases it's better to notify a user, that there is
	 * nothing to wait.
	 */
	if (!applied)
		cgroup_file_notify(&cgrp->events_file);
}
