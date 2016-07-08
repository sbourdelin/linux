/*
 * Based on arch/x86/include/asm/pmem.h
 *
 * Copyright(c) 2016 SK hynix Inc. Kwangwoo Lee <kwangwoo.lee@sk.com>
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
#ifndef __ASM_PMEM_H__
#define __ASM_PMEM_H__

#ifdef CONFIG_ARCH_HAS_PMEM_API
#include <linux/uaccess.h>
#include <asm/cacheflush.h>

/**
 * arch_memcpy_to_pmem - copy data to persistent memory
 * @dst: destination buffer for the copy
 * @src: source buffer for the copy
 * @n: length of the copy in bytes
 *
 * Copy data to persistent memory media. if ARCH_HAS_PMEM_API is defined,
 * then MEMREMAP_WB is used to memremap() during probe. A subsequent
 * arch_wmb_pmem() need to guarantee durability.
 */
static inline void arch_memcpy_to_pmem(void __pmem *dst, const void *src,
		size_t n)
{
	int unwritten;

	unwritten = __copy_from_user_inatomic((void __force *) dst,
			(void __user *) src, n);
	if (WARN(unwritten, "%s: fault copying %p <- %p unwritten: %d\n",
				__func__, dst, src, unwritten))
		BUG();

	__flush_dcache_area(dst, n);
}

static inline int arch_memcpy_from_pmem(void *dst, const void __pmem *src,
		size_t n)
{
	memcpy(dst, (void __force *) src, n);
	return 0;
}

/**
 * arch_wmb_pmem - synchronize writes to persistent memory
 *
 * After a series of arch_memcpy_to_pmem() operations this need to be called to
 * ensure that written data is durable on persistent memory media.
 */
static inline void arch_wmb_pmem(void)
{
	/*
	 * We've already arranged for pmem writes to avoid the cache in
	 * arch_memcpy_to_pmem()
	 */
	wmb();

	/*
	 * pcommit_sfence() on X86 has been removed and will be replaced with
	 * a function after ARMv8.2 which will support DC CVAP to ensure
	 * Point-of-Persistency. Until then, mark here with a comment to keep
	 * the point for __clean_dcache_area_pop().
	 */
}

/**
 * arch_wb_cache_pmem - write back a cache range
 * @vaddr:	virtual start address
 * @size:	number of bytes to write back
 *
 * Write back a cache range. Leave data in cache for performance of next access.
 * This function requires explicit ordering with an arch_wmb_pmem() call.
 */
static inline void arch_wb_cache_pmem(void __pmem *addr, size_t size)
{
	/* cache clean PoC */
	__clean_dcache_area(addr, size);
}

/**
 * arch_copy_from_iter_pmem - copy data from an iterator to PMEM
 * @addr:	PMEM destination address
 * @bytes:	number of bytes to copy
 * @i:		iterator with source data
 *
 * Copy data from the iterator 'i' to the PMEM buffer starting at 'addr'.
 * This function requires explicit ordering with an arch_wmb_pmem() call.
 */
static inline size_t arch_copy_from_iter_pmem(void __pmem *addr, size_t bytes,
		struct iov_iter *i)
{
	void *vaddr = (void __force *)addr;
	size_t len;

	/*
	 * ARCH_HAS_NOCACHE_UACCESS is not defined and the default mapping is
	 * MEMREMAP_WB. Instead of using copy_from_iter_nocache(), use cacheable
	 * version and call arch_wb_cache_pmem().
	 */
	len = copy_from_iter(vaddr, bytes, i);

	arch_wb_cache_pmem(addr, bytes);

	return len;
}

/**
 * arch_clear_pmem - zero a PMEM memory range
 * @addr:	virtual start address
 * @size:	number of bytes to zero
 *
 * Write zeros into the memory range starting at 'addr' for 'size' bytes.
 * This function requires explicit ordering with an arch_wmb_pmem() call.
 */
static inline void arch_clear_pmem(void __pmem *addr, size_t size)
{
	void *vaddr = (void __force *)addr;

	memset(vaddr, 0, size);
	arch_wb_cache_pmem(addr, size);
}

/**
 * arch_invalidate_pmem - invalidate a PMEM memory range
 * @addr:	virtual start address
 * @size:	number of bytes to zero
 *
 * After finishing ARS(Address Range Scrubbing), clean and invalidate the
 * address range.
 */
static inline void arch_invalidate_pmem(void __pmem *addr, size_t size)
{
	__flush_dcache_area(addr, size);
}

static inline bool __arch_has_wmb_pmem(void)
{
	/* return false until arch_wmb_pmem() guarantee PoP on ARMv8.2. */
	return false;
}
#endif /* CONFIG_ARCH_HAS_PMEM_API */
#endif /* __ASM_PMEM_H__ */
