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
#include <asm/setup.h>
#include <asm/bootparam.h>
#include <asm/cacheflush.h>

extern pmdval_t early_pmd_flags;
int __init __early_make_pgtable(unsigned long, pmdval_t);

/*
 * Since sme_me_mask is set early in the boot process it must reside in
 * the .data section so as not to be zeroed out when the .bss section is
 * later cleared.
 */
unsigned long sme_me_mask __section(.data) = 0;
EXPORT_SYMBOL_GPL(sme_me_mask);

/* Buffer used for early in-place encryption by BSP, no locking needed */
static char sme_early_buffer[PAGE_SIZE] __aligned(PAGE_SIZE);

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

int sme_set_mem_unenc(void *vaddr, unsigned long size)
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
EXPORT_SYMBOL_GPL(sme_set_mem_unenc);

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

static void __init *sme_bootdata_mapping(void *vaddr, unsigned long size)
{
	unsigned long paddr = (unsigned long)vaddr - __PAGE_OFFSET;
	pmdval_t pmd_flags, pmd;
	void *ret = vaddr;

	/* Use early_pmd_flags but remove the encryption mask */
	pmd_flags = early_pmd_flags & ~sme_me_mask;

	do {
		pmd = (paddr & PMD_MASK) + pmd_flags;
		__early_make_pgtable((unsigned long)vaddr, pmd);

		vaddr += PMD_SIZE;
		paddr += PMD_SIZE;
		size = (size < PMD_SIZE) ? 0 : size - PMD_SIZE;
	} while (size);

	return ret;
}

void __init sme_map_bootdata(char *real_mode_data)
{
	struct boot_params *boot_data;
	unsigned long cmdline_paddr;

	if (!sme_me_mask)
		return;

	/*
	 * The bootdata will not be encrypted, so it needs to be mapped
	 * as unencrypted data so it can be copied properly.
	 */
	boot_data = sme_bootdata_mapping(real_mode_data, sizeof(boot_params));

	/*
	 * Determine the command line address only after having established
	 * the unencrypted mapping.
	 */
	cmdline_paddr = boot_data->hdr.cmd_line_ptr |
			((u64)boot_data->ext_cmd_line_ptr << 32);
	if (cmdline_paddr)
		sme_bootdata_mapping(__va(cmdline_paddr), COMMAND_LINE_SIZE);
}

void __init sme_encrypt_ramdisk(resource_size_t paddr, unsigned long size)
{
	if (!sme_me_mask)
		return;

	sme_early_mem_enc(paddr, size);
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
