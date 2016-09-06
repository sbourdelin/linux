/*
 * kobject trace points
 *
 * Copyright (C) 2016 Shuah Khan <shuahkh@osg.samsung.com>
 *
 */
#include <linux/string.h>
#include <linux/types.h>

#define CREATE_TRACE_POINTS
#include <trace/events/kobject.h>

/* kobject_init event */
EXPORT_TRACEPOINT_SYMBOL_GPL(kobject_init);

/* kobject_class events */
EXPORT_TRACEPOINT_SYMBOL_GPL(kobject_add);
EXPORT_TRACEPOINT_SYMBOL_GPL(kobject_init_and_add);
EXPORT_TRACEPOINT_SYMBOL_GPL(kobject_create_and_add);
EXPORT_TRACEPOINT_SYMBOL_GPL(kobject_set_name);
EXPORT_TRACEPOINT_SYMBOL_GPL(kobject_get);
EXPORT_TRACEPOINT_SYMBOL_GPL(kobject_del);
EXPORT_TRACEPOINT_SYMBOL_GPL(kobject_cleanup);

/* kobject_put() event */
EXPORT_TRACEPOINT_SYMBOL_GPL(kobject_put);

/* kobject_move event */
EXPORT_TRACEPOINT_SYMBOL_GPL(kobject_move);

/* kobject_rename event */
EXPORT_TRACEPOINT_SYMBOL_GPL(kobject_rename);
