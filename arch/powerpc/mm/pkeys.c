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
#include <linux/pkeys.h>                /* PKEY_*                       */
#include <uapi/asm-generic/mman-common.h>

#define pkeyshift(pkey) ((arch_max_pkey()-pkey-1) * AMR_BITS_PER_PKEY)

static inline bool pkey_allows_readwrite(int pkey)
{
	int pkey_shift = pkeyshift(pkey);

	if (!(read_uamor() & (0x3UL << pkey_shift)))
		return true;

	return !(read_amr() & ((AMR_AD_BIT|AMR_WD_BIT) << pkey_shift));
}

/*
 * set the access right in AMR IAMR and UAMOR register
 * for @pkey to that specified in @init_val.
 */
int __arch_set_user_pkey_access(struct task_struct *tsk, int pkey,
		unsigned long init_val)
{
	u64 old_amr, old_uamor, old_iamr;
	int pkey_shift = (arch_max_pkey()-pkey-1) * AMR_BITS_PER_PKEY;
	u64 new_amr_bits = 0x0ul;
	u64 new_iamr_bits = 0x0ul;
	u64 new_uamor_bits = 0x3ul;

	/* Set the bits we need in AMR:  */
	if (init_val & PKEY_DISABLE_ACCESS)
		new_amr_bits |= AMR_AD_BIT;
	if (init_val & PKEY_DISABLE_WRITE)
		new_amr_bits |= AMR_WD_BIT;

	/*
	 * By default execute is disabled.
	 * To enable execute, PKEY_ENABLE_EXECUTE
	 * needs to be specified.
	 */
	if ((init_val & PKEY_DISABLE_EXECUTE))
		new_iamr_bits |= IAMR_EX_BIT;

	/* Shift the bits in to the correct place in AMR for pkey: */
	new_amr_bits	<<= pkey_shift;
	new_iamr_bits	<<= pkey_shift;
	new_uamor_bits	<<= pkey_shift;

	/* Get old AMR and mask off any old bits in place: */
	old_amr	= read_amr();
	old_amr	&= ~((u64)(AMR_AD_BIT|AMR_WD_BIT) << pkey_shift);

	old_iamr = read_iamr();
	old_iamr &= ~(0x3ul << pkey_shift);

	old_uamor = read_uamor();
	old_uamor &= ~(0x3ul << pkey_shift);

	/* Write old part along with new part: */
	write_amr(old_amr | new_amr_bits);
	write_iamr(old_iamr | new_iamr_bits);
	write_uamor(old_uamor | new_uamor_bits);

	return 0;
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
	if (vma_pkey(vma) != vma->vm_mm->context.execute_only_pkey)
		return false;

	return true;
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
	 * Look for a protection-key-drive execute-only mapping
	 * which is now being given permissions that are not
	 * execute-only.  Move it back to the default pkey.
	 */
	if (vma_is_pkey_exec_only(vma) &&
	    (prot & (PROT_READ|PROT_WRITE))) {
		return 0;
	}
	/*
	 * The mapping is execute-only.  Go try to get the
	 * execute-only protection key.  If we fail to do that,
	 * fall through as if we do not have execute-only
	 * support.
	 */
	if (prot == PROT_EXEC) {
		pkey = execute_only_pkey(vma->vm_mm);
		if (pkey > 0)
			return pkey;
	}
	/*
	 * This is a vanilla, non-pkey mprotect (or we failed to
	 * setup execute-only), inherit the pkey from the VMA we
	 * are working on.
	 */
	return vma_pkey(vma);
}
