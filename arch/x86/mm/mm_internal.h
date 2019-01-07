/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __X86_MM_INTERNAL_H
#define __X86_MM_INTERNAL_H

void *alloc_low_pages(unsigned int num);
static inline void *alloc_low_page(void)
{
	return alloc_low_pages(1);
}

unsigned long __init init_range_memory_mapping(unsigned long r_start,
	unsigned long r_end);
void set_alloc_range(unsigned long low, unsigned long high);
void __init memory_map_top_down(unsigned long map_start,
				       unsigned long map_end);
void __init memory_map_bottom_up(unsigned long map_start,
					unsigned long map_end);
void early_ioremap_page_table_range_init(void);

unsigned long kernel_physical_mapping_init(unsigned long start,
					     unsigned long end,
					     unsigned long page_size_mask);
void zone_sizes_init(void);

extern int after_bootmem;

void update_cache_mode_entry(unsigned entry, enum page_cache_mode cache);

#endif	/* __X86_MM_INTERNAL_H */
