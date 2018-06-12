/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Chris Dearman (chris@mips.com)
 * Leonid Yegoshin (yegoshin@mips.com)
 * Copyright (C) 2012 Mips Technologies, Inc.
 * Copyright (C) 2018 Intel Corporation.
 */
#ifndef __ASM_MACH_INTEL_MIPS_KERNEL_ENTRY_INIT_H
#define __ASM_MACH_INTEL_MIPS_KERNEL_ENTRY_INIT_H

	.macro  platform_eva_init

	.set    push
	.set    reorder
	/*
	 * Get Config.K0 value and use it to program
	 * the segmentation registers
	 */
	mfc0    t1, CP0_CONFIG
	andi    t1, 0x7 /* CCA */
	move    t2, t1
	ins     t2, t1, 16, 3
	/* SegCtl0 */
	li      t0, ((MIPS_SEGCFG_UK << MIPS_SEGCFG_AM_SHIFT) |              \
		(5 << MIPS_SEGCFG_PA_SHIFT) | (2 << MIPS_SEGCFG_C_SHIFT) |   \
		(1 << MIPS_SEGCFG_EU_SHIFT)) |                               \
		(((MIPS_SEGCFG_MSK << MIPS_SEGCFG_AM_SHIFT) |                \
		(0 << MIPS_SEGCFG_PA_SHIFT) |                                \
		(1 << MIPS_SEGCFG_EU_SHIFT)) << 16)
	ins     t0, t1, 16, 3
	mtc0    t0, $5, 2

	/* SegCtl1 */
	li      t0, ((MIPS_SEGCFG_UK << MIPS_SEGCFG_AM_SHIFT) |              \
		(1 << MIPS_SEGCFG_PA_SHIFT) | (2 << MIPS_SEGCFG_C_SHIFT) |   \
		(1 << MIPS_SEGCFG_EU_SHIFT)) |                               \
		(((MIPS_SEGCFG_UK << MIPS_SEGCFG_AM_SHIFT) |                 \
		(2 << MIPS_SEGCFG_PA_SHIFT) |                                \
		(1 << MIPS_SEGCFG_EU_SHIFT)) << 16)
	ins     t0, t1, 16, 3
	mtc0    t0, $5, 3

	/* SegCtl2 */
	li      t0, ((MIPS_SEGCFG_MUSUK << MIPS_SEGCFG_AM_SHIFT) |           \
		(0 << MIPS_SEGCFG_PA_SHIFT) |                                \
		(1 << MIPS_SEGCFG_EU_SHIFT)) |                               \
		(((MIPS_SEGCFG_MUSK << MIPS_SEGCFG_AM_SHIFT) |               \
		(0 << MIPS_SEGCFG_PA_SHIFT)/*| (2 << MIPS_SEGCFG_C_SHIFT)*/ | \
		(1 << MIPS_SEGCFG_EU_SHIFT)) << 16)
	ins     t0, t1, 0, 3
	mtc0    t0, $5, 4

	jal     mips_ihb
	mfc0    t0, $16, 5
	li      t2, 0x40000000      /* K bit */
	or      t0, t0, t2
	mtc0    t0, $16, 5
	sync
	jal     mips_ihb

	.set    pop
	.endm

	.macro	kernel_entry_setup
	sync
	ehb
	platform_eva_init
	.endm

	.macro	smp_slave_setup
	sync
	ehb
	platform_eva_init
	.endm

#endif /* __ASM_MACH_INTEL_MIPS_KERNEL_ENTRY_INIT_H */
