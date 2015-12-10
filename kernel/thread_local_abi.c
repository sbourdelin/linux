/*
 * Copyright (C) 2015 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * thread_local_abi system call
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

#include <linux/init.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/syscalls.h>

static int getcpu_cache_update(struct task_struct *t)
{
	if (put_user(raw_smp_processor_id(), &t->thread_local_abi->cpu)) {
		t->thread_local_abi_len = 0;
		t->thread_local_abi = NULL;
		return -1;
	}
	return 0;
}

/*
 * This resume handler should always be executed between a migration
 * triggered by preemption and return to user-space.
 */
void getcpu_cache_handle_notify_resume(struct task_struct *t)
{
	BUG_ON(!getcpu_cache_active(t));
	if (unlikely(t->flags & PF_EXITING))
		return;
	if (getcpu_cache_update(t))
		force_sig(SIGSEGV, t);
}

/*
 * If parent process has a thread-local ABI, the child inherits. Only applies
 * when forking a process, not a thread.
 */
void thread_local_abi_fork(struct task_struct *t)
{
	t->thread_local_abi_len = current->thread_local_abi_len;
	t->thread_local_abi = current->thread_local_abi;
}

void thread_local_abi_execve(struct task_struct *t)
{
	t->thread_local_abi_len = 0;
	t->thread_local_abi = NULL;
}

/*
 * sys_thread_local_abi - setup thread-local ABI for caller thread
 */
SYSCALL_DEFINE3(thread_local_abi, struct thread_local_abi __user *, tlap,
		size_t, len, int, flags)
{
	size_t minlen;

	if (flags)
		return -EINVAL;
	if (current->thread_local_abi && tlap)
		return -EBUSY;
	/* Agree on the intersection of userspace and kernel features */
	if (!tlap)
		minlen = 0;
	else
		minlen = min_t(size_t, len, sizeof(struct thread_local_abi));
	current->thread_local_abi_len = minlen;
	current->thread_local_abi = tlap;
	/*
	 * Migration checks ->thread_local_abi_len to see if notify_resume
	 * flag should be set. Therefore, we need to ensure that
	 * the scheduler sees ->thread_local_abi_len before we update
	 * the getcpu cache content with the current CPU number.
	 */
	barrier();	/* Store thread_local_abi_len before update content */
	if (getcpu_cache_active(current)) {
		if (getcpu_cache_update(current))
			return -EFAULT;
	}
	return minlen;
}
