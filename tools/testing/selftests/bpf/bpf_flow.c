// SPDX-License-Identifier: GPL-2.0
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <linux/pkt_cls.h>
#include <linux/bpf.h>
#include <linux/in.h>
#include <linux/if_ether.h>
#include <linux/icmp.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/if_packet.h>
#include <sys/socket.h>
#include <linux/if_tunnel.h>
#include <linux/mpls.h>
#include "bpf_helpers.h"
#include "bpf_endian.h"

int _version SEC("version") = 1;
#define PROG(F) SEC(#F) int bpf_func_##F

/* These are the identifiers of the BPF programs that will be used in tail
 * calls. Name is limited to 16 characters, with the terminating character and
 * bpf_func_ above, we have only 6 to work with, anything after will be cropped.
 */
enum {
	IP,
	IPV6,
	IPV6OP,	/* Destination/Hop-by-Hop Options IPv6 Extension header */
	IPV6FR,	/* Fragmentation IPv6 Extension Header */
	MPLS,
	VLAN,
	GUE,
};

#define IP_MF		0x2000
#define IP_OFFSET	0x1FFF
#define IP6_MF		0x0001
#define IP6_OFFSET	0xFFF8

struct vlan_hdr {
	__be16 h_vlan_TCI;
	__be16 h_vlan_encapsulated_proto;
};

struct gre_hdr {
	__be16 flags;
	__be16 proto;
};

#define GUE_PORT 6080
/* Taken from include/net/gue.h. Move that to uapi, instead? */
struct guehdr {
	union {
		struct {
#if defined(__LITTLE_ENDIAN_BITFIELD)
			__u8	hlen:5,
				control:1,
				version:2;
#elif defined (__BIG_ENDIAN_BITFIELD)
			__u8	version:2,
				control:1,
				hlen:5;
#else
#error  "Please fix <asm/byteorder.h>"
#endif
			__u8	proto_ctype;
			__be16	flags;
		};
		__be32	word;
	};
};

enum flow_dissector_key_id {
	FLOW_DISSECTOR_KEY_CONTROL, /* struct flow_dissector_key_control */
	FLOW_DISSECTOR_KEY_BASIC, /* struct flow_dissector_key_basic */
	FLOW_DISSECTOR_KEY_IPV4_ADDRS, /* struct flow_dissector_key_ipv4_addrs */
	FLOW_DISSECTOR_KEY_IPV6_ADDRS, /* struct flow_dissector_key_ipv6_addrs */
	FLOW_DISSECTOR_KEY_PORTS, /* struct flow_dissector_key_ports */
	FLOW_DISSECTOR_KEY_ICMP, /* struct flow_dissector_key_icmp */
	FLOW_DISSECTOR_KEY_ETH_ADDRS, /* struct flow_dissector_key_eth_addrs */
	FLOW_DISSECTOR_KEY_TIPC, /* struct flow_dissector_key_tipc */
	FLOW_DISSECTOR_KEY_ARP, /* struct flow_dissector_key_arp */
	FLOW_DISSECTOR_KEY_VLAN, /* struct flow_dissector_key_flow_vlan */
	FLOW_DISSECTOR_KEY_FLOW_LABEL, /* struct flow_dissector_key_flow_tags */
	FLOW_DISSECTOR_KEY_GRE_KEYID, /* struct flow_dissector_key_keyid */
	FLOW_DISSECTOR_KEY_MPLS_ENTROPY, /* struct flow_dissector_key_keyid */
	FLOW_DISSECTOR_KEY_ENC_KEYID, /* struct flow_dissector_key_keyid */
	FLOW_DISSECTOR_KEY_ENC_IPV4_ADDRS, /* struct flow_dissector_key_ipv4_addrs */
	FLOW_DISSECTOR_KEY_ENC_IPV6_ADDRS, /* struct flow_dissector_key_ipv6_addrs */
	FLOW_DISSECTOR_KEY_ENC_CONTROL, /* struct flow_dissector_key_control */
	FLOW_DISSECTOR_KEY_ENC_PORTS, /* struct flow_dissector_key_ports */
	FLOW_DISSECTOR_KEY_MPLS, /* struct flow_dissector_key_mpls */
	FLOW_DISSECTOR_KEY_TCP, /* struct flow_dissector_key_tcp */
	FLOW_DISSECTOR_KEY_IP, /* struct flow_dissector_key_ip */
	FLOW_DISSECTOR_KEY_CVLAN, /* struct flow_dissector_key_flow_vlan */

	FLOW_DISSECTOR_KEY_MAX,
};

struct flow_dissector_key_control {
	__u16	thoff;
	__u16	addr_type;
	__u32	flags;
};

#define FLOW_DIS_IS_FRAGMENT	(1 << 0)
#define FLOW_DIS_FIRST_FRAG	(1 << 1)
#define FLOW_DIS_ENCAPSULATION	(1 << 2)

struct flow_dissector_key_basic {
	__be16	n_proto;
	__u8	ip_proto;
	__u8	padding;
};

struct flow_dissector_key_ipv4_addrs {
	__be32 src;
	__be32 dst;
};

struct flow_dissector_key_ipv6_addrs {
	struct in6_addr src;
	struct in6_addr dst;
};

struct flow_dissector_key_addrs {
	union {
		struct flow_dissector_key_ipv4_addrs v4addrs;
		struct flow_dissector_key_ipv6_addrs v6addrs;
	};
};

struct flow_dissector_key_ports {
	union {
		__be32 ports;
		struct {
			__be16 src;
			__be16 dst;
		};
	};
};

struct bpf_map_def SEC("maps") jmp_table = {
	.type = BPF_MAP_TYPE_PROG_ARRAY,
	.key_size = sizeof(__u32),
	.value_size = sizeof(__u32),
	.max_entries = 8
};

struct bpf_dissect_cb {
	__u16 nhoff;
	__u16 flags;
};

/* Dispatches on ETHERTYPE */
static __always_inline int parse_eth_proto(struct __sk_buff *skb, __be16 proto)
{
	switch (proto) {
	case bpf_htons(ETH_P_IP):
		bpf_tail_call(skb, &jmp_table, IP);
		break;
	case bpf_htons(ETH_P_IPV6):
		bpf_tail_call(skb, &jmp_table, IPV6);
		break;
	case bpf_htons(ETH_P_MPLS_MC):
	case bpf_htons(ETH_P_MPLS_UC):
		bpf_tail_call(skb, &jmp_table, MPLS);
		break;
	case bpf_htons(ETH_P_8021Q):
	case bpf_htons(ETH_P_8021AD):
		bpf_tail_call(skb, &jmp_table, VLAN);
		break;
	default:
		/* Protocol not supported */
		return BPF_DROP;
	}

	return BPF_DROP;
}

static __always_inline int write_ports(struct __sk_buff *skb, __u8 proto)
{
	struct bpf_dissect_cb *cb = (struct bpf_dissect_cb *)(skb->cb);
	struct flow_dissector_key_ports ports;

	/* The supported protocols always start with the ports */
	if (bpf_skb_load_bytes(skb, cb->nhoff, &ports, sizeof(ports)))
		return BPF_DROP;

	if (proto == IPPROTO_UDP && ports.dst == bpf_htons(GUE_PORT)) {
		/* GUE encapsulation */
		cb->nhoff += sizeof(struct udphdr);
		bpf_tail_call(skb, &jmp_table, GUE);
		return BPF_DROP;
	}

	if (bpf_flow_dissector_write_keys(skb, &ports, sizeof(ports),
					  FLOW_DISSECTOR_KEY_PORTS))
		return BPF_DROP;

	return BPF_OK;
}

SEC("dissect")
int dissect(struct __sk_buff *skb)
{
	if (!skb->vlan_present)
		return parse_eth_proto(skb, skb->protocol);
	else
		return parse_eth_proto(skb, skb->vlan_proto);
}

/* Parses on IPPROTO_* */
static __always_inline int parse_ip_proto(struct __sk_buff *skb, __u8 proto)
{
	struct bpf_dissect_cb *cb = (struct bpf_dissect_cb *)(skb->cb);
	__u8 *data_end = (__u8 *)(long)skb->data_end;
	__u8 *data = (__u8 *)(long)skb->data;
	__u32 data_len = data_end - data;
	struct gre_hdr gre;
	struct ethhdr eth;
	struct tcphdr tcp;

	switch (proto) {
	case IPPROTO_ICMP:
		if (cb->nhoff + sizeof(struct icmphdr) > data_len)
			return BPF_DROP;
		return BPF_OK;
	case IPPROTO_IPIP:
		cb->flags |= FLOW_DIS_ENCAPSULATION;
		bpf_tail_call(skb, &jmp_table, IP);
		break;
	case IPPROTO_IPV6:
		cb->flags |= FLOW_DIS_ENCAPSULATION;
		bpf_tail_call(skb, &jmp_table, IPV6);
		break;
	case IPPROTO_GRE:
		if (bpf_skb_load_bytes(skb, cb->nhoff, &gre, sizeof(gre)))
			return BPF_DROP;

		if (bpf_htons(gre.flags & GRE_VERSION))
			/* Only inspect standard GRE packets with version 0 */
			return BPF_OK;

		cb->nhoff += sizeof(gre); /* Step over GRE Flags and Protocol */
		if (GRE_IS_CSUM(gre.flags))
			cb->nhoff += 4; /* Step over chksum and Padding */
		if (GRE_IS_KEY(gre.flags))
			cb->nhoff += 4; /* Step over key */
		if (GRE_IS_SEQ(gre.flags))
			cb->nhoff += 4; /* Step over sequence number */

		cb->flags |= FLOW_DIS_ENCAPSULATION;

		if (gre.proto == bpf_htons(ETH_P_TEB)) {
			if (bpf_skb_load_bytes(skb, cb->nhoff, &eth,
					       sizeof(eth)))
				return BPF_DROP;

			cb->nhoff += sizeof(eth);

			return parse_eth_proto(skb, eth.h_proto);
		} else {
			return parse_eth_proto(skb, gre.proto);
		}

	case IPPROTO_TCP:
		if (cb->nhoff + sizeof(struct tcphdr) > data_len)
			return BPF_DROP;

		if (bpf_skb_load_bytes(skb, cb->nhoff, &tcp, sizeof(tcp)))
			return BPF_DROP;

		if (tcp.doff < 5)
			return BPF_DROP;

		if (cb->nhoff + (tcp.doff << 2) > data_len)
			return BPF_DROP;

		return write_ports(skb, proto);
	case IPPROTO_UDP:
	case IPPROTO_UDPLITE:
		if (cb->nhoff + sizeof(struct udphdr) > data_len)
			return BPF_DROP;

		return write_ports(skb, proto);
	default:
		return BPF_DROP;
	}

	return BPF_DROP;
}

static __always_inline int parse_ipv6_proto(struct __sk_buff *skb, __u8 nexthdr)
{
	struct bpf_dissect_cb *cb = (struct bpf_dissect_cb *)(skb->cb);
	struct flow_dissector_key_control control;
	struct flow_dissector_key_basic basic;

	switch (nexthdr) {
	case IPPROTO_HOPOPTS:
	case IPPROTO_DSTOPTS:
		bpf_tail_call(skb, &jmp_table, IPV6OP);
		break;
	case IPPROTO_FRAGMENT:
		bpf_tail_call(skb, &jmp_table, IPV6FR);
		break;
	default:
		control.thoff = cb->nhoff;
		control.addr_type = FLOW_DISSECTOR_KEY_IPV6_ADDRS;
		control.flags = cb->flags;
		if (bpf_flow_dissector_write_keys(skb, &control,
						  sizeof(control),
						  FLOW_DISSECTOR_KEY_CONTROL))
			return BPF_DROP;

		memset(&basic, 0, sizeof(basic));
		basic.n_proto = bpf_htons(ETH_P_IPV6);
		basic.ip_proto = nexthdr;
		if (bpf_flow_dissector_write_keys(skb, &basic, sizeof(basic),
					      FLOW_DISSECTOR_KEY_BASIC))
			return BPF_DROP;

		return parse_ip_proto(skb, nexthdr);
	}

	return BPF_DROP;
}

PROG(IP)(struct __sk_buff *skb)
{
	struct bpf_dissect_cb *cb = (struct bpf_dissect_cb *)(skb->cb);
	__u8 *data_end = (__u8 *)(long)skb->data_end;
	struct flow_dissector_key_control control;
	struct flow_dissector_key_addrs addrs;
	struct flow_dissector_key_basic basic;
	__u8 *data = (__u8 *)(long)skb->data;
	__u32 data_len = data_end - data;
	bool done = false;
	struct iphdr iph;

	if (bpf_skb_load_bytes(skb, cb->nhoff, &iph, sizeof(iph)))
		return BPF_DROP;

	/* IP header cannot be smaller than 20 bytes */
	if (iph.ihl < 5)
		return BPF_DROP;

	addrs.v4addrs.src = iph.saddr;
	addrs.v4addrs.dst = iph.daddr;
	if (bpf_flow_dissector_write_keys(skb, &addrs, sizeof(addrs.v4addrs),
				      FLOW_DISSECTOR_KEY_IPV4_ADDRS))
		return BPF_DROP;

	cb->nhoff += iph.ihl << 2;
	if (cb->nhoff > data_len)
		return BPF_DROP;

	if (iph.frag_off & bpf_htons(IP_MF | IP_OFFSET)) {
		cb->flags |= FLOW_DIS_IS_FRAGMENT;
		if (iph.frag_off & bpf_htons(IP_OFFSET))
			/* From second fragment on, packets do not have headers
			 * we can parse.
			 */
			done = true;
		else
			cb->flags |= FLOW_DIS_FIRST_FRAG;
	}


	control.thoff = cb->nhoff;
	control.addr_type = FLOW_DISSECTOR_KEY_IPV4_ADDRS;
	control.flags = cb->flags;
	if (bpf_flow_dissector_write_keys(skb, &control, sizeof(control),
					  FLOW_DISSECTOR_KEY_CONTROL))
		return BPF_DROP;

	memset(&basic, 0, sizeof(basic));
	basic.n_proto = bpf_htons(ETH_P_IP);
	basic.ip_proto = iph.protocol;
	if (bpf_flow_dissector_write_keys(skb, &basic, sizeof(basic),
				      FLOW_DISSECTOR_KEY_BASIC))
		return BPF_DROP;

	if (done)
		return BPF_OK;

	return parse_ip_proto(skb, iph.protocol);
}

PROG(IPV6)(struct __sk_buff *skb)
{
	struct bpf_dissect_cb *cb = (struct bpf_dissect_cb *)(skb->cb);
	struct flow_dissector_key_addrs addrs;
	struct ipv6hdr ip6h;

	if (bpf_skb_load_bytes(skb, cb->nhoff, &ip6h, sizeof(ip6h)))
		return BPF_DROP;

	addrs.v6addrs.src = ip6h.saddr;
	addrs.v6addrs.dst = ip6h.daddr;
	if (bpf_flow_dissector_write_keys(skb, &addrs, sizeof(addrs.v6addrs),
				      FLOW_DISSECTOR_KEY_IPV6_ADDRS))
		return BPF_DROP;

	cb->nhoff += sizeof(struct ipv6hdr);

	return parse_ipv6_proto(skb, ip6h.nexthdr);
}

PROG(IPV6OP)(struct __sk_buff *skb)
{
	struct bpf_dissect_cb *cb = (struct bpf_dissect_cb *)(skb->cb);
	__u8 proto;
	__u8 hlen;

	if (bpf_skb_load_bytes(skb, cb->nhoff, &proto, sizeof(proto)))
		return BPF_DROP;

	if (bpf_skb_load_bytes(skb, cb->nhoff + sizeof(proto), &hlen,
			       sizeof(hlen)))
		return BPF_DROP;
	/* hlen is in 8-octects and does not include the first 8 bytes
	 * of the header
	 */
	cb->nhoff += (1 + hlen) << 3;

	return parse_ipv6_proto(skb, proto);
}

PROG(IPV6FR)(struct __sk_buff *skb)
{
	struct bpf_dissect_cb *cb = (struct bpf_dissect_cb *)(skb->cb);
	__be16 frag_off;
	__u8 proto;

	if (bpf_skb_load_bytes(skb, cb->nhoff, &proto, sizeof(proto)))
		return BPF_DROP;

	if (bpf_skb_load_bytes(skb, cb->nhoff + 2, &frag_off, sizeof(frag_off)))
		return BPF_DROP;

	cb->nhoff += 8;
	cb->flags |= FLOW_DIS_IS_FRAGMENT;
	if (!(frag_off & bpf_htons(IP6_OFFSET)))
		cb->flags |= FLOW_DIS_FIRST_FRAG;

	return parse_ipv6_proto(skb, proto);
}

PROG(MPLS)(struct __sk_buff *skb)
{
	struct bpf_dissect_cb *cb = (struct bpf_dissect_cb *)(skb->cb);
	struct mpls_label mpls;

	if (bpf_skb_load_bytes(skb, cb->nhoff, &mpls, sizeof(mpls)))
		return BPF_DROP;

	cb->nhoff += sizeof(mpls);

	if (mpls.entry & MPLS_LS_S_MASK) {
		/* This is the last MPLS header. The network layer packet always
		 * follows the MPLS header. Peek forward and dispatch based on
		 * that.
		 */
		__u8 version;

		if (bpf_skb_load_bytes(skb, cb->nhoff, &version,
				       sizeof(version)))
			return BPF_DROP;

		/* IP version is always the first 4 bits of the header */
		switch (version & 0xF0) {
		case 4:
			bpf_tail_call(skb, &jmp_table, IP);
			break;
		case 6:
			bpf_tail_call(skb, &jmp_table, IPV6);
			break;
		default:
			return BPF_DROP;
		}
	} else {
		bpf_tail_call(skb, &jmp_table, MPLS);
	}

	return BPF_DROP;
}

PROG(VLAN)(struct __sk_buff *skb)
{
	struct bpf_dissect_cb *cb = (struct bpf_dissect_cb *)(skb->cb);
	struct vlan_hdr vlan;
	__be16 proto;

	/* Peek back to see if single or double-tagging */
	if (bpf_skb_load_bytes(skb, cb->nhoff - sizeof(proto), &proto,
			       sizeof(proto)))
		return BPF_DROP;

	/* Account for double-tagging */
	if (proto == bpf_htons(ETH_P_8021AD)) {
		if (bpf_skb_load_bytes(skb, cb->nhoff, &vlan, sizeof(vlan)))
			return BPF_DROP;

		if (vlan.h_vlan_encapsulated_proto != bpf_htons(ETH_P_8021Q))
			return BPF_DROP;

		cb->nhoff += sizeof(vlan);
	}

	if (bpf_skb_load_bytes(skb, cb->nhoff, &vlan, sizeof(vlan)))
		return BPF_DROP;

	cb->nhoff += sizeof(vlan);
	/* Only allow 8021AD + 8021Q double tagging and no triple tagging.*/
	if (vlan.h_vlan_encapsulated_proto == bpf_htons(ETH_P_8021AD) ||
	    vlan.h_vlan_encapsulated_proto == bpf_htons(ETH_P_8021Q))
		return BPF_DROP;

	return parse_eth_proto(skb, vlan.h_vlan_encapsulated_proto);
}

PROG(GUE)(struct __sk_buff *skb)
{
	struct bpf_dissect_cb *cb = (struct bpf_dissect_cb *)(skb->cb);
	struct guehdr gue;

	if (bpf_skb_load_bytes(skb, cb->nhoff, &gue, sizeof(gue)))
		return BPF_DROP;

	cb->nhoff += sizeof(gue);
	cb->nhoff += gue.hlen << 2;

	cb->flags |= FLOW_DIS_ENCAPSULATION;
	return parse_ip_proto(skb, gue.proto_ctype);
}

char __license[] SEC("license") = "GPL";
