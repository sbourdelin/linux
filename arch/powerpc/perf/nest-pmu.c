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

	update_events_in_group(nest_events, idx, pmu_ptr);
	return 0;
}

