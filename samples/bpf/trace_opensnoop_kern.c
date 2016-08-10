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

struct bpf_map_def SEC("maps") cgroup_map = {
	.type = BPF_MAP_TYPE_CGROUP_ARRAY,
	.key_size = sizeof(u32),
	.value_size = sizeof(u32),
	.max_entries = 1,
};

SEC("kprobe/sys_open")
int bpf_prog1(struct pt_regs *ctx)
{
	const char *filename = (char *)PT_REGS_PARM1(ctx);
	char fmt[] = "Opening file: %s\n";

	if (!bpf_current_task_in_cgroup(&cgroup_map, 0))
		return 0;

	bpf_trace_printk(fmt, sizeof(fmt), filename);

	return 1;
}

char _license[] SEC("license") = "GPL";
u32 _version SEC("version") = LINUX_VERSION_CODE;
