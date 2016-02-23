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

static int opal_nest_counters_probe(struct platform_device *pdev)
{
	struct device_node *child, *parent;
	struct perchip_nest_info *pcni;
	u32 idx, range[4], pages;
	int rc=0, i=0;

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
