#ifndef _LINUX_PFN_T_H_
#define _LINUX_PFN_T_H_
#include <linux/mm.h>

/*
 * PFN_FLAGS_MASK - mask of all the possible valid pfn_t flags
 * PFN_SG_CHAIN - pfn is a pointer to the next scatterlist entry
 * PFN_SG_LAST - pfn references a page and is the last scatterlist entry
 * PFN_DEV - pfn is not covered by system memmap by default
 * PFN_MAP - pfn has a dynamic page mapping established by a device driver
 *
 * Note that the bottom two bits in the pfn_t match the bottom two bits in the
 * scatterlist so sg_is_chain() and sg_is_last() work.  These bits are also
 * used by the radix tree for its own purposes, but a PFN cannot be in both a
 * radix tree and a scatterlist simultaneously.  If a PFN is moved between the
 * two usages, care should be taken to clear/set these bits appropriately.
 */
#define PFN_FLAG_BITS	4
#define PFN_FLAGS_MASK	((1 << PFN_FLAG_BITS) - 1)
#define __PFN_MAX	((1 << (BITS_PER_LONG - PFN_FLAG_BITS)) - 1)
#define PFN_SG_CHAIN	0x01UL
#define PFN_SG_LAST	0x02UL
#define PFN_SG_MASK	(PFN_SG_CHAIN | PFN_SG_LAST)
#define PFN_DEV		0x04UL
#define PFN_MAP		0x08UL

#if 0
#define PFN_T_BUG_ON(x)	BUG_ON(x)
#else
#define PFN_T_BUG_ON(x)	BUILD_BUG_ON_INVALID(x)
#endif

static inline pfn_t __pfn_to_pfn_t(unsigned long pfn, u64 flags)
{
	pfn_t pfn_t = { .val = (pfn << PFN_FLAG_BITS) | flags };

	PFN_T_BUG_ON(pfn & ~__PFN_MAX);
	PFN_T_BUG_ON(flags & ~PFN_FLAGS_MASK);

	return pfn_t;
}

/* a default pfn to pfn_t conversion assumes that @pfn is pfn_valid() */
static inline pfn_t pfn_to_pfn_t(unsigned long pfn)
{
	return __pfn_to_pfn_t(pfn, 0);
}

static inline unsigned long pfn_t_to_pfn(pfn_t pfn)
{
	unsigned long v = pfn.val;
	return v >> PFN_FLAG_BITS;
}

extern pfn_t phys_to_pfn_t(phys_addr_t addr, u64 flags);

static inline bool pfn_t_has_page(pfn_t pfn)
{
	return (pfn.val & PFN_MAP) == PFN_MAP || (pfn.val & PFN_DEV) == 0;
}

static inline struct page *pfn_t_to_page(pfn_t pfn)
{
	if (pfn_t_has_page(pfn))
		return pfn_to_page(pfn_t_to_pfn(pfn));
	return NULL;
}

static inline phys_addr_t pfn_t_to_phys(pfn_t pfn)
{
	return PFN_PHYS(pfn_t_to_pfn(pfn));
}

static inline void *pfn_t_to_virt(pfn_t pfn)
{
	if (pfn_t_has_page(pfn))
		return __va(pfn_t_to_phys(pfn));
	return NULL;
}

static inline pfn_t page_to_pfn_t(struct page *page)
{
	return pfn_to_pfn_t(page_to_pfn(page));
}

static inline int pfn_t_valid(pfn_t pfn)
{
	return pfn_valid(pfn_t_to_pfn(pfn));
}

#ifdef CONFIG_MMU
static inline pte_t pfn_t_pte(pfn_t pfn, pgprot_t pgprot)
{
	return pfn_pte(pfn_t_to_pfn(pfn), pgprot);
}
#endif

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static inline pmd_t pfn_t_pmd(pfn_t pfn, pgprot_t pgprot)
{
	return pfn_pmd(pfn_t_to_pfn(pfn), pgprot);
}
#endif

#ifdef __HAVE_ARCH_PTE_DEVMAP
static inline bool pfn_t_devmap(pfn_t pfn)
{
	const u64 flags = PFN_DEV|PFN_MAP;

	return (pfn.val & flags) == flags;
}
#else
static inline bool pfn_t_devmap(pfn_t pfn)
{
	return false;
}
pte_t pte_mkdevmap(pte_t pte);
pmd_t pmd_mkdevmap(pmd_t pmd);
#endif
#endif /* _LINUX_PFN_T_H_ */
