#ifdef CONFIG_CRITICAL_SECTION_EVENTS
#undef TRACE_SYSTEM
#define TRACE_SYSTEM critical

#if !defined(_TRACE_CRITICAL_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_CRITICAL_H

#include <linux/ktime.h>
#include <linux/tracepoint.h>
#include <linux/string.h>
#include <asm/sections.h>

DECLARE_EVENT_CLASS(critical_template,

	TP_PROTO(unsigned long ip, unsigned long parent_ip),

	TP_ARGS(ip, parent_ip),

	TP_STRUCT__entry(
		__field(u32, caller_offs)
		__field(u32, parent_offs)
	),

	TP_fast_assign(
		__entry->caller_offs = (u32)(ip - (unsigned long)_stext);
		__entry->parent_offs = (u32)(parent_ip - (unsigned long)_stext);
	),

	TP_printk("caller=%pF parent=%pF\n",
		  (void *)((unsigned long)(_stext) + __entry->caller_offs),
		  (void *)((unsigned long)(_stext) + __entry->parent_offs))
);

DEFINE_EVENT(critical_template, critical_start,
	     TP_PROTO(unsigned long ip, unsigned long parent_ip),
	     TP_ARGS(ip, parent_ip));

DEFINE_EVENT(critical_template, critical_stop,
	     TP_PROTO(unsigned long ip, unsigned long parent_ip),
	     TP_ARGS(ip, parent_ip));
#endif /* _TRACE_CRITICAL_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
#endif
