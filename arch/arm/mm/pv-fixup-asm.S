/*
 *  Copyright (C) 2015 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This assembly is required to safely remap the physical address space
 * for Keystone 2
 */
#include <linux/linkage.h>
#include <asm/asm-offsets.h>
#include <asm/cp15.h>
#include <asm/memory.h>
#include <asm/pgtable.h>

	.section ".idmap.text", "ax"

#define L1_ORDER 3
#define L2_ORDER 3

/*
 *	attr_mod_table:
 *		describe transforms to be made to the early boot pgtable
 *		This is poked by early init code
 *	mod descriptor list:
 *		64 bit test mask
 *		64 bit test value
 *		64 bit clear mask
 *		64 bit set mask
 *      	next descriptor
 *		...
 *		0x0000_00000 0x0000_0000 end of list
 */
/* TODO: what segment?, test w/ XIP kernel? */
	 .globl attr_mod_table
attr_mod_table:
	.zero   8*MAX_ATTR_MOD_ENTRIES*4 + 1

/*
 *	lpae_pgtables_remap_asm(long long offset, unsigned long pg,
 *		void* boot_data)
 *
 *	Rewrite initial boot page tables with new physical addresses and or
 *	attributes.
 *	This function starts in identity mapped VA -> low PA
 *	The body runs in low PA with MMU off
 * 	The function ends in "identity mapped" VA -> high PA
 *	The function returns to kernel VA space -> high PA
 *
 *	- r0    PA delta low
 *	- r1	PA delta high
 *	- r2    address of top level table
 *	- r3    address of dtb (or atags))
 *
 *	uses null terminated list of attribute modifications in attr_mod_table
 */
ENTRY(lpae_pgtables_remap_asm)
	stmfd	sp!, {r4-r11, lr}

	mrc	p15, 0, r8, c1, c0, 0		@ read control reg
	bic	ip, r8, #CR_M			@ disable caches and MMU
	mcr	p15, 0, ip, c1, c0, 0
	dsb
	isb

	/* Update level 2 entries covering the kernel */
	ldr	r6, =(_end - 1)
	add	r7, r2, #0x1000
	add	r6, r7, r6, lsr #SECTION_SHIFT - L2_ORDER
	add	r7, r7, #PAGE_OFFSET >> (SECTION_SHIFT - L2_ORDER)
1:	ldrd	r4, [r7]
	adds	r4, r4, r0
	adc	r5, r5, r1
	strd	r4, [r7], #1 << L2_ORDER
	cmp	r7, r6
	bls	1b

	/* Update level 2 entries for the boot data */
	add	r7, r2, #0x1000
	add	r7, r7, r3, lsr #SECTION_SHIFT - L2_ORDER
	bic	r7, r7, #(1 << L2_ORDER) - 1
	ldrd	r4, [r7]
	adds	r4, r4, r0
	adc	r5, r5, r1
	strd	r4, [r7], #1 << L2_ORDER
	ldrd	r4, [r7]
	adds	r4, r4, r0
	adc	r5, r5, r1
	strd	r4, [r7]

	/* Update level 1 entries */
	mov	r6, #4
	mov	r7, r2
2:	ldrd	r4, [r7]
	adds	r4, r4, r0
	adc	r5, r5, r1
	strd	r4, [r7], #1 << L1_ORDER
	subs	r6, r6, #1
	bne	2b

	/* Update HW page table regs with new PA */
	mrrc	p15, 0, r4, r5, c2		@ read TTBR0
	adds	r4, r4, r0			@ update physical address
	adc	r5, r5, r1
	mcrr	p15, 0, r4, r5, c2		@ write back TTBR0
	mrrc	p15, 1, r4, r5, c2		@ read TTBR1
	adds	r4, r4, r0			@ update physical address
	adc	r5, r5, r1
	mcrr	p15, 1, r4, r5, c2		@ write back TTBR1

	dsb

	/* Update attributes of all level 2 entries in 1GB space */
	/* TODO: fix/test BE8 THUMB2 kernel */
	adrl	r3, attr_mod_table
	add	r7, r2, #0x1000
	add	r6, r7, #0x4000
	bl	3f				@ NOT C ABI

	/* Update attributes of the 4 level 1 entries */
	/* TODO: delete this or allow mod entries to match only L1 */
	mov	r7, r2
	add	r6, r7, #32
	bl	3f				@ NOT C ABI
	b	7f

3:	ldrd	r4, [r7]
	orrs	r11, r4, r5
	beq	6f				@ skip unused entries
	mov 	r10, r3
4:	ldrd	r8, [r10]
	orrs	r11, r8, r9
	beq	6f				@ end of mod table?
	and	r0, r4, r8			@ no, load test mask
	and	r1, r5, r9
	ldrd	r8, [r10, #8]			@ load test bits
	cmp	r0, r8
	cmpeq	r1, r9
	bne	5f				@ does entry match desc?
	ldrd	r8, [r10, #16]			@ yes, load mod clear mask
	bic	r4, r4, r8
	bic	r5, r5, r9
	ldrd	r8, [r10, #24]			@ load mod set mask
	orr	r4, r4, r8
	orr	r5, r5, r9
5:	add     r10, r10, #32			@ try next mod desc
	b	4b
6:	strd	r4, [r7], #1 << L2_ORDER
	cmp	r7, r6
	bls	3b
	bx 	lr

7:
	mov	ip, #0
	mcr	p15, 0, ip, c7, c5, 0		@ I+BTB cache invalidate
	mcr	p15, 0, ip, c8, c7, 0		@ local_flush_tlb_all()
	dsb
	isb

	mrc	p15, 0, r8, c1, c0, 0		@ re-enable MMU
	orr	r8, r8, #CR_M
	mcr	p15, 0, r8, c1, c0, 0
	dsb
	isb

	ldmfd	sp!, {r4-r11, pc}
ENDPROC(lpae_pgtables_remap_asm)
