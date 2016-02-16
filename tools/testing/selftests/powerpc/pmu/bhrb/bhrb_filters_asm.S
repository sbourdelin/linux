/*
 * Assembly functions for BHRB test
 *
 * Copyright (C) 2015 Anshuman Khandual, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */

#include <ppc-asm.h>

#define LR_SAVE r19

#define LOAD_ADDR(reg, label)			\
	lis     reg,(label)@highest;		\
	ori     reg,reg,(label)@higher;		\
	rldicr  reg,reg,32,31;			\
	oris    reg,reg,(label)@h;		\
	ori     reg,reg,(label)@l;		\

#define LOAD_LABEL(reg1, label1, reg2, label2)	\
	lis     reg1,(label1)@highest;		\
	ori     reg1,reg1,(label1)@higher;	\
	rldicr  reg1,reg1,32,31;		\
	oris    reg1,reg1,(label1)@h;		\
	ori     reg1,reg1,(label1)@l;		\
	lis     reg2,(label2)@highest;		\
	ori     reg2,reg2,(label2)@higher;	\
	rldicr  reg2,reg2,32,31;		\
	oris    reg2,reg2,(label2)@h;		\
	ori     reg2,reg2,(label2)@l;		\

FUNC_START(start_loop)
label:
	b label0			/* ANY */
	blr				/* ANY_RETURN */
label0:
	b label1			/* ANY */

label1:
	b label2			/* ANY */

label2:
	b label3			/* ANY */

label3:
	mflr LR_SAVE
	bl label4			/* ANY | ANY_CALL */
	mtlr LR_SAVE
	b start_loop			/* ANY */
label4:
	mflr LR_SAVE
	li r20, 12
	cmpi r3, r20, 12
	bcl 12, 4 * cr3+2, label5	/* ANY | ANY_CALL | COND */
	li r20, 12
	cmpi r4, r20, 20
	bcl 12, 4 * cr4+0, label5	/* ANY | ANY_CALL | COND */
	LOAD_ADDR(r20, label5)
	mtctr r20
	li r22, 10
	cmpi r0, r22, 10
	bcctrl 12, 4*cr0+2		/* ANY | NY_CALL | IND_CALL | COND */
	LOAD_ADDR(r20, label5)
	mtlr r20
	li      r20, 10
	cmpi    r0, r20, 10
	bclrl   12, 4*cr0+2		/* ANY | ANY_CALL | IND_CALL | COND */
	mtlr LR_SAVE
	blr				/* ANY | ANY_RETURN */

label5:
	blr				/* ANY | ANY_RETURN */
FUNC_END(start_loop)

/* Volatile Registers */
#define REG1	r4
#define REG2	r5

FUNC_START(fetch_all_branches)
	LOAD_LABEL(REG1, label, REG2, label0)
	st REG1, 0(r3)
	st REG2, 4(r3)

	LOAD_LABEL(REG1, label0, REG2, label1)
	st REG1, 8(r3)
	st REG2, 12(r3)

	LOAD_LABEL(REG1, label1, REG2, label2)
	st REG1, 16(r3)
	st REG2, 20(r3)

	LOAD_LABEL(REG1, label2, REG2, label3)
	st REG1, 24(r3)
	st REG2, 28(r3)

	LOAD_LABEL(REG1, label3, REG2, label4)
	addi REG1, REG1, 4
	st REG1, 32(r3)
	st REG2, 36(r3)

	LOAD_LABEL(REG1, label3, REG2, label)
	addi REG1, REG1, 12
	st REG1, 40(r3)
	st REG2, 44(r3)

	LOAD_LABEL(REG1, label4, REG2, label5)
	addi REG1, REG1, 12
	st REG1, 48(r3)
	st REG2, 52(r3)

	LOAD_LABEL(REG1, label4, REG2, label5)
	addi REG1, REG1, 24
	st REG1, 56(r3)
	st REG2, 60(r3)

	LOAD_LABEL(REG1, label4, REG2, label5)
	addi REG1, REG1, 60
	st REG1, 64(r3)
	st REG2, 68(r3)

	LOAD_LABEL(REG1, label4, REG2, label5)
	addi REG1, REG1, 96
	st REG1, 72(r3)
	st REG2, 76(r3)

	LOAD_LABEL(REG1, label4, REG2, label5)
	addi REG1, REG1, 104
	st REG1, 80(r3)
	st REG2, 84(r3)

	LOAD_LABEL(REG1, label5, REG2, label4)
	st REG1, 88(r3)
	addi REG2, REG2, 16
	st REG2, 92(r3)

	LOAD_LABEL(REG1, label5, REG2, label4)
	st REG1, 96(r3)
	addi REG2, REG2, 28
	st REG2, 100(r3)

	LOAD_LABEL(REG1, label5, REG2, label4)
	st REG1, 104(r3)
	addi REG2, REG2, 64
	st REG2, 108(r3)

	LOAD_LABEL(REG1, label5, REG2, label4)
	st REG1, 112(r3)
	addi REG2, REG2, 100
	st REG2, 116(r3)

	LOAD_LABEL(REG1, label4, REG2, label3)
	addi REG1, REG1, 104
	st REG1, 120(r3)
	addi REG2, REG2, 8
	st REG2, 124(r3)
	blr
FUNC_END(fetch_all_branches)

FUNC_START(fetch_all_calls)
	LOAD_LABEL(REG1, label3, REG2, label4)
	addi REG1, REG1, 4
	st REG1, 0(r3)
	st REG2, 4(r3)

	LOAD_LABEL(REG1, label3, REG2, label4)
	addi REG1, REG1, 4
	st REG1, 8(r3)
	st REG2, 12(r3)

	LOAD_LABEL(REG1, label4, REG2, label5)
	addi REG1, REG1, 12
	st REG1, 16(r3)
	st REG2, 20(r3)

	LOAD_LABEL(REG1, label4, REG2, label5)
	addi REG1, REG1, 24
	st REG1, 24(r3)
	st REG2, 28(r3)

	LOAD_LABEL(REG1, label4, REG2, label5)
	addi REG1, REG1, 60
	st REG1, 32(r3)
	st REG2, 36(r3)

	LOAD_LABEL(REG1, label4, REG2, label5)
	addi REG1, REG1, 96
	st REG1, 40(r3)
	st REG2, 44(r3)
	blr
FUNC_END(fetch_all_calls)

FUNC_START(fetch_all_rets)
	LOAD_LABEL(REG1, label5, REG2, label4)
	st REG1, 0(r3)
	addi REG2, REG2, 16
	st REG2, 4(r3)

	LOAD_LABEL(REG1, label5, REG2, label4)
	st REG1, 8(r3)
	addi REG2, REG2, 28
	st REG2, 12(r3)

	LOAD_LABEL(REG1, label5, REG2, label4)
	st REG1, 16(r3)
	addi REG2, REG2, 64
	st REG2, 20(r3)

	LOAD_LABEL(REG1, label5, REG2, label4)
	st REG1, 24(r3)
	addi REG2, REG2, 100
	st REG2, 28(r3)

	LOAD_LABEL(REG1, label4, REG2, label3)
	addi REG1, REG1, 104
	st REG1, 32(r3)
	addi REG2, REG2, 8
	st REG2, 36(r3)
	blr
FUNC_END(fetch_all_rets)

FUNC_START(fetch_all_conds)
	LOAD_LABEL(REG1, label4, REG2, label5)
	addi REG1, REG1, 12
	st REG1, 0(r3)
	st REG2, 4(r3)

	LOAD_LABEL(REG1, label4, REG2, label5)
	addi REG1, REG1, 24
	st REG1, 8(r3)
	st REG2, 12(r3)

	LOAD_LABEL(REG1, label4, REG2, label5)
	addi REG1, REG1, 60
	st REG1, 16(r3)
	st REG2, 20(r3)

	LOAD_LABEL(REG1, label4, REG2, label5)
	addi REG1, REG1, 96
	st REG1, 24(r3)
	st REG2, 28(r3)
	blr
FUNC_END(fetch_all_conds)

FUNC_START(fetch_all_inds)
	LOAD_LABEL(REG1, label4, REG2, label5)
	addi REG1, REG1, 60
	st REG1, 0(r3)
	st REG2, 4(r3)

	LOAD_LABEL(REG1, label4, REG2, label5)
	addi REG1, REG1, 96
	st REG1, 8(r3)
	st REG2, 12(r3)
	blr
FUNC_END(fetch_all_inds)
