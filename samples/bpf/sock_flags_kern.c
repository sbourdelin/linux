#include <uapi/linux/bpf.h>
#include <linux/socket.h>
#include "bpf_helpers.h"

SEC("cgroup/sock1")
int bpf_prog1(struct bpf_sock *sk)
{
	char fmt[] = "socket: family %d type %d protocol %d\n";

	bpf_trace_printk(fmt, sizeof(fmt), sk->family, sk->type, sk->protocol);

	/* block PF_INET6, SOCK_RAW, IPPROTO_ICMPV6 sockets
	 * ie., make ping6 fail
	 */
	if (sk->family == PF_INET6 && sk->type == 3 && sk->protocol == 58)
		return 0;

	return 1;
}

SEC("cgroup/sock2")
int bpf_prog2(struct bpf_sock *sk)
{
	char fmt[] = "socket: family %d type %d protocol %d\n";

	bpf_trace_printk(fmt, sizeof(fmt), sk->family, sk->type, sk->protocol);

	/* block PF_INET, SOCK_RAW, IPPROTO_ICMP sockets
	 * ie., make ping fail
	 */
	if (sk->family == PF_INET && sk->type == 3 && sk->protocol == 1)
		return 0;

	return 1;
}

char _license[] SEC("license") = "GPL";
