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
 *
 * To handle priority boosting due to an acquired rt-mutex, The task->prio
 * field is queried in each iteration of the loop. For originally non-RT tasks,
 * it will have to break out of the MCS wait queue just like what is done
 * in the OSQ lock. Then it has to retry RT spinning if it has been boosted
 * to RT priority.
 *
 * Another RT requirement is that the CPU need to be preemptible even when
 * waiting for a spinlock. If the task has already acquired the lock, we
 * will let it run to completion to release the lock and reenable preemption.
 * For non-nested spinlock, a spinlock waiter will periodically check
 * need_resched flag to see if it should break out of the waiting loop and
 * yield the CPU as long as the preemption count indicates just one
 * preempt_disabled(). For nested spinlock with outer lock acquired, it will
 * boost its priority to the highest RT priority level to try to acquire the
 * inner lock, finish up its work, release the locks and reenable preemption.
 */
#include <linux/sched.h>
#include "qspinlock_stat.h"

#ifndef MAX
#define MAX(a, b)	(((a) >= (b)) ? (a) : (b))
#endif

/*
 * Rescheduling is only needed when it is in the task context, the
 * PREEMPT_NEED_RESCHED flag is set and the preemption count is one.
 * If only the TIF_NEED_RESCHED flag is set, it will be moved to RT
 * spinning with a minimum priority of 1.
 */
#define rt_should_resched()	(preempt_count() == \
				(PREEMPT_OFFSET | PREEMPT_NEED_RESCHED))

/*
 * For proper unqueuing from the MCS wait queue, we need to store the encoded
 * tail code as well the previous node pointer into the extra MCS node. Since
 * CPUs in interrupt context won't use the per-CPU MCS nodes anymore. So only
 * one is needed for process context CPUs. As a result, we can use the
 * additional nodes for data storage. Here, we allow 2 nodes per cpu in case
 * we want to put softIRQ CPUs into the queue as well.
 */
struct rt_node {
	struct mcs_spinlock	mcs;
	struct mcs_spinlock	__reserved;
	struct mcs_spinlock	*prev;
	u32			tail;
};

/*
 * =========================[ Helper Functions ]=========================
 */

static u32 cmpxchg_tail_acquire(struct qspinlock *lock, u32 old, u32 new)
{
	struct __qspinlock *l = (void *)lock;

	return cmpxchg_acquire(&l->tail, old >> _Q_TAIL_OFFSET,
			       new >> _Q_TAIL_OFFSET) << _Q_TAIL_OFFSET;
}

static u32 cmpxchg_tail_release(struct qspinlock *lock, u32 old, u32 new)
{
	struct __qspinlock *l = (void *)lock;

	return cmpxchg_release(&l->tail, old >> _Q_TAIL_OFFSET,
			       new >> _Q_TAIL_OFFSET) << _Q_TAIL_OFFSET;
}

static inline void rt_write_prev(struct mcs_spinlock *node,
				 struct mcs_spinlock *prev)
{
	WRITE_ONCE(((struct rt_node *)node)->prev, prev);
}

static inline u32 rt_read_tail(struct mcs_spinlock *node)
{
	return READ_ONCE(((struct rt_node *)node)->tail);
}

static inline struct mcs_spinlock *rt_read_prev(struct mcs_spinlock *node)
{
	return READ_ONCE(((struct rt_node *)node)->prev);
}

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
	else if (need_resched() && !min_prio)
		min_prio = 1;
	if (!(prio = rt_task_priority(task, min_prio)))
		return false;

	qstat_inc_either(qstat_rt_spin_task, qstat_rt_spin_irq, task);

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

		/*
		 * If another task needs this CPU, we will yield it if in
		 * the process context and it is not a nested spinlock call.
		 * Otherwise, we will raise our RT priority to try to get
		 * the lock ASAP.
		 */
		if (!task || !rt_should_resched())
			continue;

		if (outerlock) {
			if (min_prio < MAX_RT_PRIO)
				min_prio = MAX_RT_PRIO;
			continue;
		}

		/*
		 * In the unlikely event that we need to relinquish the CPU,
		 * we need to make sure that we are not the highest priority
		 * task waiting for the lock.
		 */
		if (mypdprio) {
			lockpend = READ_ONCE(l->locked_pending);
			pdprio = (u8)(lockpend >> _Q_PENDING_OFFSET);
			if (pdprio == mypdprio)
				cmpxchg_relaxed(&l->pending, pdprio, 0);
		}
		qstat_inc(qstat_rt_resched, true);
		schedule_preempt_disabled();
	}
	return true;
}

/*
 * MCS wait queue unqueuing code, borrow mostly from osq_lock.c.
 */
static struct mcs_spinlock *
mcsq_wait_next(struct qspinlock *lock, struct mcs_spinlock *node,
	       struct mcs_spinlock *prev)
{
	 struct mcs_spinlock *next = NULL;
	 u32 tail = rt_read_tail(node);
	 u32 old;

	/*
	 * If there is a prev node in queue, the 'old' value will be
	 * the prev node's tail value. Otherwise, it's set to 0 since if
	 * we're the only one in queue, the queue will then become empty.
	 */
	old = prev ? rt_read_tail(prev) : 0;

	for (;;) {
		if ((atomic_read(&lock->val) & _Q_TAIL_MASK) == tail &&
		    cmpxchg_tail_acquire(lock, tail, old) == tail) {
			/*
			 * We are at the queue tail, we moved the @lock back.
			 * @prev will now observe @lock and will complete its
			 * unlock()/unqueue().
			 */
			break;
		}

		/*
		 * We must xchg() the @node->next value, because if we were to
		 * leave it in, a concurrent unlock()/unqueue() from
		 * @node->next might complete Step-A and think its @prev is
		 * still valid.
		 *
		 * If the concurrent unlock()/unqueue() wins the race, we'll
		 * wait for either @lock to point to us, through its Step-B, or
		 * wait for a new @node->next from its Step-C.
		 */
		if (node->next) {
			next = xchg(&node->next, NULL);
			if (next)
				break;
		}

		cpu_relax();
	}
	return next;
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
 * Initialize the RT fields of a MCS node.
 */
static inline void rt_init_node(struct mcs_spinlock *node, u32 tail)
{
	struct rt_node *n = (struct rt_node *)node;

	n->prev = NULL;
	n->tail = tail;
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
 * Return: true if it has been unqueued and need to retry locking.
 *	   false if it becomes the wait queue head & proceed to next step.
 */
static bool rt_wait_node_or_unqueue(struct qspinlock *lock,
				    struct mcs_spinlock *node,
				    struct mcs_spinlock *prev)
{
	struct mcs_spinlock *next;

	rt_write_prev(node, prev);	/* Save previous node pointer */

	while (!READ_ONCE(node->locked)) {
		if (rt_task_priority(current, 0) || need_resched())
			goto unqueue;
		cpu_relax();
	}
	return false;

unqueue:
	qstat_inc_either(qstat_rt_unqueue_sched, qstat_rt_unqueue_prio,
			 need_resched());

	/*
	 * Step - A  -- stabilize @prev
	 *
	 * Undo our @prev->next assignment; this will make @prev's
	 * unlock()/unqueue() wait for a next pointer since @lock points
	 * to us (or later).
	 */
	for (;;) {
		if (prev->next == node &&
		    cmpxchg(&prev->next, node, NULL) == node)
			break;

		/*
		 * We can only fail the cmpxchg() racing against an unlock(),
		 * in which case we should observe @node->locked becoming
		 * true.
		 */
		if (smp_load_acquire(&node->locked))
			return false;

		cpu_relax();

		/*
		 * Or we race against a concurrent unqueue()'s step-B, in which
		 * case its step-C will write us a new @node->prev pointer.
		 */
		prev = rt_read_prev(node);
	}

	/*
	 * Step - B -- stabilize @next
	 *
	 * Similar to unlock(), wait for @node->next or move @lock from @node
	 * back to @prev.
	 */
	next = mcsq_wait_next(lock, node, prev);

	/*
	 * Step - C -- unlink
	 *
	 * @prev is stable because its still waiting for a new @prev->next
	 * pointer, @next is stable because our @node->next pointer is NULL and
	 * it will wait in Step-A.
	 */
	if (next) {
		rt_write_prev(next, prev);
		WRITE_ONCE(prev->next, next);
	}

	/*
	 * Release the node.
	 */
	__this_cpu_dec(mcs_nodes[0].count);

	/*
	 * Yield the CPU if needed by another task with the right condition.
	 */
	if (rt_should_resched()) {
		qstat_inc(qstat_rt_resched, true);
		schedule_preempt_disabled();
	}

	return true;	/* Need to retry RT spinning */
}

/*
 * We need to make the non-RT tasks wait longer if RT tasks are spinning for
 * the lock. This is done to reduce the chance that a non-RT task may
 * accidently grab the lock away from the RT tasks in the short interval
 * where the pending priority may be reset after an RT task acquires the lock.
 *
 * Return: RT_RETRY if it needs to retry locking.
 *	   1 if lock acquired.
 */
static u32 rt_spin_lock_or_retry(struct qspinlock *lock,
				 struct mcs_spinlock *node)
{
	struct __qspinlock *l = (void *)lock;
	struct mcs_spinlock *next;
	bool retry = false;
	u32 tail;

	for (;;) {
		u16 lockpend = READ_ONCE(l->locked_pending);

		if (!lockpend) {
			lockpend = cmpxchg_acquire(&l->locked_pending, 0,
						   _Q_LOCKED_VAL);
			if (!lockpend)
				break;
		}
		/*
		 * We need to break out of the non-RT wait queue and do
		 * RT spinnning if we become an RT task or another task needs
		 * the CPU.
		 */
		if (rt_task_priority(current, 0) || need_resched()) {
			retry = true;
			goto unlock;
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

unlock:
	/*
	 * Remove itself from the MCS wait queue (unlock).
	 */
	tail = rt_read_tail(node);
	if (cmpxchg_tail_release(lock, tail, 0) == tail)
		goto release;

	/*
	 * Second case.
	 */
	next = xchg(&node->next, NULL);
	if (!next)
		next = mcsq_wait_next(lock, node, NULL);

	if (next)
		WRITE_ONCE(next->locked, 1);

release:
	/*
	 * Release the node.
	 */
	__this_cpu_dec(mcs_nodes[0].count);

	/*
	 * Yield the CPU if needed by another task with the right condition.
	 */
	if (retry && rt_should_resched()) {
		qstat_inc(qstat_rt_resched, true);
		schedule_preempt_disabled();
	}

	return retry ? RT_RETRY : 1;
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
	qstat_inc(qstat_rt_spin_nest, true);
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
