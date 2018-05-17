#ifndef __ASM_X86_REFCOUNT_H
#define __ASM_X86_REFCOUNT_H
/*
 * x86-specific implementation of refcount_t. Based on PAX_REFCOUNT from
 * PaX/grsecurity.
 */
#include <linux/refcount.h>

/*
 * This is the first portion of the refcount error handling, which lives in
 * .text.unlikely, and is jumped to from the CPU flag check (in the
 * following macros). This saves the refcount value location into CX for
 * the exception handler to use (in mm/extable.c), and then triggers the
 * central refcount exception. The fixup address for the exception points
 * back to the regular execution flow in .text.
 *
 * The logic and data are encapsulated within an assembly macro, which is then
 * called on each use. This hack is necessary to prevent GCC from considering
 * the inline assembly blocks as costly in time and space, which can prevent
 * function inlining and lead to other bad compilation decisions. GCC computes
 * inline assembly cost according to the number of perceived number of assembly
 * instruction, based on the number of new-lines and semicolons in the assembly
 * block. The macros will eventually be compiled into a single instruction that
 * will be actually executed (unless an exception happens). This scheme allows
 * GCC to better understand the inline asm cost.
 */
asm(".macro __REFCOUNT_EXCEPTION counter:req\n\t"
    ".pushsection .text..refcount\n"
    "111:\tlea \\counter, %" _ASM_CX "\n"
    "112:\t" ASM_UD2 "\n\t"
    ASM_UNREACHABLE
    ".popsection\n"
    "113:\n\t"
    _ASM_EXTABLE_REFCOUNT(112b, 113b) "\n\t"
    ".endm");

/* Trigger refcount exception if refcount result is negative. */
asm(".macro __REFCOUNT_CHECK_LT_ZERO counter:req\n\t"
    "js 111f\n\t"
    "__REFCOUNT_EXCEPTION \\counter\n\t"
    ".endm");

/* Trigger refcount exception if refcount result is zero or negative. */
asm(".macro __REFCOUNT_CHECK_LE_ZERO counter:req\n\t"
    "jz 111f\n\t"
    "__REFCOUNT_CHECK_LT_ZERO counter=\\counter\n\t"
    ".endm");

/* Trigger refcount exception unconditionally. */
asm(".macro __REFCOUNT_ERROR counter:req\n\t"
    "jmp 111f\n\t"
    "__REFCOUNT_EXCEPTION counter=\\counter\n\t"
    ".endm");

static __always_inline void refcount_add(unsigned int i, refcount_t *r)
{
	asm volatile(LOCK_PREFIX "addl %1,%0\n\t"
		"__REFCOUNT_CHECK_LT_ZERO %[counter]"
		: [counter] "+m" (r->refs.counter)
		: "ir" (i)
		: "cc", "cx");
}

static __always_inline void refcount_inc(refcount_t *r)
{
	asm volatile(LOCK_PREFIX "incl %0\n\t"
		"__REFCOUNT_CHECK_LT_ZERO %[counter]"
		: [counter] "+m" (r->refs.counter)
		: : "cc", "cx");
}

static __always_inline void refcount_dec(refcount_t *r)
{
	asm volatile(LOCK_PREFIX "decl %0\n\t"
		"__REFCOUNT_CHECK_LE_ZERO %[counter]"
		: [counter] "+m" (r->refs.counter)
		: : "cc", "cx");
}

static __always_inline __must_check
bool refcount_sub_and_test(unsigned int i, refcount_t *r)
{
	GEN_BINARY_SUFFIXED_RMWcc(LOCK_PREFIX "subl",
				  "__REFCOUNT_CHECK_LT_ZERO %[counter]",
				  r->refs.counter, "er", i, "%0", e, "cx");
}

static __always_inline __must_check bool refcount_dec_and_test(refcount_t *r)
{
	GEN_UNARY_SUFFIXED_RMWcc(LOCK_PREFIX "decl",
				 "__REFCOUNT_CHECK_LT_ZERO %[counter]",
				 r->refs.counter, "%0", e, "cx");
}

static __always_inline __must_check
bool refcount_add_not_zero(unsigned int i, refcount_t *r)
{
	int c, result;

	c = atomic_read(&(r->refs));
	do {
		if (unlikely(c == 0))
			return false;

		result = c + i;

		/* Did we try to increment from/to an undesirable state? */
		if (unlikely(c < 0 || c == INT_MAX || result < c)) {
			asm volatile("__REFCOUNT_ERROR %[counter]"
				     : : [counter] "m" (r->refs.counter)
				     : "cc", "cx");
			break;
		}

	} while (!atomic_try_cmpxchg(&(r->refs), &c, result));

	return c != 0;
}

static __always_inline __must_check bool refcount_inc_not_zero(refcount_t *r)
{
	return refcount_add_not_zero(1, r);
}

#endif
