/*
 * BPF program to set congestion control to dctcp when both hosts are
 * in the same datacenter (as deteremined by IPv6 prefix).
 */

#include <uapi/linux/bpf.h>
#include <uapi/linux/tcp.h>
#include <uapi/linux/if_ether.h>
#include <uapi/linux/if_packet.h>
#include <uapi/linux/ip.h>
#include <linux/socket.h>
#include "bpf_helpers.h"

#define DEBUG 1

SEC("sockops")
int bpf_cong(struct bpf_socket_ops *skops)
{
	char fmt1[] = "BPF command: %d\n";
	char fmt2[] = "  Returning %d\n";
	char cong[] = "dctcp";
	int rv = 0;
	int op;

	/* For testing purposes, only execute rest of BPF program
	 * if neither port numberis 55601
	 */
	if (skops->remote_port != 55601 && skops->local_port != 55601)
		return -1;

	op = (int) skops->op;

#ifdef DEBUG
	bpf_trace_printk(fmt1, sizeof(fmt1), op);
#endif

	/* Check if both hosts are in the same datacenter. For this
	 * example they are if the 1st 5.5 bytes in the IPv6 address
	 * are the same.
	 */
	if (skops->family == AF_INET6 &&
	    skops->local_ip6[0] == skops->remote_ip6[0] &&
	    (skops->local_ip6[1] & 0xfff00000) ==
	    (skops->remote_ip6[1] & 0xfff00000)) {
		switch (op) {
		case BPF_SOCKET_OPS_NEEDS_ECN:
			rv = 1;
			break;
		case BPF_SOCKET_OPS_ACTIVE_ESTABLISHED_CB:
			rv = bpf_setsockopt(skops, SOL_TCP, TCP_CONGESTION,
					    cong, sizeof(cong));
			break;
		case BPF_SOCKET_OPS_PASSIVE_ESTABLISHED_CB:
			rv = bpf_setsockopt(skops, SOL_TCP, TCP_CONGESTION,
					    cong, sizeof(cong));
			break;
		default:
			rv = -1;
		}
	} else {
		rv = -1;
	}
#ifdef DEBUG
	bpf_trace_printk(fmt2, sizeof(fmt2), rv);
#endif
	return rv;
}
char _license[] SEC("license") = "GPL";
