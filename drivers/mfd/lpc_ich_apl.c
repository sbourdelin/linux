/*
 * Intel Apollo Lake In-Vehicle Infotainment (IVI) systems used in cars support
 *
 * Copyright (C) 2016 Intel Corporation
 *
 * Author: Tan, Jui Nee <jui.nee.tan@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <asm/p2sb.h>
#include <linux/pci.h>
#include <linux/mfd/core.h>
#include <linux/mfd/lpc_ich.h>
#include <linux/pinctrl/pinctrl.h>

#include "lpc_ich_apl.h"

/* Offset data for Apollo Lake GPIO communities */
#define APL_GPIO_SOUTHWEST_OFFSET	0xc00000
#define APL_GPIO_NORTHWEST_OFFSET	0xc40000
#define APL_GPIO_NORTH_OFFSET		0xc50000
#define APL_GPIO_WEST_OFFSET		0xc70000

#define APL_GPIO_SOUTHWEST_NPIN		43
#define APL_GPIO_NORTHWEST_NPIN		77
#define APL_GPIO_NORTH_NPIN		78
#define APL_GPIO_WEST_NPIN		47

#define APL_GPIO_COMMUNITY_MAX		4

#define APL_GPIO_IRQ 14

#define PCI_IDSEL_P2SB	0x0d

static struct resource apl_gpio_io_res[] = {
	DEFINE_RES_MEM_NAMED(APL_GPIO_NORTH_OFFSET,
		APL_GPIO_NORTH_NPIN * SZ_8, "apl_pinctrl_n"),
	DEFINE_RES_MEM_NAMED(APL_GPIO_NORTHWEST_OFFSET,
		APL_GPIO_NORTHWEST_NPIN * SZ_8, "apl_pinctrl_nw"),
	DEFINE_RES_MEM_NAMED(APL_GPIO_WEST_OFFSET,
		APL_GPIO_WEST_NPIN * SZ_8, "apl_pinctrl_w"),
	DEFINE_RES_MEM_NAMED(APL_GPIO_SOUTHWEST_OFFSET,
		APL_GPIO_SOUTHWEST_NPIN * SZ_8, "apl_pinctrl_sw"),
	DEFINE_RES_IRQ(APL_GPIO_IRQ),
};

static struct mfd_cell apl_gpio_devices[] = {
	{
		.name = "apl-pinctrl",
		.id = 0,
		.num_resources = ARRAY_SIZE(apl_gpio_io_res),
		.resources = &apl_gpio_io_res[0],
		.ignore_resource_conflicts = true,
	},
	{
		.name = "apl-pinctrl",
		.id = 1,
		.num_resources = ARRAY_SIZE(apl_gpio_io_res),
		.resources = &apl_gpio_io_res[1],
		.ignore_resource_conflicts = true,
	},
	{
		.name = "apl-pinctrl",
		.id = 2,
		.num_resources = ARRAY_SIZE(apl_gpio_io_res),
		.resources = &apl_gpio_io_res[2],
		.ignore_resource_conflicts = true,
	},
	{
		.name = "apl-pinctrl",
		.id = 3,
		.num_resources = ARRAY_SIZE(apl_gpio_io_res),
		.resources = &apl_gpio_io_res[3],
		.ignore_resource_conflicts = true,
	},
};

int lpc_ich_add_gpio(struct pci_dev *dev, enum lpc_chipsets chipset)
{
	unsigned int i;
	int ret;
	struct resource base;

	if (chipset != LPC_APL)
		return -ENODEV;
	/*
	 * Apollo lake, has not 1, but 4 gpio controllers,
	 * handle it a bit differently.
	 */

	ret = p2sb_bar(dev, PCI_DEVFN(PCI_IDSEL_P2SB, 0), &base);
	if (ret)
		goto warn_continue;

	for (i = 0; i < APL_GPIO_COMMUNITY_MAX; i++) {
		struct resource *res = &apl_gpio_io_res[i];

		/* Fill MEM resource */
		res->start += base.start;
		res->end += base.start;
		res->flags = base.flags;

		res++;
	}

	ret = mfd_add_devices(&dev->dev, 0,
		apl_gpio_devices, ARRAY_SIZE(apl_gpio_devices),
			NULL, 0, NULL);

	if (ret)
warn_continue:
		dev_warn(&dev->dev,
			"Failed to add Apollo Lake GPIO: %d\n",
				ret);

	return ret;
}
