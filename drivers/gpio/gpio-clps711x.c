/*
 *  CLPS711X GPIO driver
 *
 *  Copyright (C) 2012,2013 Alexander Shiyan <shc_work@mail.ru>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/ioport.h>
#include <linux/gpio/driver.h>
#include <linux/platform_device.h>
#include "gpio-mmio-compat.h"

int cirrus_clps711x_parse_dt(struct platform_device *pdev,
			     struct bgpio_pdata *pdata,
			     unsigned long *flags)
{
	struct device_node *np = pdev->dev.of_node;
	int id = np ? of_alias_get_id(np, "gpio") : pdev->id;
	struct resource *res;
	struct resource nres[] = {
		DEFINE_RES_MEM_NAMED(0, 1, "dat"),
		DEFINE_RES_MEM_NAMED(0, 1, "dirout"),
	};

	if ((id < 0) || (id > 4))
		return -ENODEV;

	if (id == 4) {
		/* PORTE is 3 lines only */
		pdata->ngpio = 3;
	}

	pdata->base = id * 8;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) != 0x1)
		return -EINVAL;
	set_resource_address(&nres[0], res->start, 0x1);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res || resource_size(res) != 0x1)
		return -EINVAL;
	set_resource_address(&nres[1], res->start, 0x1);

	if (id == 3) {
		/* PORTD is inverted logic for direction register */
		nres[1].name = "dirin";
	}

	return platform_device_add_resources(pdev, nres, ARRAY_SIZE(nres));
}

MODULE_AUTHOR("Alexander Shiyan <shc_work@mail.ru>");
MODULE_DESCRIPTION("CLPS711X GPIO driver");
MODULE_ALIAS("platform:clps711x-gpio");
MODULE_ALIAS("clps711x-gpio");
