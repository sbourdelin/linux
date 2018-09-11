// SPDX-License-Identifier: GPL-2.0
#include <linux/filter.h>
#include <linux/ptrace.h>
#include <linux/version.h>
#include <uapi/linux/bpf.h>
#include <linux/rds.h>
#include "bpf_helpers.h"

#define bpf_printk(fmt, ...)				\
({							\
	char ____fmt[] = fmt;				\
	bpf_trace_printk(____fmt, sizeof(____fmt),	\
			##__VA_ARGS__);			\
})

SEC("socksg")
int main_prog(struct sk_msg_md *msg)
{
	int start, end, err;
	unsigned char *d;

	start = 0;
	end = 6;

	err = bpf_msg_pull_data(msg, start, end, 0);
	if (err) {
		bpf_printk("socksg: pull_data err %i\n", err);
		return SOCKSG_PASS;
	}

	if (msg->data + 6 > msg->data_end)
		return SOCKSG_PASS;

	d = (unsigned char *)msg->data;
	bpf_printk("%x %x %x\n", d[0], d[1], d[2]);
	bpf_printk("%x %x %x\n", d[3], d[4], d[5]);

	return SOCKSG_PASS;
}

char _license[] SEC("license") = "GPL";
u32 _version SEC("version") = LINUX_VERSION_CODE;
