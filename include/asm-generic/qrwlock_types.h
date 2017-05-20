#ifndef __ASM_GENERIC_QRWLOCK_TYPES_H
#define __ASM_GENERIC_QRWLOCK_TYPES_H

/*
 * The queue read/write lock data structure
 */

typedef struct qrwlock {
	atomic_t		cnts;
	arch_spinlock_t		wait_lock;
} arch_rwlock_t;

#define	__ARCH_RW_LOCK_UNLOCKED {		\
	.cnts = ATOMIC_INIT(0),			\
	.wait_lock = __ARCH_SPIN_LOCK_UNLOCKED,	\
}

#include <linux/types.h>
#include <linux/spinlock_types.h>

#endif /* __ASM_GENERIC_QRWLOCK_TYPES_H */
