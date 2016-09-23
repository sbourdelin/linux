/* Copyright (c) 2016 Facebook
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 */

#define MAP_SIZE (1 << 20)

#define KBUILD_MODNAME "ilarouter"
#include <linux/if_ether.h>
#include <linux/ipv6.h>
#include <uapi/linux/bpf.h>
#include "ila.h"
#include "inet_helper.h"
#include "bpf_helpers.h"

char _license[] SEC("license") = "GPL";
unsigned int version SEC("version") = 1;

struct bpf_map_def SEC("maps") ila_lookup_map = {
	.type = BPF_MAP_TYPE_HASH,
	.key_size = sizeof(struct in6_addr),
	.value_size = sizeof(struct in6_addr),
	.max_entries = MAP_SIZE,
};

#define IPV6_DEST_OFF (ETH_HLEN + offsetof(struct ipv6hdr, daddr))

struct addr {
	__u64 addr_hi;
	__u64 addr_lo;
} __packed;

SEC("classifier")
int ila_lookup(struct __sk_buff *skb)
{
	unsigned long dataptr = (unsigned long)skb->data;
	struct ethhdr *eth;
	struct ipv6hdr *sir;
	struct addr *pkt_addr;
	struct addr stack_addr;
	struct addr *reply;
#ifdef DEBUG
	char lookup_request[] = "Lookup request for sir: %llx, iden: %llx\n";
	char lookup_fail[] = "Lookup failed\n";
	char lookup_success[] = "Lookup success. hi: %llx, lo: %llx\n";
#endif

	/* Invalid packet: length too short
	 * compiler optimization/verifier bypass: this way it won't assume
	 * that we copied over a pkt_ptr,
	 * which has register range of 0 (from (r1 + 0))
	 */
	if (dataptr + sizeof(struct ethhdr) +
	    sizeof(struct ipv6hdr) > skb->data_end)
		goto redirect;

	/* Ethernet header */
	eth = (struct ethhdr *)dataptr;

	/* Irrelevant packet: not IPv6 */
	if (eth->h_proto != htons(ETH_P_IPV6))
		goto redirect;

	/* SIR Address header */
	sir = (struct ipv6hdr *)(dataptr + sizeof(struct ethhdr));
#ifdef DEBUG
	{
		/* ILA Address header */
		struct ilahdr *ila = (struct ilahdr *)sir;

		/* For debugging purposes,
		 * we don't care about non-SIR/ILA addresses
		 */
		if (ila->destination_address.c)
			goto redirect;

		switch (ila->destination_address.type) {
		case SIR_T_LOCAL:
		case SIR_T_VIRTUAL:
			break;
		default:
			goto redirect;
		}
	}
#endif

	pkt_addr = (struct addr *)&(sir->daddr);

	stack_addr.addr_hi = pkt_addr->addr_hi;
	stack_addr.addr_lo = pkt_addr->addr_lo;

	reply = bpf_map_lookup_elem(&ila_lookup_map, &stack_addr);
	if (!reply) {
#ifdef DEBUG
		/* Comment out if too noisy */
		bpf_trace_printk(lookup_request, sizeof(lookup_request),
				 _ntohll(pkt_addr->addr_hi),
				 _ntohll(pkt_addr->addr_lo));

		bpf_trace_printk(lookup_fail, sizeof(lookup_fail));
#endif
		goto redirect;
	}

#ifdef DEBUG
	bpf_trace_printk(lookup_request, sizeof(lookup_request),
			 _ntohll(pkt_addr->addr_hi),
			 _ntohll(pkt_addr->addr_lo));

	bpf_trace_printk(lookup_success, sizeof(lookup_success),
			 _ntohll(reply->addr_hi), _ntohll(reply->addr_lo));
#endif

	stack_addr.addr_hi = reply->addr_hi;
	stack_addr.addr_lo = reply->addr_lo;

	bpf_skb_store_bytes(skb, IPV6_DEST_OFF, &stack_addr,
			    sizeof(struct in6_addr), 0);

redirect:
	return bpf_redirect(skb->ifindex, 1);
}
