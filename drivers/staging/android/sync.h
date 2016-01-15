/*
 * include/linux/sync.h
 *
 * Copyright (C) 2012 Google, Inc.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef _LINUX_SYNC_H
#define _LINUX_SYNC_H

#include <linux/types.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/fence.h>

#include "uapi/sync.h"

struct sync_fence;

struct sync_fence_cb {
	struct fence_cb cb;
	struct fence *fence;
	struct sync_fence *sync_fence;
};

/**
 * struct sync_fence - sync fence
 * @file:		file representing this fence
 * @kref:		reference count on fence.
 * @name:		name of sync_fence.  Useful for debugging
 * @sync_fence_list:	membership in global fence list
 * @num_fences		number of sync_pts in the fence
 * @wq:			wait queue for fence signaling
 * @status:		0: signaled, >0:active, <0: error
 * @cbs:		sync_pts callback information
 */
struct sync_fence {
	struct file		*file;
	struct kref		kref;
	char			name[32];
#ifdef CONFIG_DEBUG_FS
	struct list_head	sync_fence_list;
#endif
	int num_fences;

	wait_queue_head_t	wq;
	atomic_t		status;

	struct sync_fence_cb	cbs[];
};

struct sync_fence_waiter;
typedef void (*sync_callback_t)(struct sync_fence *fence,
				struct sync_fence_waiter *waiter);

/**
 * struct sync_fence_waiter - metadata for asynchronous waiter on a fence
 * @work:		wait_queue for the fence waiter
 * @callback:		function pointer to call when fence signals
 */
struct sync_fence_waiter {
	wait_queue_t work;
	sync_callback_t callback;
};

static inline void sync_fence_waiter_init(struct sync_fence_waiter *waiter,
					  sync_callback_t callback)
{
	INIT_LIST_HEAD(&waiter->work.task_list);
	waiter->callback = callback;
}

/*
 * API for fence_timeline implementers
 */

struct fence *sync_pt_create(struct fence_timeline *parent, int size);

/**
 * sync_fence_create() - creates a sync fence
 * @name:	name of fence to create
 * @fence:	fence to add to the sync_fence
 *
 * Creates a fence containg @fence.  Once this is called, the fence takes
 * ownership of @fence.
 */
struct sync_fence *sync_fence_create(const char *name, struct fence *fence);

/**
 * sync_fence_create_dma() - creates a sync fence from dma-fence
 * @name:	name of fence to create
 * @pt:	dma-fence to add to the fence
 *
 * Creates a fence containg @pt.  Once this is called, the fence takes
 * ownership of @pt.
 */
struct sync_fence *sync_fence_create_dma(const char *name, struct fence *pt);

/*
 * API for sync_fence consumers
 */

/**
 * sync_fence_merge() - merge two fences
 * @name:	name of new fence
 * @a:		fence a
 * @b:		fence b
 *
 * Creates a new fence which contains copies of all the sync_pts in both
 * @a and @b.  @a and @b remain valid, independent fences. Returns the
 * new merged fence or NULL in case of error.
 */
struct sync_fence *sync_fence_merge(const char *name,
				    struct sync_fence *a, struct sync_fence *b);

/**
 * sync_fence_fdget() - get a fence from an fd
 * @fd:		fd referencing a fence
 *
 * Ensures @fd references a valid fence, increments the refcount of the backing
 * file, and returns the fence. Returns the fence or NULL in case of error.
 */
struct sync_fence *sync_fence_fdget(int fd);

/**
 * sync_fence_put() - puts a reference of a sync fence
 * @fence:	fence to put
 *
 * Puts a reference on @fence.  If this is the last reference, the fence and
 * all it's sync_pts will be freed
 */
void sync_fence_put(struct sync_fence *fence);

/**
 * sync_fence_install() - installs a fence into a file descriptor
 * @fence:	fence to install
 * @fd:		file descriptor in which to install the fence
 *
 * Installs @fence into @fd.  @fd's should be acquired through
 * get_unused_fd_flags(O_CLOEXEC).
 */
void sync_fence_install(struct sync_fence *fence, int fd);

/**
 * sync_fence_wait_async() - registers and async wait on the fence
 * @fence:		fence to wait on
 * @waiter:		waiter callback struck
 *
 * Registers a callback to be called when @fence signals or has an error.
 * @waiter should be initialized with sync_fence_waiter_init().
 *
 * Returns 1 if @fence has already signaled, 0 if not or <0 if error.
 */
int sync_fence_wait_async(struct sync_fence *fence,
			  struct sync_fence_waiter *waiter);

/**
 * sync_fence_cancel_async() - cancels an async wait
 * @fence:		fence to wait on
 * @waiter:		waiter callback struck
 *
 * Cancels a previously registered async wait.  Will fail gracefully if
 * @waiter was never registered or if @fence has already signaled @waiter.
 *
 * Returns 0 if waiter was removed from fence's async waiter list.
 * Returns -ENOENT if waiter was not found on fence's async waiter list.
 */
int sync_fence_cancel_async(struct sync_fence *fence,
			    struct sync_fence_waiter *waiter);

/**
 * sync_fence_wait() - wait on fence
 * @fence:	fence to wait on
 * @tiemout:	timeout in ms
 *
 * Wait for @fence to be signaled or have an error.  Waits indefinitely
 * if @timeout < 0.
 *
 * Returns 0 if fence signaled, > 0 if it is still active and <0 on error
 */
int sync_fence_wait(struct sync_fence *fence, long timeout);

#ifdef CONFIG_DEBUG_FS

void sync_timeline_debug_add(struct fence_timeline *obj);
void sync_timeline_debug_remove(struct fence_timeline *obj);
void sync_fence_debug_add(struct sync_fence *fence);
void sync_fence_debug_remove(struct sync_fence *fence);
void sync_dump(void);

#else
# define sync_timeline_debug_add(obj)
# define sync_timeline_debug_remove(obj)
# define sync_fence_debug_add(fence)
# define sync_fence_debug_remove(fence)
# define sync_dump()
#endif
int sync_fence_wake_up_wq(wait_queue_t *curr, unsigned mode,
			  int wake_flags, void *key);

#endif /* _LINUX_SYNC_H */
