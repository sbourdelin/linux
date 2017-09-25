#ifndef _ASM_S390_PAGE_MEMOPS_H
#define _ASM_S390_PAGE_MEMOPS_H

#include <linux/mm_types.h>
#include <linux/highmem.h>
#include <asm/checksum.h>

static inline u32 calc_page_checksum(struct page *page)
{
	return csum_partial(page_address(page), PAGE_SIZE, 0);
}

static inline int memcmp_pages(struct page *page1, struct page *page2)
{
	return memcmp(page_address(page1), page_address(page2), PAGE_SIZE);
}

#endif
