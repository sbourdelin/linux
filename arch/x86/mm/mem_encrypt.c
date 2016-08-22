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

#include <asm/mem_encrypt.h>

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
