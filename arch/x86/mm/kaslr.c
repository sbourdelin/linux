#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/init.h>
#include <linux/memory.h>
#include <linux/random.h>
#include <xen/xen.h>

#include <asm/processor.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/e820.h>
#include <asm/init.h>
#include <asm/setup.h>
#include <asm/kaslr.h>
#include <asm/kasan.h>

#include "mm_internal.h"

/* Hold the pgd entry used on booting additional CPUs */
pgd_t trampoline_pgd_entry;

static const unsigned long memory_rand_start = __PAGE_OFFSET_BASE;

#if defined(CONFIG_KASAN)
static const unsigned long memory_rand_end = KASAN_SHADOW_START;
#elfif defined(CONFIG_X86_ESPFIX64)
static const unsigned long memory_rand_end = ESPFIX_BASE_ADDR;
#elfif defined(CONFIG_EFI)
static const unsigned long memory_rand_end = EFI_VA_START;
#else
static const unsigned long memory_rand_end = __START_KERNEL_map;
#endif

/* Default values */
unsigned long page_offset_base = __PAGE_OFFSET_BASE;
EXPORT_SYMBOL(page_offset_base);
unsigned long vmalloc_base = __VMALLOC_BASE;
EXPORT_SYMBOL(vmalloc_base);
unsigned long vmemmap_base = __VMEMMAP_BASE;
EXPORT_SYMBOL(vmemmap_base);

static struct kaslr_memory_region {
	unsigned long *base;
	unsigned short size_tb;
} kaslr_regions[] = {
	{ &page_offset_base, 64/* Maximum */ },
	{ &vmalloc_base, VMALLOC_SIZE_TB },
	{ &vmemmap_base, 1 },
};

#define TB_SHIFT 40

/* Size in Terabytes + 1 hole */
static inline unsigned long get_padding(struct kaslr_memory_region *region)
{
	return ((unsigned long)region->size_tb + 1) << TB_SHIFT;
}

void __init kernel_randomize_memory(void)
{
	size_t i;
	unsigned long addr = memory_rand_start;
	unsigned long padding, rand, mem_tb, page_offset_padding;
	struct rnd_state rnd_st;
	unsigned long remain_padding = memory_rand_end - memory_rand_start;

	if (!kaslr_enabled())
		return;

	/* Take the additional space when Xen is not active. */
	if (!xen_domain())
		page_offset_base -= __XEN_SPACE;

	/*
	 * Update Physical memory mapping to available and
	 * add padding if needed (especially for memory hotplug support).
	 */
	page_offset_padding = CONFIG_RANDOMIZE_MEMORY_PHYSICAL_PADDING;

#ifdef CONFIG_MEMORY_HOTPLUG
	page_offset_padding = max(1UL, page_offset_padding);
#endif

	BUG_ON(kaslr_regions[0].base != &page_offset_base);
	mem_tb = ((max_pfn << PAGE_SHIFT) >> TB_SHIFT) + page_offset_padding;

	if (mem_tb < kaslr_regions[0].size_tb)
		kaslr_regions[0].size_tb = mem_tb;

	for (i = 0; i < ARRAY_SIZE(kaslr_regions); i++)
		remain_padding -= get_padding(&kaslr_regions[i]);

	prandom_seed_state(&rnd_st, kaslr_get_random_boot_long());

	/* Position each section randomly with minimum 1 terabyte between */
	for (i = 0; i < ARRAY_SIZE(kaslr_regions); i++) {
		padding = remain_padding / (ARRAY_SIZE(kaslr_regions) - i);
		prandom_bytes_state(&rnd_st, &rand, sizeof(rand));
		padding = (rand % (padding + 1)) & PUD_MASK;
		addr += padding;
		*kaslr_regions[i].base = addr;
		addr += get_padding(&kaslr_regions[i]);
		remain_padding -= padding;
	}
}

/*
 * Create PGD aligned trampoline table to allow real mode initialization
 * of additional CPUs. Consume only 1 additonal low memory page.
 */
void __meminit kaslr_trampoline_init(unsigned long page_size_mask)
{
	unsigned long addr, next, end;
	pgd_t *pgd;
	pud_t *pud_page, *tr_pud_page;
	int i;

	if (!kaslr_enabled()) {
		trampoline_pgd_entry = init_level4_pgt[pgd_index(PAGE_OFFSET)];
		return;
	}

	tr_pud_page = alloc_low_page();
	set_pgd(&trampoline_pgd_entry, __pgd(_PAGE_TABLE | __pa(tr_pud_page)));

	addr = 0;
	end = ISA_END_ADDRESS;
	pgd = pgd_offset_k((unsigned long)__va(addr));
	pud_page = (pud_t *) pgd_page_vaddr(*pgd);

	for (i = pud_index(addr); i < PTRS_PER_PUD; i++, addr = next) {
		pud_t *pud, *tr_pud;
		pmd_t *pmd;

		tr_pud = tr_pud_page + pud_index(addr);
		pud = pud_page + pud_index((unsigned long)__va(addr));
		next = (addr & PUD_MASK) + PUD_SIZE;

		if (addr >= end || !pud_val(*pud)) {
			if (!after_bootmem &&
			    !e820_any_mapped(addr & PUD_MASK, next, E820_RAM) &&
			    !e820_any_mapped(addr & PUD_MASK, next,
					    E820_RESERVED_KERN))
				set_pud(tr_pud, __pud(0));
			continue;
		}

		if (page_size_mask & (1<<PG_LEVEL_1G)) {
			set_pte((pte_t *)tr_pud,
				pfn_pte((__pa(addr) & PUD_MASK) >> PAGE_SHIFT,
					PAGE_KERNEL_LARGE));
			continue;
		}

		pmd = pmd_offset(pud, 0);
		set_pud(tr_pud, __pud(_PAGE_TABLE | __pa(pmd)));
	}
}
