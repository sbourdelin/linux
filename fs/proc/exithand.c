// SPDX-License-Identifier: GPL-2.0
/* Synchronous exit notification of non-child processes
 *
 * Simple file descriptor /proc/pid/exithand. Read blocks (and poll
 * reports non-readable) until process either dies or becomes
 * a zombie.
 */
#include <linux/printk.h>
#include <linux/sched/signal.h>
#include <linux/poll.h>
#include "internal.h"

static int proc_tgid_exithand_open(struct inode *inode, struct file *file)
{
	struct task_struct* task = get_proc_task(inode);
	/* If get_proc_task failed, it means the task is dead, which
	 * is fine, since a subsequent read will return
	 * immediately.  */
	if (task && !thread_group_leader(task))
		return -EINVAL;
	return 0;
}

static ssize_t proc_tgid_exithand_read(struct file * file,
				       char __user * buf,
				       size_t count, loff_t *ppos)
{
	struct task_struct* task = NULL;
	wait_queue_entry_t wait;
	ssize_t res = 0;
	bool locked = false;

	for (;;) {
		/* Retrieve the task from the struct pid each time
		 * through the loop in case the exact struct task
		 * changes underneath us (e.g., if in exec.c, the
		 * execing process kills the group leader and starts
		 * using its PID).  The struct signal should be the
		 * same though even in this case.
		 */
		task = get_proc_task(file_inode(file));
		res = 0;
		if (!task)
			goto out;  /* No task?  Must have died.  */

		BUG_ON(!thread_group_leader(task));

		/* Synchronizes with exit.c machinery. */
		read_lock(&tasklist_lock);
		locked = true;

		res = 0;
		if (task->exit_state)
			goto out;

		res = -EAGAIN;
		if (file->f_flags & O_NONBLOCK)
			goto out;

		/* Tell exit.c to go to the trouble of waking our
		 * runqueue when this process gets around to
		 * exiting. */
		task->signal->exithand_is_interested = true;

		/* Even if the task identity changes, task->signal
		 * should be invariant across the wait, making it safe
		 * to go remove our wait record from the wait queue
		 * after we come back from schedule.  */

		init_waitqueue_entry(&wait, current);
		add_wait_queue(&wait_exithand, &wait);

		read_unlock(&tasklist_lock);
		locked = false;

		put_task_struct(task);
		task = NULL;

		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
		set_current_state(TASK_RUNNING);
		remove_wait_queue(&wait_exithand, &wait);

		res = -ERESTARTSYS;
		if (signal_pending(current))
			goto out;
	}
out:
	if (locked)
		read_unlock(&tasklist_lock);
	if (task)
		put_task_struct(task);
	return res;
}

static __poll_t proc_tgid_exithand_poll(struct file *file, poll_table *wait)
{
	__poll_t mask = 0;
	struct task_struct* task = get_proc_task(file_inode(file));
	if (!task) {
		mask |= POLLIN;
	} else if (READ_ONCE(task->exit_state)) {
		mask |= POLLIN;
	} else {
		read_lock(&tasklist_lock);
		task->signal->exithand_is_interested = true;
		read_unlock(&tasklist_lock);
		poll_wait(file,	&wait_exithand,	wait);
	}
	return mask;
}

const struct file_operations proc_tgid_exithand_operations = {
	.open           = proc_tgid_exithand_open,
	.read		= proc_tgid_exithand_read,
	.poll           = proc_tgid_exithand_poll,
};
