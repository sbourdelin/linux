/* SPDX-License-Identifier: GPL-2.0 */
/*
 * crc64.h
 *
 * See lib/crc64.c for the related specification and polynomical arithmetic.
 */
#ifndef _LINUX_CRC64_H
#define _LINUX_CRC64_H

#include <linux/types.h>

__le64 crc64_le_update(__le64 crc, const void *_p, size_t len);
__le64 crc64_le(const void *p, size_t len);
__le64 crc64_le_bch(const void *p, size_t len);
#endif /* _LINUX_CRC64_H */
