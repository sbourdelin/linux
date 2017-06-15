/*
 * Sample BPF program to set send and receive buffers to 150KB, sndcwnd clamp
 * to 100 packets and SYN and SYN_ACK RTOs to 10ms when both hosts are within
 * the same datacenter. For his example, we assume they are within the same
 * datacenter when the first 5.5 bytes of their IPv6 addresses are the same.
 */

#include <uapi/linux/bpf.h>
#include <uapi/linux/if_ether.h>
#include <uapi/linux/if_packet.h>
#include <uapi/linux/ip.h>
#include <linux/socket.h>
#include "bpf_helpers.h"

#define DEBUG 1

SEC("sockops")
int bpf_clamp(struct bpf_socket_ops *skops)
{
	char fmt1[] = "BPF command: %d\n";
	char fmt2[] = "  Returning %d\n";
	int bufsize = 150000;
	int to_init = 10;
	int clamp = 100;
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

	/* Check that both hosts are within same datacenter. For this example
	 * it is the case when the first 5.5 bytes of their IPv6 addresses are
	 * the same.
	 */
	if (skops->family == AF_INET6 &&
	    skops->local_ip6[0] == skops->remote_ip6[0] &&
	    (skops->local_ip6[1] & 0xfff00000) ==
	    (skops->remote_ip6[1] & 0xfff00000)) {
		switch (op) {
		case BPF_SOCKET_OPS_TIMEOUT_INIT:
			rv = to_init;
			break;
		case BPF_SOCKET_OPS_TCP_CONNECT_CB:
			/* Set sndbuf and rcvbuf of active connections */
			rv = bpf_setsockopt(skops, SOL_SOCKET, SO_SNDBUF,
					    &bufsize, sizeof(bufsize));
			rv = -rv*100 + bpf_setsockopt(skops, SOL_SOCKET,
						      SO_RCVBUF, &bufsize,
						      sizeof(bufsize));
			break;
		case BPF_SOCKET_OPS_ACTIVE_ESTABLISHED_CB:
			rv = bpf_setsockopt(skops, SOL_TCP,
					    TCP_BPF_SNDCWND_CLAMP,
					    &clamp, sizeof(clamp));
			break;
		case BPF_SOCKET_OPS_PASSIVE_ESTABLISHED_CB:
			/* Set sndbuf and rcvbuf of passive connections */
			rv = bpf_setsockopt(skops, SOL_TCP,
					    TCP_BPF_SNDCWND_CLAMP,
					    &clamp, sizeof(clamp));
			rv = -rv*100 + bpf_setsockopt(skops, SOL_SOCKET,
						      SO_SNDBUF, &bufsize,
						      sizeof(bufsize));
			rv = -rv*200 + bpf_setsockopt(skops, SOL_SOCKET,
						      SO_RCVBUF, &bufsize,
						      sizeof(bufsize));
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
