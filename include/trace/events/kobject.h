/*
 * kobject trace points
 *
 * Copyright (C) 2016 Shuah Khan <shuahkh@osg.samsung.com>
 *
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM kobject

#if !defined(_TRACE_KOBJECT_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_KOBJECT_H

#include <linux/tracepoint.h>
#include <linux/kobject.h>

/* kobject_init_class */
DECLARE_EVENT_CLASS(kobject_init_class,

	TP_PROTO(struct kobject *kobj),

	TP_ARGS(kobj),

	TP_STRUCT__entry(
		__field(void *, kobj)
		__field(int, state_initialized)
	),

	TP_fast_assign(
		__entry->kobj = kobj;
		__entry->state_initialized = kobj->state_initialized;
	),

	TP_printk("KOBJECT: %p state=%d",
		  __entry->kobj,
		  __entry->state_initialized
	));

/**
 * kobject_init	- called from kobject_init() when kobject is initialized
 * @kobj:	- pointer to struct kobject
 */
DEFINE_EVENT(kobject_init_class, kobject_init,

	TP_PROTO(struct kobject *kobj),
	TP_ARGS(kobj));

/* kobject_class */
DECLARE_EVENT_CLASS(kobject_class,

	TP_PROTO(struct kobject *kobj),
	TP_ARGS(kobj),

	TP_STRUCT__entry(
		__field(void *, kobj)
		__string(name, kobject_name(kobj))
		__field(int, state_initialized)
		__field(void *, parent)
		__string(pname, kobj->parent ? kobject_name(kobj->parent) : "")
		__field(int, count)
	),

	TP_fast_assign(
		__entry->kobj = kobj;
		__assign_str(name, kobject_name(kobj));
		__entry->state_initialized = kobj->state_initialized;
		__entry->parent = kobj->parent;
		__assign_str(pname,
			     kobj->parent ? kobject_name(kobj->parent) : "");
		__entry->count = atomic_read(&kobj->kref.refcount);
	),

	TP_printk("KOBJECT: %s (%p) state=%d parent= %s (%p) counter= %d",
		  __get_str(name),
		  __entry->kobj,
		  __entry->state_initialized,
		  __get_str(pname),
		  __entry->parent,
		  __entry->count
	));

/**
 * kobject_add	- called from kobject_add() when kobject is added
 * @kobj:	- pointer to struct kobject
 */
DEFINE_EVENT(kobject_class, kobject_add,

	TP_PROTO(struct kobject *kobj),
	TP_ARGS(kobj));

/**
 * kobject_init_and_add	- called from kobject_init_and_add()
 * @kobj:		- pointer to struct kobject
 */
DEFINE_EVENT(kobject_class, kobject_init_and_add,

	TP_PROTO(struct kobject *kobj),
	TP_ARGS(kobj));

/**
 * kobject_create_and_add	- called from kobject_create_and_add()
 * @kobj:			- pointer to struct kobject
 */
DEFINE_EVENT(kobject_class, kobject_create_and_add,

	TP_PROTO(struct kobject *kobj),
	TP_ARGS(kobj));

/**
 * kobject_set_name	- called from kobject_set_name()
 * @kobj:		- pointer to struct kobject
 */
DEFINE_EVENT(kobject_class, kobject_set_name,

	TP_PROTO(struct kobject *kobj),
	TP_ARGS(kobj));

/**
 * kobject_del	- called from kobject_del()
 * @kobj:	- pointer to struct kobject
 */
DEFINE_EVENT(kobject_class, kobject_del,

	TP_PROTO(struct kobject *kobj),
	TP_ARGS(kobj));

/**
 * kobject_cleanup	- called from kobject_cleanup()
 * @kobj:		- pointer to struct kobject
 */
DEFINE_EVENT(kobject_class, kobject_cleanup,

	TP_PROTO(struct kobject *kobj),
	TP_ARGS(kobj));
/**
 * kobject_get	- called from kobject_get()
 * @kobj:	- pointer to struct kobject
 */
DEFINE_EVENT(kobject_class, kobject_get,

	TP_PROTO(struct kobject *kobj),
	TP_ARGS(kobj));

/**
 * kobject_put	- called from kobject_put()
 * @kobj:	- pointer to struct kobject
 *
 * Trace is called before kref_put() to avoid use-after-free on kobj, in case
 * kobject is released. Decrement the counter before printing the value.
 */
TRACE_EVENT(kobject_put,

	TP_PROTO(struct kobject *kobj),
	TP_ARGS(kobj),

	TP_STRUCT__entry(
		__field(void *, kobj)
		__string(name, kobject_name(kobj))
		__field(int, state_initialized)
		__field(void *, parent)
		__string(pname, kobj->parent ? kobject_name(kobj->parent) : "")
		__field(int, count)
	),

	TP_fast_assign(
		__entry->kobj = kobj;
		__assign_str(name, kobject_name(kobj));
		__entry->state_initialized = kobj->state_initialized;
		__entry->parent = kobj->parent;
		__assign_str(pname,
			     kobj->parent ? kobject_name(kobj->parent) : "");
		__entry->count = atomic_read(&kobj->kref.refcount) - 1;
	),

	TP_printk("KOBJECT: %s (%p) state=%d parent= %s (%p) counter= %d",
		  __get_str(name),
		  __entry->kobj,
		  __entry->state_initialized,
		  __get_str(pname),
		  __entry->parent,
		  __entry->count
	));

/* kobject_move_class */
DECLARE_EVENT_CLASS(kobject_move_class,

	TP_PROTO(struct kobject *kobj, struct kobject *old_parent),
	TP_ARGS(kobj, old_parent),

	TP_STRUCT__entry(
		__field(void *, kobj)
		__string(name, kobject_name(kobj))
		__field(int, state_initialized)
		__field(void *, parent)
		__string(pname, kobj->parent ? kobject_name(kobj->parent) : "")
	),

	TP_fast_assign(
		__entry->kobj = kobj;
		__assign_str(name, kobject_name(kobj));
		__entry->state_initialized = kobj->state_initialized;
		__entry->parent = kobj->parent;
		__assign_str(pname,
			     kobj->parent ? kobject_name(kobj->parent) : "");
	),

	TP_printk("KOBJECT: %s (%p) state=%d parent= %s (%p)",
		  __get_str(name),
		  __entry->kobj,
		  __entry->state_initialized,
		  __get_str(pname),
		  __entry->parent
	));

/**
 * kobject_move	- called from kobject_move()
 * @kobj:	- pointer to struct kobject
 */
DEFINE_EVENT(kobject_move_class, kobject_move,

	TP_PROTO(struct kobject *kobj, struct kobject *old_parent),
	TP_ARGS(kobj, old_parent));

/* kobject_rename_class */
DECLARE_EVENT_CLASS(kobject_rename_class,

	TP_PROTO(struct kobject *kobj, const char *old),
	TP_ARGS(kobj, old),

	TP_STRUCT__entry(
		__field(void *, kobj)
		__string(name, kobject_name(kobj))
		__string(oldname, old)
	),

	TP_fast_assign(
		__entry->kobj = kobj;
		__assign_str(name, kobject_name(kobj));
		__assign_str(oldname, old);
	),

	TP_printk("KOBJECT: %s (%p) oldname= %s",
		  __get_str(name),
		  __entry->kobj,
		  __get_str(oldname)
	));

/**
 * kobject_rename	- called from kobject_rename()
 * @kobj:		- pointer to struct kobject
 */
DEFINE_EVENT(kobject_rename_class, kobject_rename,

	TP_PROTO(struct kobject *kobj, const char *old),
	TP_ARGS(kobj, old));

#endif /* #if !defined(_TRACE_KOBJECT_H) ... */

/* This part must be outside protection */
#include <trace/define_trace.h>
