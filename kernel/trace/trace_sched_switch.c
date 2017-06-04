/*
 * trace context switch
 *
 * Copyright (C) 2007 Steven Rostedt <srostedt@redhat.com>
 *
 */
#include <linux/module.h>
#include <linux/kallsyms.h>
#include <linux/uaccess.h>
#include <linux/ftrace.h>
#include <trace/events/sched.h>

#include "trace.h"

static int		sched_cmdline_ref;
static int		sched_tgid_ref;
static DEFINE_MUTEX(sched_register_mutex);

#define RECORD_CMD	BIT(0)
#define RECORD_TGID	BIT(1)

static void
probe_sched_switch(void *ignore, bool preempt,
		   struct task_struct *prev, struct task_struct *next)
{
	struct task_struct *tasks[2] = { prev, next };

	tracing_record_taskinfo(tasks, 2,
				(sched_cmdline_ref != 0),
				(sched_tgid_ref != 0));
}

static void
probe_sched_wakeup(void *ignore, struct task_struct *wakee)
{
	tracing_record_taskinfo_single(current,
				       (sched_cmdline_ref != 0),
				       (sched_tgid_ref != 0));
}

static int tracing_sched_register(void)
{
	int ret;

	ret = register_trace_sched_wakeup(probe_sched_wakeup, NULL);
	if (ret) {
		pr_info("wakeup trace: Couldn't activate tracepoint"
			" probe to kernel_sched_wakeup\n");
		return ret;
	}

	ret = register_trace_sched_wakeup_new(probe_sched_wakeup, NULL);
	if (ret) {
		pr_info("wakeup trace: Couldn't activate tracepoint"
			" probe to kernel_sched_wakeup_new\n");
		goto fail_deprobe;
	}

	ret = register_trace_sched_switch(probe_sched_switch, NULL);
	if (ret) {
		pr_info("sched trace: Couldn't activate tracepoint"
			" probe to kernel_sched_switch\n");
		goto fail_deprobe_wake_new;
	}

	return ret;
fail_deprobe_wake_new:
	unregister_trace_sched_wakeup_new(probe_sched_wakeup, NULL);
fail_deprobe:
	unregister_trace_sched_wakeup(probe_sched_wakeup, NULL);
	return ret;
}

static void tracing_sched_unregister(void)
{
	unregister_trace_sched_switch(probe_sched_switch, NULL);
	unregister_trace_sched_wakeup_new(probe_sched_wakeup, NULL);
	unregister_trace_sched_wakeup(probe_sched_wakeup, NULL);
}

static void tracing_start_sched_switch(int flags)
{
	bool sched_register;
	mutex_lock(&sched_register_mutex);
	sched_register = (!sched_cmdline_ref && !sched_tgid_ref);

	if (flags & RECORD_CMD)
		sched_cmdline_ref++;

	if (flags & RECORD_TGID)
		sched_tgid_ref++;

	if (sched_tgid_ref == 1)
		tracing_alloc_tgid_map();

	if (sched_register && (sched_cmdline_ref || sched_tgid_ref))
		tracing_sched_register();
	mutex_unlock(&sched_register_mutex);
}

static void tracing_stop_sched_switch(int flags)
{
	mutex_lock(&sched_register_mutex);
	if (flags & RECORD_CMD)
		sched_cmdline_ref--;

	if (flags & RECORD_TGID)
		sched_tgid_ref--;

	if (!sched_cmdline_ref && !sched_tgid_ref)
		tracing_sched_unregister();
	mutex_unlock(&sched_register_mutex);
}

void tracing_start_taskinfo_record(bool cmdline, bool tgid)
{
	int flags;

	if (!cmdline && !tgid)
		return;
	flags  = (tgid ? RECORD_TGID : 0);
	flags |= (cmdline ? RECORD_CMD : 0);
	tracing_start_sched_switch(flags);
}

void tracing_stop_taskinfo_record(bool cmdline, bool tgid)
{
	int flags;

	if (!cmdline && !tgid)
		return;
	flags  = (tgid ? RECORD_TGID : 0);
	flags |= (cmdline ? RECORD_CMD : 0);
	tracing_stop_sched_switch(flags);
}
