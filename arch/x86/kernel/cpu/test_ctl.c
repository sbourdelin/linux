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
#include <linux/reboot.h>
#include <linux/syscore_ops.h>
#include <linux/debugfs.h>
#include <asm/msr.h>

#define DISABLE_SPLIT_LOCK_AC		0
#define ENABLE_SPLIT_LOCK_AC		1
#define INHERIT_SPLIT_LOCK_AC_FIRMWARE	2

/* After disabling #AC for split lock in handler, re-enable it 1 msec later. */
#define reenable_split_lock_delay	msecs_to_jiffies(1)

static void delayed_reenable_split_lock(struct work_struct *w);
static DEFINE_PER_CPU(struct delayed_work, reenable_delayed_work);
static unsigned long disable_split_lock_jiffies;
static DEFINE_MUTEX(reexecute_split_lock_mutex);

static int split_lock_ac_kernel = DISABLE_SPLIT_LOCK_AC;
static int split_lock_ac_firmware = DISABLE_SPLIT_LOCK_AC;

static DEFINE_MUTEX(split_lock_mutex);

struct debugfs_file {
	char				name[32];
	int				mode;
	const struct file_operations	*fops;
};

enum {
	KERNEL_MODE_RE_EXECUTE,
	KERNEL_MODE_PANIC,
	KERNEL_MODE_LAST
};

static int kernel_mode_reaction = KERNEL_MODE_RE_EXECUTE;

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

	/* Get previous firmware setting. */
	if (orig_val & MSR_TEST_CTL_ENABLE_AC_SPLIT_LOCK)
		split_lock_ac_firmware = ENABLE_SPLIT_LOCK_AC;
	else
		split_lock_ac_firmware = DISABLE_SPLIT_LOCK_AC;

	/*
	 * By default configuration, kernel inherits firmware split lock
	 * setting. Kernel can be configured to explicitly enable or disable
	 * #AC for split lock to override firmware setting.
	 */
	if (CONFIG_SPLIT_LOCK_AC_ENABLE_DEFAULT ==
	    INHERIT_SPLIT_LOCK_AC_FIRMWARE)
		split_lock_ac_kernel = split_lock_ac_firmware;
	else
		split_lock_ac_kernel = CONFIG_SPLIT_LOCK_AC_ENABLE_DEFAULT;
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

static void restore_split_lock_ac(int split_lock_ac_val)
{
	_setup_split_lock(split_lock_ac_val);
}

/* Restore firmware setting for #AC exception for split lock. */
void restore_split_lock_ac_firmware(void)
{
	if (!boot_cpu_has(X86_FEATURE_SPLIT_LOCK_AC))
		return;

	/* Don't restore the firmware setting if kernel didn't change it. */
	if (split_lock_ac_kernel == split_lock_ac_firmware)
		return;

	restore_split_lock_ac(split_lock_ac_firmware);
}

/* Restore kernel setting for #AC enable bit for split lock. */
void restore_split_lock_ac_kernel(void)
{
	if (!boot_cpu_has(X86_FEATURE_SPLIT_LOCK_AC))
		return;

	restore_split_lock_ac(split_lock_ac_kernel);
}

static void split_lock_cpu_reboot(void *unused)
{
	restore_split_lock_ac_firmware();
}

static int split_lock_reboot_notify(struct notifier_block *nb,
				    unsigned long code, void *unused)
{
	on_each_cpu_mask(cpu_online_mask, split_lock_cpu_reboot, NULL, 1);

	return NOTIFY_DONE;
}

static struct notifier_block split_lock_reboot_nb = {
	.notifier_call = split_lock_reboot_notify,
};

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

	/* If configured as panic for split lock in kernel mode, panic. */
	if (kernel_mode_reaction == KERNEL_MODE_PANIC && !user_mode(regs))
		panic("Alignment Check exception for split lock in kernel.");

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
	restore_split_lock_ac_firmware();
	cancel_delayed_work(&per_cpu(reenable_delayed_work, cpu));

	return 0;
}

static int split_lock_bsp_suspend(void)
{
	restore_split_lock_ac_firmware();

	return 0;
}

static void split_lock_bsp_resume(void)
{
	restore_split_lock_ac_kernel();
}

static struct syscore_ops split_lock_syscore_ops = {
	.suspend	= split_lock_bsp_suspend,
	.resume		= split_lock_bsp_resume,
};

static int enable_show(void *data, u64 *val)
{
	*val = split_lock_ac_kernel;

	return 0;
}

static int enable_store(void *data, u64 val)
{
	u64 msr_val;
	int cpu;

	if (val != DISABLE_SPLIT_LOCK_AC && val != ENABLE_SPLIT_LOCK_AC)
		return -EINVAL;

	/* No need to update MSR if new setting is the same as old one. */
	if (val == split_lock_ac_kernel)
		return 0;

	mutex_lock(&split_lock_mutex);
	mutex_lock(&reexecute_split_lock_mutex);

	/*
	 * Wait until it's out of any re-executed split lock instruction
	 * window.
	 */
	wait_for_reexecution();

	split_lock_ac_kernel = val;
	/* Read split lock setting on the current CPU. */
	rdmsrl(MSR_TEST_CTL, msr_val);
	/* Change the split lock setting. */
	if (split_lock_ac_kernel == DISABLE_SPLIT_LOCK_AC)
		msr_val &= ~MSR_TEST_CTL_ENABLE_AC_SPLIT_LOCK;
	else
		msr_val |= MSR_TEST_CTL_ENABLE_AC_SPLIT_LOCK;
	/* Update the split lock setting on all online CPUs. */
	for_each_online_cpu(cpu)
		wrmsrl_on_cpu(cpu, MSR_TEST_CTL, msr_val);

	mutex_unlock(&reexecute_split_lock_mutex);
	mutex_unlock(&split_lock_mutex);

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(enable_ops, enable_show, enable_store, "%llx\n");

static int __init debugfs_setup_split_lock(void)
{
	struct debugfs_file debugfs_files[] = {
		{"enable",      0600, &enable_ops},
	};
	struct dentry *split_lock_dir, *fd;
	int i;

	split_lock_dir = debugfs_create_dir("split_lock", arch_debugfs_dir);
	if (!split_lock_dir)
		goto out;

	/*  Create files under split_lock_dir. */
	for (i = 0; i < ARRAY_SIZE(debugfs_files); i++) {
		fd = debugfs_create_file(debugfs_files[i].name,
					 debugfs_files[i].mode,
					 split_lock_dir, NULL,
					 debugfs_files[i].fops);
		if (!fd)
			goto out_cleanup;
	}

	return 0;

out_cleanup:
	debugfs_remove_recursive(split_lock_dir);
out:

	return -ENOMEM;
}

static int __init split_lock_init(void)
{
	int ret;

	if (!boot_cpu_has(X86_FEATURE_SPLIT_LOCK_AC))
		return -ENODEV;

	ret = debugfs_setup_split_lock();
	if (ret)
		pr_warn("debugfs for #AC for split lock cannot be set up\n");

	if (IS_ENABLED(CONFIG_SPLIT_LOCK_AC_PANIC_ON_KERNEL))
		kernel_mode_reaction = KERNEL_MODE_PANIC;

	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "x86/split_lock:online",
				split_lock_online, split_lock_offline);
	if (ret < 0)
		return ret;

	register_syscore_ops(&split_lock_syscore_ops);

	register_reboot_notifier(&split_lock_reboot_nb);

	return 0;
}

late_initcall(split_lock_init);
