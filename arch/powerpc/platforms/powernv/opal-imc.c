/*
 * OPAL IMC interface detection driver
 * Supported on POWERNV platform
 *
 * Copyright	(C) 2017 Madhavan Srinivasan, IBM Corporation.
 *		(C) 2017 Anju T Sudhakar, IBM Corporation.
 *		(C) 2017 Hemant K Shaw, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or later version.
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
#include <linux/crash_dump.h>
#include <asm/opal.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/cputable.h>
#include <asm/imc-pmu.h>

u64 nest_max_offset;
u64 core_max_offset;

static int imc_event_prop_update(char *name, struct imc_events *events)
{
	char *buf;

	if (!events || !name)
		return -EINVAL;

	/* memory for content */
	buf = kzalloc(IMC_MAX_NAME_VAL_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	events->ev_name = name;
	events->ev_value = buf;
	return 0;
}

static int imc_event_prop_str(struct property *pp, char *name,
			      struct imc_events *events)
{
	int ret;

	ret = imc_event_prop_update(name, events);
	if (ret)
		return ret;

	if (!pp->value || (strnlen(pp->value, pp->length) == pp->length) ||
	   (pp->length > IMC_MAX_NAME_VAL_LEN))
		return -EINVAL;
	strncpy(events->ev_value, (const char *)pp->value, pp->length);

	return 0;
}

static int imc_event_prop_val(char *name, u32 val,
			      struct imc_events *events)
{
	int ret;

	ret = imc_event_prop_update(name, events);
	if (ret)
		return ret;
	snprintf(events->ev_value, IMC_MAX_NAME_VAL_LEN, "event=0x%x", val);

	return 0;
}

static int set_event_property(struct property *pp, char *event_prop,
			      struct imc_events *events, char *ev_name)
{
	char *buf;
	int ret;

	buf = kzalloc(IMC_MAX_NAME_VAL_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	sprintf(buf, "%s.%s", ev_name, event_prop);
	ret = imc_event_prop_str(pp, buf, events);
	if (ret) {
		if (events->ev_name)
			kfree(events->ev_name);
		if (events->ev_value)
			kfree(events->ev_value);
	}
	return ret;
}

/*
 * Updates the maximum offset for an event in the pmu with domain
 * "pmu_domain".
 */
static void update_max_value(u32 value, int pmu_domain)
{
	switch (pmu_domain) {
	case IMC_DOMAIN_NEST:
		if (nest_max_offset < value)
			nest_max_offset = value;
		break;
	case IMC_DOMAIN_CORE:
		if (core_max_offset < value)
			core_max_offset = value;
		break;
	default:
		/* Unknown domain, return */
		return;
	}
}

/*
 * imc_events_node_parser: Parse the event node "dev" and assign the parsed
 *                         information to event "events".
 *
 * Parses the "reg", "scale" and "unit" properties of this event.
 * "reg" gives us the event offset in the counter memory.
 */
static int imc_events_node_parser(struct device_node *dev,
				  struct imc_events *events,
				  struct property *event_scale,
				  struct property *event_unit,
				  struct property *name_prefix,
				  u32 reg, int pmu_domain)
{
	struct property *name, *pp;
	char *ev_name;
	u32 val;
	int idx = 0, ret;

	if (!dev)
		goto fail;

	/* Check for "event-name" property, which is the perfix for event names */
	name = of_find_property(dev, "event-name", NULL);
	if (!name)
		return -ENODEV;

	if (!name->value ||
	  (strnlen(name->value, name->length) == name->length) ||
	  (name->length > IMC_MAX_NAME_VAL_LEN))
		return -EINVAL;

	ev_name = kzalloc(IMC_MAX_NAME_VAL_LEN, GFP_KERNEL);
	if (!ev_name)
		return -ENOMEM;

	snprintf(ev_name, IMC_MAX_NAME_VAL_LEN, "%s%s",
		 (char *)name_prefix->value,
		 (char *)name->value);

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
		 * continue to parse. TODO: This could be rewritten to skip the
		 * entire event node incase of parsing issues, but that can be
		 * done later.
		 */
		if (strncmp(pp->name, "reg", 3) == 0) {
			of_property_read_u32(dev, pp->name, &val);
			val += reg;
			update_max_value(val, pmu_domain);
			ret = imc_event_prop_val(ev_name, val, &events[idx]);
			if (ret) {
				if (events[idx].ev_name)
					kfree(events[idx].ev_name);
				if (events[idx].ev_value)
					kfree(events[idx].ev_value);
				goto fail;
			}
			idx++;
			/*
			 * If the common scale and unit properties available,
			 * then, assign them to this event
			 */
			if (event_scale) {
				ret = set_event_property(event_scale, "scale",
							 &events[idx],
							 ev_name);
				if (ret)
					goto fail;
				idx++;
			}
			if (event_unit) {
				ret = set_event_property(event_unit, "unit",
							 &events[idx],
							 ev_name);
				if (ret)
					goto fail;
				idx++;
			}
		} else if (strncmp(pp->name, "unit", 4) == 0) {
			/*
			 * The event's unit and scale properties can override the
			 * PMU's event and scale properties, if present.
			 */
			ret = set_event_property(pp, "unit", &events[idx],
						 ev_name);
			if (ret)
				goto fail;
			idx++;
		} else if (strncmp(pp->name, "scale", 5) == 0) {
			ret = set_event_property(pp, "scale", &events[idx],
						 ev_name);
			if (ret)
				goto fail;
			idx++;
		}
	}

	return idx;
fail:
	return -EINVAL;
}

/*
 * get_nr_children : Returns the number of events(along with scale and unit)
 * 		     for a pmu device node.
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
 * imc_free_events : Cleanup the "events" list having "nr_entries" entries.
 */
static void imc_free_events(struct imc_events *events, int nr_entries)
{
	int i;

	/* Nothing to clean, return */
	if (!events)
		return;

	for (i = 0; i < nr_entries; i++) {
		if (events[i].ev_name)
			kfree(events[i].ev_name);
		if (events[i].ev_value)
			kfree(events[i].ev_value);
	}

	kfree(events);
}

/*
 * imc_events_setup() : First finds the event node for the pmu and
 *                      gets the number of supported events, then
 * allocates memory for the same and parse the events.
 */
static int imc_events_setup(struct device_node *parent,
					   int pmu_index,
					   struct imc_pmu *pmu_ptr,
					   u32 prop,
					   int *idx)
{
	struct device_node *ev_node = NULL, *dir = NULL;
	u32 reg;
	struct property *scale_pp, *unit_pp, *name_prefix;
	int ret = 0, nr_children = 0;

	/*
	 * Fetch the actual node where the events for this PMU exist.
	 */
	dir = of_find_node_by_phandle(prop);
	if (!dir)
		return -ENODEV;
	/*
	 * Get the maximum no. of events in this node.
	 * Multiply by 3 to account for .scale and .unit properties
	 * This number suggests the amount of memory needed to setup the
	 * events for this pmu.
	 */
	nr_children = get_nr_children(dir) * 3;

	pmu_ptr->events = kzalloc((sizeof(struct imc_events) * nr_children),
			 GFP_KERNEL);
	if (!pmu_ptr->events)
		return -ENOMEM;

	/*
	 * Check if there is a common "scale" and "unit" properties inside
	 * the PMU node for all the events supported by this PMU.
	 */
	scale_pp = of_find_property(parent, "scale", NULL);
	unit_pp = of_find_property(parent, "unit", NULL);

	/*
	 * Get the event-prefix property from the PMU node
	 * which needs to be attached with the event names.
	 */
	name_prefix = of_find_property(parent, "events-prefix", NULL);
	if (!name_prefix)
		goto free_events;

	/*
	 * "reg" property gives out the base offset of the counters data
	 * for this PMU.
	 */
	of_property_read_u32(parent, "reg", &reg);

	if (!name_prefix->value ||
	   (strnlen(name_prefix->value, name_prefix->length) == name_prefix->length) ||
	   (name_prefix->length > IMC_MAX_NAME_VAL_LEN))
		goto free_events;

	/* Loop through event nodes */
	for_each_child_of_node(dir, ev_node) {
		ret = imc_events_node_parser(ev_node, &pmu_ptr->events[*idx], scale_pp,
				unit_pp, name_prefix, reg, pmu_ptr->domain);
		if (ret < 0) {
			/* Unable to parse this event */
			if (ret == -ENOMEM)
				goto free_events;
			continue;
		}

		/*
		 * imc_event_node_parser will return number of
		 * event entries created for this. This could include
		 * event scale and unit files also.
		 */
		*idx += ret;
	}
	return 0;

free_events:
	imc_free_events(pmu_ptr->events, *idx);
	return -ENODEV;

}

/* imc_get_mem_addr_nest: Function to get nest counter memory region for each chip */
static int imc_get_mem_addr_nest(struct device_node *node,
				 struct imc_pmu *pmu_ptr,
				 u32 offset)
{
	int nr_chips = 0, i, j;
	u64 *base_addr_arr, baddr;
	u32 *chipid_arr, size = pmu_ptr->counter_mem_size, pages;

	nr_chips = of_property_count_u32_elems(node, "chip-id");
	if (!nr_chips)
		return -ENODEV;

	base_addr_arr = kzalloc((sizeof(u64) * nr_chips), GFP_KERNEL);
	chipid_arr = kzalloc((sizeof(u32) * nr_chips), GFP_KERNEL);
	if (!base_addr_arr || !chipid_arr)
		return -ENOMEM;

	of_property_read_u32_array(node, "chip-id", chipid_arr, nr_chips);
	of_property_read_u64_array(node, "base-addr", base_addr_arr, nr_chips);

	pmu_ptr->mem_info = kzalloc((sizeof(struct imc_mem_info) * nr_chips), GFP_KERNEL);
	if (!pmu_ptr->mem_info) {
		if (base_addr_arr)
			kfree(base_addr_arr);
		if (chipid_arr)
			kfree(chipid_arr);

		return -ENOMEM;
		}

	for (i = 0; i < nr_chips; i++) {
		pmu_ptr->mem_info[i].id = chipid_arr[i];
		baddr = base_addr_arr[i] + offset;
		for (j = 0; j < (size/PAGE_SIZE); j++) {
			pages = PAGE_SIZE * j;
			pmu_ptr->mem_info[i].vbase[j] = phys_to_virt(baddr + pages);
		}
	}
	return 0;
}

/*
 * imc_pmu_create : Takes the parent device which is the pmu unit, pmu_index
 *		    and domain as the inputs.
 * Allocates memory for the pmu, sets up its domain (NEST/CORE), and
 * calls imc_events_setup() to allocate memory for the events supported
 * by this pmu. Assigns a name for the pmu.
 *
 * If everything goes fine, it calls, init_imc_pmu() to setup the pmu device
 * and register it.
 */
static int imc_pmu_create(struct device_node *parent, int pmu_index, int domain)
{
	u32 prop = 0;
	struct property *pp;
	char *buf;
	int idx = 0, ret = 0;
	struct imc_pmu *pmu_ptr;
	u32 offset;

	if (!parent)
		return -EINVAL;

	/* memory for pmu */
	pmu_ptr = kzalloc(sizeof(struct imc_pmu), GFP_KERNEL);
	if (!pmu_ptr)
		return -ENOMEM;

	pmu_ptr->domain = domain;

	/* Needed for hotplug/migration */
	if (pmu_ptr->domain == IMC_DOMAIN_CORE)
		core_imc_pmu = pmu_ptr;
	else if (pmu_ptr->domain == IMC_DOMAIN_NEST)
		per_nest_pmu_arr[pmu_index] = pmu_ptr;

	pp = of_find_property(parent, "name", NULL);
	if (!pp) {
		ret = -ENODEV;
		goto free_pmu;
	}

	if (!pp->value ||
	   (strnlen(pp->value, pp->length) == pp->length) ||
	   (pp->length > IMC_MAX_NAME_VAL_LEN)) {
		ret = -EINVAL;
		goto free_pmu;
	}

	buf = kzalloc(IMC_MAX_NAME_VAL_LEN, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto free_pmu;
	}
	/* Save the name to register it later */
	if (pmu_ptr->domain == IMC_DOMAIN_NEST)
		sprintf(buf, "nest_%s", (char *)pp->value);
	else
		sprintf(buf, "%s_imc", (char *)pp->value);
	pmu_ptr->pmu.name = (char *)buf;

	if (of_property_read_u32(parent, "size", &pmu_ptr->counter_mem_size))
		pmu_ptr->counter_mem_size = 0;

	if (!of_property_read_u32(parent, "offset", &offset)) {
		if (imc_get_mem_addr_nest(parent, pmu_ptr, offset))
			goto free_pmu;
		pmu_ptr->imc_counter_mmaped = 1;
	}

	/*
	 * "events" property inside a PMU node contains the phandle value
	 * for the actual events node. The "events" node for the IMC PMU
	 * is not in this node, rather inside "imc-counters" node, since,
	 * we want to factor out the common events (thereby, reducing the
	 * size of the device tree)
	 */
	if (!of_property_read_u32(parent, "events", &prop)) {
		if (prop)
			imc_events_setup(parent, pmu_index, pmu_ptr, prop, &idx);
	}
	/* Function to register IMC pmu */
	ret = init_imc_pmu(pmu_ptr->events, idx, pmu_ptr);
	if (ret) {
		pr_err("IMC PMU %s Register failed\n", pmu_ptr->pmu.name);
		goto free_events;
	}
	return 0;

free_events:
	if (pmu_ptr->events)
		imc_free_events(pmu_ptr->events, idx);
free_pmu:
	if (pmu_ptr)
		kfree(pmu_ptr);
	return ret;
}

static int opal_imc_counters_probe(struct platform_device *pdev)
{
	struct device_node *imc_dev = NULL;
	int pmu_count = 0, domain;
	u32 type;

	if (!pdev || !pdev->dev.of_node)
		return -ENODEV;

	/*
	 * Check whether this is kdump kernel. If yes, just return.
	 */
	if (is_kdump_kernel())
		return -ENODEV;

	imc_dev = pdev->dev.of_node;
	if (!imc_dev)
		return -ENODEV;
	for_each_compatible_node(imc_dev, NULL, IMC_DTB_UNIT_COMPAT) {
		if (of_property_read_u32(imc_dev, "type", &type))
			continue;
		if (type == IMC_COUNTER_PER_CHIP)
			domain = IMC_DOMAIN_NEST;
		else if (type == IMC_COUNTER_PER_CORE)
			domain = IMC_DOMAIN_CORE;
		else
			continue;
		if (!imc_pmu_create(imc_dev, pmu_count, domain))
			pmu_count++;
	}
	return 0;
}

static const struct of_device_id opal_imc_match[] = {
	{ .compatible = IMC_DTB_COMPAT },
	{},
};

static struct platform_driver opal_imc_driver = {
	.driver = {
		.name = "opal-imc-counters",
		.of_match_table = opal_imc_match,
	},
	.probe = opal_imc_counters_probe,
};

MODULE_DEVICE_TABLE(of, opal_imc_match);
module_platform_driver(opal_imc_driver);
MODULE_DESCRIPTION("PowerNV OPAL IMC driver");
MODULE_LICENSE("GPL");
