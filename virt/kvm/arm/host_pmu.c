#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/kvm_host.h>
#include <linux/list.h>
#include <linux/perf_event.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock_types.h>
#include <linux/sysfs.h>

#include <asm/kvm_emulate.h>

enum host_pmu_events {
	tlb_invalidate,
	KVM_HOST_MAX_EVENTS,
};

struct host_pmu {
	struct pmu pmu;
	spinlock_t event_list_lock;
	struct list_head event_list_head;
} host_pmu;
#define to_host_pmu(p) (container_of(p, struct host_pmu, pmu))

typedef void (*configure_event_fn)(struct kvm *kvm, bool enable);
typedef u64 (*get_event_count_fn)(struct kvm *kvm);

struct kvm_event_cb {
	enum host_pmu_events event;
	get_event_count_fn get_event_count;
	configure_event_fn configure_event;
};

struct event_data {
	bool enable;
	struct kvm *kvm;
	struct kvm_event_cb *cb;
	struct work_struct work;
	struct list_head event_list;
};

static u64 get_tlb_invalidate_count(struct kvm *kvm)
{
	struct kvm_vcpu *vcpu;
	u64 val = 0;
	int i;

	kvm_for_each_vcpu(i, vcpu, kvm)
		val += vcpu->stat.tlb_invalidate;

	return val;
}

static void configure_tlb_invalidate(struct kvm *kvm, bool enable)
{
	struct kvm_vcpu *vcpu;
	int i;

	kvm_arm_halt_guest(kvm);
	kvm_for_each_vcpu(i, vcpu, kvm) {
		unsigned long hcr = vcpu_get_hcr(vcpu);

		if (enable)
			hcr |= HCR_TTLB;
		else
			hcr &= ~HCR_TTLB;

		vcpu_set_hcr(vcpu, hcr);
	}
	kvm_arm_resume_guest(kvm);
}

static struct kvm_event_cb event_callbacks[] = {
	{
		.event			= tlb_invalidate,
		.get_event_count	= get_tlb_invalidate_count,
		.configure_event	= configure_tlb_invalidate,
	}
};

static ssize_t events_sysfs_show(struct device *dev,
				 struct device_attribute *attr, char *page)
{
	struct perf_pmu_events_attr *pmu_attr;

	pmu_attr = container_of(attr, struct perf_pmu_events_attr, attr);

	return sprintf(page, "event=0x%03llx,vm=?\n", pmu_attr->id);
}
PMU_EVENT_ATTR(tlb_invalidate, event_attr_tlb_invalidate, tlb_invalidate,
	       events_sysfs_show);

static struct attribute *event_attrs[] = {
	&event_attr_tlb_invalidate.attr.attr,
	NULL,
};

static struct attribute_group events_attr_group = {
	.name	= "events",
	.attrs	= event_attrs,
};


#define VM_MASK	GENMASK_ULL(31, 0)
#define EVENT_MASK	GENMASK_ULL(32, 39)
#define EVENT_SHIFT	(32)

#define to_pid(cfg)	((cfg) & VM_MASK)
#define to_event(cfg)	(((cfg) & EVENT_MASK) >> EVENT_SHIFT)

PMU_FORMAT_ATTR(vm, "config:0-31");
PMU_FORMAT_ATTR(event, "config:32-39");

static struct attribute *format_attrs[] = {
	&format_attr_vm.attr,
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

	/*
	 * Ensure outstanding work items related to this event are
	 * completed before freeing resources.
	 */
	flush_work(&e_data->work);

	kvm_put_kvm(e_data->kvm);

	spin_lock(&host_pmu->event_list_lock);
	list_del(&e_data->event_list);
	spin_unlock(&host_pmu->event_list_lock);
	kfree(e_data);
}

void host_event_work(struct work_struct *work)
{
	struct event_data *e_data = container_of(work, struct event_data, work);
	struct kvm *kvm = e_data->kvm;

	e_data->cb->configure_event(kvm, e_data->enable);
}

static int host_event_init(struct perf_event *event)
{
	struct host_pmu *host_pmu = to_host_pmu(event->pmu);
	int event_id = to_event(event->attr.config);
	pid_t task_pid = to_pid(event->attr.config);
	struct event_data *e_data, *pos;
	bool found = false;
	struct pid *pid;
	struct kvm *kvm;
	int ret = 0;

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

	if (event->attach_state == PERF_ATTACH_TASK)
		return -EOPNOTSUPP;

	if (event->cpu < 0)
		return -EINVAL;

	if (event_id >= KVM_HOST_MAX_EVENTS)
		return -EINVAL;

	pid = find_get_pid(task_pid);
	spin_lock(&kvm_lock);
	list_for_each_entry(kvm, &vm_list, vm_list) {
		if (kvm->pid == pid) {
			kvm_get_kvm(kvm);
			found = true;
			break;
		}
	}
	spin_unlock(&kvm_lock);
	put_pid(pid);

	if (!found)
		return -EINVAL;

	spin_lock(&host_pmu->event_list_lock);
	/* Make sure we don't already have the (event_id, kvm) pair */
	list_for_each_entry(pos, &host_pmu->event_list_head, event_list) {
		if (pos->cb->event == event_id &&
		    pos->kvm->pid == pid) {
			kvm_put_kvm(kvm);
			ret = -EOPNOTSUPP;
			goto unlock;
		}
	}

	e_data = kzalloc(sizeof(*e_data), GFP_KERNEL);
	e_data->kvm = kvm;
	e_data->cb = &event_callbacks[event_id];
	INIT_WORK(&e_data->work, host_event_work);
	event->pmu_private = e_data;
	event->cpu = cpumask_first(cpu_online_mask);
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
	struct kvm *kvm = e_data->kvm;
	struct hw_perf_event *hw = &event->hw;
	u64 prev_count, new_count;

	do {
		prev_count = local64_read(&hw->prev_count);
		new_count = cb->get_event_count(kvm);
	} while (local64_xchg(&hw->prev_count, new_count) != prev_count);

	local64_add(new_count - prev_count, &event->count);
}

static void host_event_start(struct perf_event *event, int flags)
{
	struct event_data *e_data = event->pmu_private;
	struct kvm_event_cb *cb = e_data->cb;
	struct kvm *kvm = e_data->kvm;
	u64 val;

	val = cb->get_event_count(kvm);
	local64_set(&event->hw.prev_count, val);

	e_data->enable = true;
	schedule_work(&e_data->work);
}

static void host_event_stop(struct perf_event *event, int flags)
{
	struct event_data *e_data = event->pmu_private;

	e_data->enable = false;
	schedule_work(&e_data->work);

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

int kvm_host_pmu_init(void)
{
	init_host_pmu(&host_pmu);

	return perf_pmu_register(&host_pmu.pmu, "kvm", -1);
}

void kvm_host_pmu_teardown(void)
{
	perf_pmu_unregister(&host_pmu.pmu);
}
