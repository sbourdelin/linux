/* Copyright (c) 2016 Facebook
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 */

#define MAP_SIZE (1 << 20)

#define KBUILD_MODNAME "ilarouter"
#include <uapi/linux/if_ether.h>
#include <uapi/linux/ipv6.h>
#include <uapi/linux/bpf.h>
#include "bpf_helpers.h"

struct ila_addr {
	u64 addr_hi;
	u64 addr_lo;
} __packed;

struct ila_info {
	struct ila_addr addr;
	u16 mac[3];
} __packed;

char _license[] SEC("license") = "GPL";
unsigned int version SEC("version") = 1;

struct bpf_map_def SEC("map_ila_lookup_map") ila_lookup_map = {
	.type = BPF_MAP_TYPE_HASH,
	.key_size = sizeof(struct in6_addr),
	.value_size = sizeof(struct ila_info),
	.max_entries = MAP_SIZE,
};

SEC("xdp_ila_lookup")
int ila_lookup(struct xdp_md *ctx)
{
	unsigned long dataptr = (unsigned long)ctx->data;
	struct ethhdr *eth;
	struct ipv6hdr *sir;
	struct ila_addr *pkt_addr;
	struct ila_info *reply;
	u16 *dst_mac;

	/* Invalid packet: length too short
	 * compiler optimization/verifier bypass:
	 * this way it won't assume that we copied over a pkt_ptr,
	 * which has register range of 0 (from (r1 + 0))
	 */
	if (dataptr + sizeof(struct ethhdr) + sizeof(struct ipv6hdr) >
	    (unsigned long)ctx->data_end)
		return XDP_PASS;

	/* Ethernet header */
	eth = (struct ethhdr *)dataptr;

	/* Irrelevant packet: not IPv6 */
	if (eth->h_proto != htons(ETH_P_IPV6))
		return XDP_PASS;

	/* Sir Address header */
	sir = (struct ipv6hdr *)(dataptr + sizeof(struct ethhdr));

	/* We don't have to check for C bit or Type, since
	 * userspace mapping inserts guarantees that only valid values
	 * will be inserted into the map in network byte-order.
	 * Hence, a lookup fail implies either C bit/Type is invalid,
	 * or mapping does not exist, in both cases we pass the packet without
	 * modifications.
	 */
	pkt_addr = (struct ila_addr *)&(sir->daddr);
	reply = bpf_map_lookup_elem(&ila_lookup_map, pkt_addr);

	if (!reply)
		return XDP_PASS;

	pkt_addr->addr_hi = reply->addr.addr_hi;
	pkt_addr->addr_lo = reply->addr.addr_lo;

	dst_mac = (u16 *)eth;
	dst_mac[0] = reply->mac[0];
	dst_mac[1] = reply->mac[1];
	dst_mac[2] = reply->mac[2];

	return XDP_TX;
}

