/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2015-2018 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#ifndef _SIMD_H
#define _SIMD_H

typedef enum {
	HAVE_NO_SIMD = 1 << 0,
	HAVE_FULL_SIMD = 1 << 1,
	HAVE_SIMD_IN_USE = 1 << 31
} simd_context_t;

#include <linux/sched.h>
#include <asm/simd.h>

static inline void simd_relax(simd_context_t *ctx)
{
#ifdef CONFIG_PREEMPT
	if ((*ctx & HAVE_SIMD_IN_USE) && need_resched()) {
		simd_put(ctx);
		simd_get(ctx);
	}
#endif
}

#endif /* _SIMD_H */
