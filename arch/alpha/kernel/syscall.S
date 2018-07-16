/* SPDX-License-Identifier: GPL-2.0 */

#include <asm/unistd.h>

#define __SYSCALL(nr, entry, nargs) .quad entry

#define sys_ni_syscall alpha_ni_syscall

.data
.align 3
.globl sys_call_table
sys_call_table:
#include <asm/syscall_table.h>

.size sys_call_table, . - sys_call_table
.type sys_call_table, @object

.ifne (. - sys_call_table) - (__NR_syscalls * 8)
.err
.endif
