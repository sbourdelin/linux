/* Copyright (c) 2013-2015 PLUMgrid, http://plumgrid.com
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 */
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <uapi/linux/bpf.h>
#include <linux/version.h>
#include <linux/ftrace.h>
#include "bpf_helpers.h"

#define _(P) ({typeof(P) val = 0; bpf_probe_read(&val, sizeof(val), &P); val; })

/* kprobe is NOT a stable ABI
 * kernel functions can be removed, renamed or completely change semantics.
 * Number of arguments and their positions can change, etc.
 * In such case this bpf+kprobe example will no longer be meaningful
 */
SEC("ftrace")
int bpf_prog1(struct ftrace_regs *ctx)
{
	char devname[IFNAMSIZ];
	struct net_device *dev;
	struct sk_buff *skb;

	skb = (struct sk_buff *) FTRACE_REGS_PARAM1(ctx);
	dev = _(skb->dev);

	bpf_probe_read(devname, sizeof(devname), dev->name);
	if (devname[0] == 'l' && devname[1] == 'o') {
		char fmt[] = "track dev: %s";

		bpf_trace_printk(fmt, sizeof(fmt), devname);
		return 1;
	} else {
		return 0;
	}
}

char _license[] SEC("license") = "GPL";
u32 _version SEC("version") = LINUX_VERSION_CODE;
