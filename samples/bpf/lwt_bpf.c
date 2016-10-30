/* Copyright (c) 2016 Thomas Graf <tgraf@tgraf.ch>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 */

#include <stdint.h>
#include <stddef.h>
#include <linux/bpf.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/icmpv6.h>
#include <linux/if_ether.h>
#include "bpf_helpers.h"
#include <string.h>

# define printk(fmt, ...)						\
		({							\
			char ____fmt[] = fmt;				\
			bpf_trace_printk(____fmt, sizeof(____fmt),	\
				     ##__VA_ARGS__);			\
		})

#define CB_MAGIC 1234

/* Let all packets pass */
SEC("nop")
int do_nop(struct __sk_buff *skb)
{
	return BPF_OK;
}

/* Print some context information per packet to tracing buffer.
 */
SEC("ctx_test")
int do_ctx_test(struct __sk_buff *skb)
{
	skb->cb[0] = CB_MAGIC;
	printk("len %d hash %d protocol %d\n", skb->len, skb->hash,
	       skb->protocol);
	printk("cb %d ingress_ifindex %d ifindex %d\n", skb->cb[0],
	       skb->ingress_ifindex, skb->ifindex);

	return BPF_OK;
}

/* Print content of skb->cb[] to tracing buffer */
SEC("print_cb")
int do_print_cb(struct __sk_buff *skb)
{
	printk("cb0: %x cb1: %x cb2: %x\n", skb->cb[0], skb->cb[1],
	       skb->cb[2]);
	printk("cb3: %x cb4: %x\n", skb->cb[3], skb->cb[4]);

	return BPF_OK;
}

/* Print source and destination IPv4 address to tracing buffer */
SEC("data_test")
int do_data_test(struct __sk_buff *skb)
{
	void *data = (void *)(long)skb->data;
	void *data_end = (void *)(long)skb->data_end;
	struct iphdr *iph = data;

	if (data + sizeof(*iph) > data_end) {
		printk("packet truncated\n");
		return BPF_DROP;
	}

	printk("src: %x dst: %x\n", iph->saddr, iph->daddr);

	return BPF_OK;
}

#define IP_CSUM_OFF offsetof(struct iphdr, check)
#define IP_DST_OFF offsetof(struct iphdr, daddr)
#define IP_SRC_OFF offsetof(struct iphdr, saddr)
#define IP_PROTO_OFF offsetof(struct iphdr, protocol)
#define TCP_CSUM_OFF offsetof(struct tcphdr, check)
#define UDP_CSUM_OFF offsetof(struct udphdr, check)
#define IS_PSEUDO 0x10

static inline int rewrite(struct __sk_buff *skb, uint32_t old_ip,
			  uint32_t new_ip, int rw_daddr)
{
	int ret, off = 0, flags = IS_PSEUDO;
	uint8_t proto;

	ret = bpf_skb_load_bytes(skb, IP_PROTO_OFF, &proto, 1);
	if (ret < 0) {
		printk("bpf_l4_csum_replace failed: %d\n", ret);
		return BPF_DROP;
	}

	switch (proto) {
	case IPPROTO_TCP:
		off = TCP_CSUM_OFF;
		break;

	case IPPROTO_UDP:
		off = UDP_CSUM_OFF;
		flags |= BPF_F_MARK_MANGLED_0;
		break;

	case IPPROTO_ICMPV6:
		off = offsetof(struct icmp6hdr, icmp6_cksum);
		break;
	}

	if (off) {
		ret = bpf_l4_csum_replace(skb, off, old_ip, new_ip,
					  flags | sizeof(new_ip));
		if (ret < 0) {
			printk("bpf_l4_csum_replace failed: %d\n");
			return BPF_DROP;
		}
	}

	ret = bpf_l3_csum_replace(skb, IP_CSUM_OFF, old_ip, new_ip, sizeof(new_ip));
	if (ret < 0) {
		printk("bpf_l3_csum_replace failed: %d\n", ret);
		return BPF_DROP;
	}

	if (rw_daddr)
		ret = bpf_skb_store_bytes(skb, IP_DST_OFF, &new_ip, sizeof(new_ip), 0);
	else
		ret = bpf_skb_store_bytes(skb, IP_SRC_OFF, &new_ip, sizeof(new_ip), 0);

	if (ret < 0) {
		printk("bpf_skb_store_bytes() failed: %d\n", ret);
		return BPF_DROP;
	}

	return BPF_OK;
}

/* Rewrite IPv4 destination address from 192.168.254.2 to 192.168.254.3 */
SEC("rw_out")
int do_rw_out(struct __sk_buff *skb)
{
	uint32_t old_ip, new_ip = 0x3fea8c0;
	int ret;

	ret = bpf_skb_load_bytes(skb, IP_DST_OFF, &old_ip, 4);
	if (ret < 0) {
		printk("bpf_skb_load_bytes failed: %d\n", ret);
		return BPF_DROP;
	}

	if (old_ip == 0x2fea8c0) {
		printk("out: rewriting from %x to %x\n", old_ip, new_ip);
		return rewrite(skb, old_ip, new_ip, 1);
	}

	return BPF_OK;
}

SEC("redirect")
int do_redirect(struct __sk_buff *skb)
{
	uint64_t smac = SRC_MAC, dmac = DST_MAC;
	int ret, ifindex = DST_IFINDEX;
	struct ethhdr ehdr;

	ret = bpf_skb_push(skb, 14, 0);
	if (ret < 0) {
		printk("skb_push() failed: %d\n", ret);
	}

	ehdr.h_proto = __constant_htons(ETH_P_IP);
	memcpy(&ehdr.h_source, &smac, 6);
	memcpy(&ehdr.h_dest, &dmac, 6);

	ret = bpf_skb_store_bytes(skb, 0, &ehdr, sizeof(ehdr), 0);
	if (ret < 0) {
		printk("skb_store_bytes() failed: %d\n", ret);
		return BPF_DROP;
	}

	ret = bpf_redirect(ifindex, 0);
	if (ret < 0) {
		printk("bpf_redirect() failed: %d\n", ret);
		return BPF_DROP;
	}

	printk("redirected to %d\n", ifindex);

	return BPF_REDIRECT;
}

/* Drop all packets */
SEC("drop_all")
int do_drop_all(struct __sk_buff *skb)
{
	printk("dropping with: %d\n", BPF_DROP);
	return BPF_DROP;
}

char _license[] SEC("license") = "GPL";
