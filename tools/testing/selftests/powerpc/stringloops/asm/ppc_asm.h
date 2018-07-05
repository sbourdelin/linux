/* SPDX-License-Identifier: GPL-2.0 */

#include <ppc-asm.h>

#ifndef r1
#define r1 sp
#endif

#define _GLOBAL(A) FUNC_START(test_ ## A)

#ifndef __powerpc64__
#define SZL		4
#define PPC_LLU		lwzu
#define PPC_LCMPI	cmplwi
#define PPC_ROTLI	rotlwi
#define PPC_CNTLZL	cntlzw
#define PPC_SRLI	srwi
#endif
