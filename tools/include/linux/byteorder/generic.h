#ifndef _TOOLS_LINUX_BYTEORDER_GENERIC_H
#define _TOOLS_LINUX_BYTEORDER_GENERIC_H

#include <endian.h>
#include <byteswap.h>

#define cpu_to_le64 __cpu_to_le64
#define le64_to_cpu __le64_to_cpu
#define cpu_to_le32 __cpu_to_le32
#define le32_to_cpu __le32_to_cpu
#define cpu_to_le16 __cpu_to_le16
#define le16_to_cpu __le16_to_cpu
#define cpu_to_be64 __cpu_to_be64
#define be64_to_cpu __be64_to_cpu
#define cpu_to_be32 __cpu_to_be32
#define be32_to_cpu __be32_to_cpu
#define cpu_to_be16 __cpu_to_be16
#define be16_to_cpu __be16_to_cpu

#if __BYTE_ORDER == __BIG_ENDIAN
#define __cpu_to_le16 bswap_16
#define __cpu_to_le32 bswap_32
#define __cpu_to_le64 bswap_64
#define __le16_to_cpu bswap_16
#define __le32_to_cpu bswap_32
#define __le64_to_cpu bswap_64
#define __cpu_to_be16
#define __cpu_to_be32
#define __cpu_to_be64
#define __be16_to_cpu
#define __be32_to_cpu
#define __be64_to_cpu
#else
#define __cpu_to_le16
#define __cpu_to_le32
#define __cpu_to_le64
#define __le16_to_cpu
#define __le32_to_cpu
#define __le64_to_cpu
#define __cpu_to_be16 bswap_16
#define __cpu_to_be32 bswap_32
#define __cpu_to_be64 bswap_64
#define __be16_to_cpu bswap_16
#define __be32_to_cpu bswap_32
#define __be64_to_cpu bswap_64
#endif

#endif /* _TOOLS_LINUX_BYTEORDER_GENERIC_H */
