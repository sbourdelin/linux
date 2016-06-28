/*
 * Purpose: Create a platform device to bind with Intel Apollo Lake
 * Pinctrl GPIO platform driver
 * Copyright (C) 2016 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/mfd/core.h>
#include <linux/mfd/lpc_ich.h>
#include <linux/pinctrl/pinctrl.h>
#include <asm/p2sb.h>

#if defined(CONFIG_X86_INTEL_APL)
/* Offset data for Apollo Lake GPIO communities */
#define APL_GPIO_SOUTHWEST_OFFSET	0xc00000
#define APL_GPIO_NORTHWEST_OFFSET	0xc40000
#define APL_GPIO_NORTH_OFFSET		0xc50000
#define APL_GPIO_WEST_OFFSET		0xc70000

#define APL_GPIO_SOUTHWEST_NPIN		43
#define APL_GPIO_NORTHWEST_NPIN		77
#define APL_GPIO_NORTH_NPIN		78
#define APL_GPIO_WEST_NPIN		47

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

static struct pinctrl_pin_desc apl_pinctrl_pdata;

static struct mfd_cell apl_gpio_devices[] = {
	{
		.name = "apl-pinctrl",
		.id = 0,
		.num_resources = ARRAY_SIZE(apl_gpio_io_res),
		.resources = apl_gpio_io_res,
		.pdata_size = sizeof(apl_pinctrl_pdata),
		.platform_data = &apl_pinctrl_pdata,
		.ignore_resource_conflicts = true,
	},
	{
		.name = "apl-pinctrl",
		.id = 1,
		.num_resources = ARRAY_SIZE(apl_gpio_io_res),
		.resources = apl_gpio_io_res,
		.pdata_size = sizeof(apl_pinctrl_pdata),
		.platform_data = &apl_pinctrl_pdata,
		.ignore_resource_conflicts = true,
	},
	{
		.name = "apl-pinctrl",
		.id = 2,
		.num_resources = ARRAY_SIZE(apl_gpio_io_res),
		.resources = apl_gpio_io_res,
		.pdata_size = sizeof(apl_pinctrl_pdata),
		.platform_data = &apl_pinctrl_pdata,
		.ignore_resource_conflicts = true,
	},
	{
		.name = "apl-pinctrl",
		.id = 3,
		.num_resources = ARRAY_SIZE(apl_gpio_io_res),
		.resources = apl_gpio_io_res,
		.pdata_size = sizeof(apl_pinctrl_pdata),
		.platform_data = &apl_pinctrl_pdata,
		.ignore_resource_conflicts = true,
	},
};

int lpc_ich_misc(struct pci_dev *dev)
{
	unsigned int apl_p2sb = PCI_DEVFN(PCI_IDSEL_P2SB, 0);
	unsigned int i;
	int ret;

	/*
	 * Apollo lake, has not 1, but 4 gpio controllers,
	 * handle it a bit differently.
	 */

	for (i = 0; i < ARRAY_SIZE(apl_gpio_io_res)-1; i++) {
		struct resource *res = &apl_gpio_io_res[i];

		apl_gpio_devices[i].resources = res;

		/* Fill MEM resource */
		ret = p2sb_bar(dev, apl_p2sb, res++);
		if (ret)
			goto warn_continue;

		apl_pinctrl_pdata.name = kasprintf(GFP_KERNEL, "%u",
			i + 1);
	}

	if (apl_pinctrl_pdata.name)
		ret = mfd_add_devices(&dev->dev, apl_gpio_devices->id,
			apl_gpio_devices, ARRAY_SIZE(apl_gpio_devices),
				NULL, 0, NULL);
	else
		ret = -ENOMEM;

warn_continue:
	if (ret)
		dev_warn(&dev->dev,
			"Failed to add Apollo Lake GPIO %s: %d\n",
				apl_pinctrl_pdata.name, ret);

	kfree(apl_pinctrl_pdata.name);
	return 0;
}
#endif
