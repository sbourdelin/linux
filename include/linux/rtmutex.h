/*
 * RT Mutexes: blocking mutual exclusion locks with PI support
 *
 * started by Ingo Molnar and Thomas Gleixner:
 *
 *  Copyright (C) 2004-2006 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *  Copyright (C) 2006, Timesys Corp., Thomas Gleixner <tglx@timesys.com>
 *
 * This file contains the public data structure and API definitions.
 */

#ifndef __LINUX_RT_MUTEX_H
#define __LINUX_RT_MUTEX_H

#include <linux/linkage.h>
#include <linux/rbtree.h>
#include <linux/spinlock_types.h>

extern int max_lock_depth; /* for sysctl */

/**
 * The rt_mutex structure
 *
 * @wait_lock:	spinlock to protect the structure
 * @waiters:	rbtree root to enqueue waiters in priority order
 * @waiters_leftmost: top waiter
 * @owner:	the mutex owner
 */
struct rt_mutex {
	raw_spinlock_t		wait_lock;
	struct rb_root          waiters;
	struct rb_node          *waiters_leftmost;
	struct task_struct	*owner;
#ifdef CONFIG_DEBUG_RT_MUTEXES
	int			save_state;
	const char 		*name, *file;
	int			line;
	void			*magic;
#endif
};

/*
 * This is the control structure for tasks blocked on a rt_mutex,
 * which is allocated on the kernel stack on of the blocked task.
 *
 * @tree_entry:		pi node to enqueue into the mutex waiters tree
 * @pi_tree_entry:	pi node to enqueue into the mutex owner waiters tree
 * @task:		task reference to the blocked task
 */
struct rt_mutex_waiter {
	struct rb_node          tree_entry;
	struct rb_node          pi_tree_entry;
	struct task_struct	*task;
	struct rt_mutex		*lock;
#ifdef CONFIG_DEBUG_RT_MUTEXES
	unsigned long		ip;
	struct pid		*deadlock_task_pid;
	struct rt_mutex		*deadlock_lock;
#endif
	int prio;
	/* Updated under waiter's pi_lock and rt_mutex lock */
	u64 dl_runtime, dl_period;
	/*
	 * Copied directly from above.
	 * Updated under owner's pi_lock, rq lock, and rt_mutex lock.
	 */
	u64 dl_runtime_copy, dl_period_copy;
};

struct hrtimer_sleeper;

#ifdef CONFIG_DEBUG_RT_MUTEXES
 extern int rt_mutex_debug_check_no_locks_freed(const void *from,
						unsigned long len);
 extern void rt_mutex_debug_check_no_locks_held(struct task_struct *task);
#else
 static inline int rt_mutex_debug_check_no_locks_freed(const void *from,
						       unsigned long len)
 {
	return 0;
 }
# define rt_mutex_debug_check_no_locks_held(task)	do { } while (0)
#endif

#ifdef CONFIG_DEBUG_RT_MUTEXES
# define __DEBUG_RT_MUTEX_INITIALIZER(mutexname) \
	, .name = #mutexname, .file = __FILE__, .line = __LINE__
# define rt_mutex_init(mutex)			__rt_mutex_init(mutex, __func__)
 extern void rt_mutex_debug_task_free(struct task_struct *tsk);
#else
# define __DEBUG_RT_MUTEX_INITIALIZER(mutexname)
# define rt_mutex_init(mutex)			__rt_mutex_init(mutex, NULL)
# define rt_mutex_debug_task_free(t)			do { } while (0)
#endif

#define __RT_MUTEX_INITIALIZER(mutexname) \
	{ .wait_lock = __RAW_SPIN_LOCK_UNLOCKED(mutexname.wait_lock) \
	, .waiters = RB_ROOT \
	, .owner = NULL \
	__DEBUG_RT_MUTEX_INITIALIZER(mutexname)}

#define DEFINE_RT_MUTEX(mutexname) \
	struct rt_mutex mutexname = __RT_MUTEX_INITIALIZER(mutexname)

/**
 * rt_mutex_is_locked - is the mutex locked
 * @lock: the mutex to be queried
 *
 * Returns 1 if the mutex is locked, 0 if unlocked.
 */
static inline int rt_mutex_is_locked(struct rt_mutex *lock)
{
	return lock->owner != NULL;
}

extern void __rt_mutex_init(struct rt_mutex *lock, const char *name);
extern void rt_mutex_destroy(struct rt_mutex *lock);

extern void rt_mutex_lock(struct rt_mutex *lock);
extern int rt_mutex_lock_interruptible(struct rt_mutex *lock);
extern int rt_mutex_timed_lock(struct rt_mutex *lock,
			       struct hrtimer_sleeper *timeout);

extern int rt_mutex_trylock(struct rt_mutex *lock);

extern void rt_mutex_unlock(struct rt_mutex *lock);

#endif
