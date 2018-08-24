/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/simd.h>
#ifndef _ASM_SIMD_H
#define _ASM_SIMD_H

#include <linux/hardirq.h>

/*
 * may_use_simd - whether it is allowable at this time to issue SIMD
 *                instructions or access the SIMD register file
 *
 * As architectures typically don't preserve the SIMD register file when
 * taking an interrupt, !in_interrupt() should be a reasonable default.
 */
static __must_check inline bool may_use_simd(void)
{
	return !in_interrupt();
}

static inline simd_context_t simd_get(void)
{
	return HAVE_NO_SIMD;
}

static inline void simd_put(simd_context_t prior_context)
{
}

#endif /* _ASM_SIMD_H */
