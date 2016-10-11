/* include/linux/memtrack.h
 *
 * Copyright (C) 2016 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef _MEMTRACK_
#define _MEMTRACK_

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/slab.h>

#ifdef CONFIG_MEMTRACK
struct memtrack_buffer {
	size_t size;
	atomic_t userspace_handles;
	int id;
	const char *tag;
#ifdef CONFIG_MEMTRACK_DEBUG
	pid_t pid;
#endif
};

int proc_memtrack(struct seq_file *m, struct pid_namespace *ns, struct pid *pid,
		struct task_struct *task);
int memtrack_buffer_init(struct memtrack_buffer *buffer, size_t size);
void memtrack_buffer_remove(struct memtrack_buffer *buffer);
void memtrack_buffer_install(struct memtrack_buffer *buffer,
		struct task_struct *tsk);
void memtrack_buffer_uninstall(struct memtrack_buffer *buffer,
		struct task_struct *tsk);

/**
 * memtrack_buffer_set_tag - add a descriptive tag to a memtrack entry
 *
 * @buffer: the memtrack entry to tag
 * @tag: a string describing the buffer
 *
 * The tag is optional and provided only as information to userspace.  It has
 * no special meaning in the kernel.
 */
static inline int memtrack_buffer_set_tag(struct memtrack_buffer *buffer,
		const char *tag)
{
	const char *d = kstrdup(tag, GFP_KERNEL);

	if (!d)
		return -ENOMEM;

	kfree(buffer->tag);
	buffer->tag = d;
	return 0;
}
#else
struct memtrack_buffer { };

static inline int memtrack_buffer_init(struct memtrack_buffer *buffer,
		size_t size)
{
	return -ENOENT;
}

static inline void memtrack_buffer_remove(struct memtrack_buffer *buffer)
{
}

static inline void memtrack_buffer_install(struct memtrack_buffer *buffer,
		struct task_struct *tsk)
{
}

static inline void memtrack_buffer_uninstall(struct memtrack_buffer *buffer,
		struct task_struct *tsk)
{
}

static inline int memtrack_buffer_set_tag(struct memtrack_buffer *buffer,
		const char *tag)
{
	return -ENOENT;
}

#endif /* CONFIG_MEMTRACK */
#endif /* _MEMTRACK_ */
