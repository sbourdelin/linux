/*
 * Copyright (C) 2017 ARM Ltd.
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
#include <kvm/host_pmu.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/kvm_host.h>
#include <linux/list.h>
#include <linux/perf_event.h>
#include <linux/pid.h>
#include <linux/slab.h>
#include <linux/spinlock_types.h>
#include <linux/sysfs.h>

struct host_pmu {
	int nr_events;
	struct pmu pmu;
	struct kvm_event_cb *cbs;
	spinlock_t event_list_lock;
	struct list_head event_list_head;
} host_pmu;
#define to_host_pmu(p) (container_of(p, struct host_pmu, pmu))

struct event_data {
	int event_id;
	struct kvm_vcpu *vcpu;
	struct kvm_event_cb *cb;
	struct list_head event_list;
};

static struct attribute_group events_attr_group = {
	.name	= "events",
};


#define EVENT_MASK	GENMASK_ULL(0, 7)
#define to_event(cfg)	((cfg) & EVENT_MASK)

PMU_FORMAT_ATTR(event, "config:0-7");

static struct attribute *format_attrs[] = {
	&format_attr_event.attr,
	NULL,
};

static struct attribute_group format_attr_group = {
	.name	= "format",
	.attrs	= format_attrs,
};

static const struct attribute_group *attr_groups[] = {
	&events_attr_group,
	&format_attr_group,
	NULL,
};

static void host_event_destroy(struct perf_event *event)
{
	struct host_pmu *host_pmu = to_host_pmu(event->pmu);
	struct event_data *e_data = event->pmu_private;

	spin_lock(&host_pmu->event_list_lock);
	list_del(&e_data->event_list);
	spin_unlock(&host_pmu->event_list_lock);
	kfree(e_data);
}

static int host_event_init(struct perf_event *event)
{
	struct host_pmu *host_pmu = to_host_pmu(event->pmu);
	int event_id = to_event(event->attr.config);
	struct event_data *e_data, *pos;
	struct kvm_vcpu *vcpu;
	bool found = false;
	struct pid *pid;
	struct kvm *kvm;
	int i, ret = 0;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	if (has_branch_stack(event)	||
	    is_sampling_event(event)	||
	    event->attr.exclude_user	||
	    event->attr.exclude_kernel	||
	    event->attr.exclude_hv	||
	    event->attr.exclude_idle	||
	    event->attr.exclude_guest) {
		return -EINVAL;
	}

	if (event->attach_state != PERF_ATTACH_TASK)
		return -EOPNOTSUPP;

	if (event_id >= host_pmu->nr_events)
		return -EINVAL;

	pid = get_task_pid(event->hw.target, PIDTYPE_PID);
	spin_lock(&kvm_lock);
	list_for_each_entry(kvm, &vm_list, vm_list) {
		kvm_for_each_vcpu(i, vcpu, kvm) {
			if (vcpu->pid == pid) {
				found = true;
				goto found;
			}
		}
	}
found:
	spin_unlock(&kvm_lock);
	put_pid(pid);

	if (!found)
		return -EINVAL;

	spin_lock(&host_pmu->event_list_lock);
	/* Make sure we don't already have the (event_id, kvm) pair */
	list_for_each_entry(pos, &host_pmu->event_list_head, event_list) {
		if (pos->event_id == event_id &&
		    pos->vcpu->pid == pid) {
			ret = -EOPNOTSUPP;
			goto unlock;
		}
	}

	e_data = kzalloc(sizeof(*e_data), GFP_KERNEL);
	e_data->event_id = event_id;
	e_data->vcpu = vcpu;
	e_data->cb = &host_pmu->cbs[event_id];

	event->pmu_private = e_data;
	event->destroy = host_event_destroy;

	list_add_tail(&e_data->event_list, &host_pmu->event_list_head);

unlock:
	spin_unlock(&host_pmu->event_list_lock);

	return ret;
}

static void host_event_update(struct perf_event *event)
{
	struct event_data *e_data = event->pmu_private;
	struct kvm_event_cb *cb = e_data->cb;
	struct kvm_vcpu *vcpu = e_data->vcpu;
	struct hw_perf_event *hw = &event->hw;
	u64 prev_count, new_count;

	do {
		prev_count = local64_read(&hw->prev_count);
		new_count = cb->get_event_count(vcpu);
	} while (local64_xchg(&hw->prev_count, new_count) != prev_count);

	local64_add(new_count - prev_count, &event->count);
}

static void host_event_start(struct perf_event *event, int flags)
{
	struct event_data *e_data = event->pmu_private;
	struct kvm_event_cb *cb = e_data->cb;
	struct kvm_vcpu *vcpu = e_data->vcpu;
	u64 val;

	val = cb->get_event_count(vcpu);
	local64_set(&event->hw.prev_count, val);

	cb->configure_event(vcpu, true);
}

static void host_event_stop(struct perf_event *event, int flags)
{
	struct event_data *e_data = event->pmu_private;
	struct kvm_event_cb *cb = e_data->cb;
	struct kvm_vcpu *vcpu = e_data->vcpu;

	cb->configure_event(vcpu, false);

	if (flags & PERF_EF_UPDATE)
		host_event_update(event);
}

static int host_event_add(struct perf_event *event, int flags)
{
	if (flags & PERF_EF_START)
		host_event_start(event, flags);

	return 0;
}

static void host_event_del(struct perf_event *event, int flags)
{
	host_event_stop(event, PERF_EF_UPDATE);
}

static void host_event_read(struct perf_event *event)
{
	host_event_update(event);
}

static void init_host_pmu(struct host_pmu *host_pmu)
{
	host_pmu->pmu = (struct pmu) {
		.task_ctx_nr	= perf_sw_context,
		.attr_groups	= attr_groups,
		.event_init	= host_event_init,
		.add		= host_event_add,
		.del		= host_event_del,
		.start		= host_event_start,
		.stop		= host_event_stop,
		.read		= host_event_read,
		.capabilities	= PERF_PMU_CAP_NO_INTERRUPT,
	};

	INIT_LIST_HEAD(&host_pmu->event_list_head);
	spin_lock_init(&host_pmu->event_list_lock);
}

int kvm_host_pmu_register(int nr_events, struct kvm_event_cb *cbs,
			  struct attribute **event_attrs)
{
	host_pmu.nr_events  = nr_events;
	host_pmu.cbs = cbs;
	events_attr_group.attrs = event_attrs;

	init_host_pmu(&host_pmu);

	return perf_pmu_register(&host_pmu.pmu, "kvm", -1);
}

void kvm_host_pmu_unregister(void)
{
	perf_pmu_unregister(&host_pmu.pmu);
}
