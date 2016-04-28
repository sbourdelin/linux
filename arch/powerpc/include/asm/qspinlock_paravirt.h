#ifndef CONFIG_PARAVIRT_SPINLOCKS
#error "do not include this file"
#endif

#ifndef _ASM_QSPINLOCK_PARAVIRT_H
#define _ASM_QSPINLOCK_PARAVIRT_H

#include  <asm/qspinlock_paravirt_types.h>

extern void pv_lock_init(void);
extern void native_queued_spin_lock_slowpath(struct qspinlock *lock, u32 val);
extern void __pv_init_lock_hash(void);
extern void __pv_queued_spin_lock_slowpath(struct qspinlock *lock, u32 val);
extern void __pv_queued_spin_unlock(struct qspinlock *lock);

static inline void pv_queued_spin_lock(struct qspinlock *lock, u32 val)
{
	pv_lock_op.lock(lock, val);
}

static inline void pv_queued_spin_unlock(struct qspinlock *lock)
{
	pv_lock_op.unlock(lock);
}

static inline void pv_wait(u8 *ptr, u8 val)
{
	pv_lock_op.wait(ptr, val, -1);
}

static inline void pv_kick(int cpu)
{
	pv_lock_op.kick(cpu);
}

#endif
