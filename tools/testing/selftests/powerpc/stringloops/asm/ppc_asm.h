/* SPDX-License-Identifier: GPL-2.0 */
#if !defined(CONFIG_PPC64) && !defined(CONFIG_PPC32)
#ifdef __powerpc64__
#define CONFIG_PPC64
#else
#define CONFIG_PPC32
#endif
#endif

#include <ppc-asm.h>

#ifndef r1
#define r1 sp
#endif

#define _GLOBAL(A) FUNC_START(test_ ## A)

#ifdef __powerpc64__
#define SZL		8
#define PPC_LLU		ldu
#define PPC_LCMPI	cmpldi
#define PPC_ROTLI	rotldi
#else
#define SZL		4
#define PPC_LLU		lwzu
#define PPC_LCMPI	cmplwi
#define PPC_ROTLI	rotlwi
#endif
