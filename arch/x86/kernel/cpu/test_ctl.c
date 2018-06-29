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
#include <linux/cpu.h>
#include <asm/msr.h>

/* By default, #AC for split lock is enabled. */
static bool enable_ac_split_lock = true;

/* Detect feature of #AC for split lock by probing bit 29 in MSR_TEST_CTL. */
void detect_ac_split_lock(void)
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
	setup_force_cpu_cap(X86_FEATURE_AC_SPLIT_LOCK);

	/*
	 * Need to restore split lock setting to original firmware setting
	 * before leaving.
	 */
	wrmsrl(MSR_TEST_CTL, orig_val);
}

/*
 * #AC handler for split lock is called by generic #AC handler.
 *
 * On split lock in kernel, warn and disable #AC for split lock on current CPU.
 *
 * On split lock in user process, send SIGBUS in the generic #AC handler.
 */
bool do_ac_split_lock(struct pt_regs *regs)
{
	/* Generic #AC handler will handle split lock in user. */
	if (user_mode(regs))
		return false;

	/* Clear the split lock bit to disable the feature on local CPU. */
	msr_clear_bit(MSR_TEST_CTL, MSR_TEST_CTL_ENABLE_AC_SPLIT_LOCK_SHIFT);

	WARN_ONCE(1, "A split lock issue is detected. Please FIX it\n");

	return true;
}

void setup_ac_split_lock(void)
{
	if (enable_ac_split_lock) {
		msr_set_bit(MSR_TEST_CTL,
			    MSR_TEST_CTL_ENABLE_AC_SPLIT_LOCK_SHIFT);
		pr_info_once("#AC for split lock is enabled\n");
	} else {
		msr_clear_bit(MSR_TEST_CTL,
			      MSR_TEST_CTL_ENABLE_AC_SPLIT_LOCK_SHIFT);
		pr_info_once("#AC for split lock is disabled\n");
	}
}
