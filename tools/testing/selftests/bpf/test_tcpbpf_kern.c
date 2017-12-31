// SPDX-License-Identifier: GPL-2.0
#include <stddef.h>
#include <string.h>
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/ip.h>
#include <linux/in6.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <netinet/in.h>
#include "bpf_helpers.h"
#include "bpf_endian.h"

struct globals {
	__u32 event_map;
	__u32 total_retrans;
	__u32 data_segs_in;
	__u32 data_segs_out;
	__u64 bytes_received;
	__u64 bytes_acked;
};

struct bpf_map_def SEC("maps") global_map = {
	.type = BPF_MAP_TYPE_ARRAY,
	.key_size = sizeof(__u32),
	.value_size = sizeof(struct globals),
	.max_entries = 2,
};

static inline void update_event_map(int event)
{
	__u32 key = 0;
	struct globals g, *gp;

	gp = bpf_map_lookup_elem(&global_map, &key);
	if (gp == NULL) {
		struct globals g = {0, 0, 0, 0, 0, 0};

		g.event_map |= (1 << event);
		bpf_map_update_elem(&global_map, &key, &g,
			    BPF_ANY);
	} else {
		g = *gp;
		g.event_map |= (1 << event);
		bpf_map_update_elem(&global_map, &key, &g,
			    BPF_ANY);
	}
}

int _version SEC("version") = 1;

SEC("sockops")
int bpf_testcb(struct bpf_sock_ops *skops)
{
	int rv = -1;
	int op;
	int init_seq = 0;
	int ret = 0;
	int v = 0;

	/* For testing purposes, only execute rest of BPF program
	 * if remote port number is in the range 12877..12887
	 * I.e. the active side of the connection
	 */
	if ((bpf_ntohl(skops->remote_port) < 12877 ||
	     bpf_ntohl(skops->remote_port) >= 12887)) {
		skops->reply = -1;
		return 1;
	}

	op = (int) skops->op;

	/* Check that both hosts are within same datacenter. For this example
	 * it is the case when the first 5.5 bytes of their IPv6 addresses are
	 * the same.
	 */
	if (1) {
		update_event_map(op);

		switch (op) {
		case BPF_SOCK_OPS_ACTIVE_ESTABLISHED_CB:
			skops->bpf_sock_ops_flags = 0xfff;
			init_seq = skops->snd_nxt;
			break;
		case BPF_SOCK_OPS_PASSIVE_ESTABLISHED_CB:
			init_seq = skops->snd_nxt;
			skops->bpf_sock_ops_flags = 0xfff;
			skops->sk_txhash = 0x12345f;
			v = 0xff;
			ret = bpf_setsockopt(skops, SOL_IPV6, IPV6_TCLASS, &v,
					     sizeof(v));
			break;
		case BPF_SOCK_OPS_RTO_CB:
			break;
		case BPF_SOCK_OPS_RETRANS_CB:
			break;
		case BPF_SOCK_OPS_STATE_CB:
			if (skops->args[1] == BPF_TCP_CLOSE) {
				__u32 key = 0;
				struct globals g, *gp;

				gp = bpf_map_lookup_elem(&global_map, &key);
				if (!gp)
					break;
				g = *gp;
				g.total_retrans = skops->total_retrans;
				g.data_segs_in = skops->data_segs_in;
				g.data_segs_out = skops->data_segs_out;
				g.bytes_received = skops->bytes_received;
				g.bytes_acked = skops->bytes_acked;
				bpf_map_update_elem(&global_map, &key, &g,
						    BPF_ANY);
			}
			break;
		default:
			rv = -1;
		}
	} else {
		rv = -1;
	}
	skops->reply = rv;
	return 1;
}
char _license[] SEC("license") = "GPL";
