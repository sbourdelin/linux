/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/linkage.h>

#ifndef CONFIG_MMU
#define sys_mmap2 sys_mmap_pgoff
#endif

#define __SYSCALL(nr, entry, nargs) .long entry
	.section .rodata
ALIGN
ENTRY(sys_call_table)
#include <asm/syscall_table.h>
#undef __SYSCALL
