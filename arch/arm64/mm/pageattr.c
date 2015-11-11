/*
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sched.h>

#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>

#include "mm.h"

static int update_pte_range(struct mm_struct *mm, pmd_t *pmd,
				unsigned long addr, unsigned long end,
				pgprot_t clear, pgprot_t set)
{
	pte_t *pte;
	int err = 0;

	if (pmd_sect(*pmd)) {
		if (!IS_ENABLED(CONFIG_DEBUG_CHANGE_PAGEATTR)) {
			err = -EINVAL;
			goto out;
		}
		pte = pte_alloc_one_kernel(&init_mm, addr);
		if (!pte) {
			err = -ENOMEM;
			goto out;
		}
		split_pmd(pmd, pte);
		__pmd_populate(pmd, __pa(pte), PMD_TYPE_TABLE);
	}


	pte = pte_offset_kernel(pmd, addr);
	if (pte_none(*pte)) {
		err = -EFAULT;
		goto out;
	}

	do {
		pte_t p = *pte;

		p = clear_pte_bit(p, clear);
		p = set_pte_bit(p, set);
		set_pte(pte, p);

	} while (pte++, addr += PAGE_SIZE, addr != end);

out:
	return err;
}


static int update_pmd_range(struct mm_struct *mm, pud_t *pud,
				unsigned long addr, unsigned long end,
				pgprot_t clear, pgprot_t set)
{
	pmd_t *pmd;
	unsigned long next;
	int err = 0;

	if (pud_sect(*pud)) {
		if (!IS_ENABLED(CONFIG_DEBUG_CHANGE_PAGEATTR)) {
			err = -EINVAL;
			goto out;
		}
		pmd = pmd_alloc_one(&init_mm, addr);
		if (!pmd) {
			err = -ENOMEM;
			goto out;
		}
		split_pud(pud, pmd);
		pud_populate(&init_mm, pud, pmd);
	}


	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd)) {
		err = -EFAULT;
		goto out;
	}

	do {
		next = pmd_addr_end(addr, end);
		if (((addr | end) & ~SECTION_MASK) == 0) {
			unsigned long paddr = pmd_pfn(*pmd) << PAGE_SHIFT;
			pgprot_t prot = __pgprot((pmd_val(*pmd) ^ paddr));

			pgprot_val(prot) &= ~pgprot_val(clear);
			pgprot_val(prot) |= pgprot_val(set);
			set_pmd(pmd, __pmd(paddr | pgprot_val(prot)));
		} else {
			err = update_pte_range(mm, pmd, addr, next, clear, set);
		}
		if (err)
			break;
	} while (pmd++, addr = next, addr != end);
out:
	return err;
}


static int update_pud_range(struct mm_struct *mm, pgd_t *pgd,
					unsigned long addr, unsigned long end,
					pgprot_t clear, pgprot_t set)
{
	pud_t *pud;
	unsigned long next;
	int err = 0;

	pud = pud_offset(pgd, addr);
	if (pud_none(*pud)) {
		err = -EFAULT;
		goto out;
	}

	do {
		next = pud_addr_end(addr, end);
		if (pud_sect(*pud) && ((addr | next) & ~PUD_MASK) == 0) {
			unsigned long paddr = pud_pfn(*pud) << PAGE_SHIFT;
			pgprot_t prot = __pgprot(pud_val(*pud) ^ paddr);

			pgprot_val(prot) &= ~pgprot_val(clear);
			pgprot_val(prot) |= pgprot_val(set);
			set_pud(pud, __pud(paddr | pgprot_val(prot)));
		} else {
			err = update_pmd_range(mm, pud, addr, next, clear, set);
		}
		if (err)
			break;
	} while (pud++, addr = next, addr != end);

out:
	return err;
}

static int update_page_range(unsigned long addr,
				unsigned long end, pgprot_t clear,
				pgprot_t set)
{
	pgd_t *pgd;
	unsigned long next;
	int err;
	struct mm_struct *mm = &init_mm;

	BUG_ON(addr >= end);
	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd)) {
		err = -EFAULT;
		goto out;
	}

	do {
		next = pgd_addr_end(addr, end);
		err = update_pud_range(mm, pgd, addr, next, clear, set);
		if (err)
			break;
	} while (pgd++, addr = next, addr != end);

out:
	return err;
}

static int change_memory_common(unsigned long addr, int numpages,
				pgprot_t set_mask, pgprot_t clear_mask)
{
	unsigned long start = addr;
	unsigned long size = PAGE_SIZE*numpages;
	unsigned long end = start + size;
	int ret;

	if (!PAGE_ALIGNED(addr)) {
		start &= PAGE_MASK;
		end = start + size;
		WARN_ON_ONCE(1);
	}

	if (start < PAGE_OFFSET && !is_vmalloc_addr((void *)start) &&
		(start < MODULES_VADDR || start >= MODULES_END))
		return -EINVAL;

	if (end < PAGE_OFFSET && !is_vmalloc_addr((void *)end) &&
		(end < MODULES_VADDR || end >= MODULES_END))
		return -EINVAL;

	ret = update_page_range(addr, end, clear_mask, set_mask);

	flush_tlb_kernel_range(start, end);
	return ret;
}

int set_memory_ro(unsigned long addr, int numpages)
{
	return change_memory_common(addr, numpages,
					__pgprot(PTE_RDONLY),
					__pgprot(PTE_WRITE));
}

int set_memory_rw(unsigned long addr, int numpages)
{
	return change_memory_common(addr, numpages,
					__pgprot(PTE_WRITE),
					__pgprot(PTE_RDONLY));
}

int set_memory_nx(unsigned long addr, int numpages)
{
	return change_memory_common(addr, numpages,
					__pgprot(PTE_PXN),
					__pgprot(0));
}
EXPORT_SYMBOL_GPL(set_memory_nx);

int set_memory_x(unsigned long addr, int numpages)
{
	return change_memory_common(addr, numpages,
					__pgprot(0),
					__pgprot(PTE_PXN));
}
EXPORT_SYMBOL_GPL(set_memory_x);
