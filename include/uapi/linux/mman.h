#ifndef _UAPI_LINUX_MMAN_H
#define _UAPI_LINUX_MMAN_H

#include <asm/mman.h>

#define MREMAP_MAYMOVE	1 /* VMA can move after remap and resize */
#define MREMAP_FIXED	2 /* VMA can remap at particular address */

/* NOTE: MREMAP_FIXED must be set with MREMAP_MAYMOVE, not alone */

#define OVERCOMMIT_GUESS		0
#define OVERCOMMIT_ALWAYS		1
#define OVERCOMMIT_NEVER		2

#endif /* _UAPI_LINUX_MMAN_H */
