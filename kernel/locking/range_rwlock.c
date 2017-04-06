#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/range_rwlock.h>
#include <linux/sched/signal.h>
#include <linux/sched/debug.h>
#include <linux/sched/wake_q.h>
#include <linux/sched.h>
#include <linux/export.h>

#define range_entry(ptr, type, member) container_of(ptr, type, member)

#define range_interval_tree_foreach(node, root, start, last)	\
	for (node = interval_tree_iter_first(root, start, last); \
	     node; node = interval_tree_iter_next(node, start, last))

/*
 * Fastpath range intersection/overlap between A: [a0, a1] and B: [b0, b1]
 * is given by:
 *
 *        a0 <= b1 && b0 <= a1
 *
 * ... where A holds the lock range and B holds the smallest 'start' and
 * largest 'last' in the tree. For the later, we rely on the root node,
 * which by augmented interval tree property, holds the largest value in
 * its last-in-subtree. This allows mitigating some of the tree walk overhead
 * for non-intersecting ranges, maintained and consulted in O(1).
 */
static inline bool
__range_intersects_intree(struct range_rwlock_tree *tree, struct range_rwlock *lock)
{
	struct interval_tree_node *root;

	if (unlikely(RB_EMPTY_ROOT(&tree->root)))
		return false;

	root = range_entry(tree->root.rb_node, struct interval_tree_node, rb);

	return lock->node.start <= root->__subtree_last &&
		tree->leftmost->start <= lock->node.last;
}

static inline void
__range_tree_insert(struct range_rwlock_tree *tree, struct range_rwlock *lock)
{
	if (unlikely(RB_EMPTY_ROOT(&tree->root)) ||
	    lock->node.start < tree->leftmost->start)
		tree->leftmost = &lock->node;

	lock->seqnum = tree->seqnum++;
	interval_tree_insert(&lock->node, &tree->root);
}

static inline void
__range_tree_remove(struct range_rwlock_tree *tree, struct range_rwlock *lock)
{
	if (tree->leftmost == &lock->node) {
		struct rb_node *next = rb_next(&tree->leftmost->rb);
		tree->leftmost = range_entry(next, struct interval_tree_node, rb);
	}

	interval_tree_remove(&lock->node, &tree->root);
}

/*
 * lock->waiter reader tracking.
 */
#define RANGE_FLAG_READER	1UL

static inline struct task_struct *range_lock_waiter(struct range_rwlock *lock)
{
	return (struct task_struct *)
		((unsigned long) lock->waiter & ~RANGE_FLAG_READER);
}

static inline void range_lock_set_reader(struct range_rwlock *lock)
{
	lock->waiter = (struct task_struct *)
		((unsigned long)lock->waiter | RANGE_FLAG_READER);
}

static inline void range_lock_clear_reader(struct range_rwlock *lock)
{
	lock->waiter = (struct task_struct *)
		((unsigned long)lock->waiter & ~RANGE_FLAG_READER);
}

static inline bool range_lock_is_reader(struct range_rwlock *lock)
{
	return (unsigned long) lock->waiter & RANGE_FLAG_READER;
}

static inline void
__range_rwlock_init(struct range_rwlock *lock,
		    unsigned long start, unsigned long last)
{
	WARN_ON(start > last);

	lock->node.start = start;
	lock->node.last = last;
	RB_CLEAR_NODE(&lock->node.rb);
	lock->blocking_ranges = 0;
	lock->waiter = NULL;
	lock->seqnum = 0;
}

/**
 * range_rwlock_init - Initialize the range lock
 * @lock: the range lock to be initialized
 * @start: start of the interval (inclusive)
 * @last: last location in the interval (inclusive)
 *
 * Initialize the range's [start, last] such that it can
 * later be locked. User is expected to enter a sorted
 * range, such that @start <= @last.
 *
 * It is not allowed to initialize an already locked range.
 */
void range_rwlock_init(struct range_rwlock *lock, unsigned long start,
		     unsigned long last)
{
	__range_rwlock_init(lock, start, last);
}
EXPORT_SYMBOL_GPL(range_rwlock_init);

/**
 * range_rwlock_init_full - Initialize a full range lock
 * @lock: the range lock to be initialized
 *
 * Initialize the full range.
 *
 * It is not allowed to initialize an already locked range.
 */
void range_rwlock_init_full(struct range_rwlock *lock)
{
	__range_rwlock_init(lock, 0, RANGE_RWLOCK_FULL);
}
EXPORT_SYMBOL_GPL(range_rwlock_init_full);

static inline void
range_rwlock_unblock(struct range_rwlock *lock, struct wake_q_head *wake_q)
{
	if (!--lock->blocking_ranges)
		wake_q_add(wake_q, range_lock_waiter(lock));
}

static inline int wait_for_ranges(struct range_rwlock_tree *tree,
				  struct range_rwlock *lock, long state)
{
	int ret = 0;

	while (true) {
		set_current_state(state);

		/* do we need to go to sleep? */
		if (!lock->blocking_ranges)
			break;

		if (unlikely(signal_pending_state(state, current))) {
			struct interval_tree_node *node;
			unsigned long flags;
			DEFINE_WAKE_Q(wake_q);

			ret = -EINTR;
			/*
			 * We're not taking the lock after all, cleanup
			 * after ourselves.
			 */
			spin_lock_irqsave(&tree->lock, flags);

			range_lock_clear_reader(lock);
			__range_tree_remove(tree, lock);

			if (!__range_intersects_intree(tree, lock))
				goto unlock;

			range_interval_tree_foreach(node, &tree->root,
						    lock->node.start,
						    lock->node.last) {
				struct range_rwlock *blked;
				blked = range_entry(node,
						    struct range_rwlock, node);

				if (range_lock_is_reader(lock) &&
				    range_lock_is_reader(blked))
					continue;

				/* unaccount for threads _we_ are blocking */
				if (lock->seqnum < blked->seqnum)
					range_rwlock_unblock(blked, &wake_q);
			}

		unlock:
			spin_unlock_irqrestore(&tree->lock, flags);
			wake_up_q(&wake_q);
			break;
		}

		schedule();
	}

	__set_current_state(TASK_RUNNING);
	return ret;
}

static __always_inline int __sched
__range_read_lock_common(struct range_rwlock_tree *tree,
			 struct range_rwlock *lock, long state)
{
	struct interval_tree_node *node;
	unsigned long flags;

	spin_lock_irqsave(&tree->lock, flags);
	range_lock_set_reader(lock);

	if (!__range_intersects_intree(tree, lock))
		goto insert;

	range_interval_tree_foreach(node, &tree->root,
				    lock->node.start, lock->node.last) {
		struct range_rwlock *blocked_lock;
		blocked_lock = range_entry(node, struct range_rwlock, node);

		if (!range_lock_is_reader(blocked_lock))
			lock->blocking_ranges++;
	}
insert:
	__range_tree_insert(tree, lock);

	lock->waiter = current;
	spin_unlock_irqrestore(&tree->lock, flags);

	return wait_for_ranges(tree, lock, state);
}

/**
 * range_read_lock - Lock for reading
 * @tree: interval tree
 * @lock: the range lock to be locked
 *
 * Returns when the lock has been acquired or sleep until
 * until there are no overlapping ranges.
 */
void range_read_lock(struct range_rwlock_tree *tree, struct range_rwlock *lock)
{
	might_sleep();
	__range_read_lock_common(tree, lock, TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL_GPL(range_read_lock);

/**
 * range_read_lock_interruptible - Lock for reading (interruptible)
 * @tree: interval tree
 * @lock: the range lock to be locked
 *
 * Lock the range like range_read_lock(), and return 0 if the
 * lock has been acquired or sleep until until there are no
 * overlapping ranges. If a signal arrives while waiting for the
 * lock then this function returns -EINTR.
 */
int range_read_lock_interruptible(struct range_rwlock_tree *tree,
				  struct range_rwlock *lock)
{
	might_sleep();
	return __range_read_lock_common(tree, lock, TASK_INTERRUPTIBLE);
}
EXPORT_SYMBOL_GPL(range_read_lock_interruptible);

/**
 * range_read_lock_killable - Lock for reading (killable)
 * @tree: interval tree
 * @lock: the range lock to be locked
 *
 * Lock the range like range_read_lock(), and return 0 if the
 * lock has been acquired or sleep until until there are no
 * overlapping ranges. If a signal arrives while waiting for the
 * lock then this function returns -EINTR.
 */
int range_read_lock_killable(struct range_rwlock_tree *tree,
			     struct range_rwlock *lock)
{
	might_sleep();
	return __range_read_lock_common(tree, lock, TASK_KILLABLE);
}
EXPORT_SYMBOL_GPL(range_read_lock_killable);

/**
 * range_read_trylock - Trylock for reading
 * @tree: interval tree
 * @lock: the range lock to be trylocked
 *
 * The trylock is against the range itself, not the @tree->lock.
 *
 * Returns 1 if successful, 0 if contention (must block to acquire).
 */
int range_read_trylock(struct range_rwlock_tree *tree, struct range_rwlock *lock)
{
	int ret = true;
	unsigned long flags;
	struct interval_tree_node *node;

	spin_lock_irqsave(&tree->lock, flags);

	if (!__range_intersects_intree(tree, lock))
		goto insert;

	/*
	 * We have overlapping ranges in the tree, ensure that we can
	 * in fact share the lock.
	 */
	range_interval_tree_foreach(node, &tree->root,
				    lock->node.start, lock->node.last) {
		struct range_rwlock *blocked_lock;
		blocked_lock = range_entry(node, struct range_rwlock, node);

		if (!range_lock_is_reader(blocked_lock)) {
			ret = false;
			goto unlock;
		}
	}
insert:
	range_lock_set_reader(lock);
	__range_tree_insert(tree, lock);
unlock:
	spin_unlock_irqrestore(&tree->lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(range_read_trylock);

/**
 * range_read_unlock - Unlock for reading
 * @tree: interval tree
 * @lock: the range lock to be unlocked
 *
 * Wakes any blocked readers, when @lock is the only conflicting range.
 *
 * It is not allowed to unlock an unacquired read lock.
 */
void range_read_unlock(struct range_rwlock_tree *tree, struct range_rwlock *lock)
{
	struct interval_tree_node *node;
	unsigned long flags;
	DEFINE_WAKE_Q(wake_q);

	spin_lock_irqsave(&tree->lock, flags);

	range_lock_clear_reader(lock);
	__range_tree_remove(tree, lock);

	if (!__range_intersects_intree(tree, lock)) {
		/* nobody to wakeup, we're done */
		spin_unlock_irqrestore(&tree->lock, flags);
		return;
	}

	range_interval_tree_foreach(node, &tree->root,
				    lock->node.start, lock->node.last) {
		struct range_rwlock *blocked_lock;
		blocked_lock = range_entry(node, struct range_rwlock, node);

		if (!range_lock_is_reader(blocked_lock))
			range_rwlock_unblock(blocked_lock, &wake_q);
	}

	spin_unlock_irqrestore(&tree->lock, flags);
	wake_up_q(&wake_q);
}
EXPORT_SYMBOL_GPL(range_read_unlock);

static __always_inline int __sched
__range_write_lock_common(struct range_rwlock_tree *tree,
			  struct range_rwlock *lock, long state)
{
	struct interval_tree_node *node;
	unsigned long flags;

	spin_lock_irqsave(&tree->lock, flags);

	if (!__range_intersects_intree(tree, lock))
		goto insert;

	range_interval_tree_foreach(node, &tree->root,
				    lock->node.start, lock->node.last) {
		/*
		 * As a writer, we always consider an existing node. We
		 * need to block; either the intersecting node is another
		 * writer or we have a reader that needs to finish.
		 */
		lock->blocking_ranges++;
	}
insert:
	__range_tree_insert(tree, lock);

	lock->waiter = current;
	spin_unlock_irqrestore(&tree->lock, flags);

	return wait_for_ranges(tree, lock, state);
}

/**
 * range_write_lock - Lock for writing
 * @tree: interval tree
 * @lock: the range lock to be locked
 *
 * Returns when the lock has been acquired or sleep until
 * until there are no overlapping ranges.
 */
void range_write_lock(struct range_rwlock_tree *tree, struct range_rwlock *lock)
{
	might_sleep();
	__range_write_lock_common(tree, lock, TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL_GPL(range_write_lock);

/**
 * range_write_lock_interruptible - Lock for writing (interruptible)
 * @tree: interval tree
 * @lock: the range lock to be locked
 *
 * Lock the range like range_write_lock(), and return 0 if the
 * lock has been acquired or sleep until until there are no
 * overlapping ranges. If a signal arrives while waiting for the
 * lock then this function returns -EINTR.
 */
int range_write_lock_interruptible(struct range_rwlock_tree *tree,
				   struct range_rwlock *lock)
{
	might_sleep();
	return __range_write_lock_common(tree, lock, TASK_INTERRUPTIBLE);
}
EXPORT_SYMBOL_GPL(range_write_lock_interruptible);

/**
 * range_write_lock_killable - Lock for writing (killable)
 * @tree: interval tree
 * @lock: the range lock to be locked
 *
 * Lock the range like range_write_lock(), and return 0 if the
 * lock has been acquired or sleep until until there are no
 * overlapping ranges. If a signal arrives while waiting for the
 * lock then this function returns -EINTR.
 */
int range_write_lock_killable(struct range_rwlock_tree *tree,
			      struct range_rwlock *lock)
{
	might_sleep();
	return __range_write_lock_common(tree, lock, TASK_KILLABLE);
}
EXPORT_SYMBOL_GPL(range_write_lock_killable);

/**
 * range_write_trylock - Trylock for writing
 * @tree: interval tree
 * @lock: the range lock to be trylocked
 *
 * The trylock is against the range itself, not the @tree->lock.
 *
 * Returns 1 if successful, 0 if contention (must block to acquire).
 */
int range_write_trylock(struct range_rwlock_tree *tree, struct range_rwlock *lock)
{
	int intersects;
	unsigned long flags;

	spin_lock_irqsave(&tree->lock, flags);
	intersects = __range_intersects_intree(tree, lock);

	if (!intersects) {
		range_lock_clear_reader(lock);
		__range_tree_insert(tree, lock);
	}

	spin_unlock_irqrestore(&tree->lock, flags);

	return !intersects;
}
EXPORT_SYMBOL_GPL(range_write_trylock);

/**
 * range_write_unlock - Unlock for writing
 * @tree: interval tree
 * @lock: the range lock to be unlocked
 *
 * Wakes any blocked readers, when @lock is the only conflicting range.
 *
 * It is not allowed to unlock an unacquired write lock.
 */
void range_write_unlock(struct range_rwlock_tree *tree, struct range_rwlock *lock)
{
	struct interval_tree_node *node;
	unsigned long flags;
	DEFINE_WAKE_Q(wake_q);

	spin_lock_irqsave(&tree->lock, flags);

	range_lock_clear_reader(lock);
	__range_tree_remove(tree, lock);

	if (!__range_intersects_intree(tree, lock)) {
		/* nobody to wakeup, we're done */
		spin_unlock_irqrestore(&tree->lock, flags);
		return;
	}

	range_interval_tree_foreach(node, &tree->root,
				    lock->node.start, lock->node.last) {
		struct range_rwlock *blocked_lock;
		blocked_lock = range_entry(node, struct range_rwlock, node);

		range_rwlock_unblock(blocked_lock, &wake_q);
	}

	spin_unlock_irqrestore(&tree->lock, flags);
	wake_up_q(&wake_q);
}
EXPORT_SYMBOL_GPL(range_write_unlock);

/**
 * range_downgrade_write - Downgrade write range lock to read lock
 * @tree: interval tree
 * @lock: the range lock to be downgraded
 *
 * Wakes any blocked readers, when @lock is the only conflicting range.
 *
 * It is not allowed to downgrade an unacquired write lock.
 */
void range_downgrade_write(struct range_rwlock_tree *tree,
			   struct range_rwlock *lock)
{
	unsigned long flags;
	struct interval_tree_node *node;
	DEFINE_WAKE_Q(wake_q);

	spin_lock_irqsave(&tree->lock, flags);

	WARN_ON(range_lock_is_reader(lock));

	range_interval_tree_foreach(node, &tree->root,
				    lock->node.start, lock->node.last) {
		struct range_rwlock *blocked_lock;
		blocked_lock = range_entry(node, struct range_rwlock, node);

		/*
		 * Unaccount for any blocked reader lock. Wakeup if possible.
		 */
		if (range_lock_is_reader(blocked_lock))
			range_rwlock_unblock(blocked_lock, &wake_q);
	}

	range_lock_set_reader(lock);
	spin_unlock_irqrestore(&tree->lock, flags);
	wake_up_q(&wake_q);
}
EXPORT_SYMBOL_GPL(range_downgrade_write);
