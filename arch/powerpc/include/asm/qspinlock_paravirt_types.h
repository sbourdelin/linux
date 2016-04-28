#ifndef _ASM_QSPINLOCK_PARAVIRT_TYPES_H
#define _ASM_QSPINLOCK_PARAVIRT_TYPES_H

struct pv_lock_ops {
	void (*lock)(struct qspinlock *lock, u32 val);
	void (*unlock)(struct qspinlock *lock);
	void (*wait)(u8 *ptr, u8 val, int cpu);
	void (*kick)(int cpu);
};

extern struct pv_lock_ops pv_lock_op;

#endif
