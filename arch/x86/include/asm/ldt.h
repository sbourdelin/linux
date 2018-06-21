/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Ptrace interface to the LDT
 *
 */

#ifndef _ARCH_X86_INCLUDE_ASM_LDT_H

#include <linux/regset.h>
#include <uapi/asm/ldt.h>

extern user_regset_active_fn regset_ldt_active;
extern user_regset_get_fn regset_ldt_get;
extern user_regset_set_fn regset_ldt_set;

#endif	/* _ARCH_X86_INCLUDE_ASM_LDT_H */
