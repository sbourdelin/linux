/* SPDX-License-Identifier: GPL-2.0 */
/*
 * GCC stack protector support.
 *
 */

#ifndef _ASM_STACKPROTECTOR_H
#define _ASM_STACKPROTECTOR_H

#include <linux/random.h>
#include <linux/version.h>
#include <asm/reg.h>

/*
 * Initialize the stackprotector canary value.
 *
 * NOTE: this must only be called from functions that never return,
 * and it must always be inlined.
 */
static __always_inline void boot_init_stack_canary(void)
{
	unsigned long canary;

	/*
	 * The stack_canary must be located at the offset given to
	 * -mstack-protector-guard-offset in the Makefile
	 */
	BUILD_BUG_ON(offsetof(struct task_struct, stack_canary) != sizeof(long));

	/* Try to get a semi random initial value. */
	get_random_bytes(&canary, sizeof(canary));
	canary ^= mftb();
	canary ^= LINUX_VERSION_CODE;

	current->stack_canary = canary;
}

#endif	/* _ASM_STACKPROTECTOR_H */
