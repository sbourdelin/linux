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

#ifndef __X86_MEM_ENCRYPT_H__
#define __X86_MEM_ENCRYPT_H__

#ifndef __ASSEMBLY__

#include <linux/init.h>

#ifdef CONFIG_AMD_MEM_ENCRYPT

extern unsigned long sme_me_mask;

u8 sme_get_me_loss(void);

void __init sme_early_init(void);

#define __sme_pa(x)		(__pa((x)) | sme_me_mask)
#define __sme_pa_nodebug(x)	(__pa_nodebug((x)) | sme_me_mask)

#define __sme_va(x)		(__va((x) & ~sme_me_mask))

#else	/* !CONFIG_AMD_MEM_ENCRYPT */

#define sme_me_mask		0UL

static inline u8 sme_get_me_loss(void)
{
	return 0;
}

static inline void __init sme_early_init(void)
{
}

#define __sme_pa		__pa
#define __sme_pa_nodebug	__pa_nodebug

#define __sme_va		__va

#endif	/* CONFIG_AMD_MEM_ENCRYPT */

#endif	/* __ASSEMBLY__ */

#endif	/* __X86_MEM_ENCRYPT_H__ */
