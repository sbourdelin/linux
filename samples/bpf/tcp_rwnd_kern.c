/*
 * BPF program to set initial receive window to 40 packets when using IPv6
 * and the first 5.5 bytes of the IPv6 addresses are not the same (in this
 * example that means both hosts are not the same datacenter.
 */

#include <uapi/linux/bpf.h>
#include <uapi/linux/if_ether.h>
#include <uapi/linux/if_packet.h>
#include <uapi/linux/ip.h>
#include <linux/socket.h>
#include "bpf_helpers.h"

#define DEBUG 1

SEC("sockops")
int bpf_rwnd(struct bpf_socket_ops *skops)
{
	char fmt1[] = "BPF command: %d\n";
	char fmt2[] = "  Returning %d\n";
	int rv = -1;
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

	/* Check for RWND_INIT operation and IPv6 addresses */
	if (op == BPF_SOCKET_OPS_RWND_INIT &&
		skops->family == AF_INET6) {

		/* If the first 5.5 bytes of the IPv6 address are not the same
		 * then both hosts are not in the same datacenter
		 * so use a larger initial advertized window (40 packets)
		 */
		if (skops->local_ip6[0] != skops->remote_ip6[0] ||
		    (skops->local_ip6[1] & 0xfffff000) !=
		    (skops->remote_ip6[1] & 0xfffff000))
			bpf_trace_printk(fmt2, sizeof(fmt2), -1);
			rv = 40;
	}
#ifdef DEBUG
	bpf_trace_printk(fmt2, sizeof(fmt2), rv);
#endif
	return rv;
}
char _license[] SEC("license") = "GPL";
