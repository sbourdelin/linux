/*
 * DMA operations supporting pseudo-bypass for PHB3+
 *
 * Author: Russell Currey <ruscur@russell.cc>
 *
 * Copyright 2018 IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <linux/export.h>
#include <linux/memblock.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/hash.h>

#include <asm/pci-bridge.h>
#include <asm/ppc-pci.h>
#include <asm/pnv-pci.h>
#include <asm/tce.h>

#include "pci.h"

/* select and allocate a TCE using the bitmap */
static int dma_pseudo_bypass_select_tce(struct pnv_ioda_pe *pe, phys_addr_t addr)
{
	int tce;
	__be64 old, new;

	spin_lock(&pe->tce_alloc_lock);
	tce = bitmap_find_next_zero_area(pe->tce_bitmap,
					 pe->tce_count,
					 0,
					 1,
					 0);
	bitmap_set(pe->tce_bitmap, tce, 1);
	old = pe->tces[tce];
	new = cpu_to_be64(addr | TCE_PCI_READ | TCE_PCI_WRITE);
	pe->tces[tce] = new;
	pe_info(pe, "allocating TCE %i 0x%016llx (old 0x%016llx)\n",
		tce, new, old);
	spin_unlock(&pe->tce_alloc_lock);

	return tce;
}

/*
 * The tracking table for assigning TCEs has two entries per TCE.
 * - @entry1 contains the physical address and the smallest bit indicates
 *     if it's currently valid.
 * - @entry2 contains the DMA address returned in the upper 34 bits, and a
 *     refcount in the lower 30 bits.
 */
static dma_addr_t dma_pseudo_bypass_get_address(struct device *dev,
					    phys_addr_t addr)
{
	struct pci_dev *pdev = container_of(dev, struct pci_dev, dev);
	struct pci_controller *hose = pci_bus_to_host(pdev->bus);
	struct pnv_phb *phb = hose->private_data;
	struct pnv_ioda_pe *pe;
        u64 i, entry1, entry2, dma_prefix, tce, ret;
	u64 offset = addr & ((1 << phb->ioda.max_tce_order) - 1);

	pe = &phb->ioda.pe_array[pci_get_pdn(pdev)->pe_number];

	/* look through the tracking table for a free entry */
	for (i = 0; i < pe->tce_count; i++) {
		entry1 = pe->tce_tracker[i * 2];
		entry2 = pe->tce_tracker[i * 2 + 1];
		dma_prefix = entry2 >> 34;

		/* if the address is the same and the entry is valid */
		if (entry1 == ((addr - offset) | 1)) {
			/* all we need to do here is increment the refcount */
			ret = cmpxchg(&pe->tce_tracker[i * 2 + 1],
				      entry2, entry2 + 1);
			if (ret != entry2) {
				/* conflict, start looking again just in case */
				i--;
				continue;
			}
			return (dma_prefix << phb->ioda.max_tce_order) | offset;
		/* if the entry is invalid then we want to replace it */
		} else if (!(entry1 & 1)) {
			/* set the real address, note that it isn't valid yet */
			ret = cmpxchg(&pe->tce_tracker[i * 2],
				      entry1, (addr - offset));
			if (ret != entry1) {
				/* conflict, start looking again */
				i--;
				continue;
			}

			/* now we can allocate a TCE */
			tce = dma_pseudo_bypass_select_tce(pe, addr - offset);

			/* set new value, including TCE index and new refcount */
			ret = cmpxchg(&pe->tce_tracker[i * 2 + 1],
				      entry2, tce << 34 | 1);
			if (ret != entry2) {
				/*
				 * XXX In this case we need to throw out
				 * everything, including the TCE we just
				 * allocated.  For now, just leave it.
				 */
				i--;
				continue;
			}

			/* now set the valid bit */
			ret = cmpxchg(&pe->tce_tracker[i * 2],
				      (addr - offset), (addr - offset) | 1);
			if (ret != (addr - offset)) {
				/*
				 * XXX Same situation as above.  We'd probably
				 * want to null out entry2 as well.
				 */
				i--;
				continue;
			}
			return (tce << phb->ioda.max_tce_order) | offset;
		/* it's a valid entry but not ours, keep looking */
		} else {
			continue;
		}
	}
	/* If we get here, the table must be full, so error out. */
	return -1ULL;
}

/*
 * For the moment, unmapping just decrements the refcount and doesn't actually
 * remove the TCE.  This is because it's very likely that a previously allocated
 * TCE will be used again, and this saves having to invalidate it.
 *
 * TODO implement some kind of garbage collection that clears unused TCE entries
 * once the table reaches a certain size.
 */
static void dma_pseudo_bypass_unmap_address(struct device *dev, dma_addr_t dma_addr)
{
	struct pci_dev *pdev = container_of(dev, struct pci_dev, dev);
	struct pci_controller *hose = pci_bus_to_host(pdev->bus);
	struct pnv_phb *phb = hose->private_data;
	struct pnv_ioda_pe *pe;
	u64 i, entry1, entry2, dma_prefix, refcount;

	pe = &phb->ioda.pe_array[pci_get_pdn(pdev)->pe_number];

	for (i = 0; i < pe->tce_count; i++) {
		entry1 = pe->tce_tracker[i * 2];
		entry2 = pe->tce_tracker[i * 2 + 1];
		dma_prefix = entry2 >> 34;
		refcount = entry2 & ((1 << 30) - 1);

		/* look through entry2 until we find our address */
		if (dma_prefix == (dma_addr >> phb->ioda.max_tce_order)) {
			refcount--;
			cmpxchg(&pe->tce_tracker[i * 2 + 1], entry2, (dma_prefix << 34) | refcount);
			if (!refcount) {
				/*
				 * Here is where we would remove the valid bit
				 * from entry1, clear the entry in the TCE table
				 * and invalidate the TCE - but we want to leave
				 * them until the table fills up (for now).
				 */
			}
			break;
		}
	}
}

static int dma_pseudo_bypass_dma_supported(struct device *dev, u64 mask)
{
	/*
	 * Normally dma_supported() checks if the mask is capable of addressing
	 * all of memory.  Since we map physical memory in chunks that the
	 * device can address, the device will be able to address whatever it
	 * wants - just not all at once.
	 */
	return 1;
}

static void *dma_pseudo_bypass_alloc_coherent(struct device *dev,
					  size_t size,
					  dma_addr_t *dma_handle,
					  gfp_t flag,
					  unsigned long attrs)
{
	void *ret;
	struct page *page;
	int node = dev_to_node(dev);

	/* ignore region specifiers */
	flag &= ~(__GFP_HIGHMEM);

	page = alloc_pages_node(node, flag, get_order(size));
	if (page == NULL)
		return NULL;
	ret = page_address(page);
	memset(ret, 0, size);
	*dma_handle = dma_pseudo_bypass_get_address(dev, __pa(ret));

	return ret;
}

static void dma_pseudo_bypass_free_coherent(struct device *dev,
					 size_t size,
					 void *vaddr,
					 dma_addr_t dma_handle,
					 unsigned long attrs)
{
	free_pages((unsigned long)vaddr, get_order(size));
}

static int dma_pseudo_bypass_mmap_coherent(struct device *dev,
				       struct vm_area_struct *vma,
				       void *cpu_addr,
				       dma_addr_t handle,
				       size_t size,
				       unsigned long attrs)
{
	unsigned long pfn = page_to_pfn(virt_to_page(cpu_addr));

	return remap_pfn_range(vma, vma->vm_start,
			       pfn + vma->vm_pgoff,
			       vma->vm_end - vma->vm_start,
			       vma->vm_page_prot);
}

static inline dma_addr_t dma_pseudo_bypass_map_page(struct device *dev,
						struct page *page,
						unsigned long offset,
						size_t size,
						enum dma_data_direction dir,
						unsigned long attrs)
{
	BUG_ON(dir == DMA_NONE);

	/* XXX I don't know if this is necessary (or even desired) */
	if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC))
		__dma_sync_page(page, offset, size, dir);

	return dma_pseudo_bypass_get_address(dev, page_to_phys(page) + offset);
}

static inline void dma_pseudo_bypass_unmap_page(struct device *dev,
					 dma_addr_t dma_address,
					 size_t size,
					 enum dma_data_direction direction,
					 unsigned long attrs)
{
	dma_pseudo_bypass_unmap_address(dev, dma_address);
}


static int dma_pseudo_bypass_map_sg(struct device *dev, struct scatterlist *sgl,
			     int nents, enum dma_data_direction direction,
			     unsigned long attrs)
{
	struct scatterlist *sg;
	int i;


	for_each_sg(sgl, sg, nents, i) {
		sg->dma_address = dma_pseudo_bypass_get_address(dev, sg_phys(sg));
		sg->dma_length = sg->length;

		if (attrs & DMA_ATTR_SKIP_CPU_SYNC)
			continue;

		__dma_sync_page(sg_page(sg), sg->offset, sg->length, direction);
	}

	return nents;
}

static void dma_pseudo_bypass_unmap_sg(struct device *dev, struct scatterlist *sgl,
				int nents, enum dma_data_direction direction,
				unsigned long attrs)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i) {
		dma_pseudo_bypass_unmap_address(dev, sg->dma_address);
	}
}

static u64 dma_pseudo_bypass_get_required_mask(struct device *dev)
{
	/*
	 * there's no limitation on our end, the driver should just call
	 * set_mask() with as many bits as the device can address.
	 */
	return -1ULL;
}

static int dma_pseudo_bypass_mapping_error(struct device *dev, dma_addr_t dma_addr)
{
	return dma_addr == -1ULL;
}


const struct dma_map_ops dma_pseudo_bypass_ops = {
	.alloc				= dma_pseudo_bypass_alloc_coherent,
	.free				= dma_pseudo_bypass_free_coherent,
	.mmap				= dma_pseudo_bypass_mmap_coherent,
	.map_sg				= dma_pseudo_bypass_map_sg,
	.unmap_sg			= dma_pseudo_bypass_unmap_sg,
	.dma_supported			= dma_pseudo_bypass_dma_supported,
	.map_page			= dma_pseudo_bypass_map_page,
	.unmap_page			= dma_pseudo_bypass_unmap_page,
	.get_required_mask		= dma_pseudo_bypass_get_required_mask,
	.mapping_error			= dma_pseudo_bypass_mapping_error,
};
EXPORT_SYMBOL(dma_pseudo_bypass_ops);
