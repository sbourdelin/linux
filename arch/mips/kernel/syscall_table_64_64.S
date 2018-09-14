/* SPDX-License-Identifier: GPL-2.0 */

#define __SYSCALL(nr, entry, nargs) PTR entry
	
.align	3
.type	sys_call_table, @object
EXPORT(sys_call_table)
#include <asm/syscall_table_64_64.h>
.size	sys_call_table,.-sys_call_table
