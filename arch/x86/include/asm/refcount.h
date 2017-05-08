#ifndef __ASM_X86_REFCOUNT_H
#define __ASM_X86_REFCOUNT_H
/*
 * x86-specific implementation of refcount_t. Ported from PAX_REFCOUNT in
 * PaX/grsecurity before the use of named text sections, and changed to use
 * "jns" instead of "jno" to trap on all signed results, not just when
 * overflowing.
 */
#include <linux/refcount.h>
#include <asm/irq_vectors.h>

#define REFCOUNT_EXCEPTION				\
	"movl $0x7fffffff, %[counter]\n\t"		\
	"int $"__stringify(X86_REFCOUNT_VECTOR)"\n"	\
	"0:\n\t"					\
	_ASM_EXTABLE(0b, 0b)

#define REFCOUNT_CHECK					\
	"jns 0f\n\t"					\
	REFCOUNT_EXCEPTION

static __always_inline void refcount_add(unsigned int i, refcount_t *r)
{
	asm volatile(LOCK_PREFIX "addl %1,%0\n\t"
		REFCOUNT_CHECK
		: [counter] "+m" (r->refs.counter)
		: "ir" (i)
		: "cc", "cx");
}

static __always_inline void refcount_inc(refcount_t *r)
{
	asm volatile(LOCK_PREFIX "incl %0\n\t"
		REFCOUNT_CHECK
		: [counter] "+m" (r->refs.counter)
		: : "cc", "cx");
}

static __always_inline void refcount_dec(refcount_t *r)
{
	asm volatile(LOCK_PREFIX "decl %0\n\t"
		REFCOUNT_CHECK
		: [counter] "+m" (r->refs.counter)
		: : "cc", "cx");
}

static __always_inline __must_check
bool refcount_sub_and_test(unsigned int i, refcount_t *r)
{
	GEN_BINARY_SUFFIXED_RMWcc(LOCK_PREFIX "subl", REFCOUNT_CHECK,
				  r->refs.counter, "er", i, "%0", e);
}

static __always_inline __must_check bool refcount_dec_and_test(refcount_t *r)
{
	GEN_UNARY_SUFFIXED_RMWcc(LOCK_PREFIX "decl", REFCOUNT_CHECK,
				 r->refs.counter, "%0", e);
}

static __always_inline __must_check bool refcount_inc_not_zero(refcount_t *r)
{
	int c;

	c = atomic_read(&(r->refs));
	do {
		if (unlikely(c <= 0))
			break;
	} while (!atomic_try_cmpxchg(&(r->refs), &c, c + 1));

	/* Did we start or finish in an undesirable state? */
	if (unlikely(c <= 0 || c + 1 < 0)) {
		asm volatile(REFCOUNT_EXCEPTION
			: : [counter] "m" (r->refs.counter)
			: "cc", "cx");
	}

	return c != 0;
}

#endif
