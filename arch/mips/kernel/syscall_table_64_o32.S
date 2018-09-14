/* SPDX-License-Identifier: GPL-2.0 */

#define __SYSCALL(nr, entry, nargs) PTR entry
	
.align	3
.type	sys32_call_table,@object
EXPORT(sys32_call_table)
#include <asm/syscall_table_64_o32.h>
.size	sys32_call_table,.-sys32_call_table
