#undef TRACE_SYSTEM
#define TRACE_SYSTEM sync

#if !defined(_TRACE_SYNC_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SYNC_H

#include <linux/sync.h>
#include <linux/tracepoint.h>

TRACE_EVENT(sync_wait,
	TP_PROTO(struct sync_fence *fence, int begin),

	TP_ARGS(fence, begin),

	TP_STRUCT__entry(
			__string(name, fence->name)
			__field(s32, status)
			__field(u32, begin)
	),

	TP_fast_assign(
			__assign_str(name, fence->name);
			__entry->status = atomic_read(&fence->status);
			__entry->begin = begin;
	),

	TP_printk("%s name=%s state=%d", __entry->begin ? "begin" : "end",
			__get_str(name), __entry->status)
);

TRACE_EVENT(sync_fence,
	TP_PROTO(struct fence *fence),

	TP_ARGS(fence),

	TP_STRUCT__entry(
		__string(timeline, fence->ops->get_timeline_name(fence))
		__array(char, value, 32)
	),

	TP_fast_assign(
		__assign_str(timeline, fence->ops->get_timeline_name(fence));
		if (fence->ops->fence_value_str) {
			fence->ops->fence_value_str(fence, __entry->value,
							sizeof(__entry->value));
		} else {
			__entry->value[0] = '\0';
		}
	),

	TP_printk("name=%s value=%s", __get_str(timeline), __entry->value)
);

#endif /* if !defined(_TRACE_SYNC_H) || defined(TRACE_HEADER_MULTI_READ) */

/* This part must be outside protection */
#include <trace/define_trace.h>
