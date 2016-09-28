/*
 * Copyright (c) 2007-2011 Nicira Networks.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */

#ifndef _LINUX_SW_FLOW_H
#define _LINUX_SW_FLOW_H 1

#include <net/ip_tunnels.h>
#include <uapi/linux/openvswitch.h>

struct vlan_head {
	__be16 tpid; /* Vlan type. Generally 802.1q or 802.1ad.*/
	__be16 tci;  /* 0 if no VLAN, VLAN_TAG_PRESENT set otherwise. */
};

struct sw_flow_key {
	u8 tun_opts[IP_TUNNEL_OPTS_MAX];
	u8 tun_opts_len;
	struct ip_tunnel_key tun_key;	/* Encapsulating tunnel key. */
	struct {
		u32	priority;	/* Packet QoS priority. */
		u32	skb_mark;	/* SKB mark. */
		u16	in_port;	/* Input switch port (or DP_MAX_PORTS). */
	} __packed phy; /* Safe when right after 'tun_key'. */
	u8 tun_proto;			/* Protocol of encapsulating tunnel. */
	u32 ovs_flow_hash;		/* Datapath computed hash value.  */
	u32 recirc_id;			/* Recirculation ID.  */
	struct {
		u8     src[ETH_ALEN];	/* Ethernet source address. */
		u8     dst[ETH_ALEN];	/* Ethernet destination address. */
		struct vlan_head vlan;
		struct vlan_head cvlan;
		__be16 type;		/* Ethernet frame type. */
	} eth;
	union {
		struct {
			__be32 top_lse;	/* top label stack entry */
		} mpls;
		struct {
			u8     proto;	/* IP protocol or lower 8 bits of ARP opcode. */
			u8     tos;	    /* IP ToS. */
			u8     ttl;	    /* IP TTL/hop limit. */
			u8     frag;	/* One of OVS_FRAG_TYPE_*. */
		} ip;
	};
	struct {
		__be16 src;		/* TCP/UDP/SCTP source port. */
		__be16 dst;		/* TCP/UDP/SCTP destination port. */
		__be16 flags;		/* TCP flags. */
	} tp;
	union {
		struct {
			struct {
				__be32 src;	/* IP source address. */
				__be32 dst;	/* IP destination address. */
			} addr;
			struct {
				u8 sha[ETH_ALEN];	/* ARP source hardware address. */
				u8 tha[ETH_ALEN];	/* ARP target hardware address. */
			} arp;
		} ipv4;
		struct {
			struct {
				struct in6_addr src;	/* IPv6 source address. */
				struct in6_addr dst;	/* IPv6 destination address. */
			} addr;
			__be32 label;			/* IPv6 flow label. */
			struct {
				struct in6_addr target;	/* ND target address. */
				u8 sll[ETH_ALEN];	/* ND source link layer address. */
				u8 tll[ETH_ALEN];	/* ND target link layer address. */
			} nd;
		} ipv6;
	};
	struct {
		/* Connection tracking fields. */
		u16 zone;
		u32 mark;
		u8 state;
		struct ovs_key_ct_labels labels;
	} ct;

} __aligned(BITS_PER_LONG/8); /* Ensure that we can do comparisons as longs. */


#endif /* _LINUX_SW_FLOW_H */
