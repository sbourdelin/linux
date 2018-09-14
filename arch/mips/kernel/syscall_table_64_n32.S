/* SPDX-License-Identifier: GPL-2.0 */

#define __SYSCALL(nr, entry, nargs) PTR entry
	
.type	sysn32_call_table, @object
EXPORT(sysn32_call_table)
#include <asm/syscall_table_64_n32.h>
.size	sysn32_call_table,.-sysn32_call_table
