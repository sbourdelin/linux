#define KBUILD_MODNAME "foo"
#include <uapi/linux/bpf.h>
#include "bpf_helpers.h"

SEC("xdp_sock")
int xdp_sock_prog(struct xdp_md *ctx)
{
	return bpf_xdpsk_redirect();
}

char _license[] SEC("license") = "GPL";
