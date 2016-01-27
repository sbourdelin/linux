/*
 * Copyright (C) 2015 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * getcpu cache system call
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
 */

#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/syscalls.h>

static int getcpu_cache_update(int32_t __user *cpu_cache)
{
	if (put_user(raw_smp_processor_id(), cpu_cache))
		return -1;
	return 0;
}

/*
 * This resume handler should always be executed between a migration
 * triggered by preemption and return to user-space.
 */
void __getcpu_cache_handle_notify_resume(struct task_struct *t)
{
	if (unlikely(t->flags & PF_EXITING))
		return;
	if (getcpu_cache_update(t->cpu_cache))
		force_sig(SIGSEGV, t);
}

/*
 * If parent process has a thread-local ABI, the child inherits. Only applies
 * when forking a process, not a thread.
 */
void getcpu_cache_fork(struct task_struct *t)
{
	t->cpu_cache = current->cpu_cache;
}

void getcpu_cache_execve(struct task_struct *t)
{
	t->cpu_cache = NULL;
}

void getcpu_cache_exit(struct task_struct *t)
{
	t->cpu_cache = NULL;
}

/*
 * sys_getcpu_cache - setup getcpu cache for caller thread
 */
SYSCALL_DEFINE2(getcpu_cache, int32_t __user **, cpu_cachep, int, flags)
{
	int32_t __user *cpu_cache;

	if (unlikely(flags))
		return -EINVAL;
	/* Check if cpu_cache is already registered. */
	if (current->cpu_cache) {
		if (put_user(current->cpu_cache, cpu_cachep))
			return -EFAULT;
		return 0;
	}
	if (get_user(cpu_cache, cpu_cachep))
			return -EFAULT;
	if (unlikely(!IS_ALIGNED((unsigned long)cpu_cache, sizeof(int32_t))
			|| !cpu_cache))
		return -EINVAL;
	/*
	 * Do an initial cpu cache update to ensure we won't hit
	 * SIGSEGV if put_user() fails in the resume notifier.
	 */
	if (getcpu_cache_update(cpu_cache)) {
		return -EFAULT;
	}
	current->cpu_cache = cpu_cache;
	/*
	 * Migration checks the getcpu cache to see whether the
	 * notify_resume flag should be set.
	 * Therefore, we need to ensure that the scheduler sees
	 * the getcpu cache pointer update before we update the getcpu
	 * cache content with the current CPU number.
	 *
	 * Set cpu_cache pointer before updating content.
	 */
	barrier();
	/*
	 * Set the resume notifier to ensure we update the current CPU
	 * number before returning to userspace if needed. This handles
	 * migration happening between the initial
	 * get_cpu_cache_update() call and setting the current
	 * cpu_cache pointer.
	 */
	getcpu_cache_set_notify_resume(current);
	return 0;
}
