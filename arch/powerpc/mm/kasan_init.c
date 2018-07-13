// SPDX-License-Identifier: GPL-2.0

#include <linux/kasan.h>
#include <linux/printk.h>
#include <linux/memblock.h>
#include <linux/bootmem.h>
#include <asm/pgalloc.h>

void __init kasan_early_init(void)
{
	unsigned long addr = KASAN_SHADOW_START & PGDIR_MASK;
	unsigned long end = KASAN_SHADOW_END;
	unsigned long next;
	pmd_t *pmd = pmd_offset(pud_offset(pgd_offset_k(addr), addr), addr);
	int i;
	phys_addr_t pa = __pa(kasan_zero_page);

	for (i = 0; i < PTRS_PER_PTE; i++)
		kasan_zero_pte[i] = pfn_pte(pa >> PAGE_SHIFT, PAGE_KERNEL_RO);

	do {
		next = pgd_addr_end(addr, end);
		pmd_populate_kernel(&init_mm, pmd, kasan_zero_pte);
	} while (pmd++, addr = next, addr != end);

	pr_info("KASAN early init done\n");
}

static void __init kasan_init_region(struct memblock_region *reg)
{
	void *start = __va(reg->base);
	void *end = __va(reg->base + reg->size);
	unsigned long k_start, k_end, k_cur, k_next;
	pmd_t *pmd;

	if (start >= end)
		return;

	k_start = (unsigned long)kasan_mem_to_shadow(start);
	k_end = (unsigned long)kasan_mem_to_shadow(end);
	pmd = pmd_offset(pud_offset(pgd_offset_k(k_start), k_start), k_start);

	for (k_cur = k_start; k_cur != k_end; k_cur = k_next, pmd++) {
		k_next = pgd_addr_end(k_cur, k_end);
		if ((void*)pmd_page_vaddr(*pmd) == kasan_zero_pte) {
			pte_t *new = pte_alloc_one_kernel(&init_mm, k_cur);

			if (!new)
				panic("kasan: pte_alloc_one_kernel() failed");
			memcpy(new, kasan_zero_pte, PTE_TABLE_SIZE);
			pmd_populate_kernel(&init_mm, pmd, new);
		}
	};

	for (k_cur = k_start; k_cur < k_end; k_cur += PAGE_SIZE) {
		phys_addr_t pa = memblock_alloc(PAGE_SIZE, PAGE_SIZE);
		pte_t pte = pfn_pte(pa >> PAGE_SHIFT, PAGE_KERNEL);

		pmd = pmd_offset(pud_offset(pgd_offset_k(k_cur), k_cur), k_cur);
		pte_update(pte_offset_kernel(pmd, k_cur), ~0, pte_val(pte));
	}
	flush_tlb_kernel_range(k_start, k_end);
}

void __init kasan_init(void)
{
	struct memblock_region *reg;

	for_each_memblock(memory, reg)
		kasan_init_region(reg);

	pr_info("KASAN init done\n");
}
