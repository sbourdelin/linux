#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/init.h>
#include <linux/memory.h>
#include <linux/random.h>

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

#define TB_SHIFT 40

/*
 * Memory base and end randomization is based on different configurations.
 * We want as much space as possible to increase entropy available.
 */
static const unsigned long memory_rand_start = __PAGE_OFFSET_BASE;

#if defined(CONFIG_KASAN)
static const unsigned long memory_rand_end = KASAN_SHADOW_START;
#elif defined(CONFIG_X86_ESPFIX64)
static const unsigned long memory_rand_end = ESPFIX_BASE_ADDR;
#elif defined(CONFIG_EFI)
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

/* Describe each randomized memory sections in sequential order */
static struct kaslr_memory_region {
	unsigned long *base;
	unsigned short size_tb;
} kaslr_regions[] = {
	{ &page_offset_base, 64/* Maximum */ },
	{ &vmalloc_base, VMALLOC_SIZE_TB },
	{ &vmemmap_base, 1 },
};

/* Size in Terabytes + 1 hole */
static inline unsigned long get_padding(struct kaslr_memory_region *region)
{
	return ((unsigned long)region->size_tb + 1) << TB_SHIFT;
}

/* Initialize base and padding for each memory section randomized with KASLR */
void __init kernel_randomize_memory(void)
{
	size_t i;
	unsigned long addr = memory_rand_start;
	unsigned long padding, rand, mem_tb, page_offset_padding;
	struct rnd_state rnd_st;
	unsigned long remain_padding = memory_rand_end - memory_rand_start;

	if (!kaslr_enabled())
		return;

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
void __meminit kaslr_trampoline_init(void)
{
	unsigned long addr, next;
	pgd_t *pgd;
	pud_t *pud_page, *tr_pud_page;
	int i;

	/* If KASLR is disabled, default to the existing page table entry */
	if (!kaslr_enabled()) {
		trampoline_pgd_entry = init_level4_pgt[pgd_index(PAGE_OFFSET)];
		return;
	}

	tr_pud_page = alloc_low_page();
	set_pgd(&trampoline_pgd_entry, __pgd(_PAGE_TABLE | __pa(tr_pud_page)));

	addr = 0;
	pgd = pgd_offset_k((unsigned long)__va(addr));
	pud_page = (pud_t *) pgd_page_vaddr(*pgd);

	for (i = pud_index(addr); i < PTRS_PER_PUD; i++, addr = next) {
		pud_t *pud, *tr_pud;

		tr_pud = tr_pud_page + pud_index(addr);
		pud = pud_page + pud_index((unsigned long)__va(addr));
		next = (addr & PUD_MASK) + PUD_SIZE;

		/* Needed to copy pte or pud alike */
		BUILD_BUG_ON(sizeof(pud_t) != sizeof(pte_t));
		*tr_pud = *pud;
	}
}
