/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_KASAN_H
#define __ASM_KASAN_H

#ifndef __ASSEMBLY__

#include <asm/pgtable-types.h>
#include <asm/fixmap.h>

#define KASAN_SHADOW_SCALE_SHIFT	3
#define KASAN_SHADOW_SIZE	((~0UL - PAGE_OFFSET + 1) >> KASAN_SHADOW_SCALE_SHIFT)

#define KASAN_SHADOW_START      (ALIGN_DOWN(FIXADDR_START - KASAN_SHADOW_SIZE, PGDIR_SIZE))
#define KASAN_SHADOW_END        (KASAN_SHADOW_START + KASAN_SHADOW_SIZE)
#define KASAN_SHADOW_OFFSET     (KASAN_SHADOW_START - (PAGE_OFFSET >> KASAN_SHADOW_SCALE_SHIFT))

void kasan_early_init(void);
void kasan_init(void);

#endif
#endif
