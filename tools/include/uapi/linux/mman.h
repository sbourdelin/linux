#ifndef _UAPI_LINUX_MMAN_H
#define _UAPI_LINUX_MMAN_H

#include <uapi/asm/mman.h>

#define MREMAP_MAYMOVE	0x01
#define MREMAP_FIXED	0x02
#define MREMAP_MIRROR	0x04

#define OVERCOMMIT_GUESS		0
#define OVERCOMMIT_ALWAYS		1
#define OVERCOMMIT_NEVER		2

#endif /* _UAPI_LINUX_MMAN_H */
