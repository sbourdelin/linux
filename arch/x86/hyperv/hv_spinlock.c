// SPDX-License-Identifier: GPL-2.0

/*
 * Hyper-V specific spinlock code.
 *
 * Copyright (C) 2018, Intel, Inc.
 *
 * Author : Yi Sun <yi.y.sun@intel.com>
 */

#define pr_fmt(fmt) "hv: " fmt

#include <linux/kernel_stat.h>
#include <linux/spinlock.h>
#include <linux/debugfs.h>
#include <linux/log2.h>
#include <linux/gfp.h>

#include <asm/mshyperv.h>
#include <asm/hyperv-tlfs.h>
#include <asm/paravirt.h>
#include <asm/qspinlock.h>
#include <asm/apic.h>

static bool __initdata hv_pvspin = true;

static void hv_qlock_kick(int cpu)
{
	apic->send_IPI(cpu, X86_PLATFORM_IPI_VECTOR);
}

static void hv_qlock_wait(u8 *byte, u8 val)
{
	unsigned long msr_val;

	if (READ_ONCE(*byte) != val)
		return;

	/*
	 * Read HV_X64_MSR_GUEST_IDLE MSR can trigger the guest's
	 * transition to the idle power state which can be exited
	 * by an IPI even if IF flag is disabled.
	 */
	if (ms_hyperv.features & HV_X64_MSR_GUEST_IDLE_AVAILABLE)
		rdmsrl(HV_X64_MSR_GUEST_IDLE, msr_val);
}

/*
 * Hyper-V does not support this so far.
 */
bool hv_vcpu_is_preempted(int vcpu)
{
	return false;
}
PV_CALLEE_SAVE_REGS_THUNK(hv_vcpu_is_preempted);

void __init hv_init_spinlocks(void)
{
	if (!hv_pvspin ||
	    !apic ||
	    !(ms_hyperv.hints & HV_X64_CLUSTER_IPI_RECOMMENDED) ||
	    !(ms_hyperv.features & HV_X64_MSR_GUEST_IDLE_AVAILABLE)) {
		pr_info("PV spinlocks disabled\n");
		return;
	}
	pr_info("PV spinlocks enabled\n");

	__pv_init_lock_hash();
	pv_lock_ops.queued_spin_lock_slowpath = __pv_queued_spin_lock_slowpath;
	pv_lock_ops.queued_spin_unlock = PV_CALLEE_SAVE(__pv_queued_spin_unlock);
	pv_lock_ops.wait = hv_qlock_wait;
	pv_lock_ops.kick = hv_qlock_kick;
	pv_lock_ops.vcpu_is_preempted = PV_CALLEE_SAVE(hv_vcpu_is_preempted);
}

static __init int hv_parse_nopvspin(char *arg)
{
	hv_pvspin = false;
	return 0;
}
early_param("hv_nopvspin", hv_parse_nopvspin);
