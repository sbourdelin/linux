// SPDX-License-Identifier: GPL-2.0
/*
 * Generic implementation of cmpxchg64().
 * Derived from implementation in arch/sparc/lib/atomic32.c
 * and from locking code implemented in lib/atomic32.c.
 */

#include <linux/cache.h>
#include <linux/export.h>
#include <linux/irqflags.h>
#include <linux/spinlock.h>
#include <linux/types.h>

/*
 * We use a hashed array of spinlocks to provide exclusive access
 * to each variable. Since this is expected to used on systems
 * with small numbers of CPUs (<= 4 or so), we use a relatively
 * small array of 16 spinlocks to avoid wasting too much memory
 * on the spinlock array.
 */
#define NR_LOCKS	16

/* Ensure that each lock is in a separate cacheline */
static union {
	raw_spinlock_t lock;
	char pad[L1_CACHE_BYTES];
} cmpxchg_lock[NR_LOCKS] __cacheline_aligned_in_smp = {
	[0 ... (NR_LOCKS - 1)] = {
		.lock =  __RAW_SPIN_LOCK_UNLOCKED(cmpxchg_lock.lock),
	},
};

static inline raw_spinlock_t *lock_addr(const u64 *v)
{
	unsigned long addr = (unsigned long) v;

	addr >>= L1_CACHE_SHIFT;
	addr ^= (addr >> 8) ^ (addr >> 16);
	return &cmpxchg_lock[addr & (NR_LOCKS - 1)].lock;
}

/*
 * Generic version of __cmpxchg_u64, to be used for cmpxchg64().
 * Takes u64 parameters.
 */
u64 __cmpxchg_u64(u64 *ptr, u64 old, u64 new)
{
	raw_spinlock_t *lock = lock_addr(ptr);
	unsigned long flags;
	u64 prev;

	raw_spin_lock_irqsave(lock, flags);
	prev = READ_ONCE(*ptr);
	if (prev == old)
		*ptr = new;
	raw_spin_unlock_irqrestore(lock, flags);

	return prev;
}
EXPORT_SYMBOL(__cmpxchg_u64);
