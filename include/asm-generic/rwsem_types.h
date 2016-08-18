#ifndef _ASM_GENERIC_RWSEM_TYPES_H
#define _ASM_GENERIC_RWSEM_TYPES_H

#ifdef __KERNEL__

/*
 * the semaphore definition
 *
 * The bias values and the counter type limits the number of
 * potential readers/writers to 32767 for 32 bits and 2147483647
 * for 64 bits.
 */
#ifdef CONFIG_64BIT
# define RWSEM_ACTIVE_MASK		0xffffffffL
#else
# define RWSEM_ACTIVE_MASK		0x0000ffffL
#endif

#define RWSEM_UNLOCKED_VALUE		0x00000000L
#define RWSEM_ACTIVE_BIAS		0x00000001L
#define RWSEM_WAITING_BIAS		(-RWSEM_ACTIVE_MASK-1)
#define RWSEM_ACTIVE_READ_BIAS		RWSEM_ACTIVE_BIAS
#define RWSEM_ACTIVE_WRITE_BIAS		(RWSEM_WAITING_BIAS + RWSEM_ACTIVE_BIAS)

#endif	/* __KERNEL__ */
#endif	/* _ASM_GENERIC_RWSEM_TYPES_H */
