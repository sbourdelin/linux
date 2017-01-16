/*
 * GCC stack protector support.
 *
 * Stack protector works by putting predefined pattern at the start of
 * the stack frame and verifying that it hasn't been overwritten when
 * returning from the function.  The pattern is called stack canary
 * and gcc expects it to be defined by a global variable called
 * "__stack_chk_guard" on PPC.  This unfortunately means that on SMP
 * we cannot have a different canary value per task.
 */

#ifndef _ASM_STACKPROTECTOR_H
#define _ASM_STACKPROTECTOR_H

#ifdef CONFIG_PPC64
#define SSP_OFFSET	0x7010
#else
#define SSP_OFFSET	0x7008
#endif

#ifndef __ASSEMBLY__

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

	/* Try to get a semi random initial value. */
	get_random_bytes(&canary, sizeof(canary));
	canary ^= mftb();
	canary ^= LINUX_VERSION_CODE;

	current->stack_canary = canary;
}
#endif /* __ASSEMBLY__ */
#endif	/* _ASM_STACKPROTECTOR_H */
