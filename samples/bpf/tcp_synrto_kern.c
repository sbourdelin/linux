/*
 * BPF program to set SYN and SYN-ACK RTOs to 10ms when using IPv6 addresses
 * and the first 5.5 bytes of the IPv6 addresses are the same (in this example
 * that means both hosts are in the same datacenter.
 */

#include <uapi/linux/bpf.h>
#include <uapi/linux/if_ether.h>
#include <uapi/linux/if_packet.h>
#include <uapi/linux/ip.h>
#include <linux/socket.h>
#include "bpf_helpers.h"

#define DEBUG 1

SEC("sockops")
int bpf_synrto(struct bpf_socket_ops *skops)
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

	/* Check for TIMEOUT_INIT operation and IPv6 addresses */
	if (op == BPF_SOCKET_OPS_TIMEOUT_INIT &&
		skops->family == AF_INET6) {

		/* If the first 5.5 bytes of the IPv6 address are the same
		 * then both hosts are in the same datacenter
		 * so use an RTO of 10ms
		 */
		if (skops->local_ip6[0] == skops->remote_ip6[0] &&
		    (skops->local_ip6[1] & 0xfff00000) ==
		    (skops->remote_ip6[1] & 0xfff00000))
			rv = 10;
	}
#ifdef DEBUG
	bpf_trace_printk(fmt2, sizeof(fmt2), rv);
#endif
	return rv;
}
char _license[] SEC("license") = "GPL";
