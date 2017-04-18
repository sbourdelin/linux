/*
 * OPAL IMC interface detection driver
 * Supported on POWERNV platform
 *
 * Copyright	(C) 2017 Madhavan Srinivasan, IBM Corporation.
 *		(C) 2017 Anju T Sudhakar, IBM Corporation.
 *		(C) 2017 Hemant K Shaw, IBM Corporation.
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
#include <linux/crash_dump.h>
#include <asm/opal.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/cputable.h>
#include <asm/imc-pmu.h>

struct perchip_nest_info nest_perchip_info[IMC_MAX_CHIPS];

/*
 * imc_pmu_setup : Setup the IMC PMUs (children of "parent").
 */
static void __init imc_pmu_setup(struct device_node *parent)
{
	if (!parent)
		return;
}

static int opal_imc_counters_probe(struct platform_device *pdev)
{
	struct device_node *imc_dev, *dn, *rm_node = NULL;
	struct perchip_nest_info *pcni;
	u32 pages, nest_offset, nest_size, chip_id;
	int i = 0;
	const __be32 *addrp;
	u64 reg_addr, reg_size;

	if (!pdev || !pdev->dev.of_node)
		return -ENODEV;

	/*
	 * Check whether this is kdump kernel. If yes, just return.
	 */
	if (is_kdump_kernel())
		return -ENODEV;

	imc_dev = pdev->dev.of_node;

	/*
	 * Nest counter data are saved in a reserved memory called HOMER.
	 * "imc-nest-offset" identifies the counter data location within HOMER.
	 * size : size of the entire nest-counters region
	 */
	if (of_property_read_u32(imc_dev, "imc-nest-offset", &nest_offset))
		goto err;

	if (of_property_read_u32(imc_dev, "imc-nest-size", &nest_size))
		goto err;

	/* Sanity check */
	if ((nest_size/PAGE_SIZE) > IMC_NEST_MAX_PAGES)
		goto err;

	/* Find the "HOMER region" for each chip */
	rm_node = of_find_node_by_path("/reserved-memory");
	if (!rm_node)
		goto err;

	/*
	 * We need to look for the "ibm,homer-image" node in the
	 * "/reserved-memory" node.
	 */
	for (dn = of_find_node_by_name(rm_node, "ibm,homer-image"); dn;
			dn = of_find_node_by_name(dn, "ibm,homer-image")) {

		/* Get the chip id to which the above homer region belongs to */
		if (of_property_read_u32(dn, "ibm,chip-id", &chip_id))
			goto err;

		pcni = &nest_perchip_info[chip_id];
		addrp = of_get_address(dn, 0, &reg_size, NULL);
		if (!addrp)
			goto err;

		/* Fetch the homer region base address */
		reg_addr = of_read_number(addrp, 2);
		pcni->pbase = reg_addr;
		/* Add the nest IMC Base offset */
		pcni->pbase = pcni->pbase + nest_offset;
		/* Fetch the size of the homer region */
		pcni->size = nest_size;

		for (i = 0; i < (pcni->size / PAGE_SIZE); i++) {
			pages = PAGE_SIZE * i;
			pcni->vbase[i] = (u64)phys_to_virt(pcni->pbase + pages);
		}
	}

	imc_pmu_setup(imc_dev);

	return 0;
err:
	return -ENODEV;
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
