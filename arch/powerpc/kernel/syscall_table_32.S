/* SPDX-License-Identifier: GPL-2.0 */

#define __SYSCALL(nr, entry, nargs) .long entry

.section .rodata,"a"
.globl sys_call_table
sys_call_table:	
#include <asm/syscall_table_32.h>

