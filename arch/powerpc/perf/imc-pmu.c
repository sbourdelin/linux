/*
 * Nest Performance Monitor counter support.
 *
 * Copyright (C) 2017 Madhavan Srinivasan, IBM Corporation.
 *           (C) 2017 Anju T Sudhakar, IBM Corporation.
 *           (C) 2017 Hemant K Shaw, IBM Corporation.
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */
#include <linux/perf_event.h>
#include <linux/slab.h>
#include <asm/opal.h>
#include <asm/imc-pmu.h>
#include <asm/cputhreads.h>
#include <asm/smp.h>
#include <linux/string.h>

struct perchip_nest_info nest_perchip_info[IMC_MAX_CHIPS];
struct imc_pmu *per_nest_pmu_arr[IMC_MAX_PMUS];
static cpumask_t nest_imc_cpumask;

static atomic_t nest_events;
/* Used to avoid races in calling enable/disable nest-pmu units*/
static DEFINE_MUTEX(imc_nest_reserve);

/* Needed for sanity check */
extern u64 nest_max_offset;

PMU_FORMAT_ATTR(event, "config:0-20");
static struct attribute *imc_format_attrs[] = {
	&format_attr_event.attr,
	NULL,
};

static struct attribute_group imc_format_group = {
	.name = "format",
	.attrs = imc_format_attrs,
};

/* Get the cpumask printed to a buffer "buf" */
static ssize_t imc_pmu_cpumask_get_attr(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	cpumask_t *active_mask;

	active_mask = &nest_imc_cpumask;
	return cpumap_print_to_pagebuf(true, buf, active_mask);
}

static DEVICE_ATTR(cpumask, S_IRUGO, imc_pmu_cpumask_get_attr, NULL);

static struct attribute *imc_pmu_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static struct attribute_group imc_pmu_cpumask_attr_group = {
	.attrs = imc_pmu_cpumask_attrs,
};

/*
 * nest_init : Initializes the nest imc engine for the current chip.
 * by default the nest engine is disabled.
 */
static void nest_init(int *cpu_opal_rc)
{
	int rc;

	/*
	 * OPAL figures out which CPU to start based on the CPU that is
	 * currently running when we call into OPAL
	 */
	rc = opal_imc_counters_stop(OPAL_IMC_COUNTERS_NEST);
	if (rc)
		cpu_opal_rc[smp_processor_id()] = 1;
}

static void nest_change_cpu_context(int old_cpu, int new_cpu)
{
	int i;

	for (i = 0;
	     (per_nest_pmu_arr[i] != NULL) && (i < IMC_MAX_PMUS); i++)
		perf_pmu_migrate_context(&per_nest_pmu_arr[i]->pmu,
							old_cpu, new_cpu);
}

static int ppc_nest_imc_cpu_online(unsigned int cpu)
{
	int nid;
	const struct cpumask *l_cpumask;
	struct cpumask tmp_mask;

	/* Find the cpumask of this node */
	nid = cpu_to_node(cpu);
	l_cpumask = cpumask_of_node(nid);

	/*
	 * If any of the cpu from this node is already present in the mask,
	 * just return, if not, then set this cpu in the mask.
	 */
	if (!cpumask_and(&tmp_mask, l_cpumask, &nest_imc_cpumask)) {
		cpumask_set_cpu(cpu, &nest_imc_cpumask);
		nest_change_cpu_context(-1, cpu);
		return 0;
	}

	return 0;
}

static int ppc_nest_imc_cpu_offline(unsigned int cpu)
{
	int nid, target = -1;
	const struct cpumask *l_cpumask;

	/*
	 * Check in the designated list for this cpu. Dont bother
	 * if not one of them.
	 */
	if (!cpumask_test_and_clear_cpu(cpu, &nest_imc_cpumask))
		return 0;

	/*
	 * Now that this cpu is one of the designated,
	 * find a next cpu a) which is online and b) in same chip.
	 */
	nid = cpu_to_node(cpu);
	l_cpumask = cpumask_of_node(nid);
	target = cpumask_next(cpu, l_cpumask);

	/*
	 * Update the cpumask with the target cpu and
	 * migrate the context if needed
	 */
	if (target >= 0 && target <= nr_cpu_ids) {
		cpumask_set_cpu(target, &nest_imc_cpumask);
		nest_change_cpu_context(cpu, target);
	}
	return 0;
}

static int nest_pmu_cpumask_init(void)
{
	const struct cpumask *l_cpumask;
	int cpu, nid;
	int *cpus_opal_rc;

	if (!cpumask_empty(&nest_imc_cpumask))
		return 0;

	/*
	 * Memory for OPAL call return value.
	 */
	cpus_opal_rc = kzalloc((sizeof(int) * nr_cpu_ids), GFP_KERNEL);
	if (!cpus_opal_rc)
		goto fail;

	/*
	 * Nest PMUs are per-chip counters. So designate a cpu
	 * from each chip for counter collection.
	 */
	for_each_online_node(nid) {
		l_cpumask = cpumask_of_node(nid);

		/* designate first online cpu in this node */
		cpu = cpumask_first(l_cpumask);
		cpumask_set_cpu(cpu, &nest_imc_cpumask);
	}

	/* Initialize Nest PMUs in each node using designated cpus */
	on_each_cpu_mask(&nest_imc_cpumask, (smp_call_func_t)nest_init,
						(void *)cpus_opal_rc, 1);

	/* Check return value array for any OPAL call failure */
	for_each_cpu(cpu, &nest_imc_cpumask) {
		if (cpus_opal_rc[cpu])
			goto fail;
	}

	cpuhp_setup_state(CPUHP_AP_PERF_POWERPC_NEST_ONLINE,
			  "POWER_NEST_IMC_ONLINE",
			  ppc_nest_imc_cpu_online,
			  ppc_nest_imc_cpu_offline);

	return 0;

fail:
	if (cpus_opal_rc)
		kfree(cpus_opal_rc);
	return -ENODEV;
}

static int nest_imc_event_init(struct perf_event *event)
{
	int chip_id;
	u32 config = event->attr.config;
	struct perchip_nest_info *pcni;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/* Sampling not supported */
	if (event->hw.sample_period)
		return -EINVAL;

	/* unsupported modes and filters */
	if (event->attr.exclude_user   ||
	    event->attr.exclude_kernel ||
	    event->attr.exclude_hv     ||
	    event->attr.exclude_idle   ||
	    event->attr.exclude_host   ||
	    event->attr.exclude_guest)
		return -EINVAL;

	if (event->cpu < 0)
		return -EINVAL;

	/* Sanity check for config (event offset) */
	if (config > nest_max_offset)
		return -EINVAL;

	chip_id = topology_physical_package_id(event->cpu);
	pcni = &nest_perchip_info[chip_id];

	/*
	 * Memory for Nest HW counter data could be in multiple pages.
	 * Hence check and pick the right event base page for chip with
	 * "chip_id" and add "config" to it".
	 */
	event->hw.event_base = pcni->vbase[config/PAGE_SIZE] +
							(config & ~PAGE_MASK);

	return 0;
}

static void imc_read_counter(struct perf_event *event)
{
	u64 *addr, data;

	/*
	 * In-Memory Collection (IMC) counters are free flowing counters.
	 * So we take a snapshot of the counter value on enable and save it
	 * to calculate the delta at later stage to present the event counter
	 * value.
	 */
	addr = (u64 *)event->hw.event_base;
	data = __be64_to_cpu(READ_ONCE(*addr));
	local64_set(&event->hw.prev_count, data);
}

static void imc_perf_event_update(struct perf_event *event)
{
	u64 counter_prev, counter_new, final_count, *addr;

	addr = (u64 *)event->hw.event_base;
	counter_prev = local64_read(&event->hw.prev_count);
	counter_new = __be64_to_cpu(READ_ONCE(*addr));
	final_count = counter_new - counter_prev;

	/*
	 * Need to update prev_count is that, counter could be
	 * read in a periodic interval from the tool side.
	 */
	local64_set(&event->hw.prev_count, counter_new);
	/* Update the delta to the event count */
	local64_add(final_count, &event->count);
}

static void nest_imc_start(int *cpu_opal_rc)
{
	int rc;

	/* Enable nest engine */
	rc = opal_imc_counters_start(OPAL_IMC_COUNTERS_NEST);
	if (rc)
		cpu_opal_rc[smp_processor_id()] = 1;

}

static int nest_imc_control(int operation)
{
	int *cpus_opal_rc, cpu;

	/*
	 * Memory for OPAL call return value.
	 */
	cpus_opal_rc = kzalloc((sizeof(int) * nr_cpu_ids), GFP_KERNEL);
	if (!cpus_opal_rc)
		return -ENOMEM;
	switch (operation) {

	case	IMC_COUNTER_ENABLE:
			/* Initialize Nest PMUs in each node using designated cpus */
			on_each_cpu_mask(&nest_imc_cpumask, (smp_call_func_t)nest_imc_start,
						(void *)cpus_opal_rc, 1);
			break;
	case	IMC_COUNTER_DISABLE:
			/* Disable the counters */
			on_each_cpu_mask(&nest_imc_cpumask, (smp_call_func_t)nest_init,
						(void *)cpus_opal_rc, 1);
			break;
	default: return -EINVAL;

	}

	/* Check return value array for any OPAL call failure */
	for_each_cpu(cpu, &nest_imc_cpumask) {
		if (cpus_opal_rc[cpu])
			return -ENODEV;
	}
	return 0;
}

static void imc_event_start(struct perf_event *event, int flags)
{
	/*
	 * In Memory Counters are free flowing counters. HW or the microcode
	 * keeps adding to the counter offset in memory. To get event
	 * counter value, we snapshot the value here and we calculate
	 * delta at later point.
	 */
	imc_read_counter(event);
}

static void imc_event_stop(struct perf_event *event, int flags)
{
	/*
	 * Take a snapshot and calculate the delta and update
	 * the event counter values.
	 */
	imc_perf_event_update(event);
}

static void nest_imc_event_start(struct perf_event *event, int flags)
{
	int rc;

	/*
	 * Nest pmu units are enabled only when it is used.
	 * See if this is triggered for the first time.
	 * If yes, take the mutex lock and enable the nest counters.
	 * If not, just increment the count in nest_events.
	 */
	if (atomic_inc_return(&nest_events) == 1) {
		mutex_lock(&imc_nest_reserve);
		rc = nest_imc_control(IMC_COUNTER_ENABLE);
		mutex_unlock(&imc_nest_reserve);
		if (rc)
			pr_err("IMC: Unbale to start the counters\n");
	}
	imc_event_start(event, flags);
}

static void nest_imc_event_stop(struct perf_event *event, int flags)
{
	int rc;

	imc_event_stop(event, flags);
	/*
	 * See if we need to disable the nest PMU.
	 * If no events are currently in use, then we have to take a
	 * mutex to ensure that we don't race with another task doing
	 * enable or disable the nest counters.
	 */
	if (atomic_dec_return(&nest_events) == 0) {
		mutex_lock(&imc_nest_reserve);
		rc = nest_imc_control(IMC_COUNTER_DISABLE);
		mutex_unlock(&imc_nest_reserve);
		if (rc)
			pr_err("IMC: Disable counters failed\n");
	}
}

static int nest_imc_event_add(struct perf_event *event, int flags)
{
	if (flags & PERF_EF_START)
		nest_imc_event_start(event, flags);

	return 0;
}

/* update_pmu_ops : Populate the appropriate operations for "pmu" */
static int update_pmu_ops(struct imc_pmu *pmu)
{
	if (!pmu)
		return -EINVAL;

	pmu->pmu.task_ctx_nr = perf_invalid_context;
	pmu->pmu.event_init = nest_imc_event_init;
	pmu->pmu.add = nest_imc_event_add;
	pmu->pmu.del = nest_imc_event_stop;
	pmu->pmu.start = nest_imc_event_start;
	pmu->pmu.stop = nest_imc_event_stop;
	pmu->pmu.read = imc_perf_event_update;
	pmu->attr_groups[IMC_CPUMASK_ATTR] = &imc_pmu_cpumask_attr_group;
	pmu->attr_groups[IMC_FORMAT_ATTR] = &imc_format_group;
	pmu->pmu.attr_groups = pmu->attr_groups;

	return 0;
}

/* dev_str_attr : Populate event "name" and string "str" in attribute */
static struct attribute *dev_str_attr(const char *name, const char *str)
{
	struct perf_pmu_events_attr *attr;

	attr = kzalloc(sizeof(*attr), GFP_KERNEL);
	if (!attr)
		return NULL;
	sysfs_attr_init(&attr->attr.attr);

	attr->event_str = str;
	attr->attr.attr.name = name;
	attr->attr.attr.mode = 0444;
	attr->attr.show = perf_event_sysfs_show;

	return &attr->attr.attr;
}

/*
 * update_events_in_group: Update the "events" information in an attr_group
 *                         and assign the attr_group to the pmu "pmu".
 */
static int update_events_in_group(struct imc_events *events,
				  int idx, struct imc_pmu *pmu)
{
	struct attribute_group *attr_group;
	struct attribute **attrs;
	int i;

	/* If there is no events for this pmu, just return zero */
	if (!events)
		return 0;

	/* Allocate memory for attribute group */
	attr_group = kzalloc(sizeof(*attr_group), GFP_KERNEL);
	if (!attr_group)
		return -ENOMEM;

	/* Allocate memory for attributes */
	attrs = kzalloc((sizeof(struct attribute *) * (idx + 1)), GFP_KERNEL);
	if (!attrs) {
		kfree(attr_group);
		return -ENOMEM;
	}

	attr_group->name = "events";
	attr_group->attrs = attrs;
	for (i = 0; i < idx; i++, events++) {
		attrs[i] = dev_str_attr((char *)events->ev_name,
					(char *)events->ev_value);
	}

	/* Save the event attribute */
	pmu->attr_groups[IMC_EVENT_ATTR] = attr_group;
	return 0;
}

/*
 * init_imc_pmu : Setup and register the IMC pmu device.
 *
 * @events:	events memory for this pmu.
 * @idx:	number of event entries created.
 * @pmu_ptr:	memory allocated for this pmu.
 *
 * init_imc_pmu() setup the cpu mask information for these pmus and setup
 * the state machine hotplug notifiers as well.
 */
int __init init_imc_pmu(struct imc_events *events, int idx,
		 struct imc_pmu *pmu_ptr)
{
	int ret = -ENODEV;

	/* Add cpumask and register for hotplug notification */
	ret = nest_pmu_cpumask_init();
	if (ret)
		return ret;

	ret = update_events_in_group(events, idx, pmu_ptr);
	if (ret)
		goto err_free;

	ret = update_pmu_ops(pmu_ptr);
	if (ret)
		goto err_free;

	ret = perf_pmu_register(&pmu_ptr->pmu, pmu_ptr->pmu.name, -1);
	if (ret)
		goto err_free;

	pr_info("%s performance monitor hardware support registered\n",
		pmu_ptr->pmu.name);

	return 0;

err_free:
	/* Only free the attr_groups which are dynamically allocated  */
	if (pmu_ptr->attr_groups[IMC_EVENT_ATTR]) {
		if (pmu_ptr->attr_groups[IMC_EVENT_ATTR]->attrs)
			kfree(pmu_ptr->attr_groups[IMC_EVENT_ATTR]->attrs);
		kfree(pmu_ptr->attr_groups[IMC_EVENT_ATTR]);
	}

	return ret;
}
