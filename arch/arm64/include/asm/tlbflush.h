/*
 * Based on arch/arm/include/asm/tlbflush.h
 *
 * Copyright (C) 1999-2003 Russell King
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __ASM_TLBFLUSH_H
#define __ASM_TLBFLUSH_H

#ifndef __ASSEMBLY__

#include <linux/sched.h>
#include <asm/cputype.h>

/*
 * Raw TLBI, DSB operations
 *
 * Where necessary, use __tlbi_*dsb() macros to avoid asm() boilerplate.
 * Drivers and most kernel code should use the TLB management routines in
 * preference to the macros below.
 *
 * The __tlbi_dsb() macro handles invoking the asm without any register
 * argument, with a single register argument, and with start (included)
 * and end (excluded) range of register arguments. For example:
 *
 * __tlbi_dsb(op, attr)
 *
 * 	tlbi op
 *	dsb attr
 *
 * __tlbi_dsb(op, attr, addr)
 *
 *	mov %[addr], =addr
 *	tlbi op, %[addr]
 *	dsb attr
 *
 * __tlbi_range_dsb(op, attr, start, end)
 *
 * 	mov %[arg], =start
 *	mov %[end], =end
 * for:
 * 	tlbi op, %[addr]
 * 	add %[addr], %[addr], #(1 << (PAGE_SHIFT - 12))
 * 	cmp %[addr], %[end]
 * 	b.ne for
 * 	dsb attr
 */

#define __TLBI_FOR_0(ig0, ig1, ig2)
#define __TLBI_INSTR_0(op, ig1, ig2)	"tlbi " #op
#define __TLBI_IO_0(ig0, ig1, ig2)	: :

#define __TLBI_FOR_1(ig0, ig1, ig2)
#define __TLBI_INSTR_1(op, ig0, ig1)	"tlbi " #op ", %0"
#define __TLBI_IO_1(ig0, arg, ig1)	: : "r" (arg)

#define __TLBI_FOR_2(ig0, start, ig1)	unsigned long addr;		       \
					for (addr = start; addr < end;	       \
						addr += 1 << (PAGE_SHIFT - 12))
#define __TLBI_INSTR_2(op, ig0, ig1)	"tlbi " #op ", %0"
#define __TLBI_IO_2(ig0, ig1, ig2)	: : "r" (addr)

#define __TLBI_FOR_N(op, a1, a2, n, ...)	__TLBI_FOR_##n(op, a1, a2)
#define __TLBI_INSTR_N(op, a1, a2, n, ...)	__TLBI_INSTR_##n(op, a1, a2)
#define __TLBI_IO_N(op, a1, a2, n, ...)	__TLBI_IO_##n(op, a1, a2)

#define __TLBI_FOR(op, ...)		__TLBI_FOR_N(op, ##__VA_ARGS__, 2, 1, 0)
#define __TLBI_INSTR(op, ...)		__TLBI_INSTR_N(op, ##__VA_ARGS__, 2, 1, 0)
#define __TLBI_IO(op, ...)		__TLBI_IO_N(op, ##__VA_ARGS__, 2, 1, 0)

#define __tlbi_asm_dsb(as, op, attr, ...) do {				       \
		__TLBI_FOR(op, ##__VA_ARGS__)				       \
			asm (__TLBI_INSTR(op, ##__VA_ARGS__)		       \
			__TLBI_IO(op, ##__VA_ARGS__));			       \
		asm volatile (	     as			"\ndsb " #attr "\n"    \
			ALTERNATIVE("nop"		"\nnop"	       "\n",   \
			__TLBI_INSTR(op, ##__VA_ARGS__)	"\ndsb " #attr "\n",   \
			ARM64_WORKAROUND_REPEAT_TLBI)			       \
		__TLBI_IO(op, ##__VA_ARGS__) : "memory"); } while (0)

#define __tlbi_dsb(...)	__tlbi_asm_dsb("", ##__VA_ARGS__)

/*
 *	TLB Management
 *	==============
 *
 *	The TLB specific code is expected to perform whatever tests it needs
 *	to determine if it should invalidate the TLB for each call.  Start
 *	addresses are inclusive and end addresses are exclusive; it is safe to
 *	round these addresses down.
 *
 *	flush_tlb_all()
 *
 *		Invalidate the entire TLB.
 *
 *	flush_tlb_mm(mm)
 *
 *		Invalidate all TLB entries in a particular address space.
 *		- mm	- mm_struct describing address space
 *
 *	flush_tlb_range(mm,start,end)
 *
 *		Invalidate a range of TLB entries in the specified address
 *		space.
 *		- mm	- mm_struct describing address space
 *		- start - start address (may not be aligned)
 *		- end	- end address (exclusive, may not be aligned)
 *
 *	flush_tlb_page(vaddr,vma)
 *
 *		Invalidate the specified page in the specified address range.
 *		- vaddr - virtual address (may not be aligned)
 *		- vma	- vma_struct describing address range
 *
 *	flush_kern_tlb_page(kaddr)
 *
 *		Invalidate the TLB entry for the specified page.  The address
 *		will be in the kernels virtual memory space.  Current uses
 *		only require the D-TLB to be invalidated.
 *		- kaddr - Kernel virtual memory address
 */
static inline void local_flush_tlb_all(void)
{
	dsb(nshst);
	__tlbi_dsb(vmalle1, nsh);
	isb();
}

static inline void flush_tlb_all(void)
{
	dsb(ishst);
	__tlbi_dsb(vmalle1is, ish);
	isb();
}

static inline void flush_tlb_mm(struct mm_struct *mm)
{
	unsigned long asid = ASID(mm) << 48;

	dsb(ishst);
	__tlbi_dsb(aside1is, ish, asid);
}

static inline void flush_tlb_page(struct vm_area_struct *vma,
				  unsigned long uaddr)
{
	unsigned long addr = uaddr >> 12 | (ASID(vma->vm_mm) << 48);

	dsb(ishst);
	__tlbi_dsb(vale1is, ish, addr);
}

/*
 * This is meant to avoid soft lock-ups on large TLB flushing ranges and not
 * necessarily a performance improvement.
 */
#define MAX_TLB_RANGE	(1024UL << PAGE_SHIFT)

static inline void __flush_tlb_range(struct vm_area_struct *vma,
				     unsigned long start, unsigned long end,
				     bool last_level)
{
	unsigned long asid = ASID(vma->vm_mm) << 48;

	if ((end - start) > MAX_TLB_RANGE) {
		flush_tlb_mm(vma->vm_mm);
		return;
	}

	start = asid | (start >> 12);
	end = asid | (end >> 12);

	dsb(ishst);
	if (last_level)
		__tlbi_dsb(vale1is, ish, start, end);
	else
		__tlbi_dsb(vae1is, ish, start, end);
}

static inline void flush_tlb_range(struct vm_area_struct *vma,
				   unsigned long start, unsigned long end)
{
	__flush_tlb_range(vma, start, end, false);
}

static inline void flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	if ((end - start) > MAX_TLB_RANGE) {
		flush_tlb_all();
		return;
	}

	start >>= 12;
	end >>= 12;

	dsb(ishst);
	__tlbi_dsb(vaae1is, ish, start, end);
	isb();
}

/*
 * Used to invalidate the TLB (walk caches) corresponding to intermediate page
 * table levels (pgd/pud/pmd).
 */
static inline void __flush_tlb_pgtable(struct mm_struct *mm,
				       unsigned long uaddr)
{
	unsigned long addr = uaddr >> 12 | (ASID(mm) << 48);

	__tlbi_dsb(vae1is, ish, addr);
}

#endif

#endif
