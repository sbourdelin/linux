#ifndef CONFIG_PARAVIRT_SPINLOCKS
#error "do not include this file"
#endif

#ifndef _ASM_QSPINLOCK_PARAVIRT_H
#define _ASM_QSPINLOCK_PARAVIRT_H

#include  <asm/qspinlock_paravirt_types.h>
#include  <linux/jump_label.h>

extern void pv_lock_init(void);
extern void native_queued_spin_lock_slowpath(struct qspinlock *lock, u32 val);
extern void __pv_init_lock_hash(void);
extern void __pv_queued_spin_lock_slowpath(struct qspinlock *lock, u32 val);
extern void __pv_queued_spin_unlock(struct qspinlock *lock);
extern struct static_key_true sharedprocessor_key;

static inline void pv_queued_spin_lock(struct qspinlock *lock, u32 val)
{
	pv_lock_op.lock(lock, val);
}

static inline void pv_queued_spin_unlock(struct qspinlock *lock)
{
	/*
	 * on powerNV and pSeries with jump_label, code will be
	 *	PowerNV:		pSeries:
	 *	nop;			b 2f;
	 *	native unlock		2:
	 *				pv unlock;
	 * In this way, we can do unlock quick in native case.
	 *
	 * IF jump_label is not enabled, we fall back into
	 * if condition, IOW, ld && cmp && bne.
	 */
	if (static_branch_likely(&sharedprocessor_key))
		native_queued_spin_unlock(lock);
	else
		pv_lock_op.unlock(lock);
}

static inline void pv_wait(u8 *ptr, u8 val)
{
	pv_lock_op.wait(ptr, val);
}

static inline void pv_kick(int cpu)
{
	pv_lock_op.kick(cpu);
}

#endif
