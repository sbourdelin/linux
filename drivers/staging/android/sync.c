/*
 * drivers/base/sync.c
 *
 * Copyright (C) 2012 Google, Inc.
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
#include <linux/export.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/anon_inodes.h>

#include "sync.h"

#define CREATE_TRACE_POINTS
#include "trace/sync.h"

static const struct fence_ops sync_fence_ops;
static const struct file_operations sync_fence_fops;

struct fence *sync_pt_create(struct fence_timeline *obj, int size, u32 value)
{
	unsigned long flags;
	struct fence *fence;

	if (size < sizeof(*fence))
		return NULL;

	fence = kzalloc(size, GFP_KERNEL);
	if (!fence)
		return NULL;

	spin_lock_irqsave(&obj->lock, flags);
	fence_timeline_get(obj);
	fence_init(fence, &sync_fence_ops, &obj->lock,
		   obj->context, value);
	list_add_tail(&fence->child_list, &obj->child_list_head);
	INIT_LIST_HEAD(&fence->active_list);
	spin_unlock_irqrestore(&obj->lock, flags);
	return fence;
}
EXPORT_SYMBOL(sync_pt_create);

static struct sync_fence *sync_fence_alloc(int size, const char *name)
{
	struct sync_fence *sync_fence;

	sync_fence = kzalloc(size, GFP_KERNEL);
	if (!sync_fence)
		return NULL;

	sync_fence->file = anon_inode_getfile("sync_fence", &sync_fence_fops,
					      sync_fence, 0);
	if (IS_ERR(sync_fence->file))
		goto err;

	kref_init(&sync_fence->kref);
	strlcpy(sync_fence->name, name, sizeof(sync_fence->name));

	init_waitqueue_head(&sync_fence->wq);

	return sync_fence;

err:
	kfree(sync_fence);
	return NULL;
}

static void fence_check_cb_func(struct fence *f, struct fence_cb *cb)
{
	struct sync_fence_cb *check;
	struct sync_fence *sync_fence;

	check = container_of(cb, struct sync_fence_cb, cb);
	sync_fence = check->sync_fence;

	if (atomic_dec_and_test(&sync_fence->status))
		wake_up_all(&sync_fence->wq);
}

/* TODO: implement a create which takes more that one sync_pt */
struct sync_fence *sync_fence_create_dma(const char *name, struct fence *fence)
{
	struct sync_fence *sync_fence;

	sync_fence = sync_fence_alloc(offsetof(struct sync_fence, cbs[1]),
				      name);
	if (!sync_fence)
		return NULL;

	sync_fence->num_fences = 1;
	atomic_set(&sync_fence->status, 1);

	sync_fence->cbs[0].fence = fence;
	sync_fence->cbs[0].sync_fence = sync_fence;
	if (fence_add_callback(fence, &sync_fence->cbs[0].cb,
			       fence_check_cb_func))
		atomic_dec(&sync_fence->status);

	sync_fence_debug_add(sync_fence);

	return sync_fence;
}
EXPORT_SYMBOL(sync_fence_create_dma);

struct sync_fence *sync_fence_create(const char *name, struct fence *fence)
{
	return sync_fence_create_dma(name, fence);
}
EXPORT_SYMBOL(sync_fence_create);

struct sync_fence *sync_fence_fdget(int fd)
{
	struct file *file = fget(fd);

	if (!file)
		return NULL;

	if (file->f_op != &sync_fence_fops)
		goto err;

	return file->private_data;

err:
	fput(file);
	return NULL;
}
EXPORT_SYMBOL(sync_fence_fdget);

void sync_fence_put(struct sync_fence *sync_fence)
{
	fput(sync_fence->file);
}
EXPORT_SYMBOL(sync_fence_put);

void sync_fence_install(struct sync_fence *sync_fence, int fd)
{
	fd_install(fd, sync_fence->file);
}
EXPORT_SYMBOL(sync_fence_install);

static void sync_fence_add_pt(struct sync_fence *sync_fence,
			      int *i, struct fence *fence)
{
	sync_fence->cbs[*i].fence = fence;
	sync_fence->cbs[*i].sync_fence = sync_fence;

	if (!fence_add_callback(fence, &sync_fence->cbs[*i].cb,
				fence_check_cb_func)) {
		fence_get(fence);
		(*i)++;
	}
}

struct sync_fence *sync_fence_merge(const char *name,
				    struct sync_fence *a, struct sync_fence *b)
{
	int num_fences = a->num_fences + b->num_fences;
	struct sync_fence *sync_fence;
	int i, i_a, i_b;
	unsigned long size = offsetof(struct sync_fence, cbs[num_fences]);

	sync_fence = sync_fence_alloc(size, name);
	if (!sync_fence)
		return NULL;

	atomic_set(&sync_fence->status, num_fences);

	/*
	 * Assume sync_fence a and b are both ordered and have no
	 * duplicates with the same context.
	 *
	 * If a sync_fence can only be created with sync_fence_merge
	 * and sync_fence_create, this is a reasonable assumption.
	 */
	for (i = i_a = i_b = 0; i_a < a->num_fences && i_b < b->num_fences; ) {
		struct fence *pt_a = a->cbs[i_a].fence;
		struct fence *pt_b = b->cbs[i_b].fence;

		if (pt_a->context < pt_b->context) {
			sync_fence_add_pt(sync_fence, &i, pt_a);

			i_a++;
		} else if (pt_a->context > pt_b->context) {
			sync_fence_add_pt(sync_fence, &i, pt_b);

			i_b++;
		} else {
			if (pt_a->seqno - pt_b->seqno <= INT_MAX)
				sync_fence_add_pt(sync_fence, &i, pt_a);
			else
				sync_fence_add_pt(sync_fence, &i, pt_b);

			i_a++;
			i_b++;
		}
	}

	for (; i_a < a->num_fences; i_a++)
		sync_fence_add_pt(sync_fence, &i, a->cbs[i_a].fence);

	for (; i_b < b->num_fences; i_b++)
		sync_fence_add_pt(sync_fence, &i, b->cbs[i_b].fence);

	if (num_fences > i)
		atomic_sub(num_fences - i, &sync_fence->status);
	sync_fence->num_fences = i;

	sync_fence_debug_add(sync_fence);
	return sync_fence;
}
EXPORT_SYMBOL(sync_fence_merge);

int sync_fence_wake_up_wq(wait_queue_t *curr, unsigned mode,
			  int wake_flags, void *key)
{
	struct sync_fence_waiter *wait;

	wait = container_of(curr, struct sync_fence_waiter, work);
	list_del_init(&wait->work.task_list);

	wait->callback(wait->work.private, wait);
	return 1;
}

int sync_fence_wait_async(struct sync_fence *sync_fence,
			  struct sync_fence_waiter *waiter)
{
	int err = atomic_read(&sync_fence->status);
	unsigned long flags;

	if (err < 0)
		return err;

	if (!err)
		return 1;

	init_waitqueue_func_entry(&waiter->work, sync_fence_wake_up_wq);
	waiter->work.private = sync_fence;

	spin_lock_irqsave(&sync_fence->wq.lock, flags);
	err = atomic_read(&sync_fence->status);
	if (err > 0)
		__add_wait_queue_tail(&sync_fence->wq, &waiter->work);
	spin_unlock_irqrestore(&sync_fence->wq.lock, flags);

	if (err < 0)
		return err;

	return !err;
}
EXPORT_SYMBOL(sync_fence_wait_async);

int sync_fence_cancel_async(struct sync_fence *sync_fence,
			    struct sync_fence_waiter *waiter)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&sync_fence->wq.lock, flags);
	if (!list_empty(&waiter->work.task_list))
		list_del_init(&waiter->work.task_list);
	else
		ret = -ENOENT;
	spin_unlock_irqrestore(&sync_fence->wq.lock, flags);
	return ret;
}
EXPORT_SYMBOL(sync_fence_cancel_async);

int sync_fence_wait(struct sync_fence *sync_fence, long timeout)
{
	long ret;
	int i;

	if (timeout < 0)
		timeout = MAX_SCHEDULE_TIMEOUT;
	else
		timeout = msecs_to_jiffies(timeout);

	trace_sync_wait(sync_fence, 1);
	for (i = 0; i < sync_fence->num_fences; ++i)
		trace_fence(sync_fence->cbs[i].fence);
	ret = wait_event_interruptible_timeout(sync_fence->wq,
					       atomic_read(&sync_fence->status) <= 0,
					       timeout);
	trace_sync_wait(sync_fence, 0);

	if (ret < 0) {
		return ret;
	} else if (ret == 0) {
		if (timeout) {
			pr_info("sync_fence timeout on [%p] after %dms\n",
				sync_fence, jiffies_to_msecs(timeout));
			sync_dump();
		}
		return -ETIME;
	}

	ret = atomic_read(&sync_fence->status);
	if (ret) {
		pr_info("sync_fence error %ld on [%p]\n", ret, sync_fence);
		sync_dump();
	}
	return ret;
}
EXPORT_SYMBOL(sync_fence_wait);

static int sync_fence_fill_driver_data(struct fence *fence,
					  void *data, int size)
{
	struct fence_timeline *parent = fence_parent(fence);

	if (!parent->ops->fill_driver_data)
		return 0;
	return parent->ops->fill_driver_data(fence, data, size);
}

static void sync_fence_value_str(struct fence *fence,
				    char *str, int size)
{
	struct fence_timeline *parent = fence_parent(fence);

	if (!parent->ops->fence_value_str) {
		if (size)
			*str = 0;
		return;
	}
	parent->ops->fence_value_str(fence, str, size);
}

static void sync_fence_timeline_value_str(struct fence *fence,
					     char *str, int size)
{
	struct fence_timeline *parent = fence_parent(fence);

	if (!parent->ops->timeline_value_str) {
		if (size)
			*str = 0;
		return;
	}
	parent->ops->timeline_value_str(parent, str, size);
}

static const struct fence_ops sync_fence_ops = {
	.get_driver_name = fence_default_get_driver_name,
	.get_timeline_name = fence_default_get_timeline_name,
	.enable_signaling = fence_default_enable_signaling,
	.signaled = fence_default_signaled,
	.wait = fence_default_wait,
	.release = fence_default_release,
	.fill_driver_data = sync_fence_fill_driver_data,
	.fence_value_str = sync_fence_value_str,
	.timeline_value_str = sync_fence_timeline_value_str,
};

static void sync_fence_free(struct kref *kref)
{
	struct sync_fence *sync_fence = container_of(kref, struct sync_fence,
						     kref);
	int i;

	for (i = 0; i < sync_fence->num_fences; ++i) {
		fence_remove_callback(sync_fence->cbs[i].fence,
				      &sync_fence->cbs[i].cb);
		fence_put(sync_fence->cbs[i].fence);
	}

	kfree(sync_fence);
}

static int sync_fence_file_release(struct inode *inode, struct file *file)
{
	struct sync_fence *sync_fence = file->private_data;

	sync_fence_debug_remove(sync_fence);

	kref_put(&sync_fence->kref, sync_fence_free);
	return 0;
}

static unsigned int sync_fence_poll(struct file *file, poll_table *wait)
{
	struct sync_fence *sync_fence = file->private_data;
	int status;

	poll_wait(file, &sync_fence->wq, wait);

	status = atomic_read(&sync_fence->status);

	if (!status)
		return POLLIN;
	else if (status < 0)
		return POLLERR;
	return 0;
}

static long sync_fence_ioctl_wait(struct sync_fence *sync_fence,
				  unsigned long arg)
{
	__s32 value;

	if (copy_from_user(&value, (void __user *)arg, sizeof(value)))
		return -EFAULT;

	return sync_fence_wait(sync_fence, value);
}

static long sync_fence_ioctl_merge(struct sync_fence *sync_fence,
				   unsigned long arg)
{
	int fd = get_unused_fd_flags(O_CLOEXEC);
	int err;
	struct sync_fence *fence2, *fence3;
	struct sync_merge_data data;

	if (fd < 0)
		return fd;

	if (copy_from_user(&data, (void __user *)arg, sizeof(data))) {
		err = -EFAULT;
		goto err_put_fd;
	}

	fence2 = sync_fence_fdget(data.fd2);
	if (!fence2) {
		err = -ENOENT;
		goto err_put_fd;
	}

	data.name[sizeof(data.name) - 1] = '\0';
	fence3 = sync_fence_merge(data.name, sync_fence, fence2);
	if (!fence3) {
		err = -ENOMEM;
		goto err_put_fence2;
	}

	data.fence = fd;
	if (copy_to_user((void __user *)arg, &data, sizeof(data))) {
		err = -EFAULT;
		goto err_put_fence3;
	}

	sync_fence_install(fence3, fd);
	sync_fence_put(fence2);
	return 0;

err_put_fence3:
	sync_fence_put(fence3);

err_put_fence2:
	sync_fence_put(fence2);

err_put_fd:
	put_unused_fd(fd);
	return err;
}

static int sync_fill_pt_info(struct fence *fence, void *data, int size)
{
	struct sync_pt_info *info = data;
	int ret;

	if (size < sizeof(struct sync_pt_info))
		return -ENOMEM;

	info->len = sizeof(struct sync_pt_info);

	if (fence->ops->fill_driver_data) {
		ret = fence->ops->fill_driver_data(fence, info->driver_data,
						   size - sizeof(*info));
		if (ret < 0)
			return ret;

		info->len += ret;
	}

	strlcpy(info->obj_name, fence->ops->get_timeline_name(fence),
		sizeof(info->obj_name));
	strlcpy(info->driver_name, fence->ops->get_driver_name(fence),
		sizeof(info->driver_name));
	if (fence_is_signaled(fence))
		info->status = fence->status >= 0 ? 1 : fence->status;
	else
		info->status = 0;
	info->timestamp_ns = ktime_to_ns(fence->timestamp);

	return info->len;
}

static long sync_fence_ioctl_fence_info(struct sync_fence *sync_fence,
					unsigned long arg)
{
	struct sync_fence_info_data *data;
	__u32 size;
	__u32 len = 0;
	int ret, i;

	if (copy_from_user(&size, (void __user *)arg, sizeof(size)))
		return -EFAULT;

	if (size < sizeof(struct sync_fence_info_data))
		return -EINVAL;

	if (size > 4096)
		size = 4096;

	data = kzalloc(size, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	strlcpy(data->name, sync_fence->name, sizeof(data->name));
	data->status = atomic_read(&sync_fence->status);
	if (data->status >= 0)
		data->status = !data->status;

	len = sizeof(struct sync_fence_info_data);

	for (i = 0; i < sync_fence->num_fences; ++i) {
		struct fence *fence = sync_fence->cbs[i].fence;

		ret = sync_fill_pt_info(fence, (u8 *)data + len, size - len);

		if (ret < 0)
			goto out;

		len += ret;
	}

	data->len = len;

	if (copy_to_user((void __user *)arg, data, len))
		ret = -EFAULT;
	else
		ret = 0;

out:
	kfree(data);

	return ret;
}

static long sync_fence_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct sync_fence *sync_fence = file->private_data;

	switch (cmd) {
	case SYNC_IOC_WAIT:
		return sync_fence_ioctl_wait(sync_fence, arg);

	case SYNC_IOC_MERGE:
		return sync_fence_ioctl_merge(sync_fence, arg);

	case SYNC_IOC_FENCE_INFO:
		return sync_fence_ioctl_fence_info(sync_fence, arg);

	default:
		return -ENOTTY;
	}
}

static const struct file_operations sync_fence_fops = {
	.release = sync_fence_file_release,
	.poll = sync_fence_poll,
	.unlocked_ioctl = sync_fence_ioctl,
	.compat_ioctl = sync_fence_ioctl,
};

