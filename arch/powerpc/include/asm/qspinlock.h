#ifndef _ASM_POWERPC_QSPINLOCK_H
#define _ASM_POWERPC_QSPINLOCK_H

#include <asm-generic/qspinlock_types.h>

#define SPIN_THRESHOLD (1 << 15)
#define queued_spin_unlock queued_spin_unlock
#define queued_spin_is_locked queued_spin_is_locked
#define queued_spin_unlock_wait queued_spin_unlock_wait

extern void queued_spin_unlock_wait(struct qspinlock *lock);

static inline u8 * __qspinlock_lock_byte(struct qspinlock *lock)
{
	return (u8 *)lock + 3 * IS_BUILTIN(CONFIG_CPU_BIG_ENDIAN);
}

static inline void native_queued_spin_unlock(struct qspinlock *lock)
{
	/* release semantics is required */
	smp_store_release(__qspinlock_lock_byte(lock), 0);
}

static inline int queued_spin_is_locked(struct qspinlock *lock)
{
	smp_mb();
	return atomic_read(&lock->val);
}

#ifdef CONFIG_PARAVIRT_SPINLOCKS
#include <asm/qspinlock_paravirt.h>
/*
 * try to know who is the lock holder, however it is not always true
 * Return:
 * -1, we did not know the lock holder.
 * other value, likely is the lock holder.
 */
extern int spin_lock_holder(void *lock);

static inline void queued_spin_lock_slowpath(struct qspinlock *lock, u32 val)
{
	pv_queued_spin_lock(lock, val);
}

static inline void queued_spin_unlock(struct qspinlock *lock)
{
	pv_queued_spin_unlock(lock);
}
#else
#define spin_lock_holder(l) (-1)
static inline void queued_spin_unlock(struct qspinlock *lock)
{
	native_queued_spin_unlock(lock);
}
#endif

#include <asm-generic/qspinlock.h>

/* we need override it as ppc has io_sync stuff */
#undef arch_spin_trylock
#undef arch_spin_lock
#undef arch_spin_lock_flags
#undef arch_spin_unlock
#define arch_spin_trylock arch_spin_trylock
#define arch_spin_lock arch_spin_lock
#define arch_spin_lock_flags arch_spin_lock_flags
#define arch_spin_unlock arch_spin_unlock

static inline int arch_spin_trylock(arch_spinlock_t *lock)
{
	CLEAR_IO_SYNC;
	return queued_spin_trylock(lock);
}

static inline void arch_spin_lock(arch_spinlock_t *lock)
{
	CLEAR_IO_SYNC;
	queued_spin_lock(lock);
}

static inline
void arch_spin_lock_flags(arch_spinlock_t *lock, unsigned long flags)
{
	CLEAR_IO_SYNC;
	queued_spin_lock(lock);
}

static inline void arch_spin_unlock(arch_spinlock_t *lock)
{
	SYNC_IO;
	queued_spin_unlock(lock);
}
#endif /* _ASM_POWERPC_QSPINLOCK_H */
