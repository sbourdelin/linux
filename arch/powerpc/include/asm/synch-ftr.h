/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_POWERPC_SYNCH_FTR_H
#define _ASM_POWERPC_SYNCH_FTR_H
#ifdef __KERNEL__

#include <asm/feature-fixups.h>
#include <asm/asm-const.h>

#if defined(__powerpc64__)
#    define LWSYNC	lwsync
#elif defined(CONFIG_E500)
#    define LWSYNC					\
	START_LWSYNC_SECTION(96);			\
	sync;						\
	MAKE_LWSYNC_SECTION_ENTRY(96, __lwsync_fixup);
#else
#    define LWSYNC	sync
#endif

#ifdef CONFIG_SMP
#define __PPC_ACQUIRE_BARRIER				\
	START_LWSYNC_SECTION(97);			\
	isync;						\
	MAKE_LWSYNC_SECTION_ENTRY(97, __lwsync_fixup);
#define PPC_ACQUIRE_BARRIER	 "\n" stringify_in_c(__PPC_ACQUIRE_BARRIER)
#define PPC_RELEASE_BARRIER	 stringify_in_c(LWSYNC) "\n"
#define PPC_ATOMIC_ENTRY_BARRIER "\n" stringify_in_c(sync) "\n"
#define PPC_ATOMIC_EXIT_BARRIER	 "\n" stringify_in_c(sync) "\n"
#else
#define PPC_ACQUIRE_BARRIER
#define PPC_RELEASE_BARRIER
#define PPC_ATOMIC_ENTRY_BARRIER
#define PPC_ATOMIC_EXIT_BARRIER
#endif

#endif /* __KERNEL__ */
#endif	/* _ASM_POWERPC_SYNCH_FTR_H */
