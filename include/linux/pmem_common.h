#ifndef __PMEM_COMMON_H__
#define __PMEM_COMMON_H__

#include <linux/badblocks.h>
#include <linux/types.h>
#include <linux/pfn_t.h>
#include <linux/fs.h>
#include <linux/pfn_t.h>
#include <linux/memremap.h>
#include <linux/vmalloc.h>
#include <linux/mmzone.h>
#include <linux/dax.h>
#include <linux/highmem.h>
#include <linux/blkdev.h>

static void write_pmem(void *pmem_addr, struct page *page,
	unsigned int off, unsigned int len)
{
	void *mem = kmap_atomic(page);

	memcpy_flushcache(pmem_addr, mem + off, len);
	kunmap_atomic(mem);
}

static blk_status_t read_pmem(struct page *page, unsigned int off,
	void *pmem_addr, unsigned int len)
{
	int rc;
	void *mem = kmap_atomic(page);

	rc = memcpy_mcsafe(mem + off, pmem_addr, len);
	kunmap_atomic(mem);
	if (rc)
		return BLK_STS_IOERR;
	return BLK_STS_OK;
}

#endif /* __PMEM_COMMON_H__ */

#ifdef CONFIG_ARCH_HAS_PMEM_API
#define ARCH_MEMREMAP_PMEM MEMREMAP_WB
void arch_wb_cache_pmem(void *addr, size_t size);
void arch_invalidate_pmem(void *addr, size_t size);
#else
#define ARCH_MEMREMAP_PMEM MEMREMAP_WT
static inline void arch_wb_cache_pmem(void *addr, size_t size)
{
}
static inline void arch_invalidate_pmem(void *addr, size_t size)
{
}
#endif
