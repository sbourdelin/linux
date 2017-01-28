/*
 * Copyright (c) 2014-2016, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */
#include "test/nfit_test.h"
#include <linux/blkdev.h>
#include <pmem.h>
#include <nd.h>

long __pmem_direct_access(struct pmem_device *pmem, phys_addr_t dev_addr,
		void **kaddr, pfn_t *pfn, long size)
{
	resource_size_t offset = dev_addr + pmem->data_offset;

	if (unlikely(is_bad_pmem(&pmem->bb, dev_addr / 512, size)))
		return -EIO;

	/*
	 * Limit dax to a single page at a time given vmalloc()-backed
	 * in the nfit_test case.
	 */
	if (get_nfit_res(pmem->phys_addr + offset)) {
		struct page *page;

		*kaddr = pmem->virt_addr + offset;
		page = vmalloc_to_page(pmem->virt_addr + offset);
		*pfn = page_to_pfn_t(page);
		pr_debug_ratelimited("%s: pmem: %p dev_addr: %pa pfn: %#lx\n",
				__func__, pmem, &dev_addr, page_to_pfn(page));

		return PAGE_SIZE;
	}

	*kaddr = pmem->virt_addr + offset;
	*pfn = phys_to_pfn_t(pmem->phys_addr + offset, pmem->pfn_flags);

	/*
	 * If badblocks are present, limit known good range to the
	 * requested range.
	 */
	if (unlikely(pmem->bb.count))
		return size;
	return pmem->size - pmem->pfn_pad - offset;
}
