/*
 * OPAL Nest detection interface driver
 * Supported on POWERNV platform
 *
 * Copyright IBM Corporation 2016
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <asm/opal.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/nest-pmu.h>

extern struct perchip_nest_info nest_perchip_info[NEST_MAX_CHIPS];
extern struct nest_pmu *per_nest_pmu_arr[NEST_MAX_PMUS];
extern int init_nest_pmu(struct nest_ima_events *nest_events,
				int idx, struct nest_pmu *pmu_ptr);

static int nest_event_info(char *name, struct nest_ima_events *nest_events)
{
	char *buf;

	/* memory for content */
	buf = kzalloc(NEST_MAX_PMU_NAME_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	nest_events->ev_name = name;
	nest_events->ev_value = buf;
	return 0;
}

static int nest_event_info_str(struct property *pp, char *name,
					struct nest_ima_events *nest_events)
{
	if (nest_event_info(name, nest_events))
		return -ENOMEM;

	if (!pp->value || (strnlen(pp->value, pp->length) == pp->length) ||
	   (pp->length > NEST_MAX_PMU_NAME_LEN))
		return -EINVAL;

	strncpy(nest_events->ev_value, (const char *)pp->value, pp->length);
	return 0;
}

static int nest_event_info_val(char *name, u32 val,
					struct nest_ima_events *nest_events)
{
	if (nest_event_info(name, nest_events))
		return -ENOMEM;

	sprintf(nest_events->ev_value, "event=0x%x", val);
	return 0;
}

static int nest_events_node_parser(struct device_node *dev,
					struct nest_ima_events *nest_events)
{
	struct property *name, *pp, *id;
	char *buf, *start, *ev_name;
	u32 val;
	int idx = 0, ret;

	if (!dev)
		return -EINVAL;

	/*
	* Loop through each property
	*/
	name = of_find_property(dev, "name", NULL);
	if (!name) {
		printk(KERN_INFO "No property by name\n");
		return -1;
	}

	if (!name->value ||
	  (strnlen(name->value, name->length) == name->length) ||
	  (name->length > NEST_MAX_PMU_NAME_LEN))
		return -EINVAL;

	ev_name = kzalloc(NEST_MAX_PMU_NAME_LEN, GFP_KERNEL);
	if (!ev_name)
		return -ENOMEM;

	/* Now that we got the event name, look for id */
	id = of_find_property(dev, "id", NULL);
	if (!id) {
		strncpy(ev_name, name->value, (int)strlen(name->value));
		printk(KERN_INFO "No property by id = %s\n", ev_name);
	} else {
		if (!id->value ||
		  (strnlen(id->value, id->length) == id->length) ||
		  (id->length > NEST_MAX_PMU_NAME_LEN))
			return -EINVAL;

		of_property_read_u32(dev, id->name, &val);
		sprintf(ev_name, "%s_%x", (char *)name->value, val);
	}

	for_each_property_of_node(dev, pp) {
		start = pp->name;

		/* Skip these, we don't need it */
		if (!strcmp(pp->name, "phandle") ||
		   !strcmp(pp->name, "linux,phandle") ||
		   !strcmp(pp->name, "name"))
			continue;

		if (strncmp(pp->name, "reg", 3) == 0) {
			of_property_read_u32(dev, pp->name, &val);
			ret = nest_event_info_val(ev_name, val, &nest_events[idx]);
			idx++;
		} else if (strncmp(pp->name, "unit", 4) == 0) {
			buf = kzalloc(NEST_MAX_PMU_NAME_LEN, GFP_KERNEL);
			if (!buf)
				return -ENOMEM;
			sprintf(buf,"%s.unit", ev_name);
			ret = nest_event_info_str(pp, buf, &nest_events[idx]);
			idx++;
		} else if (strncmp(pp->name, "scale", 5) == 0) {
			buf = kzalloc(NEST_MAX_PMU_NAME_LEN, GFP_KERNEL);
			if (!buf)
				return -ENOMEM;
			sprintf(buf,"%s.scale", ev_name);
			ret = nest_event_info_str(pp, buf, &nest_events[idx]);
			idx++;
		}

		if (ret)
			return ret;
	}

	return idx;
}

static int nest_pmu_create(struct device_node *parent, int pmu_index)
{
	struct device_node *ev_node;
	struct nest_ima_events *nest_events;
	struct nest_pmu *pmu_ptr;
	struct property *pp;
	char *buf;
	int idx = 0, ret;

	if (!parent)
		return -EINVAL;

	/* memory for nest pmus */
	pmu_ptr = kzalloc(sizeof(struct nest_pmu), GFP_KERNEL);
	if (!pmu_ptr)
		return -ENOMEM;

	/* Needed for hotplug/migration */
	per_nest_pmu_arr[pmu_index] = pmu_ptr;

	/* memory for nest pmu events */
	nest_events = kzalloc((sizeof(struct nest_ima_events) *
					NEST_MAX_EVENTS_SUPPORTED), GFP_KERNEL);
	if (!nest_events)
		return -ENOMEM;

	pp = of_find_property(parent, "name", NULL);
	if (!pp) {
		printk(KERN_INFO "No property by name\n");
		return -1;
	}

	if (!pp->value ||
	  (strnlen(pp->value, pp->length) == pp->length) ||
	  (pp->length > NEST_MAX_PMU_NAME_LEN))
		return -EINVAL;

	buf = kzalloc(NEST_MAX_PMU_NAME_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* Save the name to register it later */
	sprintf(buf, "nest_%s", (char *)pp->value);
	pmu_ptr->pmu.name = (char *)buf;

	/* Loop through event nodes */
	for_each_child_of_node(parent, ev_node) {
		ret = nest_events_node_parser(ev_node, &nest_events[idx]);
		if (ret < 0)
			return -1;

		/*
		 * nest_event_node_parser will return number of
		 * event entried created for this. This could include
		 * event scale and unit files also.
		 */
		idx += ret;
	}

	ret = init_nest_pmu(nest_events, idx, pmu_ptr);
	if (ret) {
		pr_err("Nest PMU %s Register failed\n", pmu_ptr->pmu.name);
		return ret;
	}

	return 0;
}

static int opal_nest_counters_probe(struct platform_device *pdev)
{
	struct device_node *child, *parent;
	struct perchip_nest_info *pcni;
	u32 idx, range[4], pages;
	int rc=0, i=0, pmu_count=0;

	if (!pdev || !pdev->dev.of_node)
		return -ENODEV;

	/*
	 * "nest-counters" folder contains two things,
	 * a) per-chip reserved memory region for Nest PMU Counter data
	 * b) Support Nest PMU units and their event files
	 */
	parent = pdev->dev.of_node;
	for_each_child_of_node(parent, child) {
		if (of_property_read_u32(child, "ibm,chip-id", &idx)) {
			pr_err("opal-nest-counters: device %s missing property\n",
							child->full_name);
			return -ENODEV;
		}

		/*
		 *"ranges" property will have four u32 cells.
		 */
		if (of_property_read_u32_array(child, "ranges", range, 4)) {
			pr_err("opal-nest-counters: range property value wrong\n");
			return -1;
		}

		pcni = &nest_perchip_info[idx];
		pcni->pbase = range[1];
		pcni->pbase = pcni->pbase << 32 | range[2];
		pcni->size = range[3];

		do
		{
			pages = PAGE_SIZE * i;
			pcni->vbase[i++] = (u64)phys_to_virt(pcni->pbase + pages);
		} while( i < (pcni->size/PAGE_SIZE));
	}

	parent = of_get_next_child(pdev->dev.of_node, NULL);
	for_each_child_of_node(parent, child) {
		rc = nest_pmu_create(child, pmu_count++);
		if (rc)
			return rc;
	}

	return rc;
}

static const struct of_device_id opal_nest_match[] = {
	{ .compatible = "ibm,opal-in-memory-counters" },
	{ },
};

static struct platform_driver opal_nest_driver = {
	.driver = {
		.name           = "opal-nest-counters",
		.of_match_table = opal_nest_match,
	},
	.probe  = opal_nest_counters_probe,
};

MODULE_DEVICE_TABLE(of, opal_nest_match);
module_platform_driver(opal_nest_driver);
MODULE_DESCRIPTION("PowerNV OPAL Nest Counters driver");
MODULE_LICENSE("GPL");
