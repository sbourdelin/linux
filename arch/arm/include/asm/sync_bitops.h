#ifndef __ASM_SYNC_BITOPS_H__
#define __ASM_SYNC_BITOPS_H__

#include <asm/bitops.h>

/* sync_bitops functions are equivalent to the SMP implementation of the
 * original functions, independently from CONFIG_SMP being defined.
 *
 * We need them because _set_bit etc are not SMP safe if !CONFIG_SMP. But
 * under Xen you might be communicating with a completely external entity
 * who might be on another CPU (e.g. two uniprocessor guests communicating
 * via event channels and grant tables). So we need a variant of the bit
 * ops which are SMP safe even on a UP kernel.
 */

#define sync_set_bit(nr, p)		_set_bit(nr, p)
#define sync_clear_bit(nr, p)		_clear_bit(nr, p)
#define sync_change_bit(nr, p)		_change_bit(nr, p)
#define sync_test_and_set_bit(nr, p)	_test_and_set_bit(nr, p)
#define sync_test_and_clear_bit(nr, p)	_test_and_clear_bit(nr, p)
#define sync_test_and_change_bit(nr, p)	_test_and_change_bit(nr, p)
#define sync_test_bit(nr, addr)		test_bit(nr, addr)

static inline unsigned long sync_cmpxchg(volatile void *ptr,
										 unsigned long old,
										 unsigned long new)
{
	unsigned long oldval;
	int size = sizeof(*(ptr));

	smp_mb();
	switch (size) {
	case 1:
		oldval = __cmpxchg8(ptr, old, new);
		break;
	case 2:
		oldval = __cmpxchg16(ptr, old, new);
		break;
	default:
		oldval = __cmpxchg(ptr, old, new, size);
		break;
	}
	smp_mb();

	return oldval;
}

#endif
