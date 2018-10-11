/* SPDX-License-Identifier: GPL-2.0 */

#define __SYSCALL(nr, entry, nargs) data8 entry
	.rodata
	.align 8
	.globl sys_call_table
sys_call_table:
#include <asm/syscall_table.h>
#undef __SYSCALL
