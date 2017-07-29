/*
 * Copyright 2017 Balbir Singh, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 * Author: Balbir Singh <bsingharora@gmail.com>
 */
#ifndef __ASM_BOOK3S_64_RADIX_KASAN_H
#define __ASM_BOOK3S_64_RADIX_KASAN_H

#ifndef __ASSEMBLY__

#define ARCH_DEFINES_KASAN_ZERO_PTE

#define RADIX_PTRS_PER_PTE	(1 << RADIX_PTE_INDEX_SIZE)
#define RADIX_PTRS_PER_PMD	(1 << RADIX_PMD_INDEX_SIZE)
#define RADIX_PTRS_PER_PUD	(1 << RADIX_PUD_INDEX_SIZE)
extern pte_t kasan_zero_pte[RADIX_PTRS_PER_PTE];
extern pmd_t kasan_zero_pmd[RADIX_PTRS_PER_PMD];
extern pud_t kasan_zero_pud[RADIX_PTRS_PER_PUD];

#include <asm/book3s/64/radix.h>

/*
 * KASAN_SHADOW_START: beginning at the end of IO region
 * KASAN_SHADOW_END: KASAN_SHADOW_START + 1/8 of kernel virtual addresses.
 */
#define KASAN_SHADOW_START      (IOREMAP_END)
#define KASAN_SHADOW_END        (KASAN_SHADOW_START + (KERN_VIRT_SIZE << 1))

/*
 * This value is used to map an address to the corresponding shadow
 * address by the following formula:
 *     shadow_addr = (address >> 3) + KASAN_SHADOW_OFFSET;
 *
 */
#define KASAN_SHADOW_OFFSET     (KASAN_SHADOW_START - (PAGE_OFFSET / 8))

#ifdef CONFIG_KASAN
void kasan_init(void);

extern struct static_key_false powerpc_kasan_enabled_key;
#define check_return_arch_not_ready() \
	do {								\
		if (!static_branch_likely(&powerpc_kasan_enabled_key))	\
			return;						\
	} while (0)


#else
static inline void kasan_init(void) { }
#endif

#endif
#endif /* __ASM_BOOK3S_64_RADIX_KASAN_H */
