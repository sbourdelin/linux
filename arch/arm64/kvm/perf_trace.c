/*
 * Copyright (C) 2016 ARM Ltd.
 * Author: Punit Agrawal <punit.agrawal@arm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/kvm_host.h>
#include <linux/trace_events.h>

typedef int (*perf_trace_callback_fn)(struct kvm *kvm, bool enable);

struct kvm_trace_hook {
	char *key;
	perf_trace_callback_fn setup_fn;
};

static struct kvm_trace_hook trace_hook[] = {
	{ },
};

static perf_trace_callback_fn find_trace_callback(const char *trace_key)
{
	int i;

	for (i = 0; trace_hook[i].key; i++)
		if (!strcmp(trace_key, trace_hook[i].key))
			return trace_hook[i].setup_fn;

	return NULL;
}

static int kvm_perf_trace_notifier(struct notifier_block *nb,
				   unsigned long event, void *data)
{
	struct perf_event *p_event = data;
	struct trace_event_call *tp_event = p_event->tp_event;
	perf_trace_callback_fn setup_trace_fn;
	struct kvm *kvm = NULL;
	struct pid *pid;
	bool found = false;

	/*
	 * Is this a trace point?
	 */
	if (!(tp_event->flags & TRACE_EVENT_FL_TRACEPOINT))
		goto out;

	/*
	 * We'll get here for events we care to monitor for KVM. As we
	 * only care about events attached to a VM, check that there
	 * is a task associated with the perf event.
	 */
	if (p_event->attach_state != PERF_ATTACH_TASK)
		goto out;

	/*
	 * This notifier gets called when perf trace event instance is
	 * added or removed. Until we can restrict this to events of
	 * interest in core, minimise the overhead below.
	 *
	 * Do we care about it? i.e., is there a callback for this
	 * trace point?
	 */
	setup_trace_fn = find_trace_callback(tp_event->tp->name);
	if (!setup_trace_fn)
		goto out;

	pid = get_task_pid(p_event->hw.target, PIDTYPE_PID);

	/*
	 * Does it match any of the VMs?
	 */
	spin_lock(&kvm_lock);
	list_for_each_entry(kvm, &vm_list, vm_list) {
		if (kvm->pid == pid) {
			found = true;
			break;
		}
	}
	spin_unlock(&kvm_lock);

	put_pid(pid);
	if (!found)
		goto out;

	switch (event) {
	case TRACE_REG_PERF_OPEN:
		setup_trace_fn(kvm, true);
		break;

	case TRACE_REG_PERF_CLOSE:
		setup_trace_fn(kvm, false);
		break;
	}

out:
	return 0;
}

static struct notifier_block kvm_perf_trace_notifier_block = {
	.notifier_call = kvm_perf_trace_notifier,
};

int kvm_perf_trace_init(void)
{
	return perf_trace_notifier_register(&kvm_perf_trace_notifier_block);
}

int kvm_perf_trace_teardown(void)
{
	return perf_trace_notifier_unregister(&kvm_perf_trace_notifier_block);
}
