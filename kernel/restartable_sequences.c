/*
 * Restartable Sequences are a lightweight interface that allows user-level
 * code to be executed atomically relative to scheduler preemption.  Typically
 * used for implementing per-cpu operations.
 *
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
 * Copyright (C) 2015, Google, Inc.,
 * Paul Turner <pjt@google.com> and Andrew Hunter <ahh@google.com>
 *
 */

#ifdef CONFIG_RESTARTABLE_SEQUENCES

#include <linux/uaccess.h>
#include <linux/preempt.h>
#include <linux/slab.h>
#include <linux/syscalls.h>

static void rseq_sched_in_nop(struct preempt_notifier *pn, int cpu) {}
static void rseq_sched_out_nop(struct preempt_notifier *pn,
			       struct task_struct *next) {}

static __read_mostly struct preempt_ops rseq_preempt_ops = {
	.sched_in = rseq_sched_in_nop,
	.sched_out = rseq_sched_out_nop,
};

unsigned long rseq_lookup(struct task_struct *p, unsigned long ip)
{
	struct task_struct *leader = p->group_leader;
	struct restartable_sequence_state *rseq_state = &leader->rseq_state;
	struct restartable_sequence_section *item;

	struct rb_node *node = rseq_state->sections.rb_node;

	while (node) {
		item = container_of(
			node, struct restartable_sequence_section, node);
		if (ip < (unsigned long)item->crit_start)
			node = node->rb_left;
		else if (ip >= (unsigned long)item->crit_end)
			node = node->rb_right;
		else
			return (unsigned long)item->crit_restart;
	}

	return 0;
}

int rseq_register_cpu_pointer(struct task_struct *p, int __user *cpu_pointer)
{
	struct restartable_sequence_state *rseq_state =
		&p->rseq_state;
	int registered = 0, rc = 0;

	if (cpu_pointer == rseq_state->cpu_pointer)
		return 0;

	if (cpu_pointer && !access_ok(VERIFY_WRITE, cpu_pointer, sizeof(int)))
		return -EINVAL;

	rcu_read_lock();
	/* Group leader always holds critical section definition. */
	if (cpu_pointer && !current->group_leader->rseq_state.cpu_pointer &&
		current->group_leader != p) {
		rc = -EINVAL;
		goto out_unlock;
	}
	smp_rmb();  /* Pairs with setting group_leaders cpu_pointer */

	if (rseq_state->cpu_pointer)
		registered = 1;
	rseq_state->cpu_pointer = cpu_pointer;

	if (cpu_pointer && !registered) {
		preempt_notifier_inc();

		preempt_notifier_init(&rseq_state->notifier,
				      &rseq_preempt_ops);
		preempt_notifier_register(&rseq_state->notifier);
	} else if (!cpu_pointer && registered) {
		preempt_notifier_unregister(&rseq_state->notifier);

		preempt_notifier_dec();
	}

	/* Will update *cpu_pointer on return. */
	if (cpu_pointer)
		set_thread_flag(TIF_NOTIFY_RESUME);

out_unlock:
	rcu_read_unlock();

	return 0;
}

void rseq_clear_state_exec(struct task_struct *task)
{
	struct restartable_sequence_section *section;
	struct rb_node *node;

	/* Ensure notifier is disabled. */
	rseq_register_cpu_pointer(task, NULL);

	/* Free and reinit */
	while ((node = rb_first(&task->rseq_state.sections))) {
		section = rb_entry(node,
				struct restartable_sequence_section, node);
		rb_erase(&section->node, &task->rseq_state.sections);
		kfree(section);
	}

	memset(&task->rseq_state, 0, sizeof(task->rseq_state));
	task->rseq_state.sections = RB_ROOT;
}

static DEFINE_MUTEX(rseq_state_mutex);

int rseq_register_critical_current(__user void *start, __user void *end,
				__user void *restart)
{
	struct restartable_sequence_state *rseq_state;
	struct restartable_sequence_section *section;
	struct rb_node **new, *parent = NULL;
	int rc = 0;

	rcu_read_lock();
	/* The critical section is shared by all threads in a process. */
	rseq_state = &current->group_leader->rseq_state;

	/* Verify section */
	if (start >= end) {
		rc = -EINVAL;
		goto out_rcu;
	}

	if (!access_ok(VERIFY_READ, start, end - start) ||
		!access_ok(VERIFY_READ, restart, 1)) {
		rc = -EINVAL;
		goto out_rcu;
	}

	if (rseq_state->cpu_pointer) {
		rc = -EBUSY;
		goto out_rcu;
	}

	new = &(rseq_state->sections.rb_node);

	section = kmalloc(
		sizeof(struct restartable_sequence_section), GFP_KERNEL);
	if (!section) {
		rc = -ENOMEM;
		goto out_rcu;
	}
	section->crit_end = end;
	section->crit_start = start;
	section->crit_restart = restart;

	mutex_lock(&rseq_state_mutex);

	while (*new) {
		struct restartable_sequence_section *this = container_of(
			*new, struct restartable_sequence_section, node);

		parent = *new;
		if (section->crit_end <= this->crit_start)
			new = &((*new)->rb_left);
		else if (section->crit_start >= this->crit_end)
			new = &((*new)->rb_right);
		else {
			/* Prevent overlapping regions */
			kfree(section);
			rc = -EBUSY;
			goto out_lock;
		}
	}

	rb_link_node(&section->node, parent, new);
	rb_insert_color(&section->node, &rseq_state->sections);

out_lock:
	mutex_unlock(&rseq_state_mutex);
out_rcu:

	smp_wmb();  /* synchronize visibility of new section */

	rcu_read_unlock();
	return rc;
}

#define SYS_RSEQ_SET_CRITICAL		0
#define SYS_RSEQ_SET_CPU_POINTER	1

/*
 * RSEQ syscall interface.
 *
 * Usage:
 *   SYS_RSEQ_SET_CRITICAL, flags, crit_start, crit_end, crit_restart)
 *    A thread with user rip in (crit_start, crit_end] that has called
 *    RSEQ_SET_CPU_POINTER will have its execution resumed at crit_restart
 *    when interrupted by preemption or signal.
 *
 *   SYS_RSEQ_SET_CPU_POINTER, flags, cpu_pointer_address
 *    Configures a (typically per-thread) value, containing the cpu which that
 *    thread is currently executing on.
 *    REQUIRES: SYS_RSEQ_SET_CRITICAL must have previously been called.
 *
 *  flags is currently unused.
 */
SYSCALL_DEFINE5(restartable_sequences,
		int, op, int, flags, long, val1, long, val2, long, val3)
{
	int rc = -EINVAL;

	if (op == SYS_RSEQ_SET_CRITICAL) {
		/* Defines (process-wide) critical section. */
		__user void *crit_start = (__user void *)val1;
		__user void *crit_end = (__user void *)val2;
		__user void *crit_restart = (__user void *)val3;

		rc = rseq_register_critical_current(
			crit_start, crit_end, crit_restart);
	} else if (op == SYS_RSEQ_SET_CPU_POINTER) {
		/*
		 * Enables RSEQ for this thread; sets location for CPU update
		 * to val1.
		 */
		int __user *cpu = (int __user *)val1;

		rc = rseq_register_cpu_pointer(current, cpu);
	}

	return rc;
}
#else
SYSCALL_DEFINE0(restartable_sequences)
{
	return -ENOSYS;
}
#endif
