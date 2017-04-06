/*
 * Range/interval rw-locking
 * -------------------------
 *
 * An interval tree of locked and to-be-locked ranges is kept. When a new range
 * lock is requested, we add its interval to the tree and store number of
 * intervals intersecting it to 'blocking_ranges'. For the reader case,
 * 'blocking_ranges' is only accounted for if the intersecting range is
 * marked as a writer. To achieve mutual exclusion of arbitrary ranges, we
 * guarantee that task is blocked until there are no overlapping ranges in the
 * tree.
 *
 * When a range is unlocked, we again walk intervals that overlap with the
 * unlocked one and decrement their 'blocking_ranges'. Naturally, we wake up
 * owner of any range lock whose 'blocking_ranges' drops to 0. Wakeup order
 * therefore relies on the order of the interval tree  -- as opposed to a
 * more traditional fifo mechanism. There is no lock stealing either, which
 * prevents starvation and guarantees fairness.
 *
 * The cost of lock and unlock of a range is O((1+R_int)log(R_all)) where R_all
 * is total number of ranges and R_int is the number of ranges intersecting the
 * operated range.
 */
#ifndef _LINUX_RANGE_RWLOCK_H
#define _LINUX_RANGE_RWLOCK_H

#include <linux/rbtree.h>
#include <linux/interval_tree.h>
#include <linux/list.h>
#include <linux/spinlock.h>

/*
 * The largest range will span [0,RANGE_RWLOCK_FULL].
 */
#define RANGE_RWLOCK_FULL  ~0UL

struct range_rwlock {
	struct interval_tree_node node;
	struct task_struct *waiter;
	/* Number of ranges which are blocking acquisition of the lock */
	unsigned int blocking_ranges;
	u64 seqnum;
};

struct range_rwlock_tree {
	struct rb_root root;
	spinlock_t lock;
	struct interval_tree_node *leftmost; /* compute smallest 'start' */
	u64 seqnum; /* track order of incoming ranges, avoid overflows */
};

#define __RANGE_RWLOCK_TREE_INITIALIZER(name)		\
	{ .leftmost = NULL                              \
	, .root = RB_ROOT				\
	, .seqnum = 0					\
	, .lock = __SPIN_LOCK_UNLOCKED(name.lock) }

#define DEFINE_RANGE_RWLOCK_TREE(name) \
       struct range_rwlock_tree name = __RANGE_RWLOCK_TREE_INITIALIZER(name)

#define __RANGE_RWLOCK_INITIALIZER(__start, __last) {	\
		.node = {			\
			.start = (__start)	\
			,.last = (__last)	\
		}				\
		, .task = NULL			\
		, .blocking_ranges = 0		\
		, .reader = false		\
		, .seqnum = 0			\
	}

#define DEFINE_RANGE_RWLOCK(name, start, last)				\
	struct range_rwlock name = __RANGE_RWLOCK_INITIALIZER((start), (last))

#define DEFINE_RANGE_RWLOCK_FULL(name)		\
	struct range_rwlock name = __RANGE_RWLOCK_INITIALIZER(0, RANGE_RWLOCK_FULL)

static inline void range_rwlock_tree_init(struct range_rwlock_tree *tree)
{
	tree->root = RB_ROOT;
	spin_lock_init(&tree->lock);
	tree->leftmost = NULL;
	tree->seqnum = 0;
}

void range_rwlock_init(struct range_rwlock *lock,
		       unsigned long start, unsigned long last);
void range_rwlock_init_full(struct range_rwlock *lock);

/*
 * lock for reading
 */
void range_read_lock(struct range_rwlock_tree *tree, struct range_rwlock *lock);
int range_read_lock_interruptible(struct range_rwlock_tree *tree,
				  struct range_rwlock *lock);
int range_read_lock_killable(struct range_rwlock_tree *tree,
			     struct range_rwlock *lock);
int range_read_trylock(struct range_rwlock_tree *tree, struct range_rwlock *lock);
void range_read_unlock(struct range_rwlock_tree *tree, struct range_rwlock *lock);

/*
 * lock for writing
 */
void range_write_lock(struct range_rwlock_tree *tree, struct range_rwlock *lock);
int range_write_lock_interruptible(struct range_rwlock_tree *tree,
				   struct range_rwlock *lock);
int range_write_lock_killable(struct range_rwlock_tree *tree,
			      struct range_rwlock *lock);
int range_write_trylock(struct range_rwlock_tree *tree, struct range_rwlock *lock);
void range_write_unlock(struct range_rwlock_tree *tree, struct range_rwlock *lock);

void range_downgrade_write(struct range_rwlock_tree *tree,
			   struct range_rwlock *lock);

#endif
