/*
 * String handling functions for PowerPC.
 *
 * Copyright (C) 1996 Paul Mackerras.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#include <asm/ppc_asm.h>
#include <asm/export.h>
#include <asm/cache.h>

	.text
	
/* This clears out any unused part of the destination buffer,
   just as the libc version does.  -- paulus */
_GLOBAL(strncpy)
	PPC_LCMPI 0,r5,0
	beqlr
	mtctr	r5
	addi	r6,r3,-1
	addi	r4,r4,-1
	.balign IFETCH_ALIGN_BYTES
1:	lbzu	r0,1(r4)
	cmpwi	0,r0,0
	stbu	r0,1(r6)
	bdnzf	2,1b		/* dec ctr, branch if ctr != 0 && !cr0.eq */
	bnelr			/* if we didn't hit a null char, we're done */
	mfctr	r5
	PPC_LCMPI 0,r5,0	/* any space left in destination buffer? */
	beqlr			/* we know r0 == 0 here */
2:	stbu	r0,1(r6)	/* clear it out if so */
	bdnz	2b
	blr
EXPORT_SYMBOL(strncpy)

_GLOBAL(strncmp)
	PPC_LCMPI 0,r5,0
	beq-	2f
	mtctr	r5
	addi	r5,r3,-1
	addi	r4,r4,-1
	.balign IFETCH_ALIGN_BYTES
1:	lbzu	r3,1(r5)
	cmpwi	1,r3,0
	lbzu	r0,1(r4)
	subf.	r3,r0,r3
	beqlr	1
	bdnzt	eq,1b
	blr
2:	li	r3,0
	blr
EXPORT_SYMBOL(strncmp)

_GLOBAL(memchr)
	PPC_LCMPI 0,r5,0
	beq-	2f
	mtctr	r5
	addi	r3,r3,-1
	.balign IFETCH_ALIGN_BYTES
1:	lbzu	r0,1(r3)
	cmpw	0,r0,r4
	bdnzf	2,1b
	beqlr
2:	li	r3,0
	blr
EXPORT_SYMBOL(memchr)

/*
 * Algorigthm:
 *
 * 1) Given a word 'x', we can test to see if it contains any 0 bytes
 *    by subtracting 0x01010101, and seeing if any of the high bits of each
 *    byte changed from 0 to 1. This works because the least significant
 *    0 byte must have had no incoming carry (otherwise it's not the least
 *    significant), so it is 0x00 - 0x01 == 0xff. For all other
 *    byte values, either they have the high bit set initially, or when
 *    1 is subtracted you get a value in the range 0x00-0x7f, none of which
 *    have their high bit set. The expression here is
 *    (x - 0x01010101) & ~x & 0x80808080), which gives 0x00000000 when
 *    there were no 0x00 bytes in the word.  You get 0x80 in bytes that
 *    match, but possibly false 0x80 matches in the next more significant
 *    byte to a true match due to carries.  For little-endian this is
 *    of no consequence since the least significant match is the one
 *    we're interested in, but big-endian needs method 2 to find which
 *    byte matches.
 * 2) Given a word 'x', we can test to see _which_ byte was zero by
 *    calculating ~(((x & ~0x80808080) - 0x80808080 - 1) | x | ~0x80808080).
 *    This produces 0x80 in each byte that was zero, and 0x00 in all
 *    the other bytes. The '| ~0x80808080' clears the low 7 bits in each
 *    byte, and the '| x' part ensures that bytes with the high bit set
 *    produce 0x00. The addition will carry into the high bit of each byte
 *    iff that byte had one of its low 7 bits set. We can then just see
 *    which was the most significant bit set and divide by 8 to find how
 *    many to add to the index.
 *    This is from the book 'The PowerPC Compiler Writer's Guide',
 *    by Steve Hoxey, Faraydon Karim, Bill Hay and Hank Warren.
 */

_GLOBAL(strlen)
	andi.   r9, r3, (SZL - 1)
	lis	r7, 0x0101
	addi	r10, r3, -SZL
	addic	r7, r7, 0x0101		/* r7 = 0x01010101 (lomagic) & clr CA */
#ifdef CONFIG_PPC64
	rldimi	r7, r7, 32, 0		/* r7 = 0x0101010101010101 (lomagic) */
#endif
	bne-	1f
2:	PPC_ROTLI	r6, r7, 31 	/* r6 = 0x80808080(80808080) (himagic)*/
	.balign IFETCH_ALIGN_BYTES
3:	PPC_LLU	r9, SZL(r10)
	/* ((x - lomagic) & ~x & himagic) == 0 means no byte in x is NUL */
	subf	r8, r7, r9
	and.	r8, r8, r6
	beq+	3b
	andc.	r8, r8, r9
	beq+	3b
#ifdef CONFIG_CPU_BIG_ENDIAN
	andc	r8, r9, r6
	orc	r9, r9, r6
	subfe	r8, r6, r8
	nor	r8, r8, r9
	PPC_CNTLZL	r8, r8
	subf	r3, r3, r10
	PPC_SRLI	r8, r8, 3
	add	r3, r3, r8
#else
	addi	r9, r8, -1
	addi	r10, r10, (SZL - 1)
	andc	r8, r9, r8
	PPC_CNTLZL	r8, r8
	subf	r3, r3, r10
	PPC_SRLI	r8, r8, 3
	subf	r3, r8, r3
#endif
	blr

1:	lbz	r9, SZL(r10)
	addi	r10, r10, 1
	cmpwi	cr1, r9, 0
	andi.	r9, r10, (SZL - 1)
	beq	cr1, 4f
	bne	1b
	b	2b
4:	addi	r10, r10, (SZL - 1)
	subf	r3, r3, r10
	blr
EXPORT_SYMBOL(strlen)
