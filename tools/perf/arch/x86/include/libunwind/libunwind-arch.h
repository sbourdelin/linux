#ifndef _LIBUNWIND_ARCH_H
#define _LIBUNWIND_ARCH_H

#include <libunwind-x86.h>
#include <../perf_regs.h>
#include <../../../../../../arch/x86/include/uapi/asm/perf_regs.h>

#define LIBUNWIND_X86_32
int libunwind__x86_reg_id(int regnum);

#include <../../../x86/util/unwind-libunwind.c>

#define LIBUNWIND__ARCH_REG_ID libunwind__x86_reg_id

#define UNWT_PREFIX	UNW_PASTE(UNW_PASTE(_U, x86), _)
#define UNWT_OBJ(fn)	UNW_PASTE(UNWT_PREFIX, fn)

#endif /* _LIBUNWIND_ARCH_H */
