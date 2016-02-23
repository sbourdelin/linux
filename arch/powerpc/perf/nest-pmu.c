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

PMU_FORMAT_ATTR(event, "config:0-20");
static struct attribute *nest_format_attrs[] = {
	&format_attr_event.attr,
	NULL,
};

static struct attribute_group nest_format_group = {
	.name = "format",
	.attrs = nest_format_attrs,
};

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

	update_events_in_group(nest_events, idx, pmu_ptr);
	update_pmu_ops(pmu_ptr);

	ret = perf_pmu_register(&pmu_ptr->pmu, pmu_ptr->pmu.name, -1);
	if (ret)
		return ret;

	pr_info("%s performance monitor hardware support registered\n",
							pmu_ptr->pmu.name);
	return 0;
}

