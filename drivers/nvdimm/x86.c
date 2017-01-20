/*
 * Copyright(c) 2015 - 2017 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/uio.h>
#include <linux/uaccess.h>
#include <linux/highmem.h>
#include <asm/cacheflush.h>
#include <asm/cpufeature.h>
#include <asm/special_insns.h>

/**
 * arch_wb_cache_pmem - write back a cache range with CLWB
 * @vaddr:	virtual start address
 * @size:	number of bytes to write back
 *
 * Write back a cache range using the CLWB (cache line write back)
 * instruction.
 */
void arch_wb_cache_pmem(void *addr, size_t size)
{
	u16 x86_clflush_size = boot_cpu_data.x86_clflush_size;
	unsigned long clflush_mask = x86_clflush_size - 1;
	void *vend = addr + size;
	void *p;

	for (p = (void *)((unsigned long)addr & ~clflush_mask);
	     p < vend; p += x86_clflush_size)
		clwb(p);
}
EXPORT_SYMBOL_GPL(arch_wb_cache_pmem);

void arch_invalidate_pmem(void *addr, size_t size)
{
	clflush_cache_range(addr, size);
}
EXPORT_SYMBOL_GPL(arch_invalidate_pmem);

void __arch_memcpy_to_pmem(void *dst, void *src, unsigned size);

void arch_memcpy_to_pmem(void *dst, void *src, unsigned size)
{
	if (((unsigned long) dst | (unsigned long) src | size) & 7) {
		/* __arch_memcpy_to_pmem assumes 8-byte alignment */
		memcpy(dst, src, size);
		arch_wb_cache_pmem(dst, size);
		return;
	}
	__arch_memcpy_to_pmem(dst, src, size);
}
EXPORT_SYMBOL_GPL(arch_memcpy_to_pmem);

static int pmem_from_user(void *dst, const void __user *src, unsigned size)
{
	int rc = __copy_from_user_nocache(dst, src, size);

	/* 'nocache' does not guarantee 'writethrough'*/
	arch_wb_cache_pmem(dst, size);

	return rc;
}

static void pmem_from_page(char *to, struct page *page, size_t offset, size_t len)
{
	char *from = kmap_atomic(page);

	arch_memcpy_to_pmem(to, from + offset, len);
	kunmap_atomic(from);
}

size_t arch_copy_from_iter_pmem(void *addr, size_t bytes, struct iov_iter *i)
{
	return copy_from_iter_ops(addr, bytes, i, pmem_from_user, pmem_from_page,
			arch_memcpy_to_pmem);
}
EXPORT_SYMBOL_GPL(arch_copy_from_iter_pmem);
