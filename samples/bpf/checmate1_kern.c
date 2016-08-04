#include <uapi/linux/bpf.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/checmate.h>
#include "bpf_helpers.h"
#include <linux/version.h>

SEC("checmate")
int prog(struct checmate_ctx *ctx)
{
	struct sockaddr address;
	struct sockaddr_in *in_addr;
	char fmt[] = "Denying access on port 1\n";

	bpf_probe_read(&address, sizeof(struct sockaddr_in),
		       ctx->socket_connect_ctx.address);
	if (address.sa_family == AF_INET) {
		in_addr = (struct sockaddr_in *) &address;
		if (be16_to_cpu(in_addr->sin_port) == 1) {
			bpf_trace_printk(fmt, sizeof(fmt));
			return -EPERM;
		}
	}
	return 0;
}

char _license[] SEC("license") = "GPL";
u32 _version SEC("version") = LINUX_VERSION_CODE;
