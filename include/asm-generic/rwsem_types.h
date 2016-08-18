#ifndef _ASM_GENERIC_RWSEM_TYPES_H
#define _ASM_GENERIC_RWSEM_TYPES_H

#ifdef __KERNEL__

/*
 * the semaphore definition
 *
 * The bias values and the counter type limits the number of
 * potential writers to 16383 for 32 bits and 1073741823 for 64 bits.
 * The combined readers and writers can go up to 65534 for 32-bits and
 * 4294967294 for 64-bits.
 */
#ifdef CONFIG_64BIT
# define RWSEM_ACTIVE_MASK		0xffffffffL
# define RWSEM_WAITING_BIAS		(-(1L << 62))
#else
# define RWSEM_ACTIVE_MASK		0x0000ffffL
# define RWSEM_WAITING_BIAS		(-(1L << 30))
#endif

#define RWSEM_UNLOCKED_VALUE		0x00000000L
#define RWSEM_ACTIVE_BIAS		0x00000001L
#define RWSEM_ACTIVE_READ_BIAS		RWSEM_ACTIVE_BIAS
#define RWSEM_ACTIVE_WRITE_BIAS		(-RWSEM_ACTIVE_MASK)

#endif	/* __KERNEL__ */
#endif	/* _ASM_GENERIC_RWSEM_TYPES_H */
