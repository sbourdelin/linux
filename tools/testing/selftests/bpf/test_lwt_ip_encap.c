// SPDX-License-Identifier: GPL-2.0
#include <linux/bpf.h>
#include <string.h>
#include "bpf_helpers.h"
#include "bpf_endian.h"

#define BPF_LWT_ENCAP_IP 2

struct iphdr {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	__u8	ihl:4,
		version:4;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	__u8	version:4,
		ihl:4;
#else
#error "Fix your compiler's __BYTE_ORDER__?!"
#endif
	__u8	tos;
	__be16	tot_len;
	__be16	id;
	__be16	frag_off;
	__u8	ttl;
	__u8	protocol;
	__sum16	check;
	__be32	saddr;
	__be32	daddr;
};

struct grehdr {
	__be16 flags;
	__be16 protocol;
};

SEC("encap_gre")
int bpf_lwt_encap_gre(struct __sk_buff *skb)
{
	char encap_header[24];
	int err;
	struct iphdr *iphdr = (struct iphdr *)encap_header;
	struct grehdr *greh = (struct grehdr *)(encap_header + sizeof(struct iphdr));

	memset(encap_header, 0, sizeof(encap_header));

	iphdr->ihl = 5;
	iphdr->version = 4;
	iphdr->tos = 0;
	iphdr->ttl = 0x40;
	iphdr->protocol = 47;  /* IPPROTO_GRE */
	iphdr->saddr = 0x640110ac;  /* 172.16.1.100 */
	iphdr->daddr = 0x640310ac;  /* 172.16.5.100 */
	iphdr->check = 0;
	iphdr->tot_len = bpf_htons(skb->len + sizeof(encap_header));

	greh->protocol = bpf_htons(0x800);

	err = bpf_lwt_push_encap(skb, BPF_LWT_ENCAP_IP, (void *)encap_header,
				 sizeof(encap_header));
	if (err)
		return BPF_DROP;

	return BPF_OK;
}

char _license[] SEC("license") = "GPL";
