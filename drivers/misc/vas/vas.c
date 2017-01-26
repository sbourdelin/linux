/*
 * Copyright 2016 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <asm/vas.h>
#include "vas-internal.h"

int vas_initialized;
int vas_num_instances;
struct vas_instance *vas_instances;

static void init_vas_chip(struct vas_instance *vinst)
{
	int i;

	for (i = 0; i < VAS_MAX_WINDOWS_PER_CHIP; i++)
		vas_window_reset(vinst, i);
}

static int init_vas_instance(struct device_node *dn,
				struct vas_instance *vinst)
{
	int rc;
	const __be32 *p;

	ida_init(&vinst->ida);
	mutex_init(&vinst->mutex);

	p = of_get_property(dn, "vas-id", NULL);
	if (!p) {
		pr_err("VAS: NULL vas-id? %p\n", p);
		return -ENODEV;
	}

	vinst->vas_id = of_read_number(p, 1);

	rc = of_property_read_u64(dn, "hvwc-bar-start", &vinst->hvwc_bar_start);
	if (rc)
		return rc;

	rc = of_property_read_u64(dn, "hvwc-bar-size", &vinst->hvwc_bar_len);
	if (rc)
		return rc;

	rc = of_property_read_u64(dn, "uwc-bar-start", &vinst->uwc_bar_start);
	if (rc)
		return rc;

	rc = of_property_read_u64(dn, "uwc-bar-size", &vinst->uwc_bar_len);
	if (rc)
		return rc;

	rc = of_property_read_u64(dn, "window-base", &vinst->win_base_addr);
	if (rc)
		return rc;

	rc = of_property_read_u64(dn, "window-shift", &vinst->win_id_shift);
	if (rc)
		return rc;

	init_vas_chip(vinst);

	return 0;
}

/*
 * Although this is read/used multiple times, it is written to only
 * during initialization.
 */
struct vas_instance *find_vas_instance(int vasid)
{
	int i;
	struct vas_instance *vinst;

	for (i = 0; i < vas_num_instances; i++) {
		vinst = &vas_instances[i];
		if (vinst->vas_id == vasid)
			return vinst;
	}
	pr_err("VAS instance for vas-id %d not found\n", vasid);
	WARN_ON_ONCE(1);
	return NULL;
}


int vas_init(void)
{
	int rc;
	struct device_node *dn;
	struct vas_instance *vinst;

	if (!pvr_version_is(PVR_POWER9))
		return -ENODEV;

	vas_num_instances = 0;
	for_each_node_by_name(dn, "vas")
		vas_num_instances++;

	if (!vas_num_instances)
		return -ENODEV;

	vas_instances = kmalloc_array(vas_num_instances, sizeof(*vinst),
					GFP_KERNEL);
	if (!vas_instances)
		return -ENOMEM;

	vinst = &vas_instances[0];
	for_each_node_by_name(dn, "vas") {
		rc = init_vas_instance(dn, vinst);
		if (rc) {
			pr_err("Error %d initializing VAS instance %ld\n", rc,
					(vinst-&vas_instances[0]));
			goto cleanup;
		}
		vinst++;
	}

	rc = -ENODEV;
	if (vinst == &vas_instances[0]) {
		/* Should not happen as we saw some above. */
		pr_err("VAS: Did not find any VAS DT nodes now!\n");
		goto cleanup;
	}

	pr_devel("VAS: Initialized %d instances\n", vas_num_instances);
	vas_initialized = 1;

	return 0;

cleanup:
	kfree(vas_instances);
	return rc;
}

void vas_exit(void)
{
	vas_initialized = 0;
	kfree(vas_instances);
}

module_init(vas_init);
module_exit(vas_exit);
MODULE_DESCRIPTION("IBM Virtual Accelerator Switchboard");
MODULE_AUTHOR("Sukadev Bhattiprolu <sukadev@linux.vnet.ibm.com>");
MODULE_LICENSE("GPL");
