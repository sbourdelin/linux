/*
 * This file contains kasan initialization code for PowerPC
 *
 * Copyright 2017 Balbir Singh, IBM Corporation.
 *
 * Derived from arm64 version
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 * Author: Andrey Ryabinin <ryabinin.a.a@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifdef CONFIG_KASAN

#define pr_fmt(fmt) "kasan: " fmt
#include <linux/kasan.h>
#include <linux/kernel.h>
#include <linux/sched/task.h>
#include <linux/memblock.h>
#include <linux/start_kernel.h>
#include <linux/mm.h>
#include <linux/pfn_t.h>

#include <asm/mmu_context.h>
#include <asm/page.h>
#include <asm/io.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <asm/sections.h>
#include <asm/tlbflush.h>

DEFINE_STATIC_KEY_FALSE(powerpc_kasan_enabled_key);
EXPORT_SYMBOL(powerpc_kasan_enabled_key);

unsigned char kasan_zero_page[PAGE_SIZE] __page_aligned_bss;
#if CONFIG_PGTABLE_LEVELS > 3
pud_t kasan_zero_pud[RADIX_PTRS_PER_PUD] __page_aligned_bss;
#endif
#if CONFIG_PGTABLE_LEVELS > 2
pmd_t kasan_zero_pmd[RADIX_PTRS_PER_PMD] __page_aligned_bss;
#endif
pte_t kasan_zero_pte[RADIX_PTRS_PER_PTE] __page_aligned_bss;

static void set_pte(pte_t *ptep, pte_t pte)
{
	*ptep = pte;
	/* No flush */
}

void __init kasan_init(void)
{
	unsigned long kimg_shadow_start, kimg_shadow_end;
	struct memblock_region *reg;
	int i;

	unsigned long pte_val = __pa(kasan_zero_page) | pgprot_val(PAGE_KERNEL)
						      | _PAGE_PTE;
	unsigned long pmd_val = __pa(kasan_zero_pte) | pgprot_val(PAGE_KERNEL)
						     | _PAGE_PTE;
	unsigned long pud_val = __pa(kasan_zero_pmd) | pgprot_val(PAGE_KERNEL);


	for (i = 0; i < PTRS_PER_PTE; i++)
		kasan_zero_pte[i] = __pte(pte_val);

	for (i = 0; i < PTRS_PER_PMD; i++)
		kasan_zero_pmd[i] = __pmd(pmd_val);

	for (i = 0; i < PTRS_PER_PUD; i++)
		kasan_zero_pud[i] = __pud(pud_val);


	kimg_shadow_start = (unsigned long)kasan_mem_to_shadow(_text);
	kimg_shadow_end = (unsigned long)kasan_mem_to_shadow(_end);


	vmemmap_populate(kimg_shadow_start, kimg_shadow_end,
			 pfn_to_nid(virt_to_pfn(lm_alias(_text))));

	for_each_memblock(memory, reg) {
		void *start = (void *)phys_to_virt(reg->base);
		void *end = (void *)phys_to_virt(reg->base + reg->size);

		if (start >= end)
			break;

		vmemmap_populate((unsigned long)kasan_mem_to_shadow(start),
				(unsigned long)kasan_mem_to_shadow(end),
				pfn_to_nid(virt_to_pfn(start)));
	}

	kimg_shadow_start = (unsigned long)
			kasan_mem_to_shadow((void *)(RADIX_KERN_VIRT_START));
	kimg_shadow_end = (unsigned long)
			kasan_mem_to_shadow((void *)(RADIX_KERN_VIRT_START +
							RADIX_KERN_VIRT_SIZE));

	kasan_populate_zero_shadow((void *)kimg_shadow_start,
					(void *)kimg_shadow_end);

	/*
	 * Kasan may reuse the contents of kasan_zero_pte directly, so we
	 * should make sure that it maps the zero page read-only.
	 */
	for (i = 0; i < PTRS_PER_PTE; i++)
		set_pte(&kasan_zero_pte[i],
			pfn_pte(virt_to_pfn(kasan_zero_page),
			__pgprot(_PAGE_PTE | _PAGE_KERNEL_RO | _PAGE_BASE)));

	memset(kasan_zero_page, 0, PAGE_SIZE);

	/* At this point kasan is fully initialized. Enable error messages */
	init_task.kasan_depth = 0;
	pr_info("KernelAddressSanitizer initialized\n");
	static_branch_inc(&powerpc_kasan_enabled_key);
}

#endif /* CONFIG_KASAN */
