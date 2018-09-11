/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2015-2018 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#ifndef _SIMD_H
#define _SIMD_H

typedef enum {
	HAVE_NO_SIMD,
	HAVE_FULL_SIMD
} simd_context_t;

#include <linux/sched.h>
#include <asm/simd.h>

static inline simd_context_t simd_relax(simd_context_t prior_context)
{
#ifdef CONFIG_PREEMPT
	if (prior_context != HAVE_NO_SIMD && need_resched()) {
		simd_put(prior_context);
		return simd_get();
	}
#endif
	return prior_context;
}

#endif /* _SIMD_H */
