/*
 * amdtp-stream-trace.h - Linux tracing application for ALSA AMDTP engine
 *
 * Copyright (c) 2016 Takashi Sakamoto
 * Licensed under the terms of the GNU General Public License, version 2.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM		snd_firewire_lib

#if !defined(_AMDTP_STREAM_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _AMDTP_STREAM_TRACE_H

#include <linux/tracepoint.h>

TRACE_EVENT(in_packet,
	TP_PROTO(u32 cip_header0, u32 cip_header1, unsigned int payload_quadlets, unsigned int index),
	TP_ARGS(cip_header0, cip_header1, payload_quadlets, index),
	TP_STRUCT__entry(
		__field(u32, cip_header0)
		__field(u32, cip_header1)
		__field(unsigned int, payload_quadlets)
		__field(unsigned int, index)
	),
	TP_fast_assign(
		__entry->cip_header0 = cip_header0;
		__entry->cip_header1 = cip_header1;
		__entry->payload_quadlets = payload_quadlets;
		__entry->index = index;
	),
	TP_printk(
		"%08x %08x: %03u: %02u",
		__entry->cip_header0,
		__entry->cip_header1,
		__entry->payload_quadlets,
		__entry->index)
);

TRACE_EVENT(out_packet,
	TP_PROTO(u32 cip_header0, u32 cip_header1, unsigned int payload_quadlets, unsigned int index),
	TP_ARGS(cip_header0, cip_header1, payload_quadlets, index),
	TP_STRUCT__entry(
		__field(u32, cip_header0)
		__field(u32, cip_header1)
		__field(unsigned int, payload_quadlets)
		__field(unsigned int, index)
	),
	TP_fast_assign(
		__entry->cip_header0 = cip_header0;
		__entry->cip_header1 = cip_header1;
		__entry->payload_quadlets = payload_quadlets;
		__entry->index = index;
	),
	TP_printk(
		"%08x %08x: %03u: %02u",
		__entry->cip_header0,
		__entry->cip_header1,
		__entry->payload_quadlets,
		__entry->index)
);

#endif

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH	.
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE	amdtp-stream-trace
#include <trace/define_trace.h>
