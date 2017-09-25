#ifndef _ASM_GENERIC_PAGE_MEMOPS_H
#define _ASM_GENERIC_PAGE_MEMOPS_H

#include <linux/mm_types.h>
#include <linux/highmem.h>
#include <linux/jhash.h>

static inline u32 calc_page_checksum(struct page *page)
{
	void *addr = kmap_atomic(page);
	u32 checksum;

	checksum = jhash2(addr, PAGE_SIZE / 4, 17);
	kunmap_atomic(addr);
	return checksum;
}

static inline int memcmp_pages(struct page *page1, struct page *page2)
{
	char *addr1, *addr2;
	int ret;

	addr1 = kmap_atomic(page1);
	addr2 = kmap_atomic(page2);
	ret = memcmp(addr1, addr2, PAGE_SIZE);
	kunmap_atomic(addr2);
	kunmap_atomic(addr1);
	return ret;
}

#endif
