/* SPDX-License-Identifier: GPL-2.0 */

#if defined(CONFIG_PPC64) && defined(CONFIG_COMPAT)
#define __SYSCALL(nr, entry, nargs) .8byte entry
.section .rodata,"a"
.p2align
.globl sys_call_table32
sys_call_table32:
#include <asm/syscall_table_c32.h>
#elif defined(CONFIG_PPC64) && !defined(CONFIG_COMPAT)
#define __SYSCALL(nr, entry, nargs) .8byte entry
.section .rodata,"a"
.p2align
.globl sys_call_table64
sys_call_table64:
#include <asm/syscall_table_64.h>
#endif
