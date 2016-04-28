/*
 * Driver for GE FPGA based GPIO
 *
 * Author: Martyn Welch <martyn.welch@ge.com>
 *
 * 2008 (c) GE Intelligent Platforms Embedded Systems, Inc.
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

/* TODO
 *
 * Configuration of output modes (totem-pole/open-drain)
 * Interrupt configuration - interrupts are always generated the FPGA relies on
 * the I/O interrupt controllers mask to stop them propergating
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/ioport.h>
#include <linux/gpio/driver.h>
#include <linux/platform_device.h>
#include "gpio-mmio-compat.h"

#define GEF_GPIO_DIRECT		0x00
#define GEF_GPIO_IN		0x04
#define GEF_GPIO_OUT		0x08
#define GEF_GPIO_TRIG		0x0C
#define GEF_GPIO_POLAR_A	0x10
#define GEF_GPIO_POLAR_B	0x14
#define GEF_GPIO_INT_STAT	0x18
#define GEF_GPIO_OVERRUN	0x1C
#define GEF_GPIO_MODE		0x20

int ge_parse_dt(struct platform_device *pdev,
	       struct bgpio_pdata *pdata,
	       unsigned long *flags)
{
	struct device_node *np = pdev->dev.of_node;

	struct resource *res;
	struct resource nres[] = {
		DEFINE_RES_MEM_NAMED(0, 1, "dat"),
		DEFINE_RES_MEM_NAMED(0, 1, "set"),
		DEFINE_RES_MEM_NAMED(0, 1, "dirin"),
	};

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) != 0x24)
		return -EINVAL;

	set_resource_address(&nres[0], res->start + GEF_GPIO_IN, 0x4);
	set_resource_address(&nres[1], res->start + GEF_GPIO_OUT, 0x4);
	set_resource_address(&nres[2], res->start + GEF_GPIO_DIRECT, 0x4);
	*flags |= BGPIOF_BIG_ENDIAN_BYTE_ORDER;

	if (of_device_is_compatible(np, "ge,imp3a-gpio"))
		pdata->ngpio = 16;
	else if (of_device_is_compatible(np, "gef,sbc310-gpio"))
		pdata->ngpio = 6;
	else if (of_device_is_compatible(np, "gef,sbc610-gpio"))
		pdata->ngpio = 19;

	return platform_device_add_resources(pdev, nres, ARRAY_SIZE(nres));
}

MODULE_DESCRIPTION("GE I/O FPGA GPIO driver");
MODULE_AUTHOR("Martyn Welch <martyn.welch@ge.com");
