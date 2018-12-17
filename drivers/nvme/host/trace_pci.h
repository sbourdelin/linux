/*
 * NVM Express device driver tracepoints
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

#undef TRACE_SYSTEM
#define TRACE_SYSTEM nvme

#if !defined(_TRACE_NVME_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_NVME_H

#include <linux/nvme.h>
#include <linux/tracepoint.h>
#include <linux/trace_seq.h>

#include "nvme.h"

const char *nvme_trace_disk_name(struct trace_seq *p, char *name);
#define __print_disk_name(name)				\
	nvme_trace_disk_name(p, name)

TRACE_EVENT(nvme_sq,
	    TP_PROTO(void *rq_disk, int qid, int sq_head, int sq_tail),
	    TP_ARGS(rq_disk, qid, sq_head, sq_tail),
	    TP_STRUCT__entry(
		 __array(char, disk, DISK_NAME_LEN)
		 __field(int, qid)
		 __field(int, sq_head)
		 __field(int, sq_tail)),
	    TP_fast_assign(
		__entry->qid = qid;
		__entry->sq_head = sq_head;
		__entry->sq_tail = sq_tail;
		__assign_disk_name(__entry->disk, rq_disk);
	    ),
	    TP_printk("nvme: %s qid=%d head=%d tail=%d",
		      __print_disk_name(__entry->disk),
		      __entry->qid, __entry->sq_head, __entry->sq_tail)
);

#endif /* _TRACE_NVME_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace_pci

/* This part must be outside protection */
#include <trace/define_trace.h>
