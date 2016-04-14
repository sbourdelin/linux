#ifndef __ASM_MCS_SPINLOCK_H
#define __ASM_MCS_SPINLOCK_H

#define arch_mcs_spin_lock_contended(l)					\
do {									\
	int locked_val;							\
	for (;;) {							\
		locked_val = READ_ONCE(*l);				\
		if (locked_val)						\
			break;						\
		cmpwait(l, locked_val);					\
	}								\
	smp_rmb();							\
} while (0)

#define arch_mcs_spin_unlock_contended(l)				\
do {									\
	smp_store_release(l, 1);					\
} while (0)

#endif  /* __ASM_MCS_SPINLOCK_H */
