#ifndef _ASMARM_SHMPARAM_H
#define _ASMARM_SHMPARAM_H

#include <asm/cachetype.h>

/*
 * This should be the size of the virtually indexed cache/ways,
 * or page size, whichever is greater since the cache aliases
 * every size/ways bytes.
 */
#define	SHMLBA (cache_is_vipt_aliasing() ? 4 * PAGE_SIZE : PAGE_SIZE)

/* Enforce SHMLBA in shmat */
#define __ARCH_FORCE_SHMLBA

#endif /* _ASMARM_SHMPARAM_H */
