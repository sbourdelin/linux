/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
#ifdef CONFIG_X86_INTEL_CET
#define rlimit_as_extra() current->thread.cet.ibt_bitmap_size
#endif

#include <asm-generic/resource.h>
