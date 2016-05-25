#ifndef _ASM_ARCHHASH_H
#define _ASM_ARCHHASH_H

/*
 * The only 68k processors that lack MULU.L and so need this workaround
 * are the original 68000 and 68010.
 */
#if defined(CONFIG_M68000) || defined(CONFIG_M68010)

#define HAVE_ARCH__HASH_32 1
/*
 * While it would be legal to substitute a different hash operation
 * entirely, let's keep it simple and just use an optimized multiply
 * by GOLDEN_RATIO_32 = 0x61C88647.
 *
 * The best way to do that appears to be to multiply by 0x8647 with
 * shifts and adds, and use mulu.w to multiply the high half by 0x61C8.
 *
 * Because the 68000 has multi-cycle shifts, this addition chain is
 * chosen to minimise the shift distances.
 *
 * Despite every attempt to spoon-feed GCC simple operations, GCC 6.1.1
 * doggedly insists on doing annoying things like converting "lsl.l #2,<reg>"
 * (12 cycles) to two adds (8+8 cycles).
 *
 * It also likes to notice two shifts in a row, like "a = x << 2" and
 * "a <<= 7", and convert that to "a = x << 9".  But shifts longer than
 * 8 bits are extra-slow on m68k, so that's a lose.
 *
 * Since the 68000 is a very simple in-order processor with no instruction
 * scheduling effects on execution time, we can safely take it out of GCC's
 * hands and write one big asm() block.
 *
 * Without calling overhead, this operation is 30 bytes (14 instructions
 * plus one immediate constant) and 166 cycles.
 */
static inline u32 __attribute_const__ __hash_32(u32 x)
{
	u32 a, b;

	asm(   "move.l %2,%0"	/* 0x0001 */
	"\n	lsl.l #2,%0"	/* 0x0004 */
	"\n	move.l %0,%1"
	"\n	lsl.l #7,%0"	/* 0x0200 */
	"\n	add.l %2,%0"	/* 0x0201 */
	"\n	add.l %0,%1"	/* 0x0205 */
	"\n	add.l %0,%0"	/* 0x0402 */
	"\n	add.l %0,%1"	/* 0x0607 */
	"\n	lsl.l #5,%0"	/* 0x8040 */
				/* 0x8647 */
	: "=&d" (a), "=&r" (b)
	: "g" (x));

	return ((u16)(x*0x61c8) << 16) + a + b;
}
#endif	/* HAVE_ARCH__HASH_32 */

#endif	/* _ASM_ARCHHASH_H */
