#include <linux/bpf.h>
#include "bpf_helpers.h"
#include "bpf_util.h"
#include "bpf_endian.h"

int _version SEC("version") = 1;

SEC("socket_kcm")
int bpf_prog1(struct __sk_buff *skb)
{
	return skb->len;
}

char _license[] SEC("license") = "GPL";
