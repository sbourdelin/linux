#undef TRACE_SYSTEM
#define TRACE_SYSTEM perf

#if !defined(_TRACE_PERF_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_PERF_H

#include <linux/tracepoint.h>

TRACE_EVENT(perf_hrtimer,
	TP_PROTO(struct pt_regs *regs, struct perf_event *event),

	TP_ARGS(regs, event),

	TP_STRUCT__entry(
		__field(struct pt_regs *, regs)
		__field(struct perf_event *, event)
	),

	TP_fast_assign(
		__entry->regs = regs;
		__entry->event = event;
	),

	TP_printk("regs=%p evt=%p", __entry->regs, __entry->event)
);
#endif /* _TRACE_PERF_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
