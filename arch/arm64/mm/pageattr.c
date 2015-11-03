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

struct page_change_data {
	pgprot_t set_mask;
	pgprot_t clear_mask;
};

static int change_page_range(pte_t *ptep, pgtable_t token, unsigned long addr,
			void *data)
{
	struct page_change_data *cdata = data;
	pte_t pte = *ptep;

	pte = clear_pte_bit(pte, cdata->clear_mask);
	pte = set_pte_bit(pte, cdata->set_mask);

	set_pte(ptep, pte);
	return 0;
}

#ifdef CONFIG_DEBUG_CHANGE_PAGEATTR
static int check_address(unsigned long addr)
{
	pgd_t *pgd = pgd_offset_k(addr);
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	int ret = -EFAULT;

	if (pgd_none(*pgd))
		goto out;

	pud = pud_offset(pgd, addr);
	if (pud_none(*pud))
		goto out;

	if (pud_sect(*pud)) {
		pmd = pmd_alloc_one(&init_mm, addr);
		if (!pmd) {
			ret = -ENOMEM;
			goto out;
		}
		split_pud(pud, pmd);
		pud_populate(&init_mm, pud, pmd);
	}

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		goto out;

	if (pmd_sect(*pmd)) {
		pte = pte_alloc_one_kernel(&init_mm, addr);
		if (!pte) {
			ret = -ENOMEM;
			goto out;
		}
		split_pmd(pmd, pte);
		__pmd_populate(pmd, __pa(pte), PMD_TYPE_TABLE);
	}

	pte = pte_offset_kernel(pmd, addr);
	if (pte_none(*pte))
		goto out;

	flush_tlb_all();
	ret = 0;

out:
	return ret;
}
#else
static int check_address(unsigned long addr)
{
	if (addr < MODULES_VADDR || addr >= MODULES_END)
		return -EINVAL;

	return 0;
}
#endif

static int change_memory_common(unsigned long addr, int numpages,
				pgprot_t set_mask, pgprot_t clear_mask)
{
	unsigned long start = addr;
	unsigned long size = PAGE_SIZE*numpages;
	unsigned long end = start + size;
	int ret;
	struct page_change_data data;

	if (addr < PAGE_OFFSET && !is_vmalloc_addr((void *)addr))
		return -EINVAL;

	if (!IS_ALIGNED(addr, PAGE_SIZE)) {
		start &= PAGE_MASK;
		end = start + size;
		WARN_ON_ONCE(1);
	}

	ret = check_address(addr);
	if (ret)
		return ret;

	data.set_mask = set_mask;
	data.clear_mask = clear_mask;

	ret = apply_to_page_range(&init_mm, start, size, change_page_range,
					&data);

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
