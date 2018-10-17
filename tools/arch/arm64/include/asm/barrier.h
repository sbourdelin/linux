/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _TOOLS_LINUX_ASM_AARCH64_BARRIER_H
#define _TOOLS_LINUX_ASM_AARCH64_BARRIER_H

/*
 * From tools/perf/perf-sys.h, last modified in:
 * f428ebd184c82a7914b2aa7e9f868918aaf7ea78 perf tools: Fix AAAAARGH64 memory barriers
 *
 * XXX: arch/arm64/include/asm/barrier.h in the kernel sources use dsb, is this
 * a case like for arm32 where we do things differently in userspace?
 */

#define mb()		asm volatile("dmb ish" ::: "memory")
#define wmb()		asm volatile("dmb ishst" ::: "memory")
#define rmb()		asm volatile("dmb ishld" ::: "memory")

/*
 * Kernel uses dmb variants on arm64 for smp_*() barriers. Pretty much the same
 * implementation as above mb()/wmb()/rmb(), though for the latter kernel uses
 * dsb. In any case, should above mb()/wmb()/rmb() change, make sure the below
 * smp_*() don't.
 */
#define smp_mb()	asm volatile("dmb ish" ::: "memory")
#define smp_wmb()	asm volatile("dmb ishst" ::: "memory")
#define smp_rmb()	asm volatile("dmb ishld" ::: "memory")

#endif /* _TOOLS_LINUX_ASM_AARCH64_BARRIER_H */
