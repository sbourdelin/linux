/*
 * Nest Performance Monitor counter support.
 *
 * Copyright (C) 2016 Madhavan Srinivasan, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include <asm/nest-pmu.h>

struct perchip_nest_info nest_perchip_info[NEST_MAX_CHIPS];
struct nest_pmu *per_nest_pmu_arr[NEST_MAX_PMUS];
static cpumask_t nest_pmu_cpu_mask;

PMU_FORMAT_ATTR(event, "config:0-20");
static struct attribute *nest_format_attrs[] = {
	&format_attr_event.attr,
	NULL,
};

static struct attribute_group nest_format_group = {
	.name = "format",
	.attrs = nest_format_attrs,
};

static ssize_t nest_pmu_cpumask_get_attr(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return cpumap_print_to_pagebuf(true, buf, &nest_pmu_cpu_mask);
}

static DEVICE_ATTR(cpumask, S_IRUGO, nest_pmu_cpumask_get_attr, NULL);

static struct attribute *nest_pmu_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static struct attribute_group nest_pmu_cpumask_attr_group = {
	.attrs = nest_pmu_cpumask_attrs,
};

static void nest_init(int *loc)
{
	int rc;

	rc = opal_nest_counters_control(
				NEST_MODE_PRODUCTION, NEST_ENGINE_START, 0, 0);
	if (rc)
		loc[smp_processor_id()] = 1;
}

static void nest_change_cpu_context(int old_cpu, int new_cpu)
{
	int i;

	for (i = 0;
		(per_nest_pmu_arr[i] != NULL) && (i < NEST_MAX_PMUS); i++)
		perf_pmu_migrate_context(&per_nest_pmu_arr[i]->pmu,
							old_cpu, new_cpu);
}

static void nest_exit_cpu(int cpu)
{
	int nid, target = -1;
	struct cpumask *l_cpumask;

	/*
	 * Check in the designated list for this cpu. Dont bother
	 * if not one of them.
	 */
	if (!cpumask_test_and_clear_cpu(cpu, &nest_pmu_cpu_mask))
		return;

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
		cpumask_set_cpu(target, &nest_pmu_cpu_mask);
		nest_change_cpu_context(cpu, target);
	}
}

static void nest_init_cpu(int cpu)
{
	int nid, fcpu, ncpu;
	struct cpumask *l_cpumask, tmp_mask;

	nid = cpu_to_node(cpu);
	l_cpumask = cpumask_of_node(nid);

	if (!cpumask_and(&tmp_mask, l_cpumask, &nest_pmu_cpu_mask)) {
		cpumask_set_cpu(cpu, &nest_pmu_cpu_mask);
		return;
	}

	fcpu = cpumask_first(l_cpumask);
	ncpu = cpumask_next(cpu, l_cpumask);
	if (cpu == fcpu) {
		if (cpumask_test_and_clear_cpu(ncpu, &nest_pmu_cpu_mask)) {
			cpumask_set_cpu(cpu, &nest_pmu_cpu_mask);
			nest_change_cpu_context(ncpu, cpu);
		}
	}
}

static int nest_pmu_cpu_notifier(struct notifier_block *self,
					unsigned long action, void *hcpu)
{
	long cpu = (long)hcpu;

	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_ONLINE:
		nest_init_cpu(cpu);
		break;
	case CPU_DOWN_PREPARE:
		nest_exit_cpu(cpu);
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block nest_pmu_cpu_nb = {
	.notifier_call  = nest_pmu_cpu_notifier,
	.priority       = CPU_PRI_PERF + 1,
};

static int nest_pmu_cpumask_init(void)
{
	const struct cpumask *l_cpumask;
	int cpu, nid;
	int *cpus_opal_rc;

	if (!cpumask_empty(&nest_pmu_cpu_mask))
		return 0;

	cpu_notifier_register_begin();

	/*
	 * Nest PMUs are per-chip counters. So designate a cpu
	 * from each chip for counter collection.
	 */
	for_each_online_node(nid) {
		l_cpumask = cpumask_of_node(nid);

		/* designate first online cpu in this node */
		cpu = cpumask_first(l_cpumask);
		cpumask_set_cpu(cpu, &nest_pmu_cpu_mask);
	}

	/*
	 * Memory for OPAL call return value.
	 */
	cpus_opal_rc = kzalloc((sizeof(int) * nr_cpu_ids), GFP_KERNEL);
	if (!cpus_opal_rc)
		goto fail;

	/* Initialize Nest PMUs in each node using designated cpus */
	on_each_cpu_mask(&nest_pmu_cpu_mask, (smp_call_func_t)nest_init,
						(void *)cpus_opal_rc, 1);

	/* Check return value array for any OPAL call failure */
	for_each_cpu(cpu, &nest_pmu_cpu_mask) {
		if (cpus_opal_rc[cpu])
			goto fail;
	}


	__register_cpu_notifier(&nest_pmu_cpu_nb);

	cpu_notifier_register_done();
	return 0;

fail:
	cpu_notifier_register_done();
	return -ENODEV;
}

static int nest_event_init(struct perf_event *event)
{
	int chip_id;
	u32 config = event->attr.config;
	struct perchip_nest_info *pcni;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/* Sampling not supported yet */
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

	chip_id = topology_physical_package_id(event->cpu);
	pcni = &nest_perchip_info[chip_id];
	event->hw.event_base = pcni->vbase[config/PAGE_SIZE] +
							(config & ~PAGE_MASK );

	return 0;
}

static void nest_read_counter(struct perf_event *event)
{
	u64 *addr, data;

	addr = (u64 *)event->hw.event_base;
	data = __be64_to_cpu(*addr);
	local64_set(&event->hw.prev_count, data);
}

static void nest_perf_event_update(struct perf_event *event)
{
	u64 counter_prev, counter_new, final_count, *addr;

	addr = (u64 *)event->hw.event_base;
	counter_prev = local64_read(&event->hw.prev_count);
	counter_new = __be64_to_cpu(*addr);
	final_count = counter_new - counter_prev;

	local64_set(&event->hw.prev_count, counter_new);
	local64_add(final_count, &event->count);
}

static void nest_event_start(struct perf_event *event, int flags)
{
	nest_read_counter(event);
}

static void nest_event_stop(struct perf_event *event, int flags)
{
	if (flags & PERF_EF_UPDATE)
		nest_perf_event_update(event);
}

static int nest_event_add(struct perf_event *event, int flags)
{
       if (flags & PERF_EF_START)
               nest_event_start(event, flags);

       return 0;
}

/*
 * Populate pmu ops in the structure
 */
static int update_pmu_ops(struct nest_pmu *pmu)
{
	if (!pmu)
		return -EINVAL;

	pmu->pmu.task_ctx_nr = perf_invalid_context;
	pmu->pmu.event_init = nest_event_init;
	pmu->pmu.add = nest_event_add;
	pmu->pmu.del = nest_event_stop;
	pmu->pmu.start = nest_event_start;
	pmu->pmu.stop = nest_event_stop;
	pmu->pmu.read = nest_perf_event_update;
	pmu->attr_groups[1] = &nest_format_group;
	pmu->attr_groups[2] = &nest_pmu_cpumask_attr_group;
	pmu->pmu.attr_groups = pmu->attr_groups;

	return 0;
}
/*
 * Populate event name and string in attribute
 */
static struct attribute *dev_str_attr(const char *name, const char *str)
{
	struct perf_pmu_events_attr *attr;

	attr = kzalloc(sizeof(*attr), GFP_KERNEL);

	sysfs_attr_init(&attr->attr.attr);

	attr->event_str = str;
	attr->attr.attr.name = name;
	attr->attr.attr.mode = 0444;
	attr->attr.show = perf_event_sysfs_show;

	return &attr->attr.attr;
}

static int update_events_in_group(
	struct nest_ima_events *nest_events, int idx, struct nest_pmu *pmu)
{
	struct attribute_group *attr_group;
	struct attribute **attrs;
	int i;

	/* Allocate memory for attribute group */
	attr_group = kzalloc(sizeof(*attr_group), GFP_KERNEL);
	if (!attr_group)
		return -ENOMEM;

	/* Allocate memory for attributes */
	attrs = kzalloc((sizeof(struct attribute *) * (idx + 1)), GFP_KERNEL);
	if (!attrs)
		return -ENOMEM;

	attr_group->name = "events";
	attr_group->attrs = attrs;
	for (i = 0; i < idx; i++, nest_events++)
		attrs[i] = dev_str_attr((char *)nest_events->ev_name,
						(char *)nest_events->ev_value);

	pmu->attr_groups[0] = attr_group;
	return 0;
}

int init_nest_pmu(struct nest_ima_events *nest_events,
					int idx, struct nest_pmu *pmu_ptr)
{
	int ret = -ENODEV;

	/* Add cpumask and register for hotplug notification */
	if (nest_pmu_cpumask_init())
		return ret;

	update_events_in_group(nest_events, idx, pmu_ptr);
	update_pmu_ops(pmu_ptr);

	ret = perf_pmu_register(&pmu_ptr->pmu, pmu_ptr->pmu.name, -1);
	if (ret)
		return ret;

	pr_info("%s performance monitor hardware support registered\n",
							pmu_ptr->pmu.name);
	return 0;
}

