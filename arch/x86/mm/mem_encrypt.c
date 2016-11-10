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

#include <linux/linkage.h>
#include <linux/init.h>
#include <linux/mm.h>

#include <asm/tlbflush.h>
#include <asm/fixmap.h>

extern pmdval_t early_pmd_flags;

/*
 * Since sme_me_mask is set early in the boot process it must reside in
 * the .data section so as not to be zeroed out when the .bss section is
 * later cleared.
 */
unsigned long sme_me_mask __section(.data) = 0;
EXPORT_SYMBOL_GPL(sme_me_mask);

/* Buffer used for early in-place encryption by BSP, no locking needed */
static char sme_early_buffer[PAGE_SIZE] __aligned(PAGE_SIZE);

/*
 * This routine does not change the underlying encryption setting of the
 * page(s) that map this memory. It assumes that eventually the memory is
 * meant to be accessed as encrypted but the contents are currently not
 * encrypted.
 */
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
		len = min_t(size_t, sizeof(sme_early_buffer), size);

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

		memcpy(sme_early_buffer, src, len);
		memcpy(dst, sme_early_buffer, len);

		early_memunmap(dst, len);
		early_memunmap(src, len);

		paddr += len;
		size -= len;
	}
}

/*
 * This routine does not change the underlying encryption setting of the
 * page(s) that map this memory. It assumes that eventually the memory is
 * meant to be accessed as not encrypted but the contents are currently
 * encrypted.
 */
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
		len = min_t(size_t, sizeof(sme_early_buffer), size);

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

		memcpy(sme_early_buffer, src, len);
		memcpy(dst, sme_early_buffer, len);

		early_memunmap(dst, len);
		early_memunmap(src, len);

		paddr += len;
		size -= len;
	}
}

void __init sme_early_init(void)
{
	unsigned int i;

	if (!sme_me_mask)
		return;

	early_pmd_flags |= sme_me_mask;

	__supported_pte_mask |= sme_me_mask;

	/* Update the protection map with memory encryption mask */
	for (i = 0; i < ARRAY_SIZE(protection_map); i++)
		protection_map[i] = __pgprot(pgprot_val(protection_map[i]) | sme_me_mask);
}
