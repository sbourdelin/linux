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
#ifndef __ASM_ARM64_PMEM_H__
#define __ASM_ARM64_PMEM_H__

#include <linux/uaccess.h>
#include <asm/cacheflush.h>

#ifdef CONFIG_ARCH_HAS_PMEM_API
/**
 * arch_memcpy_to_pmem - copy data to persistent memory
 * @dst: destination buffer for the copy
 * @src: source buffer for the copy
 * @n: length of the copy in bytes
 *
 * Copy data to persistent memory media via non-temporal stores so that
 * a subsequent arch_wmb_pmem() can flush cpu and memory controller
 * write buffers to guarantee durability.
 */
static inline void arch_memcpy_to_pmem(void __pmem *dst, const void *src,
		size_t n)
{
	int unwritten;

	/*
	 * We are copying between two kernel buffers, if
	 * __copy_from_user_inatomic_nocache() returns an error (page
	 * fault) we would have already reported a general protection fault
	 * before the WARN+BUG.
	 */
	unwritten = __copy_from_user_inatomic_nocache((void __force *) dst,
			(void __user *) src, n);
	if (WARN(unwritten, "%s: fault copying %p <- %p unwritten: %d\n",
				__func__, dst, src, unwritten))
		BUG();
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
 * After a series of arch_memcpy_to_pmem() operations this drains data
 * from cpu write buffers and any platform (memory controller) buffers
 * to ensure that written data is durable on persistent memory media.
 */
static inline void arch_wmb_pmem(void)
{
	/*
	 * PCOMMIT instruction only exists on x86. So pcommit_sfence() has been
	 * removed after wmb(). Note, that we've already arranged for pmem
	 * writes to avoid the cache via arch_memcpy_to_pmem().
	 */
	wmb();
}

/**
 * arch_wb_cache_pmem - write back a cache range
 * @vaddr:	virtual start address
 * @size:	number of bytes to write back
 *
 * Write back a cache range. This function requires explicit ordering with an
 * arch_wmb_pmem() call.
 */
static inline void arch_wb_cache_pmem(void __pmem *addr, size_t size)
{
	__clean_dcache_area_pou(addr, size);
}

/*
 * copy_from_iter_nocache() only uses non-temporal stores for iovec iterators,
 * so for other types (bvec & kvec) we must do a cache write-back.
 */
static inline bool __iter_needs_pmem_wb(struct iov_iter *i)
{
	return iter_is_iovec(i) == false;
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

	/* TODO: skip the write-back by always using non-temporal stores */
	len = copy_from_iter_nocache(vaddr, bytes, i);

	if (__iter_needs_pmem_wb(i))
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

static inline void arch_invalidate_pmem(void __pmem *addr, size_t size)
{
	/* barrier before clean and invalidate */
	mb();

	__flush_dcache_area(addr, size);
}

static inline bool __arch_has_wmb_pmem(void)
{
	return true;
}
#endif /* CONFIG_ARCH_HAS_PMEM_API */
#endif /* __ASM_ARM64_PMEM_H__ */
