#ifndef __INET_HELPER_H
#define __INET_HELPER_H

#include <linux/inet.h>

#if defined(__LITTLE_ENDIAN_BITFIELD)
#define _htonl(A) __builtin_bswap32(A)
#elif defined(__BIG_ENDIAN_BITFIELD)
#define _htonl(A) (A)
#else
#error "Fix asm/byteorder.h"
#endif

#if defined(__LITTLE_ENDIAN_BITFIELD)
#define _ntohl(A) __builtin_bswap32(A)
#elif defined(__BIG_ENDIAN_BITFIELD)
#define _ntohl(A) (A)
#else
#error "Fix asm/byteorder.h"
#endif

#if defined(__LITTLE_ENDIAN_BITFIELD)
#define _htonll(A) __builtin_bswap64(A)
#elif defined(__BIG_ENDIAN_BITFIELD)
#define _htonll(A) (A)
#else
#error "Fix asm/byteorder.h"
#endif

#if defined(__LITTLE_ENDIAN_BITFIELD)
#define _ntohll(A) __builtin_bswap64(A)
#elif defined(__BIG_ENDIAN_BITFIELD)
#define _ntohll(A) (A)
#else
#error "Fix asm/byteorder.h"
#endif

#endif
