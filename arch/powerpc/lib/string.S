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

_GLOBAL(strlen)
	andi.   r9, r3, (SZL - 1)
	addi	r10, r3, -SZL
	bne-	1f
2:	lis	r6, 0x8080
	ori	r6, r6, 0x8080		/* r6 = 0x80808080 (himagic) */
#ifdef CONFIG_PPC64
	rldimi	r6, r6, 32, 0		/* r6 = 0x8080808080808080 (himagic) */
#endif
	PPC_ROTLI  r7, r6, 1 		/* r7 = 0x01010101(01010101) (lomagic)*/
	.balign IFETCH_ALIGN_BYTES
3:	PPC_LLU	r9, SZL(r10)
	/* ((x - lomagic) & ~x & himagic) == 0 means no byte in x is NUL */
	subf	r8, r7, r9
	and.	r8, r8, r6
	beq+	3b
	andc.	r8, r8, r9
	beq+	3b
#ifdef CONFIG_PPC64
	rldicl.	r8, r9, 8, 56
	beq	20f
	rldicl.	r8, r9, 16, 56
	beq	21f
	rldicl.	r8, r9, 24, 56
	beq	22f
	rldicl.	r8, r9, 32, 56
	beq	23f
	addi	r10, r10, 4
#endif
	rlwinm.	r8, r9, 0, 0xff000000
	beq	20f
	rlwinm.	r8, r9, 0, 0x00ff0000
	beq	21f
	rlwinm.	r8, r9, 0, 0x0000ff00
	beq	22f
23:	subf	r3, r3, r10
	addi	r3, r3, 3
	blr
22:	subf	r3, r3, r10
	addi	r3, r3, 2
	blr
21:	subf	r3, r3, r10
	addi	r3, r3, 1
	blr
19:	addi	r10, r10, (SZL - 1)
20:	subf	r3, r3, r10
	blr

1:	lbz	r9, SZL(r10)
	addi	r10, r10, 1
	cmpwi	cr1, r9, 0
	andi.	r9, r10, (SZL - 1)
	beq	cr1, 19b
	bne	1b
	b	2b
EXPORT_SYMBOL(strlen)
