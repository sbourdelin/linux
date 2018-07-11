/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ASM_BOOT_H
#define __ASM_BOOT_H

#include <asm/sizes.h>

#define ARM64_MAGIC		"ARM\x64"

#define HEAD_FLAG_BE_SHIFT		0
#define HEAD_FLAG_PAGE_SIZE_SHIFT	1
#define HEAD_FLAG_BE_MASK		0x1
#define HEAD_FLAG_PAGE_SIZE_MASK	0x3

#define HEAD_FLAG_BE			1
#define HEAD_FLAG_PAGE_SIZE_4K		1
#define HEAD_FLAG_PAGE_SIZE_16K		2
#define HEAD_FLAG_PAGE_SIZE_64K		3

#define head_flag_field(flags, field) \
		(((flags) >> field##_SHIFT) & field##_MASK)

/*
 * arm64 requires the DTB to be 8 byte aligned and
 * not exceed 2MB in size.
 */
#define MIN_FDT_ALIGN		8
#define MAX_FDT_SIZE		SZ_2M

/*
 * arm64 requires the kernel image to placed
 * TEXT_OFFSET bytes beyond a 2 MB aligned base
 */
#define MIN_KIMG_ALIGN		SZ_2M

#endif
