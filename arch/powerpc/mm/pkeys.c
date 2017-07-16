/*
 * PowerPC Memory Protection Keys management
 * Copyright (c) 2015, Intel Corporation.
 * Copyright (c) 2017, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */
#include <uapi/asm-generic/mman-common.h>
#include <linux/pkeys.h>                /* PKEY_*                       */

bool pkey_inited;
#define pkeyshift(pkey) ((arch_max_pkey()-pkey-1) * AMR_BITS_PER_PKEY)
static bool is_pkey_enabled(int pkey)
{
	return !!(read_uamor() & (0x3ul << pkeyshift(pkey)));
}

static inline void init_amr(int pkey, u8 init_bits)
{
	u64 new_amr_bits = (((u64)init_bits & 0x3UL) << pkeyshift(pkey));
	u64 old_amr = read_amr() & ~((u64)(0x3ul) << pkeyshift(pkey));

	write_amr(old_amr | new_amr_bits);
}

static inline void init_iamr(int pkey, u8 init_bits)
{
	u64 new_iamr_bits = (((u64)init_bits & 0x3UL) << pkeyshift(pkey));
	u64 old_iamr = read_iamr() & ~((u64)(0x3ul) << pkeyshift(pkey));

	write_amr(old_iamr | new_iamr_bits);
}

static void pkey_status_change(int pkey, bool enable)
{
	u64 old_uamor;

	/* reset the AMR and IAMR bits for this key */
	init_amr(pkey, 0x0);
	init_iamr(pkey, 0x0);

	/* enable/disable key */
	old_uamor = read_uamor();
	if (enable)
		old_uamor |= (0x3ul << pkeyshift(pkey));
	else
		old_uamor &= ~(0x3ul << pkeyshift(pkey));
	write_uamor(old_uamor);
}

void __arch_activate_pkey(int pkey)
{
	pkey_status_change(pkey, true);
}

void __arch_deactivate_pkey(int pkey)
{
	pkey_status_change(pkey, false);
}

/*
 * set the access right in AMR IAMR and UAMOR register
 * for @pkey to that specified in @init_val.
 */
int __arch_set_user_pkey_access(struct task_struct *tsk, int pkey,
		unsigned long init_val)
{
	u64 new_amr_bits = 0x0ul;
	u64 new_iamr_bits = 0x0ul;

	if (!is_pkey_enabled(pkey))
		return -1;

	/* Set the bits we need in AMR:  */
	if (init_val & PKEY_DISABLE_ACCESS)
		new_amr_bits |= AMR_RD_BIT | AMR_WR_BIT;
	else if (init_val & PKEY_DISABLE_WRITE)
		new_amr_bits |= AMR_WR_BIT;

	init_amr(pkey, new_amr_bits);

	/*
	 * By default execute is disabled.
	 * To enable execute, PKEY_ENABLE_EXECUTE
	 * needs to be specified.
	 */
	if ((init_val & PKEY_DISABLE_EXECUTE))
		new_iamr_bits |= IAMR_EX_BIT;

	init_iamr(pkey, new_iamr_bits);
	return 0;
}

static inline bool pkey_allows_readwrite(int pkey)
{
	int pkey_shift = pkeyshift(pkey);

	if (!(read_uamor() & (0x3UL << pkey_shift)))
		return true;

	return !(read_amr() & ((AMR_RD_BIT|AMR_WR_BIT) << pkey_shift));
}

int __execute_only_pkey(struct mm_struct *mm)
{
	bool need_to_set_mm_pkey = false;
	int execute_only_pkey = mm->context.execute_only_pkey;
	int ret;

	/* Do we need to assign a pkey for mm's execute-only maps? */
	if (execute_only_pkey == -1) {
		/* Go allocate one to use, which might fail */
		execute_only_pkey = mm_pkey_alloc(mm);
		if (execute_only_pkey < 0)
			return -1;
		need_to_set_mm_pkey = true;
	}

	/*
	 * We do not want to go through the relatively costly
	 * dance to set AMR if we do not need to.  Check it
	 * first and assume that if the execute-only pkey is
	 * readwrite-disabled than we do not have to set it
	 * ourselves.
	 */
	if (!need_to_set_mm_pkey &&
	    !pkey_allows_readwrite(execute_only_pkey))
		return execute_only_pkey;

	/*
	 * Set up AMR so that it denies access for everything
	 * other than execution.
	 */
	ret = __arch_set_user_pkey_access(current, execute_only_pkey,
			(PKEY_DISABLE_ACCESS | PKEY_DISABLE_WRITE));
	/*
	 * If the AMR-set operation failed somehow, just return
	 * 0 and effectively disable execute-only support.
	 */
	if (ret) {
		mm_set_pkey_free(mm, execute_only_pkey);
		return -1;
	}

	/* We got one, store it and use it from here on out */
	if (need_to_set_mm_pkey)
		mm->context.execute_only_pkey = execute_only_pkey;
	return execute_only_pkey;
}

static inline bool vma_is_pkey_exec_only(struct vm_area_struct *vma)
{
	/* Do this check first since the vm_flags should be hot */
	if ((vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC)) != VM_EXEC)
		return false;

	return (vma_pkey(vma) == vma->vm_mm->context.execute_only_pkey);
}

/*
 * This should only be called for *plain* mprotect calls.
 */
int __arch_override_mprotect_pkey(struct vm_area_struct *vma, int prot,
		int pkey)
{
	/*
	 * Is this an mprotect_pkey() call?  If so, never
	 * override the value that came from the user.
	 */
	if (pkey != -1)
		return pkey;

	/*
	 * If the currently associated pkey is execute-only,
	 * but the requested protection requires read or write,
	 * move it back to the default pkey.
	 */
	if (vma_is_pkey_exec_only(vma) &&
	    (prot & (PROT_READ|PROT_WRITE)))
		return 0;

	/*
	 * the requested protection is execute-only. Hence
	 * lets use a execute-only pkey.
	 */
	if (prot == PROT_EXEC) {
		pkey = execute_only_pkey(vma->vm_mm);
		if (pkey > 0)
			return pkey;
	}

	/*
	 * nothing to override.
	 */
	return vma_pkey(vma);
}
