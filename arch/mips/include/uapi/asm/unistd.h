/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1995, 96, 97, 98, 99, 2000 by Ralf Baechle
 * Copyright (C) 1999, 2000 Silicon Graphics, Inc.
 *
 * Changed system calls macros _syscall5 - _syscall7 to push args 5 to 7 onto
 * the stack. Robin Farine for ACN S.A, Copyright (C) 1996 by ACN S.A
 */
#ifndef _UAPI_ASM_UNISTD_H
#define _UAPI_ASM_UNISTD_H

#include <asm/sgidefs.h>

#if _MIPS_SIM == _MIPS_SIM_ABI32

/*
 * Linux o32 style syscalls are in the range from 4000 to 4999.
 */
#define __NR_Linux 4000
#include <asm/unistd_32.h>

/*
 * Offset of the last Linux o32 flavoured syscall
 */
#define __NR_Linux_syscalls             __NR_syscalls

#endif /* _MIPS_SIM == _MIPS_SIM_ABI32 */

#define __NR_O32_Linux			4000
#define __NR_O32_Linux_syscalls		__NR_syscalls

#if _MIPS_SIM == _MIPS_SIM_ABI64

/*
 * Linux 64-bit syscalls are in the range from 5000 to 5999.
 */
#define __NR_Linux 5000
#include <asm/unistd_64.h>

/*
 * Offset of the last Linux 64-bit flavoured syscall
 */
#define __NR_Linux_syscalls             __NR_syscalls

#endif /* _MIPS_SIM == _MIPS_SIM_ABI64 */

#define __NR_64_Linux			5000
#define __NR_64_Linux_syscalls		__NR_syscalls

#if _MIPS_SIM == _MIPS_SIM_NABI32

/*
 * Linux N32 syscalls are in the range from 6000 to 6999.
 */
#define __NR_Linux 6000
#include <asm/unistd_n32.h>

/*
 * Offset of the last N32 flavoured syscall
 */
#define __NR_Linux_syscalls             __NR_syscalls

#endif /* _MIPS_SIM == _MIPS_SIM_NABI32 */

#define __NR_N32_Linux			6000
#define __NR_N32_Linux_syscalls		__NR_syscalls

#endif /* _UAPI_ASM_UNISTD_H */
