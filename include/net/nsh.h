/*
 * Network Service Header (NSH) inserted onto encapsulated packets
 * or frames to realize service function paths.
 * NSH also provides a mechanism for metadata exchange along the
 * instantiated service path.
 *
 * https://tools.ietf.org/html/draft-ietf-sfc-nsh-01
 *
 * Copyright (c) 2015 by Brocade Communications Systems, Inc.
 * All rights reserved.
 */
#ifndef __NET_NSH_H
#define __NET_NSH_H

#include <linux/types.h>
#include <linux/skbuff.h>

/*
 * NSH Base Header + Service Path Header
 *
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |Ver|O|C|R|R|R|R|R|R|   Length  |    MD Type    | Next Protocol |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |          Service Path ID                      | Service Index |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * Ver - Version, set to 0
 * O - Indicates payload is OAM.
 * C - Indicates critical metadata TLV is present (must be 0 for MD type 1).
 * Length - total header length in 4-byte words.
 * MD Type - Metadata type
 *           Type 1 - 4 mandatory 4 byte context headers.
 *           Type 2 - 0 or more var length context headers.
 * Next Protocol - protocol type of original packet.
 * Service Path ID (SPI) - identifies a service path. Participating nodes
 *                         MUST use this identifier for Service Function
 *                         Path selection.
 * Service Index (SI) - provides location within the SFP.
 */
#define NSH_BF_VER0     0
#define NSH_BF_VER_MASK 0xc0
#define NSH_BF_OAM      BIT(5)
#define NSH_BF_CRIT     BIT(4)
#define NSH_N_SPI       (1u << 24)
#define NSH_SPI_MASK    ((NSH_N_SPI-1) << 8)
#define NSH_N_SI        (1u << 8)
#define NSH_SI_MASK     (NSH_N_SI-1)

#define NSH_MD_TYPE_1   1
#define NSH_MD_TYPE_2   2

#define NSH_NEXT_PROTO_IPv4 1
#define NSH_NEXT_PROTO_IPv6 2
#define NSH_NEXT_PROTO_ETH  3

#define NSH_LEN_TYPE_1     6
#define NSH_LEN_TYPE_2_MIN 2

struct nsh_base {
	__u8 base_flags;
	__u8 length;
	__u8 md_type;
	__u8 next_proto;
};

struct nsh_header {
	struct nsh_base base;
	__be32 sp_header;
};

/*
 * When the Base Header specifies MD Type 1, four 4-byte Context Headers
 * MUST be added immediately following the Service Path Header. Thus length
 * in the base header is set to 6.
 * Context Headers that carry no metadata MUST be set to zero.
 */
#define NSH_MD_TYPE_1_NUM_HDRS 4

struct nsh_md_type_1 {
	__be32 ctx_hdr1;
	__be32 ctx_hdr2;
	__be32 ctx_hdr3;
	__be32 ctx_hdr4;
};

/*
 * When the Base Header specifies MD Type 2, zero or more variable
 * length Context Headers follow the Service Path Header.
 *
 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     |          TLV Class            |C|    Type     |R|R|R|   Len   |
 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     |                      Variable Metadata                        |
 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * TLV Class - Scope of class (e.g. may be vendor or standards body).
 * Type - Specific type of information within the scope of given class.
 *        C bit (MSB) indicates criticality. When set, receiver must process.
 * Len - Length of variable metadata in 4-byte words.
 */
#define NSH_TYPE_CRIT BIT(7)

struct nsh_md_type_2 {
	__be16 tlv_class;
	__u8 tlv_type;
	__u8 length;
};

/*
 * Context header for encap/decap.
 */
#define NSH_MD_CLASS_TYPE_1 USHRT_MAX
#define NSH_MD_TYPE_TYPE_1  U8_MAX
#define NSH_MD_LEN_TYPE_1   4

struct nsh_metadata {
	u_short class;
	u_char crit;
	u_char type;
	u_int len;  /* 4 byte words */
	void *data;
};

/*
 * Parse NSH header and notify registered listeners about any metadata.
 */
int nsh_decap(struct sk_buff *skb,
	      u_int *spi,
	      u_char *si,
	      u_char *np);

/*
 * Add NSH header.
 */
int nsh_encap(struct sk_buff *skb,
	      u_int spi,
	      u_char si,
	      u_char np,
	      u_int num_ctx_hdrs,
	      struct nsh_metadata *ctx_hdrs);


/* Register hooks to be informed of nsh metadata of specified class */
struct nsh_listener {
	struct list_head list;
	u_short class;
	u_char max_ctx_hdrs;
	int (*notify)(struct sk_buff *skb,
		      u_int service_path_id,
		      u_char service_index,
		      u_char next_proto,
		      struct nsh_metadata *ctx_hdrs,
		      u_int num_ctx_hdrs);
};

int nsh_register_listener(struct nsh_listener *listener);
int nsh_unregister_listener(struct nsh_listener *listener);
#endif /* __NET_NSH_H */
