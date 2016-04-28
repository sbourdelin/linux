#ifndef _ASM_POWERPC_QSPINLOCK_H
#define _ASM_POWERPC_QSPINLOCK_H

#include <asm-generic/qspinlock_types.h>

#define SPIN_THRESHOLD	(1 << 15)
#define	queued_spin_unlock queued_spin_unlock

static inline void native_queued_spin_unlock(struct qspinlock *lock)
{
	/* no load/store can be across the unlock()*/
	smp_store_release((u8 *)lock, 0);
}

#ifdef CONFIG_PARAVIRT_SPINLOCKS

#include <asm/qspinlock_paravirt.h>

static inline void queued_spin_lock_slowpath(struct qspinlock *lock, u32 val)
{
	pv_queued_spin_lock(lock, val);
}

static inline void queued_spin_unlock(struct qspinlock *lock)
{
	pv_queued_spin_unlock(lock);
}
#else
static inline void queued_spin_unlock(struct qspinlock *lock)
{
	native_queued_spin_unlock(lock);
}
#endif

#include <asm-generic/qspinlock.h>

#endif /* _ASM_POWERPC_QSPINLOCK_H */
