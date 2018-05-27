// SPDX-License-Identifier: GPL-2.0
/*
 * Enable #AC exception for split locked accesses in TEST_CTL MSR
 *
 * Copyright (C) 2018 Intel Corporation
 *
 * Author:
 *	Fenghua Yu <fenghua.yu@intel.com>
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/printk.h>
#include <linux/cpufeature.h>
#include <asm/msr.h>

/* Detete feature of #AC for split lock by probing bit 29 in MSR_TEST_CTL. */
void detect_split_lock_ac(void)
{
	u64 val, orig_val;
	int ret;

	/* Attempt to read the MSR. If the MSR doesn't exist, reading fails. */
	ret = rdmsrl_safe(MSR_TEST_CTL, &val);
	if (ret)
		return;

	orig_val = val;

	/* Turn on the split lock bit */
	val |= MSR_TEST_CTL_ENABLE_AC_SPLIT_LOCK;

	/*
	 * Attempt to set bit 29 in the MSR. The bit is set successfully
	 * only on processors that support #AC for split lock.
	 */
	ret = wrmsrl_safe(MSR_TEST_CTL, val);
	if (ret)
		return;

	/* The feature is supported on CPU. */
	setup_force_cpu_cap(X86_FEATURE_SPLIT_LOCK_AC);

	/*
	 * Need to restore split lock setting to original firmware setting
	 * before leaving.
	 */
	wrmsrl(MSR_TEST_CTL, orig_val);
}
