/*
 * Copyright 2010 Tilera Corporation. All Rights Reserved.
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation, version 2.
 *
 *   This program is distributed in the hope that it will be useful, but
 *   WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 *   NON INFRINGEMENT.  See the GNU General Public License for
 *   more details.
 */

#ifndef _ASM_TILE_FTRACE_H
#define _ASM_TILE_FTRACE_H

#ifdef CONFIG_FUNCTION_TRACER

#define MCOUNT_ADDR ((unsigned long)(__mcount))
#define MCOUNT_INSN_SIZE 8		/* sizeof mcount call */

#ifndef __ASSEMBLY__
extern void __mcount(void);

#define ARCH_SUPPORTS_FTRACE_OPS 1

#ifdef CONFIG_DYNAMIC_FTRACE
static inline unsigned long ftrace_call_adjust(unsigned long addr)
{
	return addr;
}

struct dyn_arch_ftrace {
};
#endif /*  CONFIG_DYNAMIC_FTRACE */

#if defined(CONFIG_FTRACE_SYSCALLS) && defined(CONFIG_COMPAT)
#include <asm/compat.h>

#define ARCH_COMPAT_SYSCALL_NUMBERS_OVERLAP 1
static inline bool arch_trace_is_compat_syscall(struct pt_regs *regs)
{
	return in_compat_syscall();
}
#endif /* CONFIG_FTRACE_SYSCALLS && CONFIG_COMPAT */

#endif /* __ASSEMBLY__ */

#endif /* CONFIG_FUNCTION_TRACER */

#endif /* _ASM_TILE_FTRACE_H */
