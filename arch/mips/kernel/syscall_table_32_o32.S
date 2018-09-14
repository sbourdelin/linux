/* SPDX-License-Identifier: GPL-2.0 */

#define __SYSCALL(nr, entry, nargs) PTR entry
	
.align	2
.type	sys_call_table, @object
EXPORT(sys_call_table)
#include <asm/syscall_table_32_o32.h>
