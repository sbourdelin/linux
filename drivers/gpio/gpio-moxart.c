/*
 * MOXA ART SoCs GPIO driver.
 *
 * Copyright (C) 2013 Jonas Jensen
 *
 * Jonas Jensen <jonas.jensen@gmail.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/ioport.h>
#include <linux/gpio/driver.h>
#include <linux/platform_device.h>
#include "gpio-mmio-compat.h"

#define GPIO_DATA_OUT		0x00
#define GPIO_DATA_IN		0x04
#define GPIO_PIN_DIRECTION	0x08

int moxart_parse_dt(struct platform_device *pdev,
		    struct bgpio_pdata *pdata,
		    unsigned long *flags)
{
	struct resource *res;
	struct resource nres[] = {
		DEFINE_RES_MEM_NAMED(0, 1, "dat"),
		DEFINE_RES_MEM_NAMED(0, 1, "set"),
		DEFINE_RES_MEM_NAMED(0, 1, "dirout"),
	};

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) != 12)
		return -EINVAL;

	*flags |= BGPIOF_READ_OUTPUT_REG_SET;
	set_resource_address(&nres[0], res->start + GPIO_DATA_IN, 0x4);
	set_resource_address(&nres[1], res->start + GPIO_DATA_OUT, 0x4);
	set_resource_address(&nres[2], res->start + GPIO_PIN_DIRECTION, 0x4);
	return platform_device_add_resources(pdev, nres, ARRAY_SIZE(nres));
}
