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

	if (!is_pkey_enabled(pkey))
		return -1;

	/* Set the bits we need in AMR:  */
	if (init_val & PKEY_DISABLE_ACCESS)
		new_amr_bits |= AMR_RD_BIT | AMR_WR_BIT;
	else if (init_val & PKEY_DISABLE_WRITE)
		new_amr_bits |= AMR_WR_BIT;

	init_amr(pkey, new_amr_bits);

	return 0;
}
