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
 */
#include <linux/sched.h>

/*
 * =========================[ Helper Functions ]=========================
 */

/*
 * Translate the priority of a task to an equivalent RT priority
 */
static u8 rt_task_priority(struct task_struct *task)
{
	int prio = READ_ONCE(task->prio);

	return (prio >= MAX_RT_PRIO) ? 0 : (u8)(MAX_RT_PRIO - prio);
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
static bool rt_spin_trylock(struct qspinlock *lock)
{
	struct __qspinlock *l = (void *)lock;
	u8 prio = rt_task_priority(current);

	BUILD_BUG_ON(_Q_PENDING_BITS != 8);

	if (!prio)
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
			u16 new = (pdprio == prio)
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

		if (pdprio < prio)
			cmpxchg_relaxed(&l->pending, pdprio, prio);
next:
		cpu_relax();
	}
	return true;
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
