#ifndef _LINUX_PFN_H_
#define _LINUX_PFN_H_

#ifndef __ASSEMBLY__
#include <linux/types.h>

/*
 * pfn_t: encapsulates a page-frame number that is optionally backed
 * by memmap (struct page).  Whether a pfn_t has a 'struct page'
 * backing is indicated by flags in the high bits of the value.
 */
typedef struct {
	u64 val;
} pfn_t;
#endif

#define PFN_ALIGN(x)	(((unsigned long)(x) + (PAGE_SIZE - 1)) & PAGE_MASK)
#define PFN_UP(x)	(((x) + PAGE_SIZE-1) >> PAGE_SHIFT)
#define PFN_DOWN(x)	((x) >> PAGE_SHIFT)
#define PFN_PHYS(x)	((phys_addr_t)(x) << PAGE_SHIFT)
#define PHYS_PFN(x)	((unsigned long)((x) >> PAGE_SHIFT))

#ifdef CONFIG_SPARSEMEM
#define PFN_SECTION_ALIGN_DOWN(x) SECTION_ALIGN_DOWN(x)
#define PFN_SECTION_ALIGN_UP(x) SECTION_ALIGN_UP(x)
#else
/*
 * In this case ZONE_DEVICE=n and we will disable 'pfn' device support,
 * but we still want pmem to compile.
 */
#define PFN_SECTION_ALIGN_DOWN(x) (x)
#define PFN_SECTION_ALIGN_UP(x) (x)
#endif

#define PHYS_SECTION_ALIGN_DOWN(x) PFN_PHYS(PFN_SECTION_ALIGN_DOWN(PHYS_PFN(x)))
#define PHYS_SECTION_ALIGN_UP(x) PFN_PHYS(PFN_SECTION_ALIGN_UP(PHYS_PFN(x)))

#endif
