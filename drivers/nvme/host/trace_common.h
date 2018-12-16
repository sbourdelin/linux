/*
 * NVM Express device driver tracepoints
 * Copyright (c) 2018 Johannes Thumshirn, SUSE Linux GmbH
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifndef TRACE_HEADER_MULTI_READ

#include <linux/nvme.h>
#include <linux/tracepoint.h>
#include <linux/trace_seq.h>

#include "nvme.h"

#ifdef CONFIG_TRACING
#include <asm/unaligned.h>
static const char *nvme_trace_create_sq(struct trace_seq *p, u8 *cdw10)
{
	const char *ret = trace_seq_buffer_ptr(p);
	u16 sqid = get_unaligned_le16(cdw10);
	u16 qsize = get_unaligned_le16(cdw10 + 2);
	u16 sq_flags = get_unaligned_le16(cdw10 + 4);
	u16 cqid = get_unaligned_le16(cdw10 + 6);


	trace_seq_printf(p, "sqid=%u, qsize=%u, sq_flags=0x%x, cqid=%u",
			 sqid, qsize, sq_flags, cqid);
	trace_seq_putc(p, 0);

	return ret;
}

static const char *nvme_trace_create_cq(struct trace_seq *p, u8 *cdw10)
{
	const char *ret = trace_seq_buffer_ptr(p);
	u16 cqid = get_unaligned_le16(cdw10);
	u16 qsize = get_unaligned_le16(cdw10 + 2);
	u16 cq_flags = get_unaligned_le16(cdw10 + 4);
	u16 irq_vector = get_unaligned_le16(cdw10 + 6);

	trace_seq_printf(p, "cqid=%u, qsize=%u, cq_flags=0x%x, irq_vector=%u",
			 cqid, qsize, cq_flags, irq_vector);
	trace_seq_putc(p, 0);

	return ret;
}

static const char *nvme_trace_admin_identify(struct trace_seq *p, u8 *cdw10)
{
	const char *ret = trace_seq_buffer_ptr(p);
	u8 cns = cdw10[0];
	u16 ctrlid = get_unaligned_le16(cdw10 + 2);

	trace_seq_printf(p, "cns=%u, ctrlid=%u", cns, ctrlid);
	trace_seq_putc(p, 0);

	return ret;
}



static const char *nvme_trace_read_write(struct trace_seq *p, u8 *cdw10)
{
	const char *ret = trace_seq_buffer_ptr(p);
	u64 slba = get_unaligned_le64(cdw10);
	u16 length = get_unaligned_le16(cdw10 + 8);
	u16 control = get_unaligned_le16(cdw10 + 10);
	u32 dsmgmt = get_unaligned_le32(cdw10 + 12);
	u32 reftag = get_unaligned_le32(cdw10 +  16);

	trace_seq_printf(p,
			 "slba=%llu, len=%u, ctrl=0x%x, dsmgmt=%u, reftag=%u",
			 slba, length, control, dsmgmt, reftag);
	trace_seq_putc(p, 0);

	return ret;
}

static const char *nvme_trace_dsm(struct trace_seq *p, u8 *cdw10)
{
	const char *ret = trace_seq_buffer_ptr(p);

	trace_seq_printf(p, "nr=%u, attributes=%u",
			 get_unaligned_le32(cdw10),
			 get_unaligned_le32(cdw10 + 4));
	trace_seq_putc(p, 0);

	return ret;
}

static const char *nvme_trace_common(struct trace_seq *p, u8 *cdw10)
{
	const char *ret = trace_seq_buffer_ptr(p);

	trace_seq_printf(p, "cdw10=%*ph", 24, cdw10);
	trace_seq_putc(p, 0);

	return ret;
}

static const char *nvme_trace_parse_admin_cmd(struct trace_seq *p,
				       u8 opcode, u8 *cdw10)
{
	switch (opcode) {
	case nvme_admin_create_sq:
		return nvme_trace_create_sq(p, cdw10);
	case nvme_admin_create_cq:
		return nvme_trace_create_cq(p, cdw10);
	case nvme_admin_identify:
		return nvme_trace_admin_identify(p, cdw10);
	default:
		return nvme_trace_common(p, cdw10);
	}
}

static const char *nvme_trace_parse_nvm_cmd(struct trace_seq *p,
				     u8 opcode, u8 *cdw10)
{
	switch (opcode) {
	case nvme_cmd_read:
	case nvme_cmd_write:
	case nvme_cmd_write_zeroes:
		return nvme_trace_read_write(p, cdw10);
	case nvme_cmd_dsm:
		return nvme_trace_dsm(p, cdw10);
	default:
		return nvme_trace_common(p, cdw10);
	}
}

static const char *nvme_trace_disk_name(struct trace_seq *p, char *name)
{
	const char *ret = trace_seq_buffer_ptr(p);

	if (*name)
		trace_seq_printf(p, "disk=%s, ", name);
	trace_seq_putc(p, 0);

	return ret;
}

#endif	/* CONFIG_TRACING */

static inline void __assign_disk_name(char *name, struct gendisk *disk)
{
	if (disk)
		memcpy(name, disk->disk_name, DISK_NAME_LEN);
	else
		memset(name, 0, DISK_NAME_LEN);
}

#define nvme_admin_opcode_name(opcode)	{ opcode, #opcode }
#define show_admin_opcode_name(val)					\
	__print_symbolic(val,						\
		nvme_admin_opcode_name(nvme_admin_delete_sq),		\
		nvme_admin_opcode_name(nvme_admin_create_sq),		\
		nvme_admin_opcode_name(nvme_admin_get_log_page),	\
		nvme_admin_opcode_name(nvme_admin_delete_cq),		\
		nvme_admin_opcode_name(nvme_admin_create_cq),		\
		nvme_admin_opcode_name(nvme_admin_identify),		\
		nvme_admin_opcode_name(nvme_admin_abort_cmd),		\
		nvme_admin_opcode_name(nvme_admin_set_features),	\
		nvme_admin_opcode_name(nvme_admin_get_features),	\
		nvme_admin_opcode_name(nvme_admin_async_event),		\
		nvme_admin_opcode_name(nvme_admin_ns_mgmt),		\
		nvme_admin_opcode_name(nvme_admin_activate_fw),		\
		nvme_admin_opcode_name(nvme_admin_download_fw),		\
		nvme_admin_opcode_name(nvme_admin_ns_attach),		\
		nvme_admin_opcode_name(nvme_admin_keep_alive),		\
		nvme_admin_opcode_name(nvme_admin_directive_send),	\
		nvme_admin_opcode_name(nvme_admin_directive_recv),	\
		nvme_admin_opcode_name(nvme_admin_dbbuf),		\
		nvme_admin_opcode_name(nvme_admin_format_nvm),		\
		nvme_admin_opcode_name(nvme_admin_security_send),	\
		nvme_admin_opcode_name(nvme_admin_security_recv),	\
		nvme_admin_opcode_name(nvme_admin_sanitize_nvm))

#define nvme_opcode_name(opcode)	{ opcode, #opcode }
#define show_nvm_opcode_name(val)				\
	__print_symbolic(val,					\
		nvme_opcode_name(nvme_cmd_flush),		\
		nvme_opcode_name(nvme_cmd_write),		\
		nvme_opcode_name(nvme_cmd_read),		\
		nvme_opcode_name(nvme_cmd_write_uncor),		\
		nvme_opcode_name(nvme_cmd_compare),		\
		nvme_opcode_name(nvme_cmd_write_zeroes),	\
		nvme_opcode_name(nvme_cmd_dsm),			\
		nvme_opcode_name(nvme_cmd_resv_register),	\
		nvme_opcode_name(nvme_cmd_resv_report),		\
		nvme_opcode_name(nvme_cmd_resv_acquire),	\
		nvme_opcode_name(nvme_cmd_resv_release))

#define show_opcode_name(qid, opcode)					\
	(qid ? show_nvm_opcode_name(opcode) : show_admin_opcode_name(opcode))

#define parse_nvme_cmd(qid, opcode, cdw10) 			\
	(qid ?							\
	 nvme_trace_parse_nvm_cmd(p, opcode, cdw10) : 		\
	 nvme_trace_parse_admin_cmd(p, opcode, cdw10))

#define __print_disk_name(name)				\
	nvme_trace_disk_name(p, name)


#endif	/* TRACE_HEADER_MULTI_READ */
