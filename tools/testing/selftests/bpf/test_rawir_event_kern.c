// SPDX-License-Identifier: GPL-2.0
// test ir decoder
//
// Copyright (C) 2018 Sean Young <sean@mess.org>

#include <linux/bpf.h>
#include "bpf_helpers.h"

SEC("rawir_event")
int bpf_decoder(struct bpf_rawir_event *e)
{
	if (e->type == BPF_RAWIR_EVENT_PULSE) {
		/*
		 * The lirc interface is microseconds, but here we receive
		 * nanoseconds.
		 */
		int microseconds = e->duration / 1000;

		if (microseconds & 0x10000)
			bpf_rc_keydown(e, 0x40, microseconds & 0xffff, 0);
	}

	return 0;
}

char _license[] SEC("license") = "GPL";
