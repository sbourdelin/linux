// SPDX-License-Identifier: GPL-2.0+

#include <linux/error-injection.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>

void override_function_with_return(struct pt_regs *regs)
{
	/* Emulate 'blr' instruction */
	regs->nip = regs->link;
}
NOKPROBE_SYMBOL(override_function_with_return);
