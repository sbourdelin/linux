/*
 * Realtime queued spinlocks
 * -------------------------
 *
 * By Waiman Long <longman@redhat.com>
 *
 * This is a variant of queued spinlocks that is designed to meet the
 * requirement of a realtime environment. Tasks with realtime priority will
 * spin on the lock instead of waiting in the queue like the other non-RT
 * tasks. Those RT tasks make use of the pending byte to store the rt_priority
 * of the highest priority task that is currently spinning. That task will
 * then acquire the lock and reset the pending priority if set previously when
 * it becomes free effectively jumping the queue ahead of the other lower
 * priority RT tasks as well as non-RT tasks. The other spinning RT tasks
 * should then bid to set this pending byte to their rt_priority level again.
 *
 * Assuming that the number of RT tasks in a system is limited, the
 * performance overhead of RT tasks spinning on the lock should be small.
 *
 * As RT qspinlock needs the whole pending byte, it cannot be used on kernel
 * configured to support 16K or more CPUs (CONFIG_NR_CPUS).
 *
 * In interrupt context, the priority of the interrupted task is not
 * meaningful. So a fixed static RT priority is used and they won't go into
 * the MCS wait queue.
 *  1) Soft IRQ = 1
 *  2) Hard IRQ = MAX_RT_PRIO
 *  3) NMI	= MAX_RT_PRIO+1
 *
 * The only additional resource that a spinlock holder may need to wait for
 * before completing a lock critical section is another spinlock. The maximum
 * level of spinlock nesting that is currently supported is 2. All those
 * nested spinlock operations are annotated by spin_lock_nested() or its
 * variants. There are currently about 70 instances of those nested spinlock
 * calls in the kernel. These call sites can be modified to pass in the
 * outer lock like what is done in the spin_lock_nest_lock() variant. In
 * doing so, we can query the highest priority task that is waiting on the
 * outer lock and adjust our waiting priority accordingly. To speed up nested
 * spinlock calls, they will have a minimum RT priority of 1 to begin with.
 */
#include <linux/sched.h>

#ifndef MAX
#define MAX(a, b)	(((a) >= (b)) ? (a) : (b))
#endif

/*
 * =========================[ Helper Functions ]=========================
 */

/*
 * Translate the priority of a task to an equivalent RT priority
 */
static u8 rt_task_priority(struct task_struct *task, u8 min_prio)
{
	int prio;

	if (!task)
		return min_prio;

	prio = READ_ONCE(task->prio);
	return (u8)MAX((prio >= MAX_RT_PRIO) ? 0 : MAX_RT_PRIO - prio,
			min_prio);
}

/*
 * Return: true if locked acquired via RT spinning.
 *	   false if need to go into MCS wait queue.
 */
static bool __rt_spin_trylock(struct qspinlock *lock,
			      struct qspinlock *outerlock, u8 min_prio)
{
	struct __qspinlock *l = (void *)lock;
	struct __qspinlock *ol = (void *)outerlock;
	struct task_struct *task = in_interrupt() ? NULL : current;
	u8 prio, mypdprio = 0;

	BUILD_BUG_ON(_Q_PENDING_BITS != 8);

	if (!task)
		min_prio = in_nmi() ? MAX_RT_PRIO + 1
			 : in_irq() ? MAX_RT_PRIO : 1;
	if (!(prio = rt_task_priority(task, min_prio)))
		return false;

	/*
	 * Spin on the lock and try to set its priority into the pending byte.
	 */
	for (;;) {
		u16 lockpend = READ_ONCE(l->locked_pending);
		u8  pdprio = (u8)(lockpend >> _Q_PENDING_OFFSET);

		if (prio < pdprio) {
			/*
			 * Higher priority task present, one more cpu_relax()
			 * before the next attempt.
			 */
			cpu_relax();
			goto next;
		}

		if (!(lockpend & _Q_LOCKED_MASK)) {
			u16 old = lockpend;
			u16 new = (pdprio == mypdprio)
				? _Q_LOCKED_VAL : (lockpend | _Q_LOCKED_VAL);

			/*
			 * Lock is free and priority <= prio, try to acquire
			 * the lock and clear the priority`if it matches my
			 * prio.
			 */
			lockpend = cmpxchg_acquire(&l->locked_pending,
						   old, new);
			if (old == lockpend)
				break;

			pdprio = (u8)(lockpend >> _Q_PENDING_OFFSET);
		}

		if (pdprio < prio) {
			/*
			 * As the RT priority can increase dynamically, we
			 * need to keep track of what priority value has
			 * been set in the pending byte of the lock.
			 */
			if (cmpxchg_relaxed(&l->pending, pdprio, prio)
					== pdprio)
				mypdprio = prio;
		}
next:
		cpu_relax();

		/*
		 * Recompute pending priority
		 */
		prio = MAX(ol ? ol->pending : 0,
			   rt_task_priority(task, min_prio));

	}
	return true;
}

/*
 * ====================[ Functions Used by qspinlock.c ]====================
 */

static inline bool rt_enabled(void)
{
	return true;
}

/*
 * Return the pending byte portion of the integer value of the lock.
 */
static inline int rt_pending(int val)
{
	return val & _Q_PENDING_MASK;
}

/*
 * Return: true if locked acquired
 *	   false if queuing in the MCS wait queue is needed.
 */
static inline bool rt_spin_trylock(struct qspinlock *lock)
{
	return __rt_spin_trylock(lock, NULL, 0);
}

/*
 * We need to make the non-RT tasks wait longer if RT tasks are spinning for
 * the lock. This is done to reduce the chance that a non-RT task may
 * accidently grab the lock away from the RT tasks in the short interval
 * where the pending priority may be reset after an RT task acquires the lock.
 *
 * Return: Current value of the lock.
 */
static u32 rt_wait_head_or_retry(struct qspinlock *lock)
{
	struct __qspinlock *l = (void *)lock;

	for (;;) {
		u16 lockpend = READ_ONCE(l->locked_pending);

		if (!lockpend) {
			lockpend = cmpxchg_acquire(&l->locked_pending, 0,
						   _Q_LOCKED_VAL);
			if (!lockpend)
				break;
		}

		/*
		 * 4 cpu_relax's if RT tasks present.
		 */
		if (lockpend & _Q_PENDING_MASK) {
			cpu_relax();
			cpu_relax();
			cpu_relax();
		}
		cpu_relax();
	}
	return atomic_read(&lock->val);
}

/*
 * =================[ Exported Nested Spinlock Functions ]=================
 */

/*
 * For nested spinlocks, we give it a minimum RT priority of 1. If the
 * outerlock is specified, it will boost its priority if the priority of
 * the highest waiting task in the outer lock is larger than itself.
 */
void __lockfunc _rt_raw_spin_lock_nested(raw_spinlock_t *lock, int subclass,
		raw_spinlock_t *outerlock) __acquires(lock)
{
	preempt_disable();
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	if (subclass) {
		spin_acquire(&lock->dep_map, subclass, 0, _RET_IP_);
	} else {
		type_check(struct lockdep_map *, &outerlock->dep_map);
		spin_acquire_nest(&lock->dep_map, 0, &outerlock->dep_map,
				  _RET_IP_);
	}
#endif
	__acquire(lock);
	__rt_spin_trylock(&lock->raw_lock,
			  outerlock ? &outerlock->raw_lock : NULL, 1);
}
EXPORT_SYMBOL(_rt_raw_spin_lock_nested);

unsigned long __lockfunc _rt_raw_spin_lock_irqsave_nested(raw_spinlock_t *lock,
		int subclass, raw_spinlock_t *outerlock) __acquires(lock)
{
	unsigned long flags;

	local_irq_save(flags);
	_rt_raw_spin_lock_nested(lock, subclass, outerlock);
	return flags;
}
EXPORT_SYMBOL(_rt_raw_spin_lock_irqsave_nested);
