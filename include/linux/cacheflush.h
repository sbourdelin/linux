#ifndef CACHEFLUSH_H
#define CACHEFLUSH_H

#include <asm/cacheflush.h>

#ifndef ARCH_HAS_FORCE_CACHE
static inline void kernel_force_cache_clean(struct page *page, size_t size) { }
static inline void kernel_force_cache_invalidate(struct page *page, size_t size) { }
#endif

#endif
