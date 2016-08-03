/* Copyright 2016 Netflix, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 */
#include <linux/version.h>
#include <uapi/linux/bpf.h>
#include <linux/ptrace.h>
#include "bpf_helpers.h"

#define MAX_IPS		8192

#define _(P) ({typeof(P) val; bpf_probe_read(&val, sizeof(val), &P); val;})

struct bpf_map_def SEC("maps") ip_map = {
	.type = BPF_MAP_TYPE_HASH,
	.key_size = sizeof(u64),
	.value_size = sizeof(u32),
	.max_entries = MAX_IPS,
};

/* from /sys/kernel/debug/tracing/events/perf/perf_hrtimer/format */
struct perf_hrtimer_args {
	unsigned long long pad;
	struct pt_regs *regs;
	struct perf_event *event;
};
SEC("tracepoint/perf/perf_hrtimer")
int do_sample(struct perf_hrtimer_args *args)
{
	struct pt_regs *regs;
	u64 ip;
	u32 *value, init_val = 1;

	regs = _(args->regs);
	ip = _(regs->ip);
	value = bpf_map_lookup_elem(&ip_map, &ip);
	if (value)
		*value += 1;
	else
		/* E2BIG not tested for this example only */
		bpf_map_update_elem(&ip_map, &ip, &init_val, BPF_ANY);

	return 0;
}
char _license[] SEC("license") = "GPL";
u32 _version SEC("version") = LINUX_VERSION_CODE;
