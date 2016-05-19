#ifndef _LIBUNWIND_ARCH_H
#define _LIBUNWIND_ARCH_H

#include <libunwind-aarch64.h>
#include <../perf_regs.h>
#include <../../../../../../arch/arm64/include/uapi/asm/perf_regs.h>

#define LIBUNWIND_AARCH64
int libunwind__aarch64_reg_id(int regnum);

#include <../../../arm64/util/unwind-libunwind.c>

#define LIBUNWIND__ARCH_REG_ID libunwind__aarch64_reg_id

#define UNWT_PREFIX	UNW_PASTE(UNW_PASTE(_U, aarch64), _)
#define UNWT_OBJ(fn)	UNW_PASTE(UNWT_PREFIX, fn)

#endif /* _LIBUNWIND_ARCH_H */
