#ifndef _SIR_H
#define _SIR_H

#include <linux/types.h>
#include <linux/in6.h>
#include <asm/byteorder.h>

#define SIR_T_LOCAL 0x1
#define SIR_T_VIRTUAL 0x3

struct in6_addr_sir {
	__be64 prefix;
	__be64 identifier_c_type;
} __packed;

struct in6_addr_ila {
	__be64 locator;
#if defined(__LITTLE_ENDIAN_BITFIELD)
	__u8 identifier:4,
	     c:1,
	     type:3;
	__u8  identifier2;
	__be16 identifier3;
	__be16 identifier4;
#elif defined(__BIG_ENDIAN_BITFIELD)
	__be32 type:3,
	       c:1,
	       identifier:28;
	__be16 identifier2;
#else
#error "Fix asm/byteorder.h"
#endif
	__be16 checksum;
} __packed;

struct sirhdr {
#if defined(__LITTLE_ENDIAN_BITFIELD)
	__u16 traffic_class:4,
	version:4,
	flow_label:4,
	traffic_class2:4;
	__be16 flow_label2;
#elif defined(__BIG_ENDIAN_BITFIELD)
	__u32 version:4,
	      traffic_class:8,
	      flow_label:20;
#else
#error "Fix asm/byteorder.h"
#endif
	__be16 payload_length;
	__u8   next_header;
	__u8   hop_limit;

	struct in6_addr source_address;
	struct in6_addr_sir destination_address;
} __packed;

struct ilahdr {
#if defined(__LITTLE_ENDIAN_BITFIELD)
	__u16 traffic_class:4,
	version:4,
	flow_label:4,
	traffic_class2:4;
	__be16 flow_label2;
#elif defined(__BIG_ENDIAN_BITFIELD)
	__u32 version:4,
	      traffic_class:8,
	      flow_label:20;
#else
#error "Fix asm/byteorder.h"
#endif
	__be16 payload_length;
	__u8   next_header;
	__u8   hop_limit;

	struct in6_addr source_address;
	struct in6_addr_ila destination_address;
} __packed;

#endif
