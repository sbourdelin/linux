/*
 * AMD Memory Encryption Support
 *
 * Copyright (C) 2016 Advanced Micro Devices, Inc.
 *
 * Author: Tom Lendacky <thomas.lendacky@amd.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/swiotlb.h>

#include <asm/mem_encrypt.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/fixmap.h>

/* Buffer used for early in-place encryption by BSP, no locking needed */
static char me_early_buffer[PAGE_SIZE] __aligned(PAGE_SIZE);

int sme_set_mem_enc(void *vaddr, unsigned long size)
{
	unsigned long addr, numpages;

	if (!sme_me_mask)
		return 0;

	addr = (unsigned long)vaddr & PAGE_MASK;
	numpages = PAGE_ALIGN(size) >> PAGE_SHIFT;

	/*
	 * The set_memory_xxx functions take an integer for numpages, make
	 * sure it doesn't exceed that.
	 */
	if (numpages > INT_MAX)
		return -EINVAL;

	return set_memory_enc(addr, numpages);
}
EXPORT_SYMBOL_GPL(sme_set_mem_enc);

int sme_set_mem_dec(void *vaddr, unsigned long size)
{
	unsigned long addr, numpages;

	if (!sme_me_mask)
		return 0;

	addr = (unsigned long)vaddr & PAGE_MASK;
	numpages = PAGE_ALIGN(size) >> PAGE_SHIFT;

	/*
	 * The set_memory_xxx functions take an integer for numpages, make
	 * sure it doesn't exceed that.
	 */
	if (numpages > INT_MAX)
		return -EINVAL;

	return set_memory_dec(addr, numpages);
}
EXPORT_SYMBOL_GPL(sme_set_mem_dec);

void __init sme_early_mem_enc(resource_size_t paddr, unsigned long size)
{
	void *src, *dst;
	size_t len;

	if (!sme_me_mask)
		return;

	local_flush_tlb();
	wbinvd();

	/*
	 * There are limited number of early mapping slots, so map (at most)
	 * one page at time.
	 */
	while (size) {
		len = min_t(size_t, sizeof(me_early_buffer), size);

		/* Create a mapping for non-encrypted write-protected memory */
		src = early_memremap_dec_wp(paddr, len);

		/* Create a mapping for encrypted memory */
		dst = early_memremap_enc(paddr, len);

		/*
		 * If a mapping can't be obtained to perform the encryption,
		 * then encrypted access to that area will end up causing
		 * a crash.
		 */
		BUG_ON(!src || !dst);

		memcpy(me_early_buffer, src, len);
		memcpy(dst, me_early_buffer, len);

		early_memunmap(dst, len);
		early_memunmap(src, len);

		paddr += len;
		size -= len;
	}
}

void __init sme_early_mem_dec(resource_size_t paddr, unsigned long size)
{
	void *src, *dst;
	size_t len;

	if (!sme_me_mask)
		return;

	local_flush_tlb();
	wbinvd();

	/*
	 * There are limited number of early mapping slots, so map (at most)
	 * one page at time.
	 */
	while (size) {
		len = min_t(size_t, sizeof(me_early_buffer), size);

		/* Create a mapping for encrypted write-protected memory */
		src = early_memremap_enc_wp(paddr, len);

		/* Create a mapping for non-encrypted memory */
		dst = early_memremap_dec(paddr, len);

		/*
		 * If a mapping can't be obtained to perform the decryption,
		 * then un-encrypted access to that area will end up causing
		 * a crash.
		 */
		BUG_ON(!src || !dst);

		memcpy(me_early_buffer, src, len);
		memcpy(dst, me_early_buffer, len);

		early_memunmap(dst, len);
		early_memunmap(src, len);

		paddr += len;
		size -= len;
	}
}

void __init *sme_early_memremap(resource_size_t paddr,
				unsigned long size)
{
	return early_memremap_dec(paddr, size);
}

void __init sme_early_init(void)
{
	unsigned int i;

	if (!sme_me_mask)
		return;

	__supported_pte_mask |= sme_me_mask;

	/* Update the protection map with memory encryption mask */
	for (i = 0; i < ARRAY_SIZE(protection_map); i++)
		protection_map[i] = __pgprot(pgprot_val(protection_map[i]) | sme_me_mask);
}

/* Architecture __weak replacement functions */
void __init mem_encrypt_init(void)
{
	if (!sme_me_mask)
		return;

	/* Make SWIOTLB use an unencrypted DMA area */
	swiotlb_clear_encryption();
}

unsigned long swiotlb_get_me_mask(void)
{
	return sme_me_mask;
}

void swiotlb_set_mem_dec(void *vaddr, unsigned long size)
{
	sme_set_mem_dec(vaddr, size);
}

void __init *efi_me_early_memremap(resource_size_t paddr,
				   unsigned long size)
{
	return sme_early_memremap(paddr, size);
}
