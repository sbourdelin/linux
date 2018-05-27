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
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/mm.h>
#include <asm/msr.h>

#define DISABLE_SPLIT_LOCK_AC		0
#define ENABLE_SPLIT_LOCK_AC		1

/* After disabling #AC for split lock in handler, re-enable it 1 msec later. */
#define reenable_split_lock_delay	msecs_to_jiffies(1)

static void delayed_reenable_split_lock(struct work_struct *w);
static DEFINE_PER_CPU(struct delayed_work, reenable_delayed_work);
static unsigned long disable_split_lock_jiffies;
static DEFINE_MUTEX(reexecute_split_lock_mutex);

static int split_lock_ac_kernel = DISABLE_SPLIT_LOCK_AC;

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

static void _setup_split_lock(int split_lock_ac_val)
{
	u64 val;

	rdmsrl(MSR_TEST_CTL, val);

	/* No need to update MSR if same value. */
	if ((val >> MSR_TEST_CTL_ENABLE_AC_SPLIT_LOCK_SHIFT & 0x1) ==
	    split_lock_ac_val)
		return;

	if (split_lock_ac_val == ENABLE_SPLIT_LOCK_AC) {
		/* Set the split lock bit to enable the feature. */
		val |= MSR_TEST_CTL_ENABLE_AC_SPLIT_LOCK;
	} else {
		/* Clear the split lock bit to disable the feature. */
		val &= ~MSR_TEST_CTL_ENABLE_AC_SPLIT_LOCK;
	}

	wrmsrl(MSR_TEST_CTL, val);
}

void setup_split_lock(void)
{
	if (!boot_cpu_has(X86_FEATURE_SPLIT_LOCK_AC))
		return;

	_setup_split_lock(split_lock_ac_kernel);

	pr_info_once("#AC execption for split lock is %sd\n",
		     split_lock_ac_kernel == ENABLE_SPLIT_LOCK_AC ? "enable"
		     : "disable");
}

static void wait_for_reexecution(void)
{
	while (time_before(jiffies, disable_split_lock_jiffies +
			   reenable_split_lock_delay))
		cpu_relax();
}

/*
 * TEST_CTL MSR is shared among threads on the same core. To simplify
 * situation, disable_split_lock_jiffies is global instead of per core.
 *
 * Multiple threads may generate #AC for split lock at the same time.
 * disable_split_lock_jiffies is updated by those threads. This may
 * postpone re-enabling split lock on this thread. But that's OK
 * because we need to make sure all threads on the same core re-execute
 * their faulting instructions before re-enabling split lock on the core.
 *
 * We want to avoid the situation when split lock is disabled on one
 * thread (thus on the whole core), then split lock is re-enabled on
 * another thread (thus on the whole core), and the faulting instruction
 * generates another #AC on the first thread.
 *
 * Before re-enabling split lock, wait until there is no re-executed
 * split lock instruction which may only exist before
 * disable_split_lock_jiffies + reenable_split_lock_delay.
 */
static void delayed_reenable_split_lock(struct work_struct *w)
{
	mutex_lock(&reexecute_split_lock_mutex);
	wait_for_reexecution();
	_setup_split_lock(ENABLE_SPLIT_LOCK_AC);
	mutex_unlock(&reexecute_split_lock_mutex);
}

/* Will the faulting instruction be re-executed? */
static bool re_execute(struct pt_regs *regs)
{
	/*
	 * The only reason for generating #AC from kernel is because of
	 * split lock. The kernel faulting instruction will be re-executed.
	 */
	if (!user_mode(regs))
		return true;

	return false;
}

static void disable_split_lock(void *unused)
{
	_setup_split_lock(DISABLE_SPLIT_LOCK_AC);
}

/*
 * #AC handler for split lock is called by generic #AC handler.
 *
 * Disable #AC for split lock on the CPU that the current task runs on
 * in order for the faulting instruction to get executed. The #AC for split
 * lock is re-enabled later.
 */
bool do_split_lock_exception(struct pt_regs *regs, unsigned long error_code)
{
	static DEFINE_RATELIMIT_STATE(ratelimit, 5 * HZ, 5);
	char str[] = "alignment check for split lock";
	struct task_struct *tsk = current;
	int cpu = task_cpu(tsk);

	if (!re_execute(regs))
		return false;

	/* Pace logging with jiffies. */
	if (__ratelimit(&ratelimit)) {
		pr_info("%s[%d] %s ip:%lx sp:%lx error:%lx",
			tsk->comm, tsk->pid, str,
			regs->ip, regs->sp, error_code);
		print_vma_addr(KERN_CONT " in ", regs->ip);
		pr_cont("\n");
	}

	mutex_lock(&reexecute_split_lock_mutex);
	smp_call_function_single(cpu, disable_split_lock, NULL, 1);
	/*
	 * Mark the time when split lock is disabled for re-executing the
	 * faulting instruction.
	 */
	disable_split_lock_jiffies = jiffies;
	mutex_unlock(&reexecute_split_lock_mutex);

	/* The faulting instruction will be re-executed when
	 * split lock is re-enabled 1 HZ later.
	 */
	schedule_delayed_work_on(cpu, &per_cpu(reenable_delayed_work, cpu),
				 reenable_split_lock_delay);

	return true;
}

static int split_lock_online(unsigned int cpu)
{
	INIT_DELAYED_WORK(&per_cpu(reenable_delayed_work, cpu),
			  delayed_reenable_split_lock);

	return 0;
}

static int split_lock_offline(unsigned int cpu)
{
	cancel_delayed_work(&per_cpu(reenable_delayed_work, cpu));

	return 0;
}

static int __init split_lock_init(void)
{
	int ret;

	if (!boot_cpu_has(X86_FEATURE_SPLIT_LOCK_AC))
		return -ENODEV;

	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "x86/split_lock:online",
				split_lock_online, split_lock_offline);
	if (ret < 0)
		return ret;

	return 0;
}

late_initcall(split_lock_init);
