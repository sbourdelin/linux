/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cet.c - Control Flow Enforcement (CET)
 *
 * Copyright (c) 2018, Intel Corporation.
 * Yu-cheng Yu <yu-cheng.yu@intel.com>
 */

#include <linux/types.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sched/signal.h>
#include <asm/msr.h>
#include <asm/user.h>
#include <asm/fpu/xstate.h>
#include <asm/fpu/types.h>
#include <asm/compat.h>
#include <asm/cet.h>

static int set_shstk_ptr(unsigned long addr)
{
	u64 r;

	if (!cpu_feature_enabled(X86_FEATURE_SHSTK))
		return -1;

	if ((addr >= TASK_SIZE_MAX) || (!IS_ALIGNED(addr, 4)))
		return -1;

	rdmsrl(MSR_IA32_U_CET, r);
	wrmsrl(MSR_IA32_PL3_SSP, addr);
	wrmsrl(MSR_IA32_U_CET, r | MSR_IA32_CET_SHSTK_EN);
	return 0;
}

static unsigned long get_shstk_addr(void)
{
	unsigned long ptr;

	if (!current->thread.cet.shstk_enabled)
		return 0;

	rdmsrl(MSR_IA32_PL3_SSP, ptr);
	return ptr;
}

int cet_setup_shstk(void)
{
	unsigned long addr, size;

	if (!cpu_feature_enabled(X86_FEATURE_SHSTK))
		return -EOPNOTSUPP;

	size = rlimit(RLIMIT_STACK);
	addr = do_mmap_locked(0, size, PROT_READ,
			      MAP_ANONYMOUS | MAP_PRIVATE, VM_SHSTK);

	/*
	 * Return actual error from do_mmap().
	 */
	if (addr >= TASK_SIZE_MAX)
		return addr;

	set_shstk_ptr(addr + size - sizeof(u64));
	current->thread.cet.shstk_base = addr;
	current->thread.cet.shstk_size = size;
	current->thread.cet.shstk_enabled = 1;
	return 0;
}

void cet_disable_shstk(void)
{
	u64 r;

	if (!cpu_feature_enabled(X86_FEATURE_SHSTK))
		return;

	rdmsrl(MSR_IA32_U_CET, r);
	r &= ~(MSR_IA32_CET_SHSTK_EN);
	wrmsrl(MSR_IA32_U_CET, r);
	wrmsrl(MSR_IA32_PL3_SSP, 0);
	current->thread.cet.shstk_enabled = 0;
}

void cet_disable_free_shstk(struct task_struct *tsk)
{
	if (!cpu_feature_enabled(X86_FEATURE_SHSTK) ||
	    !tsk->thread.cet.shstk_enabled)
		return;

	if (tsk == current)
		cet_disable_shstk();

	/*
	 * Free only when tsk is current or shares mm
	 * with current but has its own shstk.
	 */
	if (tsk->mm && (tsk->mm == current->mm) &&
	    (tsk->thread.cet.shstk_base)) {
		vm_munmap(tsk->thread.cet.shstk_base,
			  tsk->thread.cet.shstk_size);
		tsk->thread.cet.shstk_base = 0;
		tsk->thread.cet.shstk_size = 0;
	}

	tsk->thread.cet.shstk_enabled = 0;
}
