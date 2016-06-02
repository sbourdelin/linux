#ifndef _ASM_POWERPC_QSPINLOCK_H
#define _ASM_POWERPC_QSPINLOCK_H

#include <asm-generic/qspinlock_types.h>

#define SPIN_THRESHOLD (1 << 15)
#define queued_spin_unlock queued_spin_unlock

static inline void native_queued_spin_unlock(struct qspinlock *lock)
{
	u8 *locked = (u8 *)lock;
#ifdef __BIG_ENDIAN
	locked += 3;
#endif
	/* no load/store can be across the unlock()*/
	smp_store_release(locked, 0);
}

static inline void queued_spin_unlock(struct qspinlock *lock)
{
	native_queued_spin_unlock(lock);
}

#include <asm-generic/qspinlock.h>

#endif /* _ASM_POWERPC_QSPINLOCK_H */
