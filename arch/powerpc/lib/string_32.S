// SPDX-License-Identifier: GPL-2.0

/*
 * String handling functions for PowerPC32
 *
 * Copyright (C) 1996 Paul Mackerras.
 *
 */

#include <asm/processor.h>
#include <asm/errno.h>
#include <asm/ppc_asm.h>
#include <asm/export.h>

	.text

_GLOBAL(memcmp)
	cmpwi	cr0, r5, 0
	beq-	2f
	mtctr	r5
	addi	r6,r3,-1
	addi	r4,r4,-1
1:	lbzu	r3,1(r6)
	lbzu	r0,1(r4)
	subf.	r3,r0,r3
	bdnzt	2,1b
	blr
2:	li	r3,0
	blr
EXPORT_SYMBOL(memcmp)

_GLOBAL(__clear_user)
	addi	r6,r3,-4
	li	r3,0
	li	r5,0
	cmplwi	0,r4,4
	blt	7f
	/* clear a single word */
11:	stwu	r5,4(r6)
	beqlr
	/* clear word sized chunks */
	andi.	r0,r6,3
	add	r4,r0,r4
	subf	r6,r0,r6
	srwi	r0,r4,2
	andi.	r4,r4,3
	mtctr	r0
	bdz	7f
1:	stwu	r5,4(r6)
	bdnz	1b
	/* clear byte sized chunks */
7:	cmpwi	0,r4,0
	beqlr
	mtctr	r4
	addi	r6,r6,3
8:	stbu	r5,1(r6)
	bdnz	8b
	blr
90:	mr	r3,r4
	blr
91:	mfctr	r3
	slwi	r3,r3,2
	add	r3,r3,r4
	blr
92:	mfctr	r3
	blr

	EX_TABLE(11b, 90b)
	EX_TABLE(1b, 91b)
	EX_TABLE(8b, 92b)

EXPORT_SYMBOL(__clear_user)
