#undef TRACE_SYSTEM
#define TRACE_SYSTEM latency

#if !defined(_TRACE_HIST_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HIST_H

#include <linux/tracepoint.h>

DECLARE_EVENT_CLASS(latency_template,
	TP_PROTO(int ltype, cycles_t latency),

	TP_ARGS(ltype, latency),

	TP_STRUCT__entry(
		__field(int,		ltype)
		__field(cycles_t,	latency)
	),

	TP_fast_assign(
		__entry->ltype		= ltype;
		__entry->latency	= latency;
	),

	TP_printk("ltype=%d, latency=%lu",
		__entry->ltype, (unsigned long) __entry->latency)
);

DEFINE_EVENT(latency_template, latency_preempt,
	    TP_PROTO(int ltype, cycles_t latency),
	    TP_ARGS(ltype, latency));

TRACE_EVENT(latency_hrtimer_interrupt,

	TP_PROTO(long long toffset, struct task_struct *curr,
		struct task_struct *task),

	TP_ARGS(toffset, curr, task),

	TP_STRUCT__entry(
		__field(long long,	toffset)
		__array(char,		ccomm,	TASK_COMM_LEN)
		__field(int,		cprio)
		__array(char,		tcomm,	TASK_COMM_LEN)
		__field(int,		tprio)
	),

	TP_fast_assign(
		__entry->toffset = toffset;
		memcpy(__entry->ccomm, curr->comm, TASK_COMM_LEN);
		__entry->cprio  = curr->prio;
		memcpy(__entry->tcomm, task != NULL ? task->comm : "<none>",
			task != NULL ? TASK_COMM_LEN : 7);
		__entry->tprio  = task != NULL ? task->prio : -1;
	),

	TP_printk("toffset=%lld curr=%s[%d] thread=%s[%d]",
		__entry->toffset, __entry->ccomm, __entry->cprio,
		__entry->tcomm, __entry->tprio)
);

#endif /* _TRACE_HIST_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
