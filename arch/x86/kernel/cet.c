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
#include <linux/vmalloc.h>
#include <linux/bitops.h>
#include <asm/msr.h>
#include <asm/user.h>
#include <asm/fpu/xstate.h>
#include <asm/fpu/types.h>
#include <asm/compat.h>
#include <asm/cet.h>
#include <asm/special_insns.h>

#define SHSTK_SIZE_64 (0x8000 * 8)
#define SHSTK_SIZE_32 (0x8000 * 4)

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

/*
 * Verify the restore token at the address of 'ssp' is
 * valid and then set shadow stack pointer according to the
 * token.
 */
static int verify_rstor_token(bool ia32, unsigned long ssp,
			      unsigned long *new_ssp)
{
	unsigned long token;

	*new_ssp = 0;

	if (!IS_ALIGNED(ssp, 8))
		return -EINVAL;

	if (get_user(token, (unsigned long __user*)ssp))
		return -EFAULT;

	/* Is 64-bit mode flag correct? */
	if (ia32 && (token & 3) != 0)
		return -EINVAL;
	else if ((token & 3) != 1)
		return -EINVAL;

	token &= ~(1UL);

	if ((!ia32 && !IS_ALIGNED(token, 8)) || !IS_ALIGNED(token, 4))
		return -EINVAL;

	if ((ALIGN_DOWN(token, 8) - 8) != ssp)
		return -EINVAL;

	*new_ssp = token;
	return 0;
}

/*
 * Create a restore token on the shadow stack.
 * A token is always 8-byte and aligned to 8.
 */
static int create_rstor_token(bool ia32, unsigned long ssp,
			      unsigned long *new_ssp)
{
	unsigned long addr;

	*new_ssp = 0;

	if ((!ia32 && !IS_ALIGNED(ssp, 8)) || !IS_ALIGNED(ssp, 4))
		return -EINVAL;

	addr = ALIGN_DOWN(ssp, 8) - 8;

	/* Is the token for 64-bit? */
	if (!ia32)
		ssp |= 1;

	if (write_user_shstk_64(addr, ssp))
		return -EFAULT;

	*new_ssp = addr;
	return 0;
}

static unsigned long shstk_mmap(unsigned long addr, unsigned long len)
{
	struct mm_struct *mm = current->mm;
	unsigned long populate;

	down_write(&mm->mmap_sem);
	addr = do_mmap(NULL, addr, len, PROT_READ,
		       MAP_ANONYMOUS | MAP_PRIVATE, VM_SHSTK,
		       0, &populate, NULL);
	up_write(&mm->mmap_sem);

	if (populate)
		mm_populate(addr, populate);

	return addr;
}

int cet_setup_shstk(void)
{
	unsigned long addr, size;

	if (!cpu_feature_enabled(X86_FEATURE_SHSTK))
		return -EOPNOTSUPP;

	size = in_ia32_syscall() ? SHSTK_SIZE_32:SHSTK_SIZE_64;
	addr = shstk_mmap(0, size);

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

int cet_restore_signal(unsigned long ssp)
{
	unsigned long new_ssp;
	int err;

	if (!current->thread.cet.shstk_enabled)
		return 0;

	err = verify_rstor_token(in_ia32_syscall(), ssp, &new_ssp);

	if (err)
		return err;

	return set_shstk_ptr(new_ssp);
}

/*
 * Setup the shadow stack for the signal handler: first,
 * create a restore token to keep track of the current ssp,
 * and then the return address of the signal handler.
 */
int cet_setup_signal(bool ia32, unsigned long rstor_addr,
		     unsigned long *new_ssp)
{
	unsigned long ssp;
	int err;

	if (!current->thread.cet.shstk_enabled)
		return 0;

	ssp = get_shstk_addr();
	err = create_rstor_token(ia32, ssp, new_ssp);

	if (err)
		return err;

	if (ia32) {
		ssp = *new_ssp - sizeof(u32);
		err = write_user_shstk_32(ssp, (unsigned int)rstor_addr);
	} else {
		ssp = *new_ssp - sizeof(u64);
		err = write_user_shstk_64(ssp, rstor_addr);
	}

	if (err)
		return err;

	set_shstk_ptr(ssp);
	return 0;
}

static unsigned long ibt_mmap(unsigned long addr, unsigned long len)
{
	struct mm_struct *mm = current->mm;
	unsigned long populate;

	down_write(&mm->mmap_sem);
	addr = do_mmap(NULL, addr, len, PROT_READ | PROT_WRITE,
		       MAP_ANONYMOUS | MAP_PRIVATE,
		       VM_DONTDUMP, 0, &populate, NULL);
	up_write(&mm->mmap_sem);

	if (populate)
		mm_populate(addr, populate);

	return addr;
}

int cet_setup_ibt(void)
{
	u64 r;

	if (!cpu_feature_enabled(X86_FEATURE_IBT))
		return -EOPNOTSUPP;

	rdmsrl(MSR_IA32_U_CET, r);
	r |= (MSR_IA32_CET_ENDBR_EN | MSR_IA32_CET_NO_TRACK_EN);
	wrmsrl(MSR_IA32_U_CET, r);
	current->thread.cet.ibt_enabled = 1;
	return 0;
}

int cet_setup_ibt_bitmap(void)
{
	u64 r;
	unsigned long bitmap;
	unsigned long size;

	if (!cpu_feature_enabled(X86_FEATURE_IBT))
		return -EOPNOTSUPP;

	size = TASK_SIZE_MAX / PAGE_SIZE / BITS_PER_BYTE;
	bitmap = ibt_mmap(0, size);

	if (bitmap >= TASK_SIZE_MAX)
		return -ENOMEM;

	bitmap &= PAGE_MASK;

	rdmsrl(MSR_IA32_U_CET, r);
	r |= (MSR_IA32_CET_LEG_IW_EN | bitmap);
	wrmsrl(MSR_IA32_U_CET, r);

	current->thread.cet.ibt_bitmap_addr = bitmap;
	current->thread.cet.ibt_bitmap_size = size;
	return 0;
}

void cet_disable_ibt(void)
{
	u64 r;

	if (!cpu_feature_enabled(X86_FEATURE_IBT))
		return;

	rdmsrl(MSR_IA32_U_CET, r);
	r &= ~(MSR_IA32_CET_ENDBR_EN | MSR_IA32_CET_LEG_IW_EN |
	       MSR_IA32_CET_NO_TRACK_EN);
	wrmsrl(MSR_IA32_U_CET, r);
	current->thread.cet.ibt_enabled = 0;
}
