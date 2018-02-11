/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_POWERPC_NOHASH_PGALLOC_H
#define _ASM_POWERPC_NOHASH_PGALLOC_H

#include <linux/mm.h>

extern void tlb_remove_table(struct mmu_gather *tlb, void *table);
#ifdef CONFIG_PPC64
extern void tlb_flush_pgtable(struct mmu_gather *tlb, unsigned long address);
#else
/* 44x etc which is BOOKE not BOOK3E */
static inline void tlb_flush_pgtable(struct mmu_gather *tlb,
				     unsigned long address)
{

}
#endif /* !CONFIG_PPC_BOOK3E */

#ifdef CONFIG_PPC64
#include <asm/nohash/64/pgalloc.h>
#else
#include <asm/nohash/32/pgalloc.h>
#endif

static inline void pgd_ctor(void *addr)
{
	memset(addr, 0, PGD_TABLE_SIZE);
}

static inline void pud_ctor(void *addr)
{
	memset(addr, 0, PUD_TABLE_SIZE);
}

static inline void pmd_ctor(void *addr)
{
	memset(addr, 0, PMD_TABLE_SIZE);
}

#endif /* _ASM_POWERPC_NOHASH_PGALLOC_H */
