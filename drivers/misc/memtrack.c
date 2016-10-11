/* drivers/misc/memtrack.c
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
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/memtrack.h>
#include <linux/profile.h>
#include <linux/rbtree.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/mm.h>

struct memtrack_vma_list {
	struct hlist_node node;
	const struct vm_area_struct *vma;
};

struct memtrack_handle {
	struct memtrack_buffer *buffer;
	struct rb_node node;
	struct rb_root *root;
	struct kref refcount;
	struct hlist_head vma_list;
};

static struct kmem_cache *memtrack_handle_cache;

static DEFINE_MUTEX(memtrack_id_lock);
#if IS_ENABLED(CONFIG_MEMTRACK_DEBUG)
static struct dentry *debugfs_file;
static DEFINE_IDR(mem_idr);
#else
static DEFINE_IDA(mem_ida);
#endif

static struct memtrack_handle *memtrack_handle_find_locked(struct rb_root *root,
		struct memtrack_buffer *buffer, bool alloc)
{
	struct rb_node **new = &root->rb_node, *parent = NULL;
	struct memtrack_handle *handle;

	while (*new) {
		struct rb_node *node = *new;

		handle = rb_entry(node, struct memtrack_handle, node);
		parent = node;
		if (handle->buffer->id > buffer->id) {
			new = &node->rb_left;
		} else if (handle->buffer->id < buffer->id) {
			new = &node->rb_right;
		} else {
			return handle;
		}
	}

	if (alloc) {
		handle = kmem_cache_alloc(memtrack_handle_cache, GFP_KERNEL);
		if (!handle)
			return NULL;

		handle->buffer = buffer;
		handle->root = root;
		kref_init(&handle->refcount);
		INIT_HLIST_HEAD(&handle->vma_list);

		rb_link_node(&handle->node, parent, new);
		rb_insert_color(&handle->node, root);
		atomic_inc(&handle->buffer->userspace_handles);
	}

	return NULL;
}

static void memtrack_buffer_install_locked(struct rb_root *root,
		struct memtrack_buffer *buffer)
{
	struct memtrack_handle *handle;

	handle = memtrack_handle_find_locked(root, buffer, true);
	if (handle) {
		kref_get(&handle->refcount);
		return;
	}
}

/**
 * memtrack_buffer_install - add a userspace reference to a shared buffer
 *
 * @buffer: the buffer's memtrack entry
 * @tsk: the userspace task that took the reference
 *
 * This is normally called while creating a userspace handle (fd, etc.) to
 * @buffer.
 */
void memtrack_buffer_install(struct memtrack_buffer *buffer,
		struct task_struct *tsk)
{
	struct task_struct *leader;
	unsigned long flags;

	if (!buffer || !tsk)
		return;

	leader = tsk->group_leader;
	write_lock_irqsave(&leader->memtrack_lock, flags);
	memtrack_buffer_install_locked(&leader->memtrack_rb, buffer);
	write_unlock_irqrestore(&leader->memtrack_lock, flags);
}
EXPORT_SYMBOL(memtrack_buffer_install);

static void memtrack_handle_destroy(struct kref *ref)
{
	struct memtrack_handle *handle;

	handle = container_of(ref, struct memtrack_handle, refcount);
	rb_erase(&handle->node, handle->root);
	atomic_dec(&handle->buffer->userspace_handles);
	kmem_cache_free(memtrack_handle_cache, handle);
}

static void memtrack_buffer_uninstall_locked(struct rb_root *root,
		struct memtrack_buffer *buffer)
{
	struct memtrack_handle *handle;

	handle = memtrack_handle_find_locked(root, buffer, false);

	if (handle)
		kref_put(&handle->refcount, memtrack_handle_destroy);
}

static void memtrack_buffer_vm_open_locked(struct rb_root *root,
		struct memtrack_buffer *buffer,
		struct memtrack_vma_list *vma_list)
{
	struct memtrack_handle *handle;

	handle = memtrack_handle_find_locked(root, buffer, false);
	if (handle)
		hlist_add_head(&vma_list->node, &handle->vma_list);
}

static void memtrack_buffer_vm_close_locked(struct rb_root *root,
		struct memtrack_buffer *buffer,
		const struct vm_area_struct *vma)
{
	struct memtrack_handle *handle;

	handle = memtrack_handle_find_locked(root, buffer, false);
	if (handle) {
		struct memtrack_vma_list *vma_list;

		hlist_for_each_entry(vma_list, &handle->vma_list, node) {
			if (vma_list->vma == vma) {
				hlist_del(&vma_list->node);
				kfree(vma_list);
				return;
			}
		}
	}
}

/**
 * memtrack_buffer_uninstall - drop a userspace reference to a shared buffer
 *
 * @buffer: the buffer's memtrack entry
 * @tsk: the userspace task that dropped the reference
 *
 * This is normally called while tearing down a userspace handle to @buffer.
 */
void memtrack_buffer_uninstall(struct memtrack_buffer *buffer,
		struct task_struct *tsk)
{
	struct task_struct *leader;
	unsigned long flags;

	if (!buffer || !tsk)
		return;

	leader = tsk->group_leader;
	write_lock_irqsave(&leader->memtrack_lock, flags);
	memtrack_buffer_uninstall_locked(&leader->memtrack_rb, buffer);
	write_unlock_irqrestore(&leader->memtrack_lock, flags);
}
EXPORT_SYMBOL(memtrack_buffer_uninstall);

/**
 * memtrack_buffer_vm_open - account for pages mapped during vm open
 *
 * @buffer: the buffer's memtrack entry
 *
 * @vma: vma being opened
 */
void memtrack_buffer_vm_open(struct memtrack_buffer *buffer,
		const struct vm_area_struct *vma)
{
	unsigned long flags;
	struct task_struct *leader = current->group_leader;
	struct memtrack_vma_list *vma_list;

	vma_list = kmalloc(sizeof(*vma_list), GFP_KERNEL);
	if (WARN_ON(!vma_list))
		return;
	vma_list->vma = vma;

	write_lock_irqsave(&leader->memtrack_lock, flags);
	memtrack_buffer_vm_open_locked(&leader->memtrack_rb, buffer, vma_list);
	write_unlock_irqrestore(&leader->memtrack_lock, flags);
}
EXPORT_SYMBOL(memtrack_buffer_vm_open);

/**
 * memtrack_buffer_vm_close - account for pages unmapped during vm close
 *
 * @buffer: the buffer's memtrack entry
 * @vma: the vma being closed
 */
void memtrack_buffer_vm_close(struct memtrack_buffer *buffer,
		const struct vm_area_struct *vma)
{
	unsigned long flags;
	struct task_struct *leader = current->group_leader;

	write_lock_irqsave(&leader->memtrack_lock, flags);
	memtrack_buffer_vm_close_locked(&leader->memtrack_rb, buffer, vma);
	write_unlock_irqrestore(&leader->memtrack_lock, flags);
}
EXPORT_SYMBOL(memtrack_buffer_vm_close);

static int memtrack_id_alloc(struct memtrack_buffer *buffer)
{
	int ret;

	mutex_lock(&memtrack_id_lock);
#if IS_ENABLED(CONFIG_MEMTRACK_DEBUG)
	ret = idr_alloc(&mem_idr, buffer, 0, 0, GFP_KERNEL);
#else
	ret = ida_simple_get(&mem_ida, 0, 0, GFP_KERNEL);
#endif
	mutex_unlock(&memtrack_id_lock);

	return ret;
}

static void memtrack_id_free(struct memtrack_buffer *buffer)
{
	mutex_lock(&memtrack_id_lock);
#if IS_ENABLED(CONFIG_MEMTRACK_DEBUG)
	idr_remove(&mem_idr, buffer->id);
#else
	ida_simple_remove(&mem_ida, buffer->id);
#endif
	mutex_unlock(&memtrack_id_lock);
}

/**
 * memtrack_buffer_remove - deinitialize a memtrack entry
 *
 * @buffer: the memtrack entry to deinitialize
 *
 * This is normally called just before freeing the pages backing @buffer.
 */
void memtrack_buffer_remove(struct memtrack_buffer *buffer)
{
	if (!buffer)
		return;

	if (WARN_ON(atomic_read(&buffer->userspace_handles)))
		return;

	kfree(buffer->tag);
	memtrack_id_free(buffer);
}
EXPORT_SYMBOL(memtrack_buffer_remove);

/**
 * memtrack_buffer_init - initialize a memtrack entry for a shared buffer
 *
 * @buffer: the memtrack entry to initialize
 * @size: the size of the shared buffer
 *
 * This is normally called just after allocating the buffer's backing pages.
 *
 * There must be a 1-to-1 mapping between buffers and
 * struct memtrack_buffers.  That is, memtrack_buffer_init() should be called
 * only *once* for a given buffer, even if it's exported to
 * userspace in multiple forms (e.g., simultaneously as a dma-buf fd and a
 * GEM handle).
 *
 * Return 0 on success or a negative error code on failure.
 */
int memtrack_buffer_init(struct memtrack_buffer *buffer, size_t size)
{
	if (!buffer)
		return -EINVAL;

	memset(buffer, 0, sizeof(*buffer));

	buffer->id = memtrack_id_alloc(buffer);
	if (buffer->id < 0) {
		pr_err("%s: Error allocating unique identifier\n", __func__);
		return buffer->id;
	}

	buffer->size = size;
	atomic_set(&buffer->userspace_handles, 0);
#if IS_ENABLED(CONFIG_MEMTRACK_DEBUG)
	buffer->pid = current->group_leader->pid;
#endif
	return 0;
}
EXPORT_SYMBOL(memtrack_buffer_init);

static int process_notifier(struct notifier_block *self,
			unsigned long cmd, void *v)
{
	struct task_struct *task = v, *leader;
	struct rb_root *root;
	struct rb_node *node;
	unsigned long flags;

	if (!task)
		return NOTIFY_OK;

	leader = task->group_leader;
	write_lock_irqsave(&leader->memtrack_lock, flags);
	root = &leader->memtrack_rb;
	node = rb_first(root);
	while (node) {
		struct memtrack_handle *handle;

		handle = rb_entry(node, struct memtrack_handle, node);
		rb_erase(&handle->node, handle->root);
		atomic_dec(&handle->buffer->userspace_handles);
		kmem_cache_free(memtrack_handle_cache, handle);

		node = rb_next(node);
	}
	write_unlock_irqrestore(&leader->memtrack_lock, flags);

	return NOTIFY_OK;
}

static struct notifier_block process_notifier_block = {
	.notifier_call	= process_notifier,
};

static void show_memtrack_vma(struct seq_file *m,
		const struct vm_area_struct *vma,
		const struct memtrack_buffer *buf)
{
	unsigned long start = vma->vm_start;
	unsigned long end = vma->vm_end;
	unsigned long long pgoff = ((loff_t)vma->vm_pgoff) << PAGE_SHIFT;
	vm_flags_t flags = vma->vm_flags;
	vm_flags_t remap_flag = VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;

	seq_setwidth(m, 50);
	seq_printf(m, "%68lx-%08lx  %c%c%c%c%c  %08llx",
			start,
			end,
			flags & VM_READ ? 'r' : '-',
			flags & VM_WRITE ? 'w' : '-',
			flags & VM_EXEC ? 'x' : '-',
			flags & VM_MAYSHARE ? 's' : 'p',
			flags & remap_flag ? '#' : '-',
			pgoff);
	if (buf->tag) {
		seq_pad(m, ' ');
		seq_puts(m, buf->tag);
	}
	seq_putc(m, '\n');
}

int proc_memtrack(struct seq_file *m, struct pid_namespace *ns, struct pid *pid,
			struct task_struct *task)
{
	struct rb_node *node;
	unsigned long flags;

	read_lock_irqsave(&task->memtrack_lock, flags);
	if (RB_EMPTY_ROOT(&task->memtrack_rb))
		goto done;

	seq_printf(m, "%10.10s: %16.16s: %12.12s: %12.12s: %20s: %5s: %8s: pid:%d\n",
			"ref_count", "Identifier", "size", "tag",
			"startAddr-endAddr", "Flags", "pgOff", task->pid);

	for (node = rb_first(&task->memtrack_rb); node; node = rb_next(node)) {
		struct memtrack_handle *handle = rb_entry(node,
				struct memtrack_handle, node);
		struct memtrack_buffer *buffer = handle->buffer;
		struct memtrack_vma_list *vma;

		seq_printf(m, "%10d  %16d  %12zu  %12s\n",
				atomic_read(&buffer->userspace_handles),
				buffer->id, buffer->size,
				buffer->tag ? buffer->tag : "");

		hlist_for_each_entry(vma, &handle->vma_list, node)
			show_memtrack_vma(m, vma->vma, handle->buffer);
	}

done:
	read_unlock_irqrestore(&task->memtrack_lock, flags);
	return 0;
}

#if IS_ENABLED(CONFIG_MEMTRACK_DEBUG)
static int memtrack_show(struct seq_file *m, void *v)
{
	struct memtrack_buffer *buffer;
	int i;

	seq_printf(m, "%4.4s %12.12s %10s %12.12s %3.3s\n", "pid",
			"buffer_size", "ref", "Identifier", "tag");
	rcu_read_lock();
	idr_for_each_entry(&mem_idr, buffer, i)
		seq_printf(m, "%4d %12zu %10d %12d %s\n", buffer->pid,
				buffer->size,
				atomic_read(&buffer->userspace_handles),
				buffer->id, buffer->tag ? buffer->tag : "");
	rcu_read_unlock();
	return 0;
}

static int memtrack_open(struct inode *inode, struct file *file)
{
	return single_open(file, memtrack_show, inode->i_private);
}

static const struct file_operations memtrack_fops = {
	.open		= memtrack_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
#endif


static int __init memtrack_init(void)
{
	memtrack_handle_cache = KMEM_CACHE(memtrack_handle, SLAB_HWCACHE_ALIGN);
	if (!memtrack_handle_cache)
		return -ENOMEM;

#if IS_ENABLED(CONFIG_MEMTRACK_DEBUG)
	debugfs_file = debugfs_create_file("memtrack", S_IRUGO, NULL, NULL,
			&memtrack_fops);
#endif

	profile_event_register(PROFILE_TASK_EXIT, &process_notifier_block);
	return 0;
}
late_initcall(memtrack_init);

static void __exit memtrack_exit(void)
{
	kmem_cache_destroy(memtrack_handle_cache);
#if IS_ENABLED(CONFIG_MEMTRACK_DEBUG)
	debugfs_remove(debugfs_file);
#endif
	profile_event_unregister(PROFILE_TASK_EXIT, &process_notifier_block);
}
__exitcall(memtrack_exit);
