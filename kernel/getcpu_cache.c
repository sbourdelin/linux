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

#include <linux/init.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/syscalls.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/getcpu_cache.h>

static struct getcpu_cache_entry *
	add_thread_entry(struct task_struct *t,
		int32_t __user *cpu_cache)
{
	struct getcpu_cache_entry *te;

	te = kmalloc(sizeof(*te), GFP_KERNEL);
	if (!te)
		return NULL;
	te->cpu_cache = cpu_cache;
	list_add(&te->entry, &t->getcpu_cache_head);
	return te;
}

static void remove_thread_entry(struct getcpu_cache_entry *te)
{
	list_del(&te->entry);
	kfree(te);
}

static void remove_all_thread_entry(struct task_struct *t)
{
	struct getcpu_cache_entry *te, *te_tmp;

	list_for_each_entry_safe(te, te_tmp, &t->getcpu_cache_head, entry)
		remove_thread_entry(te);
}

static struct getcpu_cache_entry *
	find_thread_entry(struct task_struct *t,
		int32_t __user *cpu_cache)
{
	struct getcpu_cache_entry *te;

	list_for_each_entry(te, &t->getcpu_cache_head, entry) {
		if (te->cpu_cache == cpu_cache)
			return te;
	}
	return NULL;
}

static int getcpu_cache_update_entry(struct getcpu_cache_entry *te)
{
	if (put_user(raw_smp_processor_id(), te->cpu_cache)) {
		/*
		 * Force unregistration of each entry causing
		 * put_user() errors.
		 */
		remove_thread_entry(te);
		return -1;
	}
	return 0;

}

static int getcpu_cache_update(struct task_struct *t)
{
	struct getcpu_cache_entry *te, *te_tmp;
	int err = 0;

	list_for_each_entry_safe(te, te_tmp, &t->getcpu_cache_head, entry) {
		if (getcpu_cache_update_entry(te))
			err = -1;
	}
	return err;
}

/*
 * This resume handler should always be executed between a migration
 * triggered by preemption and return to user-space.
 */
void __getcpu_cache_handle_notify_resume(struct task_struct *t)
{
	if (unlikely(t->flags & PF_EXITING))
		return;
	if (getcpu_cache_update(t))
		force_sig(SIGSEGV, t);
}

/*
 * If parent process has a thread-local ABI, the child inherits. Only applies
 * when forking a process, not a thread.
 */
int getcpu_cache_fork(struct task_struct *t)
{
	struct getcpu_cache_entry *te;

	list_for_each_entry(te, &current->getcpu_cache_head, entry) {
		if (!add_thread_entry(t, te->cpu_cache))
			return -1;
	}
	return 0;
}

void getcpu_cache_execve(struct task_struct *t)
{
	remove_all_thread_entry(t);
}

void getcpu_cache_exit(struct task_struct *t)
{
	remove_all_thread_entry(t);
}

/*
 * sys_getcpu_cache - setup getcpu cache for caller thread
 */
SYSCALL_DEFINE3(getcpu_cache, int, cmd, int32_t __user *, cpu_cache,
		int, flags)
{
	struct getcpu_cache_entry *te;

	if (unlikely(!cpu_cache || flags))
		return -EINVAL;
	te = find_thread_entry(current, cpu_cache);
	switch (cmd) {
	case GETCPU_CACHE_CMD_REGISTER:
		/* Attempt to register cpu_cache. Check if already there. */
		if (te)
			return -EBUSY;
		te = add_thread_entry(current, cpu_cache);
		if (!te)
			return -ENOMEM;
		/*
		 * Migration walks the getcpu cache entry list to see
		 * whether the notify_resume flag should be set.
		 * Therefore, we need to ensure that the scheduler sees
		 * the list update before we update the getcpu cache
		 * content with the current CPU number.
		 *
		 * Add thread entry to list before updating content.
		 */
		barrier();
		if (getcpu_cache_update_entry(te))
			return -EFAULT;
		return 0;
	case GETCPU_CACHE_CMD_UNREGISTER:
		/* Unregistration is requested. */
		if (!te)
			return -ENOENT;
		remove_thread_entry(te);
		return 0;
	default:
		return -EINVAL;
	}
}
