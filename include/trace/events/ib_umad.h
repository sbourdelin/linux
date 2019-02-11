// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB

/*
 * Copyright (c) 2018 Intel Corporation.  All rights reserved.
 *
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM ib_umad

#if !defined(_TRACE_IB_UMAD_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_IB_UMAD_H

#include <linux/tracepoint.h>

DECLARE_EVENT_CLASS(ib_umad_template,
	TP_PROTO(struct ib_umad_file *file, struct ib_user_mad *mad),
	TP_ARGS(file, mad),

	TP_STRUCT__entry(
		__field(u8,             port_num)
		__field(u8,             sl)
		__field(u8,             path_bits)
		__field(u8,             grh_present)
		__field(u32,            id)
		__field(u32,            status)
		__field(u32,            timeout_ms)
		__field(u32,            retires)
		__field(u32,            length)
		__field(u32,            qpn)
		__field(u32,            qkey)
		__field(u8,             gid_index)
		__field(u8,             hop_limit)
		__field(u16,            lid)
		__field(u16,            attr_id)
		__field(u16,            pkey_index)
		__field(u8,             base_version)
		__field(u8,             mgmt_class)
		__field(u8,             class_version)
		__field(u8,             method)
		__field(u32,            flow_label)
		__field(u16,            mad_status)
		__field(u16,            class_specific)
		__field(u32,            attr_mod)
		__field(u64,            tid)
		__array(u8,             gid, 16)
		__field(u32,            dev_index)
		__field(u8,             traffic_class)
	),

	TP_fast_assign(
		__entry->dev_index = file->port->ib_dev->index;
		__entry->port_num = file->port->port_num;

		__entry->id = mad->hdr.id;
		__entry->status = mad->hdr.status;
		__entry->timeout_ms = mad->hdr.timeout_ms;
		__entry->retires = mad->hdr.retries;
		__entry->length = mad->hdr.length;
		__entry->qpn = mad->hdr.qpn;
		__entry->qkey = mad->hdr.qkey;
		__entry->lid = mad->hdr.lid;
		__entry->sl = mad->hdr.sl;
		__entry->path_bits = mad->hdr.path_bits;
		__entry->grh_present = mad->hdr.grh_present;
		__entry->gid_index = mad->hdr.gid_index;
		__entry->hop_limit = mad->hdr.hop_limit;
		__entry->traffic_class = mad->hdr.traffic_class;
		memcpy(__entry->gid, mad->hdr.gid, sizeof(mad->hdr.gid));
		__entry->flow_label = mad->hdr.flow_label;
		__entry->pkey_index = mad->hdr.pkey_index;

		__entry->base_version = ((struct ib_mad_hdr *)mad->data)->base_version;
		__entry->mgmt_class = ((struct ib_mad_hdr *)mad->data)->mgmt_class;
		__entry->class_version = ((struct ib_mad_hdr *)mad->data)->class_version;
		__entry->method = ((struct ib_mad_hdr *)mad->data)->method;
		__entry->mad_status = ((struct ib_mad_hdr *)mad->data)->status;
		__entry->class_specific = ((struct ib_mad_hdr *)mad->data)->class_specific;
		__entry->tid = ((struct ib_mad_hdr *)mad->data)->tid;
		__entry->attr_id = ((struct ib_mad_hdr *)mad->data)->attr_id;
		__entry->attr_mod = ((struct ib_mad_hdr *)mad->data)->attr_mod;
	),

	TP_printk("%d:%d umad_hdr: id 0x%08x status 0x%08x ms %u ret %u len %u QP%u " \
		  "qkey 0x%08x lid 0x%04x sl %u path_bits 0x%x grh 0x%x gidi %u " \
		  "hop_lim %u traf_cl %u gid %pI6c flow 0x%08x pkeyi %u  " \
		  "MAD: base_ver 0x%x class 0x%x class_ver 0x%x method 0x%x " \
		  "status 0x%04x class_specific 0x%04x tid 0x%016llx attr_id 0x%04x attr_mod 0x%08x ",
		__entry->dev_index, __entry->port_num,
		__entry->id, __entry->status, __entry->timeout_ms,
		__entry->retires, __entry->length, be32_to_cpu(__entry->qpn),
		be32_to_cpu(__entry->qkey), be16_to_cpu(__entry->lid), __entry->sl,
		__entry->path_bits, __entry->grh_present, __entry->gid_index,
		__entry->hop_limit, __entry->traffic_class, &__entry->gid,
		be32_to_cpu(__entry->flow_label), __entry->pkey_index,
		__entry->base_version, __entry->mgmt_class, __entry->class_version,
		__entry->method, be16_to_cpu(__entry->mad_status), be16_to_cpu(__entry->class_specific),
		be64_to_cpu(__entry->tid), be16_to_cpu(__entry->attr_id), be32_to_cpu(__entry->attr_mod)
	)
);

DEFINE_EVENT(ib_umad_template, ib_umad_write,
	TP_PROTO(struct ib_umad_file *file, struct ib_user_mad *mad),
	TP_ARGS(file, mad));

DEFINE_EVENT(ib_umad_template, ib_umad_read,
	TP_PROTO(struct ib_umad_file *file, struct ib_user_mad *mad),
	TP_ARGS(file, mad));

#endif /* _TRACE_IB_UMAD_H */

#include <trace/define_trace.h>
