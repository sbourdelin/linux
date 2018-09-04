/* SPDX-License-Identifier: GPL-2.0 */

#define __SYSCALL(nr, entry, nargs) .word entry

#define sys_ni_syscall sys_nis_syscall

.text
.align  4
#ifdef CONFIG_COMPAT
.globl sys_call_table32
sys_call_table32:	
#include <asm/syscall_table_c32.h>
#endif /* CONFIG_COMPAT */

.text
.align  4
.globl sys_call_table64, sys_call_table
sys_call_table64:
sys_call_table:	
#include <asm/syscall_table_64.h>

	
