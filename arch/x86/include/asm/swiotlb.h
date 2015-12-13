#ifndef _ASM_X86_SWIOTLB_H
#define _ASM_X86_SWIOTLB_H

#include <linux/swiotlb.h>

#ifdef CONFIG_SWIOTLB
extern int swiotlb;
extern int __init pci_swiotlb_detect_override(void);
extern int __init pci_swiotlb_detect_4gb(void);
extern void __init pci_swiotlb_init(void);
extern void __init pci_swiotlb_late_init(void);
#else
#define swiotlb 0
static inline int pci_swiotlb_detect_override(void)
{
	return 0;
}
static inline int pci_swiotlb_detect_4gb(void)
{
	return 0;
}
static inline void pci_swiotlb_init(void)
{
}
static inline void pci_swiotlb_late_init(void)
{
}
#endif

static inline void dma_mark_clean(void *addr, size_t size) {}

/*
 * Make certain that the pages get marked as dirty
 * now that the device has completed the DMA transaction.
 *
 * Without this we run the risk of a guest migration missing
 * the pages that the device has written to as they are not
 * tracked as a part of the dirty page tracking.
 */
static inline void dma_mark_dirty(void *addr, size_t size)
{
#ifdef CONFIG_SWIOTLB_PAGE_DIRTYING
	unsigned long pg_addr, start;

	start = (unsigned long)addr;
	pg_addr = PAGE_ALIGN(start + size);
	start &= ~(sizeof(atomic_t) - 1);

	/* trigger a write fault on each page, excluding first page */
	while ((pg_addr -= PAGE_SIZE) > start)
		atomic_add(0, (atomic_t *)pg_addr);

	/* trigger a write fault on first word of DMA */
	atomic_add(0, (atomic_t *)start);
#endif /* CONFIG_SWIOTLB_PAGE_DIRTYING */
}

extern void *x86_swiotlb_alloc_coherent(struct device *hwdev, size_t size,
					dma_addr_t *dma_handle, gfp_t flags,
					struct dma_attrs *attrs);
extern void x86_swiotlb_free_coherent(struct device *dev, size_t size,
					void *vaddr, dma_addr_t dma_addr,
					struct dma_attrs *attrs);

#endif /* _ASM_X86_SWIOTLB_H */
