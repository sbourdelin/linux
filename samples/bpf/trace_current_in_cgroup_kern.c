/* Copyright (c) 2016 Sargun Dhillon <sargun@sargun.me>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 */

#include <linux/ptrace.h>
#include <uapi/linux/bpf.h>
#include <linux/version.h>
#include "bpf_helpers.h"
#include <uapi/linux/in.h>

struct bpf_map_def SEC("maps") test_current_in_cgroup_map = {
	.type = BPF_MAP_TYPE_CGROUP_ARRAY,
	.key_size = sizeof(u32),
	.value_size = sizeof(u32),
	.max_entries = 1,
};

SEC("kprobe/sys_connect")
int bpf_prog1(struct pt_regs *ctx)
{
	struct sockaddr_in addr = {};
	void *sockaddr_arg = (void *)PT_REGS_PARM2(ctx);
	int sockaddr_len = (int)PT_REGS_PARM3(ctx);
	char fmt[] = "Connection on port %d\n";

	if (!bpf_current_in_cgroup(&test_current_in_cgroup_map, 0))
		return 0;
	if (sockaddr_len > sizeof(addr))
		return 0;
	if (bpf_probe_read(&addr, sizeof(addr), sockaddr_arg) != 0)
		return 0;
	if (addr.sin_family != AF_INET)
		return 0;

	bpf_trace_printk(fmt, sizeof(fmt), be16_to_cpu(addr.sin_port));

	return 1;
}

char _license[] SEC("license") = "GPL";
u32 _version SEC("version") = LINUX_VERSION_CODE;
