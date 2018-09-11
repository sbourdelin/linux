/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2015-2018 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include <linux/simd.h>
#ifndef _ASM_SIMD_H
#define _ASM_SIMD_H

static __must_check inline bool may_use_simd(void)
{
	return !in_interrupt();
}

#ifdef CONFIG_KERNEL_MODE_NEON
#include <asm/neon.h>

static inline simd_context_t simd_get(void)
{
	bool have_simd = may_use_simd();
	if (have_simd)
		kernel_neon_begin();
	return have_simd ? HAVE_FULL_SIMD : HAVE_NO_SIMD;
}

static inline void simd_put(simd_context_t prior_context)
{
	if (prior_context != HAVE_NO_SIMD)
		kernel_neon_end();
}
#else
static inline simd_context_t simd_get(void)
{
	return HAVE_NO_SIMD;
}

static inline void simd_put(simd_context_t prior_context)
{
}
#endif

#endif /* _ASM_SIMD_H */
