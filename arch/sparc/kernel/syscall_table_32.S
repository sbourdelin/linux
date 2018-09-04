/* SPDX-License-Identifier: GPL-2.0 */

#define __SYSCALL(nr, entry, nargs) .long entry

#define sys_ni_syscall sys_nis_syscall

.data
.align 4
.globl sys_call_table
sys_call_table:	
#include <asm/syscall_table_32.h>

