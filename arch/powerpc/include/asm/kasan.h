/*
 * Copyright 2017 Balbir Singh, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __ASM_KASAN_H
#define __ASM_KASAN_H

#ifndef __ASSEMBLY__

#include <asm/pgtable.h>

#if defined(CONFIG_KASAN) && defined(CONFIG_PPC_RADIX_MMU)
extern void kasan_init(void);
#include <asm/book3s/64/radix-kasan.h>

#else
static inline void kasan_init(void) {}
#endif

#endif
#endif
