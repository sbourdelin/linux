#undef TRACE_SYSTEM
#define TRACE_SYSTEM of

#if !defined(_TRACE_OF_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_OF_H

#include <linux/of.h>
#include <linux/tracepoint.h>

DECLARE_EVENT_CLASS(of_node_ref_template,

	TP_PROTO(int refcount, const char* dn_name),

	TP_ARGS(refcount, dn_name),

	TP_STRUCT__entry(
		__string(dn_name, dn_name)
		__field(int, refcount)
	),

	TP_fast_assign(
		__assign_str(dn_name, dn_name);
		__entry->refcount = refcount;
	),

	TP_printk("refcount=%d, dn->full_name=%s",
		  __entry->refcount, __get_str(dn_name))
);

DEFINE_EVENT(of_node_ref_template, of_node_get,
	     TP_PROTO(int refcount, const char* dn_name),
	     TP_ARGS(refcount, dn_name));

DEFINE_EVENT(of_node_ref_template, of_node_put,
	     TP_PROTO(int refcount, const char* dn_name),
	     TP_ARGS(refcount, dn_name));

TRACE_EVENT(of_node_release,

	TP_PROTO(struct device_node *dn),

	TP_ARGS(dn),

	TP_STRUCT__entry(
		__string(dn_name, dn->full_name)
		__field(unsigned long, flags)
	),

	TP_fast_assign(
		__assign_str(dn_name, dn->full_name);
		__entry->flags = dn->_flags;
	),

	TP_printk("dn->full_name=%s, dn->_flags=%lu", 
		  __get_str(dn_name), __entry->flags)
);

#define of_reconfig_action_names \
	{OF_RECONFIG_ATTACH_NODE, "ATTACH_NODE"}, \
	{OF_RECONFIG_DETACH_NODE, "DETACH_NODE"}, \
	{OF_RECONFIG_ADD_PROPERTY, "ADD_PROPERTY"}, \
	{OF_RECONFIG_REMOVE_PROPERTY, "REMOVE_PROPERTY"}, \
	{OF_RECONFIG_UPDATE_PROPERTY, "UPDATE_PROPERTY"}

TRACE_EVENT(of_reconfig_notify,

	TP_PROTO(unsigned long action, struct of_reconfig_data *ord),

	TP_ARGS(action, ord),

	TP_STRUCT__entry(
		__field(unsigned long, action)
		__string(dn_name, ord->dn->full_name)
		__string(prop_name, ord->prop ? ord->prop->name : "null")
		__string(oldprop_name, ord->old_prop ? ord->old_prop->name : "null")
	),

	TP_fast_assign(
		__entry->action = action;
		__assign_str(dn_name, ord->dn->full_name);
		__assign_str(prop_name, ord->prop ? ord->prop->name : "null");
		__assign_str(oldprop_name, ord->old_prop ? ord->old_prop->name : "null");
	),

	TP_printk("action=%s, dn->full_name=%s, prop->name=%s, old_prop->name=%s",
		  __print_symbolic(__entry->action, of_reconfig_action_names),
		  __get_str(dn_name), __get_str(prop_name), __get_str(oldprop_name))
);

#endif /*	_TRACE_OF_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
