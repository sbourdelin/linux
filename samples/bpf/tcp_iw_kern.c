/*
 * BPF program to set initial congestion window and initial receive
 * window to 40 packets and send and receive buffers to 1.5MB. This
 * would usually be done after doing appropriate checks that indicate
 * the hosts are far enough away (i.e. large RTT).
 */

#include <uapi/linux/bpf.h>
#include <uapi/linux/if_ether.h>
#include <uapi/linux/if_packet.h>
#include <uapi/linux/ip.h>
#include <linux/socket.h>
#include "bpf_helpers.h"

#define DEBUG 1

SEC("sockops")
int bpf_iw(struct bpf_socket_ops *skops)
{
	char fmt1[] = "BPF command: %d\n";
	char fmt2[] = "  Returning %d\n";
	int bufsize = 1500000;
	int rwnd_init = 40;
	int iw = 40;
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

	/* Usually there would be a check to insure the hosts are far
	 * from each other so it makes sense to increase buffer sizes
	 */
	switch (op) {
	case BPF_SOCKET_OPS_RWND_INIT:
		rv = rwnd_init;
		break;
	case BPF_SOCKET_OPS_TCP_CONNECT_CB:
		/* Set sndbuf and rcvbuf of active connections */
		rv = bpf_setsockopt(skops, SOL_SOCKET, SO_SNDBUF, &bufsize,
				    sizeof(bufsize));
		rv = -rv*100 + bpf_setsockopt(skops, SOL_SOCKET, SO_RCVBUF,
					      &bufsize, sizeof(bufsize));
		break;
	case BPF_SOCKET_OPS_ACTIVE_ESTABLISHED_CB:
		rv = bpf_setsockopt(skops, SOL_TCP, TCP_BPF_IW, &iw,
				    sizeof(iw));
		break;
	case BPF_SOCKET_OPS_PASSIVE_ESTABLISHED_CB:
		/* Set sndbuf and rcvbuf of passive connections */
		rv = bpf_setsockopt(skops, SOL_SOCKET, SO_SNDBUF, &bufsize,
				    sizeof(bufsize));
		rv = -rv*100 + bpf_setsockopt(skops, SOL_SOCKET, SO_RCVBUF,
					      &bufsize, sizeof(bufsize));
		break;
	default:
		rv = -1;
	}
#ifdef DEBUG
	bpf_trace_printk(fmt2, sizeof(fmt2), rv);
#endif
	return rv;
}
char _license[] SEC("license") = "GPL";
