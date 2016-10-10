#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/osq_lock.h>

/*
 * An MCS like lock especially tailored for optimistic spinning for sleeping
 * lock implementations (mutex, rwsem, etc).
 *
 * Using a single mcs node per CPU is safe because sleeping locks should not be
 * called from interrupt context and we have preemption disabled while
 * spinning.
 */
static DEFINE_PER_CPU_SHARED_ALIGNED(struct optimistic_spin_node, osq_node);

/*
 * We use the value 0 to represent "no CPU", thus the encoded value
 * will be the CPU number incremented by 1.
 */
static inline int encode_cpu(int cpu_nr)
{
	return cpu_nr + 1;
}

static inline struct optimistic_spin_node *decode_cpu(int encoded_cpu_val)
{
	int cpu_nr = encoded_cpu_val - 1;

	return per_cpu_ptr(&osq_node, cpu_nr);
}

static inline void set_node_locked_release(struct optimistic_spin_node *node)
{
	smp_store_release(&node->locked, 1);
}

static inline void set_node_locked_relaxed(struct optimistic_spin_node *node)
{
	WRITE_ONCE(node->locked, 1);

}

/*
 * Get a stable @node->next pointer, either for unlock() or unqueue() purposes.
 * Can return NULL in case we were the last queued and we updated @lock instead.
 */
static inline struct optimistic_spin_node *
osq_wait_next(struct optimistic_spin_queue *lock,
	      struct optimistic_spin_node *node,
	      struct optimistic_spin_node *prev)
{
	struct optimistic_spin_node *next = NULL;
	int curr = encode_cpu(smp_processor_id());
	int old;

	/*
	 * If there is a prev node in queue, then the 'old' value will be
	 * the prev node's CPU #, else it's set to OSQ_UNLOCKED_VAL since if
	 * we're currently last in queue, then the queue will then become empty.
	 */
	old = prev ? prev->cpu : OSQ_UNLOCKED_VAL;

	for (;;) {
		if (atomic_read(&lock->tail) == curr &&
		    atomic_cmpxchg_relaxed(&lock->tail, curr, old) == curr) {
			/*
			 * We were the last queued, we moved @lock back. @prev
			 * will now observe @lock and will complete its
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
			next = xchg_relaxed(&node->next, NULL);
			if (next)
				break;
		}

		cpu_relax_lowlatency();
	}

	return next;
}

#define OSQ_LOCK(EXT, FENCECB)						\
bool osq_lock##EXT(struct optimistic_spin_queue *lock)			\
{									\
	struct optimistic_spin_node *node = this_cpu_ptr(&osq_node);	\
	struct optimistic_spin_node *prev, *next;			\
	int old, curr = encode_cpu(smp_processor_id());			\
									\
	node->locked = 0;						\
	node->next = NULL;						\
	node->cpu = curr;						\
									\
	/*								\
	 * At the very least we need RELEASE semantics to initialize	\
	 * the node fields _before_ publishing it to the the lock tail.	\
	 */								\
	old = atomic_xchg_release(&lock->tail, curr);			\
	if (old == OSQ_UNLOCKED_VAL) {					\
		FENCECB;						\
		return true;						\
	}								\
									\
	prev = decode_cpu(old);						\
	node->prev = prev;						\
	WRITE_ONCE(prev->next, node);					\
									\
	/*								\
	 * Normally @prev is untouchable after the above store; because \
	 * at that moment unlock can proceed and wipe the node element  \
	 * from stack.							\
	 *								\
	 * However, since our nodes are static per-cpu storage, we're   \
	 * guaranteed their existence -- this allows us to apply	\
	 * cmpxchg in an attempt to undo our queueing.			\
	 */								\
	while (!READ_ONCE(node->locked)) {				\
		/*							\
		 * If we need to reschedule bail... so we can block.	\
		 */							\
		if (need_resched())					\
			goto unqueue;					\
									\
		cpu_relax_lowlatency();					\
	}								\
	FENCECB;							\
	return true;							\
									\
unqueue:								\
	/*								\
	 * Step - A  -- stabilize @prev					\
	 *								\
	 * Undo our @prev->next assignment; this will make @prev's      \
	 * unlock()/unqueue() wait for a next pointer since @lock	\
	 * points to us (or later).					\
	 */								\
	for (;;) {							\
		/*							\
		 * Failed calls to osq_lock() do not guarantee any	\
		 * ordering, thus always rely on RELAXED semantics.	\
		 * This also applies below, in Step - B.		\
		 */							\
		if (prev->next == node &&				\
		    cmpxchg_relaxed(&prev->next, node, NULL) == node)	\
			break;						\
									\
		/*							\
		 * We can only fail the cmpxchg() racing against an	\
		 * unlock(), in which case we should observe		\
		 * @node->locked becoming true.				\
		 */							\
		if (READ_ONCE(node->locked)) {				\
			FENCECB;					\
			return true;					\
		}							\
									\
		cpu_relax_lowlatency();					\
									\
		/*							\
		 * Or we race against a concurrent unqueue()'s step-B,  \
		 * in which  case its step-C will write us a new	\
		 * @node->prev pointer.					\
		 */							\
		prev = READ_ONCE(node->prev);				\
	}								\
									\
	/*								\
	 * Step - B -- stabilize @next					\
	 *								\
	 * Similar to unlock(), wait for @node->next or move @lock	\
	 * from @node back to @prev.					\
	 */								\
	next = osq_wait_next(lock, node, prev);				\
	if (!next)							\
		return false;						\
									\
	/*								\
	 * Step - C -- unlink						\
	 *								\
	 * @prev is stable because its still waiting for a new		\
	 * @prev->next pointer, @next is stable because our		\
	 * @node->next pointer is NULL and it will wait in Step-A.	\
	 */								\
	WRITE_ONCE(next->prev, prev);					\
	WRITE_ONCE(prev->next, next);					\
									\
	return false;							\
}

OSQ_LOCK(, smp_acquire__after_ctrl_dep())
OSQ_LOCK(_relaxed, )

#define OSQ_UNLOCK(EXT, FENCE, FENCECB)					\
void osq_unlock##EXT(struct optimistic_spin_queue *lock)                \
{									\
	struct optimistic_spin_node *node, *next;			\
	int curr = encode_cpu(smp_processor_id());			\
									\
	/*								\
	 * Fast path for the uncontended case.				\
	 */								\
	if (likely(atomic_cmpxchg_##FENCE(&lock->tail, curr,		\
					  OSQ_UNLOCKED_VAL) == curr))	\
		return;							\
									\
	/*								\
	 * Second most likely case.					\
	 */								\
	node = this_cpu_ptr(&osq_node);					\
	next = xchg(&node->next, NULL);					\
	if (next)							\
		goto done_setlocked;					\
									\
	next = osq_wait_next(lock, node, NULL);				\
	if (!next) {							\
		FENCECB;						\
		return;							\
	}								\
									\
done_setlocked:								\
	set_node_locked_##FENCE(next);					\
}

OSQ_UNLOCK(, release, smp_release__after_ctrl_dep())
OSQ_UNLOCK(_relaxed, relaxed, )
