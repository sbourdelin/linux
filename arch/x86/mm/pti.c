/*
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * This code is based in part on work published here:
 *
 *	https://github.com/IAIK/KAISER
 *
 * The original work was written by and and signed off by for the Linux
 * kernel by:
 *
 *   Signed-off-by: Richard Fellner <richard.fellner@student.tugraz.at>
 *   Signed-off-by: Moritz Lipp <moritz.lipp@iaik.tugraz.at>
 *   Signed-off-by: Daniel Gruss <daniel.gruss@iaik.tugraz.at>
 *   Signed-off-by: Michael Schwarz <michael.schwarz@iaik.tugraz.at>
 *
 * Major changes to the original code by: Dave Hansen <dave.hansen@intel.com>
 * Mostly rewritten by Thomas Gleixner <tglx@linutronix.de> and
 *		       Andy Lutomirsky <luto@amacapital.net>
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/bug.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/uaccess.h>

#include <asm/cpufeature.h>
#include <asm/hypervisor.h>
#include <asm/cmdline.h>
#include <asm/pti.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include <asm/desc.h>

#undef pr_fmt
#define pr_fmt(fmt)     "Kernel/User page tables isolation: " fmt

void __init pti_check_boottime_disable(void)
{
	bool enable = true;
	char arg[5];

	if (cmdline_find_option(boot_command_line, "pti", arg, sizeof(arg))) {
		if (!strncmp(arg, "on", 2))
			goto enable;

		if (!strncmp(arg, "off", 3)) {
			pr_info("disabled on command line.\n");
			return;
		}

		if (!strncmp(arg, "auto", 4))
			goto skip;
	}

	if (cmdline_find_option_bool(boot_command_line, "nopti")) {
		pr_info("disabled on command line.\n");
		enable = false;
	}

skip:
	if (hypervisor_is_type(X86_HYPER_XEN_PV)) {
		pr_info("disabled on XEN_PV.\n");
		enable = false;
	}

enable:
	if (enable)
		setup_force_cpu_bug(X86_BUG_CPU_SECURE_MODE_PTI);
}

/*
 * Walk the user copy of the page tables (optionally) trying to allocate
 * page table pages on the way down.
 *
 * Returns a pointer to a PMD on success, or NULL on failure.
 */
static pmd_t *pti_user_pagetable_walk_pmd(unsigned long address)
{
	pgd_t *pgd = kernel_to_user_pgdp(pgd_offset_k(address));
	gfp_t gfp = (GFP_KERNEL | __GFP_ZERO);
	pud_t *pud;
	p4d_t *p4d;

	if (address < PAGE_OFFSET) {
		WARN_ONCE(1, "attempt to walk user address\n");
		return NULL;
	}

	if (pgd_none(*pgd)) {
		unsigned long new_p4d_page = __get_free_page(gfp);
		if (!new_p4d_page)
			return NULL;

		if (pgd_none(*pgd)) {
			set_pgd(pgd, __pgd(_KERNPG_TABLE | __pa(new_p4d_page)));
			new_p4d_page = 0;
		}
		if (new_p4d_page)
			free_page(new_p4d_page);
	}
	BUILD_BUG_ON(pgd_large(*pgd) != 0);

	p4d = p4d_offset(pgd, address);
	BUILD_BUG_ON(p4d_large(*p4d) != 0);
	if (p4d_none(*p4d)) {
		unsigned long new_pud_page = __get_free_page(gfp);
		if (!new_pud_page)
			return NULL;

		if (p4d_none(*p4d)) {
			set_p4d(p4d, __p4d(_KERNPG_TABLE | __pa(new_pud_page)));
			new_pud_page = 0;
		}
		if (new_pud_page)
			free_page(new_pud_page);
	}

	pud = pud_offset(p4d, address);
	/* The user page tables do not use large mappings: */
	if (pud_large(*pud)) {
		WARN_ON(1);
		return NULL;
	}
	if (pud_none(*pud)) {
		unsigned long new_pmd_page = __get_free_page(gfp);
		if (!new_pmd_page)
			return NULL;

		if (pud_none(*pud)) {
			set_pud(pud, __pud(_KERNPG_TABLE | __pa(new_pmd_page)));
			new_pmd_page = 0;
		}
		if (new_pmd_page)
			free_page(new_pmd_page);
	}

	return pmd_offset(pud, address);
}

static void __init
pti_clone_pmds(unsigned long start, unsigned long end, pmdval_t clear)
{
	unsigned long addr;

	/*
	 * Clone the populated PMDs which cover start to end. These PMD areas
	 * can have holes.
	 */
	for (addr = start; addr < end; addr += PMD_SIZE) {
		pmd_t *pmd, *target_pmd;
		pgd_t *pgd;
		p4d_t *p4d;
		pud_t *pud;

		pgd = pgd_offset_k(addr);
		if (WARN_ON(pgd_none(*pgd)))
			return;
		p4d = p4d_offset(pgd, addr);
		if (WARN_ON(p4d_none(*p4d)))
			return;
		pud = pud_offset(p4d, addr);
		if (pud_none(*pud))
			continue;
		pmd = pmd_offset(pud, addr);
		if (pmd_none(*pmd))
			continue;

		target_pmd = pti_user_pagetable_walk_pmd(addr);
		if (WARN_ON(!target_pmd))
			return;

		/*
		 * Copy the PMD.  That is, the kernelmode and usermode
		 * tables will share the last-level page tables of this
		 * address range
		 */
		*target_pmd = pmd_clear_flags(*pmd, clear);
	}
}

/*
 * Clone the populated PMDs of the user shared fixmaps into the user space
 * visible page table.
 */
static void __init pti_clone_user_shared(void)
{
	unsigned long bot, top;

	bot = __fix_to_virt(FIX_USR_SHARED_BOTTOM);
	top = __fix_to_virt(FIX_USR_SHARED_TOP) + PAGE_SIZE;

	/* Top of the user shared block must be PMD-aligned. */
	WARN_ON(top & ~PMD_MASK);

	pti_clone_pmds(bot, top, 0);
}

/*
 * Clone the populated PMDs of the entry and irqentry text and force it RO.
 */
static void __init pti_clone_entry_text(void)
{
	pti_clone_pmds((unsigned long) __entry_text_start,
			(unsigned long) __irqentry_text_end, _PAGE_RW);
}

/*
 * Initialize kernel page table isolation
 */
void __init pti_init(void)
{
	if (!static_cpu_has_bug(X86_BUG_CPU_SECURE_MODE_PTI))
		return;

	pr_info("enabled\n");

	pti_clone_user_shared();
	pti_clone_entry_text();
}
