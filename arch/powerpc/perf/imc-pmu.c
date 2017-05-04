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

/*
 * The wrapper function is provided here, since we will have reserve
 * and release lock for imc_event_start() in the following patch.
 * Same in case of imc_event_stop().
 */
static void nest_imc_event_start(struct perf_event *event, int flags)
{
	imc_event_start(event, flags);
}

static void nest_imc_event_stop(struct perf_event *event, int flags)
{
	imc_event_stop(event, flags);
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
 */
int __init init_imc_pmu(struct imc_events *events, int idx,
		 struct imc_pmu *pmu_ptr)
{
	int ret = -ENODEV;

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
