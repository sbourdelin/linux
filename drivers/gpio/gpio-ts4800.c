/*
 * GPIO driver for the TS-4800 board
 *
 * Copyright (c) 2016 - Savoir-faire Linux
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/ioport.h>
#include <linux/gpio/driver.h>
#include <linux/platform_device.h>
#include "gpio-mmio-compat.h"

#define DEFAULT_PIN_NUMBER      16
#define INPUT_REG_OFFSET        0x00
#define OUTPUT_REG_OFFSET       0x02
#define DIRECTION_REG_OFFSET    0x04

int technologic_ts4800_parse_dt(struct platform_device *pdev,
				struct bgpio_pdata *pdata,
				unsigned long *flags)
{
	int err;
	struct resource *res;
	struct resource nres[] = {
		DEFINE_RES_MEM_NAMED(0, 1, "dat"),
		DEFINE_RES_MEM_NAMED(0, 1, "set"),
		DEFINE_RES_MEM_NAMED(0, 1, "dirout"),
	};

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) != 6)
		return -EINVAL;

	set_resource_address(&nres[0], res->start + INPUT_REG_OFFSET, 0x2);
	set_resource_address(&nres[1], res->start + OUTPUT_REG_OFFSET, 0x2);
	set_resource_address(&nres[2], res->start + DIRECTION_REG_OFFSET, 0x2);

	err = of_property_read_u32(pdev->dev.of_node, "ngpios", &pdata->ngpio);
	if (err == -EINVAL)
		pdata->ngpio = DEFAULT_PIN_NUMBER;
	else if (err)
		return err;

	return platform_device_add_resources(pdev, nres, ARRAY_SIZE(nres));
}

MODULE_AUTHOR("Julien Grossholtz <julien.grossholtz@savoirfairelinux.com>");
MODULE_DESCRIPTION("TS4800 FPGA GPIO driver");
MODULE_ALIAS("gpio-ts4800");
