/*
 * Copyright (c) 2016 Red Hat, Inc., All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM fs
#define TRACE_INCLUDE_FILE fs

#if !defined(_TRACE_FS_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_FS_H_
#include <linux/tracepoint.h>

struct block_device;
struct super_block;
#include <linux/fs.h>

#define SUPER_ID_MAX_LEN FIELD_SIZEOF(struct super_block, s_id)
#define FSTYPE_MAX_LEN 32 /* choose an arbitrary limit */

DECLARE_EVENT_CLASS(super_freezethaw_class,
	TP_PROTO(struct super_block *sb),
	TP_ARGS(sb),
	TP_STRUCT__entry(
		__array(char,	comm,		TASK_COMM_LEN		)
		__array(char,	super_id,	SUPER_ID_MAX_LEN	)
		__array(char,	fstype,		FSTYPE_MAX_LEN		)
		__field(dev_t,	dev					)
		__field(int,	frozen					)
		__field(pid_t,	pid					)
	),
	TP_fast_assign(
		memcpy(__entry->comm, current->comm, TASK_COMM_LEN);
		memcpy(__entry->super_id, sb->s_id, SUPER_ID_MAX_LEN);
		memcpy(__entry->fstype, sb->s_type->name, FSTYPE_MAX_LEN);
		__entry->dev = sb->s_dev;
		__entry->frozen = sb->s_writers.frozen;
		__entry->pid	= current->pid;
	),
	TP_printk("comm=%s pid=%d for %s filesystem '%s' (%d:%d) frozen=%d",
		__entry->comm, __entry->pid, __entry->fstype,
		__entry->super_id,
		MAJOR(__entry->dev), MINOR(__entry->dev),
		__entry->frozen
	)
);

DECLARE_EVENT_CLASS(super_freezethaw_exit_class,
	TP_PROTO(struct super_block *sb, int ret),
	TP_ARGS(sb, ret),
	TP_STRUCT__entry(
		__array(char,	comm,		TASK_COMM_LEN		)
		__array(char,	super_id,	SUPER_ID_MAX_LEN	)
		__array(char,	fstype,		FSTYPE_MAX_LEN		)
		__field(dev_t,	dev					)
		__field(int,	frozen					)
		__field(pid_t,	pid					)
		__field(int,	ret					)
	),
	TP_fast_assign(
		memcpy(__entry->comm, current->comm, TASK_COMM_LEN);
		memcpy(__entry->super_id, sb->s_id, SUPER_ID_MAX_LEN);
		memcpy(__entry->fstype, sb->s_type->name, FSTYPE_MAX_LEN);
		__entry->dev = sb->s_dev;
		__entry->frozen = sb->s_writers.frozen;
		__entry->pid	= current->pid;
		__entry->ret = ret;
	),
	TP_printk("comm=%s pid=%d for %s filesystem '%s' (%d:%d) frozen=%d ret=%d",
		__entry->comm, __entry->pid, __entry->fstype,
		__entry->super_id,
		MAJOR(__entry->dev), MINOR(__entry->dev),
		__entry->frozen, __entry->ret
	)
);

DEFINE_EVENT(super_freezethaw_class, freeze_super_enter,
	TP_PROTO(struct super_block *sb),
	TP_ARGS(sb)
);
DEFINE_EVENT(super_freezethaw_exit_class, freeze_super_exit,
	TP_PROTO(struct super_block *sb, int ret),
	TP_ARGS(sb, ret)
);

DEFINE_EVENT(super_freezethaw_class, thaw_super_enter,
	TP_PROTO(struct super_block *sb),
	TP_ARGS(sb)
);
DEFINE_EVENT(super_freezethaw_exit_class, thaw_super_exit,
	TP_PROTO(struct super_block *sb, int ret),
	TP_ARGS(sb, ret)
);

#endif /* _TRACE_FS_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
