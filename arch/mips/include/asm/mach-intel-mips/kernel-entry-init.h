/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Chris Dearman (chris@mips.com)
 * Leonid Yegoshin (yegoshin@mips.com)
 * Copyright (C) 2012 Mips Technologies, Inc.
 * Copyright (C) 2018 Intel Corporation.
 */
#ifndef __ASM_MACH_INTEL_MIPS_KERNEL_ENTRY_INIT_H
#define __ASM_MACH_INTEL_MIPS_KERNEL_ENTRY_INIT_H
/*
* Prepare segments for EVA boot:
*
* This is in case the processor boots in legacy configuration
* (SI_EVAReset is de-asserted and CONFIG5.K == 0) with 1GB DDR
*
* On entry, t1 is loaded with CP0_CONFIG
*
* ========================= Mappings =============================
* Virtual memory           Physical memory            Mapping
* 0x00000000 - 0x7fffffff  0x20000000 - 0x9ffffffff   MUSUK (kuseg)
* 0x80000000 - 0x9fffffff  0x80000000 - 0x9ffffffff   UK    (kseg0)
* 0xa0000000 - 0xbfffffff  0x20000000 - 0x3ffffffff   UK    (kseg1)
* 0xc0000000 - 0xdfffffff             -               MSK   (kseg2)
* 0xe0000000 - 0xffffffff  0xa0000000 - 0xbfffffff    UK     2nd IO
*
* user space virtual:   0x00000000 ~ 0x7fffffff
* kernel space virtual: 0x60000000 ~ 0x9fffffff
*                  physical: 0x20000000 ~ 0x5fffffff (flat 1GB)
* user/kernel space overlapped from 0x60000000 ~ 0x7fffffff (virtual)
* where physical 0x20000000 ~ 0x2fffffff (cached and uncached)
*           virtual  0xa0000000 ~ 0xafffffff (1st IO space)
*           virtual  0xf0000000 ~ 0xffffffff (2nd IO space)
*
* The last 64KB of physical memory are reserved for correct HIGHMEM
* macros arithmetics.
* Detailed KSEG and PHYS_OFFSET and PAGE_OFFSEt adaption, refer to
* asm/mach-intel-mips/spaces.h
*/
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
