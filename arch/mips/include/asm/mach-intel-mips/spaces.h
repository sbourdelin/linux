/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Author: Leonid Yegoshin (yegoshin@mips.com)
 * Copyright (C) 2012 MIPS Technologies, Inc.
 * Copyright (C) 2014 Lei Chuanhua <Chuanhua.lei@lantiq.com>
 * Copyright (C) 2018 Intel Corporation.
 */

#ifndef _ASM_INTEL_MIPS_SPACES_H
#define _ASM_INTEL_MIPS_SPACES_H

#define PAGE_OFFSET		_AC(0x60000000, UL)
#define PHYS_OFFSET		_AC(0x20000000, UL)

/* No Highmem Support */
#define HIGHMEM_START		_AC(0xffff0000, UL)

#define FIXADDR_TOP		((unsigned long)(long)(int)0xcffe0000)

#define IO_SIZE			_AC(0x10000000, UL)
#define IO_SHIFT		_AC(0x10000000, UL)

/* IO space one */
#define __pa_symbol(x)		__pa(x)

#include <asm/mach-generic/spaces.h>
#endif /* __ASM_INTEL_MIPS_SPACES_H */
