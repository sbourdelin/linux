/*
 * PowerNV OPAL in-memory console interface
 *
 * Copyright 2014 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <asm/io.h>
#include <asm/opal.h>
#include <linux/of.h>
#include <linux/types.h>

struct hdat_info {
	char *base;
	u64 size;
};

static struct hdat_info hdat_inf;

/* Read function for HDAT attribute in sysfs */
static ssize_t hdat_read(struct file *file, struct kobject *kobj,
			 struct bin_attribute *bin_attr, char *to,
			 loff_t pos, size_t count)
{
	if (!hdat_inf.base)
		return -ENODEV;

	return memory_read_from_buffer(to, count, &pos, hdat_inf.base,
					hdat_inf.size);
}


/* HDAT attribute for sysfs */
static struct bin_attribute hdat_attr = {
	.attr = {.name = "hdat", .mode = 0444},
	.read = hdat_read
};

void __init opal_hdat_sysfs_init(void)
{
	u64 hdat_addr[2];

	/* Check for the hdat-map prop in device-tree */
	if (of_property_read_u64_array(opal_node, "hdat-map", hdat_addr, 2)) {
		pr_debug("OPAL: Property hdat-map not found.\n");
		return;
	}

	/* Print out hdat-map values. [0]: base, [1]: size */
	pr_debug("OPAL: HDAT Base address: %#llx\n", hdat_addr[0]);
	pr_debug("OPAL: HDAT Size: %#llx\n", hdat_addr[1]);

	hdat_inf.base = phys_to_virt(hdat_addr[0]);
	hdat_inf.size = hdat_addr[1];

	if (sysfs_create_bin_file(opal_kobj, &hdat_attr) != 0)
		pr_debug("OPAL: sysfs file creation for HDAT failed");

}
