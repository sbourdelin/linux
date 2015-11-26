/*
 * This file contains kasan initialization code for ARM64.
 *
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 * Author: Andrey Ryabinin <ryabinin.a.a@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#define pr_fmt(fmt) "kasan: " fmt
#include <linux/kasan.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/start_kernel.h>

#include <asm/page.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>

static pgd_t tmp_pg_dir[PTRS_PER_PGD] __initdata __aligned(PGD_SIZE);
static pud_t tmp_pud[PAGE_SIZE/sizeof(pud_t)] __initdata __aligned(PAGE_SIZE);

static void __init kasan_early_pte_populate(pmd_t *pmd, unsigned long addr,
					unsigned long end)
{
	pte_t *pte;
	unsigned long next;

	if (pmd_none(*pmd))
		pmd_populate_kernel(&init_mm, pmd, kasan_zero_pte);

	pte = pte_offset_kernel(pmd, addr);
	do {
		next = addr + PAGE_SIZE;
		set_pte(pte, pfn_pte(virt_to_pfn(kasan_zero_page),
					PAGE_KERNEL));
	} while (pte++, addr = next, addr != end && pte_none(*pte));
}

static void __init kasan_early_pmd_populate(pud_t *pud,
					unsigned long addr,
					unsigned long end)
{
	pmd_t *pmd;
	unsigned long next;

	if (pud_none(*pud))
		pud_populate(&init_mm, pud, kasan_zero_pmd);

	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		kasan_early_pte_populate(pmd, addr, next);
	} while (pmd++, addr = next, addr != end && pmd_none(*pmd));
}

static void __init kasan_early_pud_populate(pgd_t *pgd,
					unsigned long addr,
					unsigned long end)
{
	pud_t *pud;
	unsigned long next;

	if (pgd_none(*pgd))
		pgd_populate(&init_mm, pgd, kasan_zero_pud);

	pud = pud_offset(pgd, addr);
	do {
		next = pud_addr_end(addr, end);
		kasan_early_pmd_populate(pud, addr, next);
	} while (pud++, addr = next, addr != end && pud_none(*pud));
}

static void __init kasan_map_early_shadow(void)
{
	unsigned long addr = KASAN_SHADOW_START;
	unsigned long end = KASAN_SHADOW_END;
	unsigned long next;
	pgd_t *pgd;

	pgd = pgd_offset_k(addr);
	do {
		next = pgd_addr_end(addr, end);
		kasan_early_pud_populate(pgd, addr, next);
	} while (pgd++, addr = next, addr != end);
}

asmlinkage void __init kasan_early_init(void)
{
	BUILD_BUG_ON(KASAN_SHADOW_OFFSET != KASAN_SHADOW_END - (1UL << 61));
	BUILD_BUG_ON(!IS_ALIGNED(KASAN_SHADOW_START, PGDIR_SIZE));
	BUILD_BUG_ON(!IS_ALIGNED(KASAN_SHADOW_END, PUD_SIZE));
	kasan_map_early_shadow();
}

static void __init clear_pmds(pud_t *pud, unsigned long addr, unsigned long end)
{
	pmd_t *pmd;
	unsigned long next;

	pmd = pmd_offset(pud, addr);

	do {
		next = pmd_addr_end(addr, end);
		if (IS_ALIGNED(addr, PMD_SIZE) && end - addr >= PMD_SIZE)
			pmd_clear(pmd);

	} while (pmd++, addr = next, addr != end);
}

static void __init clear_puds(pgd_t *pgd, unsigned long addr, unsigned long end)
{
	pud_t *pud;
	unsigned long next;

	pud = pud_offset(pgd, addr);

	do {
		next = pud_addr_end(addr, end);
		if (IS_ALIGNED(addr, PUD_SIZE) && end - addr >= PUD_SIZE)
			pud_clear(pud);

		if (!pud_none(*pud))
			clear_pmds(pud, addr, next);
	} while (pud++, addr = next, addr != end);
}

static void __init clear_page_tables(unsigned long addr, unsigned long end)
{
	pgd_t *pgd;
	unsigned long next;

	pgd = pgd_offset_k(addr);

	do {
		next = pgd_addr_end(addr, end);
		if (IS_ALIGNED(addr, PGDIR_SIZE) && end - addr >= PGDIR_SIZE)
			pgd_clear(pgd);

		if (!pgd_none(*pgd))
			clear_puds(pgd, addr, next);
	} while (pgd++, addr = next, addr != end);
}

static void copy_pagetables(void)
{
	pgd_t *pgd = tmp_pg_dir + pgd_index(KASAN_SHADOW_START);

	memcpy(tmp_pg_dir, swapper_pg_dir, sizeof(tmp_pg_dir));

	/*
	 * If kasan shadow shares PGD with other mappings,
	 * clear_page_tables() will clear puds instead of pgd,
	 * so we need temporary pud table to keep early shadow mapped.
	 */
	if (PGDIR_SIZE > KASAN_SHADOW_END - KASAN_SHADOW_START) {
		pud_t *pud;
		pmd_t *pmd;
		pte_t *pte;

		memcpy(tmp_pud, pgd_page_vaddr(*pgd), sizeof(tmp_pud));

		pgd_populate(&init_mm, pgd, tmp_pud);
		pud = pud_offset(pgd, KASAN_SHADOW_START);
		pmd = pmd_offset(pud, KASAN_SHADOW_START);
		pud_populate(&init_mm, pud, pmd);
		pte = pte_offset_kernel(pmd, KASAN_SHADOW_START);
		pmd_populate_kernel(&init_mm, pmd, pte);
	}
}

static void __init cpu_set_ttbr1(unsigned long ttbr1)
{
	asm(
	"	msr	ttbr1_el1, %0\n"
	"	isb"
	:
	: "r" (ttbr1));
}

void __init kasan_init(void)
{
	struct memblock_region *reg;

	/*
	 * We are going to perform proper setup of shadow memory.
	 * At first we should unmap early shadow (clear_page_tables()).
	 * However, instrumented code couldn't execute without shadow memory.
	 * tmp_pg_dir used to keep early shadow mapped until full shadow
	 * setup will be finished.
	 */
	copy_pagetables();
	cpu_set_ttbr1(__pa(tmp_pg_dir));
	flush_tlb_all();

	clear_page_tables(KASAN_SHADOW_START, KASAN_SHADOW_END);

	kasan_populate_zero_shadow((void *)KASAN_SHADOW_START,
			kasan_mem_to_shadow((void *)MODULES_VADDR));

	for_each_memblock(memory, reg) {
		void *start = (void *)__phys_to_virt(reg->base);
		void *end = (void *)__phys_to_virt(reg->base + reg->size);

		if (start >= end)
			break;

		/*
		 * end + 1 here is intentional. We check several shadow bytes in
		 * advance to slightly speed up fastpath. In some rare cases
		 * we could cross boundary of mapped shadow, so we just map
		 * some more here.
		 */
		vmemmap_populate((unsigned long)kasan_mem_to_shadow(start),
				(unsigned long)kasan_mem_to_shadow(end) + 1,
				pfn_to_nid(virt_to_pfn(start)));
	}

	memset(kasan_zero_page, 0, PAGE_SIZE);
	cpu_set_ttbr1(__pa(swapper_pg_dir));
	flush_tlb_all();

	/* At this point kasan is fully initialized. Enable error messages */
	init_task.kasan_depth = 0;
	pr_info("KernelAddressSanitizer initialized\n");
}
