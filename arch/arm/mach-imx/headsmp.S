/*
 * Copyright 2011 Freescale Semiconductor, Inc.
 * Copyright 2011 Linaro Ltd.
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/linkage.h>
#include <linux/init.h>
#include <asm/assembler.h>

diag_reg_offset:
	.word	g_diag_reg - .

	.macro	set_diag_reg
	adr	r0, diag_reg_offset
	ldr	r1, [r0]
	add	r1, r1, r0		@ r1 = physical &g_diag_reg
	ldr	r0, [r1]
	mcr	p15, 0, r0, c15, c0, 1	@ write diagnostic register
	.endm

ENTRY(v7_secondary_startup)
	.word	0xc070			@ 0xc07 is cortex-a7 id
	.word	0xfff0			@ mask for core type

ARM_BE8(setend be)			@ go BE8 if entered LE
	mrc     p15, 0, r0, c0, c0, 0
	adr	r1, v7_secondary_startup
	ldr	r2, [r1]
	ldr	r3, [r1, #0x4]
	and     r0, r0, r3
	cmp     r0, r2
	beq     secondary_startup

	set_diag_reg
	b	secondary_startup
ENDPROC(v7_secondary_startup)
