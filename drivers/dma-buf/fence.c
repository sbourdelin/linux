/*
 * Fence mechanism for dma-buf and to allow for asynchronous dma access
 *
 * Copyright (C) 2012 Canonical Ltd
 * Copyright (C) 2012 Texas Instruments
 *
 * Authors:
 * Rob Clark <robdclark@gmail.com>
 * Maarten Lankhorst <maarten.lankhorst@canonical.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/slab.h>
#include <linux/export.h>
#include <linux/atomic.h>
#include <linux/fence.h>

#define CREATE_TRACE_POINTS
#include <trace/events/fence.h>

EXPORT_TRACEPOINT_SYMBOL(fence_annotate_wait_on);
EXPORT_TRACEPOINT_SYMBOL(fence_emit);

/*
 * fence context counter: each execution context should have its own
 * fence context, this allows checking if fences belong to the same
 * context or not. One device can have multiple separate contexts,
 * and they're used if some engine can run independently of another.
 */
static atomic_t fence_context_counter = ATOMIC_INIT(0);

/**
 * fence_context_alloc - allocate an array of fence contexts
 * @num:	[in]	amount of contexts to allocate
 *
 * This function will return the first index of the number of fences allocated.
 * The fence context is used for setting fence->context to a unique number.
 */
unsigned fence_context_alloc(unsigned num)
{
	BUG_ON(!num);
	return atomic_add_return(num, &fence_context_counter) - num;
}
EXPORT_SYMBOL(fence_context_alloc);

/**
 * fence_timeline_create - create a new fence_timeline
 * @num:	[in]	amount of contexts to allocate
 * @size:	[in]	size to allocate struct fence_timeline
 * @drv_name:	[in]	name of the driver
 * @name:	[in]	name of the timeline
 *
 * This function will return the new fence_timeline or NULL in case of error.
 * It allocs and initializes a new fence_timeline with a proper fence context
 * number assigned to it.
 */
struct fence_timeline *fence_timeline_create(unsigned num, int size,
					     const char *drv_name,
					     const char *name)
{
	struct fence_timeline *timeline;

	if (size < sizeof(*timeline))
		return NULL;

	timeline = kzalloc(size, GFP_KERNEL);
	if (!timeline)
		return NULL;

	kref_init(&timeline->kref);
	timeline->context = fence_context_alloc(1);
	strlcpy(timeline->name, name, sizeof(timeline->name));
	strlcpy(timeline->drv_name, drv_name, sizeof(timeline->drv_name));

	INIT_LIST_HEAD(&timeline->child_list_head);
	INIT_LIST_HEAD(&timeline->active_list_head);
	spin_lock_init(&timeline->lock);

	fence_timeline_debug_add(timeline);

	return timeline;
}
EXPORT_SYMBOL(fence_timeline_create);

/**
 * fence_timeline_free - free resources of fence_timeline
 * @kref	[in]	the kref of the fence_timeline to be freed
 *
 * This function frees a fence_timeline which is matter of a simple
 * call to kfree()
 */
static void fence_timeline_free(struct kref *kref)
{
	struct fence_timeline *timeline =
		container_of(kref, struct fence_timeline, kref);

	fence_timeline_debug_remove(timeline);

	kfree(timeline);
}

/**
 * fence_timeline_get - get a reference to the timeline
 * @timeline	[in]	the fence_timeline to get a reference
 *
 * This function increase the refcnt for the given timeline.
 */
void fence_timeline_get(struct fence_timeline *timeline)
{
	kref_get(&timeline->kref);
}
EXPORT_SYMBOL(fence_timeline_get);

/**
 * fence_timeline_put - put a reference to the timeline
 * @timeline	[in]	the fence_timeline to put a reference
 *
 * This function decreases the refcnt for the given timeline
 * and frees it if gets to zero.
 */
void fence_timeline_put(struct fence_timeline *timeline)
{
	kref_put(&timeline->kref, fence_timeline_free);
}
EXPORT_SYMBOL(fence_timeline_put);

/**
 * fence_timeline_destroy - destroy a fence_timeline
 * @timeline	[in]	the fence_timeline to destroy
 *
 * This function destroys a timeline. It signals any active fence first.
 */
void fence_timeline_destroy(struct fence_timeline *timeline)
{
	timeline->destroyed = true;
	/*
	 * Ensure timeline is marked as destroyed before
	 * changing timeline's fences status.
	 */
	smp_wmb();

	/*
	 * signal any children that their parent is going away.
	 */
	fence_timeline_signal(timeline, 0);
	fence_timeline_put(timeline);
}
EXPORT_SYMBOL(fence_timeline_destroy);

/**
 * fence_timeline_signal - signal fences on a fence_timeline
 * @timeline	[in]	the fence_timeline to signal fences
 * @inc		[in[	num to increment on timeline->value
 *
 * This function signal fences on a given timeline and remove
 * those from the active_list.
 */
void fence_timeline_signal(struct fence_timeline *timeline, unsigned int inc)
{
	unsigned long flags;
	LIST_HEAD(signaled_pts);
	struct fence *fence, *next;

	spin_lock_irqsave(&timeline->lock, flags);

	timeline->value += inc;

	list_for_each_entry_safe(fence, next, &timeline->active_list_head,
				 active_list) {
		if (fence_is_signaled_locked(fence))
			list_del_init(&fence->active_list);
	}

	spin_unlock_irqrestore(&timeline->lock, flags);
}
EXPORT_SYMBOL(fence_timeline_signal);

/**
 * fence_signal_locked - signal completion of a fence
 * @fence: the fence to signal
 *
 * Signal completion for software callbacks on a fence, this will unblock
 * fence_wait() calls and run all the callbacks added with
 * fence_add_callback(). Can be called multiple times, but since a fence
 * can only go from unsignaled to signaled state, it will only be effective
 * the first time.
 *
 * Unlike fence_signal, this function must be called with fence->lock held.
 */
int fence_signal_locked(struct fence *fence)
{
	struct fence_cb *cur, *tmp;
	int ret = 0;

	if (WARN_ON(!fence))
		return -EINVAL;

	if (!ktime_to_ns(fence->timestamp)) {
		fence->timestamp = ktime_get();
		smp_mb__before_atomic();
	}

	if (test_and_set_bit(FENCE_FLAG_SIGNALED_BIT, &fence->flags)) {
		ret = -EINVAL;

		/*
		 * we might have raced with the unlocked fence_signal,
		 * still run through all callbacks
		 */
	} else
		trace_fence_signaled(fence);

	list_for_each_entry_safe(cur, tmp, &fence->cb_list, node) {
		list_del_init(&cur->node);
		cur->func(fence, cur);
	}
	return ret;
}
EXPORT_SYMBOL(fence_signal_locked);

/**
 * fence_signal - signal completion of a fence
 * @fence: the fence to signal
 *
 * Signal completion for software callbacks on a fence, this will unblock
 * fence_wait() calls and run all the callbacks added with
 * fence_add_callback(). Can be called multiple times, but since a fence
 * can only go from unsignaled to signaled state, it will only be effective
 * the first time.
 */
int fence_signal(struct fence *fence)
{
	unsigned long flags;

	if (!fence)
		return -EINVAL;

	if (!ktime_to_ns(fence->timestamp)) {
		fence->timestamp = ktime_get();
		smp_mb__before_atomic();
	}

	if (test_and_set_bit(FENCE_FLAG_SIGNALED_BIT, &fence->flags))
		return -EINVAL;

	trace_fence_signaled(fence);

	if (test_bit(FENCE_FLAG_ENABLE_SIGNAL_BIT, &fence->flags)) {
		struct fence_cb *cur, *tmp;

		spin_lock_irqsave(fence->lock, flags);
		list_for_each_entry_safe(cur, tmp, &fence->cb_list, node) {
			list_del_init(&cur->node);
			cur->func(fence, cur);
		}
		spin_unlock_irqrestore(fence->lock, flags);
	}
	return 0;
}
EXPORT_SYMBOL(fence_signal);

/**
 * fence_wait_timeout - sleep until the fence gets signaled
 * or until timeout elapses
 * @fence:	[in]	the fence to wait on
 * @intr:	[in]	if true, do an interruptible wait
 * @timeout:	[in]	timeout value in jiffies, or MAX_SCHEDULE_TIMEOUT
 *
 * Returns -ERESTARTSYS if interrupted, 0 if the wait timed out, or the
 * remaining timeout in jiffies on success. Other error values may be
 * returned on custom implementations.
 *
 * Performs a synchronous wait on this fence. It is assumed the caller
 * directly or indirectly (buf-mgr between reservation and committing)
 * holds a reference to the fence, otherwise the fence might be
 * freed before return, resulting in undefined behavior.
 */
signed long
fence_wait_timeout(struct fence *fence, bool intr, signed long timeout)
{
	signed long ret;

	if (WARN_ON(timeout < 0))
		return -EINVAL;

	if (timeout == 0)
		return fence_is_signaled(fence);

	trace_fence_wait_start(fence);
	ret = fence->ops->wait(fence, intr, timeout);
	trace_fence_wait_end(fence);
	return ret;
}
EXPORT_SYMBOL(fence_wait_timeout);

void fence_release(struct kref *kref)
{
	struct fence *fence =
			container_of(kref, struct fence, refcount);

	trace_fence_destroy(fence);

	BUG_ON(!list_empty(&fence->cb_list));

	if (fence->ops->release)
		fence->ops->release(fence);
	else
		fence_free(fence);
}
EXPORT_SYMBOL(fence_release);

void fence_free(struct fence *fence)
{
	kfree_rcu(fence, rcu);
}
EXPORT_SYMBOL(fence_free);

/**
 * fence_enable_sw_signaling - enable signaling on fence
 * @fence:	[in]	the fence to enable
 *
 * this will request for sw signaling to be enabled, to make the fence
 * complete as soon as possible
 */
void fence_enable_sw_signaling(struct fence *fence)
{
	unsigned long flags;

	if (!test_and_set_bit(FENCE_FLAG_ENABLE_SIGNAL_BIT, &fence->flags) &&
	    !test_bit(FENCE_FLAG_SIGNALED_BIT, &fence->flags)) {
		trace_fence_enable_signal(fence);

		spin_lock_irqsave(fence->lock, flags);

		if (!fence->ops->enable_signaling(fence))
			fence_signal_locked(fence);

		spin_unlock_irqrestore(fence->lock, flags);
	}
}
EXPORT_SYMBOL(fence_enable_sw_signaling);

/**
 * fence_add_callback - add a callback to be called when the fence
 * is signaled
 * @fence:	[in]	the fence to wait on
 * @cb:		[in]	the callback to register
 * @func:	[in]	the function to call
 *
 * cb will be initialized by fence_add_callback, no initialization
 * by the caller is required. Any number of callbacks can be registered
 * to a fence, but a callback can only be registered to one fence at a time.
 *
 * Note that the callback can be called from an atomic context.  If
 * fence is already signaled, this function will return -ENOENT (and
 * *not* call the callback)
 *
 * Add a software callback to the fence. Same restrictions apply to
 * refcount as it does to fence_wait, however the caller doesn't need to
 * keep a refcount to fence afterwards: when software access is enabled,
 * the creator of the fence is required to keep the fence alive until
 * after it signals with fence_signal. The callback itself can be called
 * from irq context.
 *
 */
int fence_add_callback(struct fence *fence, struct fence_cb *cb,
		       fence_func_t func)
{
	unsigned long flags;
	int ret = 0;
	bool was_set;

	if (WARN_ON(!fence || !func))
		return -EINVAL;

	if (test_bit(FENCE_FLAG_SIGNALED_BIT, &fence->flags)) {
		INIT_LIST_HEAD(&cb->node);
		return -ENOENT;
	}

	spin_lock_irqsave(fence->lock, flags);

	was_set = test_and_set_bit(FENCE_FLAG_ENABLE_SIGNAL_BIT, &fence->flags);

	if (test_bit(FENCE_FLAG_SIGNALED_BIT, &fence->flags))
		ret = -ENOENT;
	else if (!was_set) {
		trace_fence_enable_signal(fence);

		if (!fence->ops->enable_signaling(fence)) {
			fence_signal_locked(fence);
			ret = -ENOENT;
		}
	}

	if (!ret) {
		cb->func = func;
		list_add_tail(&cb->node, &fence->cb_list);
	} else
		INIT_LIST_HEAD(&cb->node);
	spin_unlock_irqrestore(fence->lock, flags);

	return ret;
}
EXPORT_SYMBOL(fence_add_callback);

/**
 * fence_remove_callback - remove a callback from the signaling list
 * @fence:	[in]	the fence to wait on
 * @cb:		[in]	the callback to remove
 *
 * Remove a previously queued callback from the fence. This function returns
 * true if the callback is successfully removed, or false if the fence has
 * already been signaled.
 *
 * *WARNING*:
 * Cancelling a callback should only be done if you really know what you're
 * doing, since deadlocks and race conditions could occur all too easily. For
 * this reason, it should only ever be done on hardware lockup recovery,
 * with a reference held to the fence.
 */
bool
fence_remove_callback(struct fence *fence, struct fence_cb *cb)
{
	unsigned long flags;
	bool ret;

	spin_lock_irqsave(fence->lock, flags);

	ret = !list_empty(&cb->node);
	if (ret)
		list_del_init(&cb->node);

	spin_unlock_irqrestore(fence->lock, flags);

	return ret;
}
EXPORT_SYMBOL(fence_remove_callback);

/**
 * fence_default_get_driver_name - default .get_driver_name op
 * @fence:	[in]	the fence to get driver name
 *
 * This function returns the name of the driver that the fence belongs.
 */
const char *fence_default_get_driver_name(struct fence *fence)
{
	struct fence_timeline *parent = fence_parent(fence);

	return parent->drv_name;
}
EXPORT_SYMBOL(fence_default_get_driver_name);

/**
 * fence_default_get_timeline_name - default get_timeline_name op
 * @fence:	[in]	the fence to retrieve timeline name
 *
 * This function returns the name of the timeline which the fence belongs to.
 */
const char *fence_default_get_timeline_name(struct fence *fence)
{
	struct fence_timeline *parent = fence_parent(fence);

	return parent->name;
}
EXPORT_SYMBOL(fence_default_get_timeline_name);

/**
 * fence_default_signaled - default .signaled fence ops
 * @fence:	[in]	the fence to check if signaled or not
 *
 * This functions checks if a fence was signaled or not.
 */
bool fence_default_signaled(struct fence *fence)
{
	struct fence_timeline *timeline = fence_parent(fence);

	return (fence->seqno > timeline->value) ? false : true;
}
EXPORT_SYMBOL(fence_default_signaled);

/**
 * fence_default_enable_signaling - default op for .enable_signaling
 * @fence:	[in]	the fence to enable signaling
 *
 * This function checks if the fence was already signaled and if not
 * adds it to the list of active fences.
 */
bool fence_default_enable_signaling(struct fence *fence)
{
	struct fence_timeline *timeline = fence_parent(fence);

	if (!timeline)
		return false;

	if (fence->ops->signaled && fence->ops->signaled(fence))
		return false;

	list_add_tail(&fence->active_list, &timeline->active_list_head);
	return true;
}
EXPORT_SYMBOL(fence_default_enable_signaling);

struct default_wait_cb {
	struct fence_cb base;
	struct task_struct *task;
};

static void
fence_default_wait_cb(struct fence *fence, struct fence_cb *cb)
{
	struct default_wait_cb *wait =
		container_of(cb, struct default_wait_cb, base);

	wake_up_state(wait->task, TASK_NORMAL);
}

/**
 * fence_default_wait - default sleep until the fence gets signaled
 * or until timeout elapses
 * @fence:	[in]	the fence to wait on
 * @intr:	[in]	if true, do an interruptible wait
 * @timeout:	[in]	timeout value in jiffies, or MAX_SCHEDULE_TIMEOUT
 *
 * Returns -ERESTARTSYS if interrupted, 0 if the wait timed out, or the
 * remaining timeout in jiffies on success.
 */
signed long
fence_default_wait(struct fence *fence, bool intr, signed long timeout)
{
	struct default_wait_cb cb;
	unsigned long flags;
	signed long ret = timeout;
	bool was_set;

	if (test_bit(FENCE_FLAG_SIGNALED_BIT, &fence->flags))
		return timeout;

	spin_lock_irqsave(fence->lock, flags);

	if (intr && signal_pending(current)) {
		ret = -ERESTARTSYS;
		goto out;
	}

	was_set = test_and_set_bit(FENCE_FLAG_ENABLE_SIGNAL_BIT, &fence->flags);

	if (test_bit(FENCE_FLAG_SIGNALED_BIT, &fence->flags))
		goto out;

	if (!was_set) {
		trace_fence_enable_signal(fence);

		if (!fence->ops->enable_signaling(fence)) {
			fence_signal_locked(fence);
			goto out;
		}
	}

	cb.base.func = fence_default_wait_cb;
	cb.task = current;
	list_add(&cb.base.node, &fence->cb_list);

	while (!test_bit(FENCE_FLAG_SIGNALED_BIT, &fence->flags) && ret > 0) {
		if (intr)
			__set_current_state(TASK_INTERRUPTIBLE);
		else
			__set_current_state(TASK_UNINTERRUPTIBLE);
		spin_unlock_irqrestore(fence->lock, flags);

		ret = schedule_timeout(ret);

		spin_lock_irqsave(fence->lock, flags);
		if (ret > 0 && intr && signal_pending(current))
			ret = -ERESTARTSYS;
	}

	if (!list_empty(&cb.base.node))
		list_del(&cb.base.node);
	__set_current_state(TASK_RUNNING);

out:
	spin_unlock_irqrestore(fence->lock, flags);
	return ret;
}
EXPORT_SYMBOL(fence_default_wait);

/**
 * fence_default_release - default .release op
 * @fence:	[in]	the fence to release
 *
 * This function removes the fence from the child_list * and active_list
 * (if it was active) and drops its timeline ref. Finally it frees the
 * fence.
 */
void fence_default_release(struct fence *fence)
{
	struct fence_timeline *timeline = fence_parent(fence);
	unsigned long flags;

	if (!timeline)
		return;

	spin_lock_irqsave(fence->lock, flags);
	list_del(&fence->child_list);
	if (!list_empty(&fence->active_list))
		list_del(&fence->active_list);

	spin_unlock_irqrestore(fence->lock, flags);

	fence_timeline_put(timeline);
	fence_free(fence);
}
EXPORT_SYMBOL(fence_default_release);

/**
 * fence_default_fill_driver_data - fence default .fill_driver_data ops
 * @fence:	[in]	the fence to get the data from
 * @data:	[out]	the data pointer to write the data
 * @size:	[in]	the size of the allocated data
 *
 * This function return a driver data. In the case the fence seqno value.
 * It is used at least by the sw_sync to send fence information to the
 * userspace.
 */
int fence_default_fill_driver_data(struct fence *fence, void *data, int size)
{
	if (size < sizeof(fence->seqno))
		return -ENOMEM;

	memcpy(data, &fence->seqno, sizeof(fence->seqno));

	return sizeof(fence->seqno);
}
EXPORT_SYMBOL(fence_default_fill_driver_data);

/**
 * fence_default_value_str - default .fence_value_str fence ops
 * @fence:	[in]	the fence to get the value from
 * @str:	[out]	the string pointer to write the value
 * @size:	[in]	the size of the allocated string
 *
 * This functions returns a string containing the value of the fence.
 */
void fence_default_value_str(struct fence *fence, char *str, int size)
{
	snprintf(str, size, "%d", fence->seqno);
}
EXPORT_SYMBOL(fence_default_value_str);

/**
 * fence_default_timeline_value_str - default .timeline_value_str fence ops
 * @fence:	[in]	the timeline child fence
 * @str:	[out]	the string pointer to write the value
 * @size:	[in]	the size of the allocated string
 *
 * This functions returns a string containing the value of the last signaled
 * fence in this timeline.
 */
void fence_default_timeline_value_str(struct fence *fence, char *str, int size)
{
	struct fence_timeline *timeline = fence_parent(fence);

	snprintf(str, size, "%d", timeline->value);
}
EXPORT_SYMBOL(fence_default_timeline_value_str);

static bool
fence_test_signaled_any(struct fence **fences, uint32_t count)
{
	int i;

	for (i = 0; i < count; ++i) {
		struct fence *fence = fences[i];
		if (test_bit(FENCE_FLAG_SIGNALED_BIT, &fence->flags))
			return true;
	}
	return false;
}

/**
 * fence_wait_any_timeout - sleep until any fence gets signaled
 * or until timeout elapses
 * @fences:	[in]	array of fences to wait on
 * @count:	[in]	number of fences to wait on
 * @intr:	[in]	if true, do an interruptible wait
 * @timeout:	[in]	timeout value in jiffies, or MAX_SCHEDULE_TIMEOUT
 *
 * Returns -EINVAL on custom fence wait implementation, -ERESTARTSYS if
 * interrupted, 0 if the wait timed out, or the remaining timeout in jiffies
 * on success.
 *
 * Synchronous waits for the first fence in the array to be signaled. The
 * caller needs to hold a reference to all fences in the array, otherwise a
 * fence might be freed before return, resulting in undefined behavior.
 */
signed long
fence_wait_any_timeout(struct fence **fences, uint32_t count,
		       bool intr, signed long timeout)
{
	struct default_wait_cb *cb;
	signed long ret = timeout;
	unsigned i;

	if (WARN_ON(!fences || !count || timeout < 0))
		return -EINVAL;

	if (timeout == 0) {
		for (i = 0; i < count; ++i)
			if (fence_is_signaled(fences[i]))
				return 1;

		return 0;
	}

	cb = kcalloc(count, sizeof(struct default_wait_cb), GFP_KERNEL);
	if (cb == NULL) {
		ret = -ENOMEM;
		goto err_free_cb;
	}

	for (i = 0; i < count; ++i) {
		struct fence *fence = fences[i];

		if (fence->ops->wait != fence_default_wait) {
			ret = -EINVAL;
			goto fence_rm_cb;
		}

		cb[i].task = current;
		if (fence_add_callback(fence, &cb[i].base,
				       fence_default_wait_cb)) {
			/* This fence is already signaled */
			goto fence_rm_cb;
		}
	}

	while (ret > 0) {
		if (intr)
			set_current_state(TASK_INTERRUPTIBLE);
		else
			set_current_state(TASK_UNINTERRUPTIBLE);

		if (fence_test_signaled_any(fences, count))
			break;

		ret = schedule_timeout(ret);

		if (ret > 0 && intr && signal_pending(current))
			ret = -ERESTARTSYS;
	}

	__set_current_state(TASK_RUNNING);

fence_rm_cb:
	while (i-- > 0)
		fence_remove_callback(fences[i], &cb[i].base);

err_free_cb:
	kfree(cb);

	return ret;
}
EXPORT_SYMBOL(fence_wait_any_timeout);

/**
 * fence_create_on_timeline - create a fence and add it to the timeline
 * or until timeout elapses
 * @obj:	[in]	timeline object
 * @ops:	[in]	fence_ops to use
 * @size:	[in]	size to allocate struct fence
 * @value:	[in]	value of this fence
 *
 * This function allocates a new fence and initialize it as a child of the
 * fence_timeline provided. The value received is the seqno used to know
 * when the fence is signaled.
 *
 * Returns NULL if fails to allocate memory or size is too small.
 */
struct fence *fence_create_on_timeline(struct fence_timeline *obj,
				       const struct fence_ops *ops, int size,
				       unsigned int value)
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
	fence_init(fence, ops, &obj->lock, obj->context, value);
	list_add_tail(&fence->child_list, &obj->child_list_head);
	INIT_LIST_HEAD(&fence->active_list);
	spin_unlock_irqrestore(&obj->lock, flags);
	return fence;
}
EXPORT_SYMBOL(fence_create_on_timeline);

/**
 * fence_init - Initialize a custom fence.
 * @fence:	[in]	the fence to initialize
 * @ops:	[in]	the fence_ops for operations on this fence
 * @lock:	[in]	the irqsafe spinlock to use for locking this fence
 * @context:	[in]	the execution context this fence is run on
 * @seqno:	[in]	a linear increasing sequence number for this context
 *
 * Initializes an allocated fence, the caller doesn't have to keep its
 * refcount after committing with this fence, but it will need to hold a
 * refcount again if fence_ops.enable_signaling gets called. This can
 * be used for other implementing other types of fence.
 *
 * context and seqno are used for easy comparison between fences, allowing
 * to check which fence is later by simply using fence_later.
 */
void
fence_init(struct fence *fence, const struct fence_ops *ops,
	     spinlock_t *lock, unsigned context, unsigned seqno)
{
	BUG_ON(!lock);
	BUG_ON(!ops || !ops->wait || !ops->enable_signaling ||
	       !ops->get_driver_name || !ops->get_timeline_name);

	kref_init(&fence->refcount);
	fence->ops = ops;
	INIT_LIST_HEAD(&fence->cb_list);
	fence->lock = lock;
	fence->context = context;
	fence->seqno = seqno;
	fence->flags = 0UL;

	trace_fence_init(fence);
}
EXPORT_SYMBOL(fence_init);
