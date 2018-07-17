/* SPDX-License-Identifier: GPL-2.0 */
/*
 * See lib/crc64.c for the related specification and polynomical arithmetic.
 */
#ifndef _LINUX_CRC64_H
#define _LINUX_CRC64_H

#include <linux/types.h>

u64 __pure crc64_update(u64 crc, const void *_p, size_t len);
u64 __pure crc64(const void *p, size_t len);
u64 __pure crc64_bch(const void *p, size_t len);
#endif /* _LINUX_CRC64_H */
