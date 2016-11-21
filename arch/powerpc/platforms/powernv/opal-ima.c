/*
 * OPAL IMA interface detection driver
 * Supported on POWERNV platform
 *
 * Copyright  (C) 2016 Madhavan Srinivasan, IBM Corporation.
 *	       (C) 2016 Hemant K Shaw, IBM Corporation.
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
#include <asm/cputable.h>
#include <asm/ima-pmu.h>

extern struct perchip_nest_info nest_perchip_info[IMA_MAX_CHIPS];
extern struct ima_pmu *per_nest_pmu_arr[IMA_MAX_PMUS];

extern int init_ima_pmu(struct ima_events *events,
			int idx, struct ima_pmu *pmu_ptr);

static int ima_event_info(char *name, struct ima_events *events)
{
	char *buf;

	/* memory for content */
	buf = kzalloc(IMA_MAX_PMU_NAME_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	events->ev_name = name;
	events->ev_value = buf;
	return 0;
}

static int ima_event_info_str(struct property *pp, char *name,
			       struct ima_events *events)
{
	int ret;

	ret = ima_event_info(name, events);
	if (ret)
		return ret;

	if (!pp->value || (strnlen(pp->value, pp->length) == pp->length) ||
	   (pp->length > IMA_MAX_PMU_NAME_LEN))
		return -EINVAL;
	strncpy(events->ev_value, (const char *)pp->value, pp->length);

	return 0;
}

static int ima_event_info_val(char *name, u32 val,
			       struct ima_events *events)
{
	int ret;

	ret = ima_event_info(name, events);
	if (ret)
		return ret;
	sprintf(events->ev_value, "event=0x%x", val);

	return 0;
}

static int set_event_property(struct property *pp, char *event_prop,
			      struct ima_events *events, char *ev_name)
{
	char *buf;
	int ret;

	buf = kzalloc(IMA_MAX_PMU_NAME_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	sprintf(buf, "%s.%s", ev_name, event_prop);
	ret = ima_event_info_str(pp, buf, events);
	if (ret) {
		kfree(events->ev_name);
		kfree(events->ev_value);
	}

	return ret;
}

/*
 * ima_events_node_parser: Parse the event node "dev" and assign the parsed
 *                         information to event "events".
 *
 * Parses the "reg" property of this event. "reg" gives us the event offset.
 * Also, parse the "scale" and "unit" properties, if any.
 */
static int ima_events_node_parser(struct device_node *dev,
				  struct ima_events *events,
				  struct property *event_scale,
				  struct property *event_unit)
{
	struct property *name, *pp;
	char *ev_name;
	u32 val;
	int idx = 0, ret;

	if (!dev)
		return -EINVAL;

	/*
	 * Loop through each property of an event node
	 */
	name = of_find_property(dev, "event-name", NULL);
	if (!name)
		return -ENODEV;

	if (!name->value ||
	  (strnlen(name->value, name->length) == name->length) ||
	  (name->length > IMA_MAX_PMU_NAME_LEN))
		return -EINVAL;

	ev_name = kzalloc(IMA_MAX_PMU_NAME_LEN, GFP_KERNEL);
	if (!ev_name)
		return -ENOMEM;

	strncpy(ev_name, name->value, name->length);

	/*
	 * Parse each property of this event node "dev". Property "reg" has
	 * the offset which is assigned to the event name. Other properties
	 * like "scale" and "unit" are assigned to event.scale and event.unit
	 * accordingly.
	 */
	for_each_property_of_node(dev, pp) {
		/*
		 * If there is an issue in parsing a single property of
		 * this event, we just clean up the buffers, but we still
		 * continue to parse.
		 */
		if (strncmp(pp->name, "reg", 3) == 0) {
			of_property_read_u32(dev, pp->name, &val);
			ret = ima_event_info_val(ev_name, val, &events[idx]);
			if (ret) {
				kfree(events[idx].ev_name);
				kfree(events[idx].ev_value);
				continue;
			}
			/*
			 * If the common scale and unit properties available,
			 * then, assign them to this event
			 */
			if (event_scale) {
				idx++;
				ret = set_event_property(event_scale, "scale",
							 &events[idx],
							 ev_name);
				if (ret)
					continue;
				idx++;
			}
			if (event_unit) {
				ret = set_event_property(event_unit, "unit",
							 &events[idx],
							 ev_name);
				if (ret)
					continue;
			}
			idx++;
		} else if (strncmp(pp->name, "unit", 4) == 0) {
			ret = set_event_property(pp, "unit", &events[idx],
						 ev_name);
			if (ret)
				continue;
			idx++;
		} else if (strncmp(pp->name, "scale", 5) == 0) {
			ret = set_event_property(pp, "scale", &events[idx],
						 ev_name);
			if (ret)
				continue;
			idx++;
		}
	}

	return idx;
}

/*
 * ima_get_domain : Returns the domain for pmu "pmu_dev".
 */
int ima_get_domain(struct device_node *pmu_dev)
{
	if (of_device_is_compatible(pmu_dev, IMA_DTB_NEST_COMPAT))
		return IMA_DOMAIN_NEST;
	else
		return UNKNOWN_DOMAIN;
}

/*
 * get_nr_children : Returns the number of children for a pmu device node.
 */
static int get_nr_children(struct device_node *pmu_node)
{
	struct device_node *child;
	int i = 0;

	for_each_child_of_node(pmu_node, child)
		i++;
	return i;
}

/*
 * ima_free_events : Cleanup the "events" list having "nr_entries" entries.
 */
static void ima_free_events(struct ima_events *events, int nr_entries)
{
	int i;

	/* Nothing to clean, return */
	if (!events)
		return;
	for (i = 0; i < nr_entries; i++) {
		kfree(events[i].ev_name);
		kfree(events[i].ev_value);
	}

	kfree(events);
}

/*
 * ima_pmu_create : Takes the parent device which is the pmu unit and a
 *                  pmu_index as the inputs.
 * Allocates memory for the pmu, sets up its domain (NEST or CORE), and
 * allocates memory for the events supported by this pmu. Assigns a name for
 * the pmu. Calls ima_events_node_parser() to setup the individual events.
 * If everything goes fine, it calls, init_ima_pmu() to setup the pmu device
 * and register it.
 */
static int ima_pmu_create(struct device_node *parent, int pmu_index)
{
	struct device_node *ev_node;
	struct ima_events *events;
	struct ima_pmu *pmu_ptr;
	struct property *pp, *scale_pp, *unit_pp;
	char *buf;
	int idx = 0, ret, nr_children = 0;

	if (!parent)
		return -EINVAL;

	/* memory for pmu */
	pmu_ptr = kzalloc(sizeof(struct ima_pmu), GFP_KERNEL);
	if (!pmu_ptr)
		return -ENOMEM;

	pmu_ptr->domain = ima_get_domain(parent);
	if (pmu_ptr->domain == UNKNOWN_DOMAIN)
		goto free_pmu;

	/* Needed for hotplug/migration */
	per_nest_pmu_arr[pmu_index] = pmu_ptr;

	/*
	 * Get the maximum no. of events in this node.
	 * Multiply by 3 to account for .scale and .unit properties
	 * This number suggests the amount of memory needed to setup the
	 * events for this pmu.
	 */
	nr_children = get_nr_children(parent) * 3;

	/* memory for pmu events */
	events = kzalloc((sizeof(struct ima_events) * nr_children),
			 GFP_KERNEL);
	if (!events) {
		ret = -ENOMEM;
		goto free_pmu;
	}

	pp = of_find_property(parent, "name", NULL);
	if (!pp) {
		ret = -ENODEV;
		goto free_events;
	}

	if (!pp->value ||
	  (strnlen(pp->value, pp->length) == pp->length) ||
	    (pp->length > IMA_MAX_PMU_NAME_LEN)) {
		ret = -EINVAL;
		goto free_events;
	}

	buf = kzalloc(IMA_MAX_PMU_NAME_LEN, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto free_events;
	}

	/* Save the name to register it later */
	sprintf(buf, "nest_%s", (char *)pp->value);
	pmu_ptr->pmu.name = (char *)buf;

	/*
	 * Check if there is a common "scale" and "unit" properties inside
	 * the PMU node for all the events supported by this PMU.
	 */
	scale_pp = of_find_property(parent, "scale", NULL);
	unit_pp = of_find_property(parent, "unit", NULL);

	/* Loop through event nodes */
	for_each_child_of_node(parent, ev_node) {
		ret = ima_events_node_parser(ev_node, &events[idx], scale_pp,
					     unit_pp);
		if (ret < 0) {
			/* Unable to parse this event */
			if (ret == -ENOMEM)
				goto free_events;
			continue;
		}

		/*
		 * ima_event_node_parser will return number of
		 * event entries created for this. This could include
		 * event scale and unit files also.
		 */
		idx += ret;
	}

	ret = init_ima_pmu(events, idx, pmu_ptr);
	if (ret) {
		pr_err("IMA PMU %s Register failed\n", pmu_ptr->pmu.name);
		goto free_events;
	}
	return 0;

free_events:
	ima_free_events(events, idx);
free_pmu:
	kfree(pmu_ptr);
	return ret;
}

/*
 * ima_pmu_setup : Setup the IMA PMUs (children of "parent").
 */
static void ima_pmu_setup(struct device_node *parent)
{
	struct device_node *child;
	int pmu_count = 0, rc = 0;

	if (!parent)
		return;

	/* Setup all the IMA pmus */
	for_each_child_of_node(parent, child) {
		ima_pmu_create(child, pmu_count);
		if (rc)
			return;
		pmu_count++;
	}
}

static int opal_ima_counters_probe(struct platform_device *pdev)
{
	struct device_node *child, *ima_dev, *rm_node = NULL;
	struct perchip_nest_info *pcni;
	u32 reg[4], pages, nest_offset, nest_size, idx;
	int i = 0;
	const char *node_name;

	if (!pdev || !pdev->dev.of_node)
		return -ENODEV;

	ima_dev = pdev->dev.of_node;

	/*
	 * nest_offset : where the nest-counters' data start.
	 * size : size of the entire nest-counters region
	 */
	if (of_property_read_u32(ima_dev, "ima-nest-offset", &nest_offset))
		goto err;
	if (of_property_read_u32(ima_dev, "ima-nest-size", &nest_size))
		goto err;

	/* Find the "homer region" for each chip */
	rm_node = of_find_node_by_path("/reserved-memory");
	if (!rm_node)
		goto err;

	for_each_child_of_node(rm_node, child) {
		if (of_property_read_string_index(child, "name", 0,
						  &node_name))
			continue;
		if (strncmp("ibm,homer-image", node_name,
			    strlen("ibm,homer-image")))
			continue;

		/* Get the chip id to which the above homer region belongs to */
		if (of_property_read_u32(child, "ibm,chip-id", &idx))
			goto err;

		/* reg property will have four u32 cells. */
		if (of_property_read_u32_array(child, "reg", reg, 4))
			goto err;

		pcni = &nest_perchip_info[idx];

		/* Fetch the homer region base address */
		pcni->pbase = reg[0];
		pcni->pbase = pcni->pbase << 32 | reg[1];
		/* Add the nest IMA Base offset */
		pcni->pbase = pcni->pbase + nest_offset;
		/* Fetch the size of the homer region */
		pcni->size = nest_size;

		do {
			pages = PAGE_SIZE * i;
			pcni->vbase[i++] = (u64)phys_to_virt(pcni->pbase +
							     pages);
		} while (i < (pcni->size / PAGE_SIZE));
	}

	ima_pmu_setup(ima_dev);
	return 0;
err:
	return -ENODEV;
}

static const struct of_device_id opal_ima_match[] = {
	{ .compatible = IMA_DTB_COMPAT },
	{},
};

static struct platform_driver opal_ima_driver = {
	.driver = {
		.name = "opal-ima-counters",
		.of_match_table = opal_ima_match,
	},
	.probe = opal_ima_counters_probe,
};

MODULE_DEVICE_TABLE(of, opal_ima_match);
module_platform_driver(opal_ima_driver);
MODULE_DESCRIPTION("PowerNV OPAL IMA driver");
MODULE_LICENSE("GPL");
