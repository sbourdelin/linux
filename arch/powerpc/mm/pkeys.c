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
	return -1;
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

	return 0;
}
