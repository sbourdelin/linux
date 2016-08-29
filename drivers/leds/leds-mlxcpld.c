/*
 * drivers/leds/leds-mlxcpld.c
 * Copyright (c) 2016 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2016 Vadim Pasternak <vadimp@mellanox.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/version.h>
#include <linux/acpi.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/leds.h>

#define MLXPLAT_CPLD_LPC_REG_BASE_ADRR     0x2500 /* LPC bus access */

/* Color codes for leds */
#define LED_IS_OFF            0x00
#define LED_RED_STATIC_ON     0x05
#define LED_RED_BLINK_HALF    0x06
#define LED_GREEN_STATIC_ON   0x0D
#define LED_GREEN_BLINK_HALF  0x0E

/**
 * mlxcpld_param -
 * @offset - offset for led access in CPLD device
 * @mask - mask for led access in CPLD device
 * @base_color - base color code for led
**/
struct mlxcpld_param {
	u8 offset;
	u8 mask;
	u8 base_color;
};

/**
 * mlxcpld_led_priv -
 * @led - led class device pointer
 * @param - led CPLD access parameters
**/
struct mlxcpld_led_priv {
	struct led_classdev cdev;
	struct mlxcpld_param param;
};
#define cdev_to_priv(c)		container_of(c, struct mlxcpld_led_priv, cdev)

/**
 * mlxcpld_led_profile (defined per system class) -
 * @offset - offset for led access in CPLD device
 * @mask - mask for led access in CPLD device
 * @base_color - base color code
 * @brightness - default brightness setting (on/off)
 * @name - led name
**/
struct mlxcpld_led_profile {
	u8 offset;
	u8 mask;
	u8 base_color;
	enum led_brightness brightness;
	const char *name;
};

/**
 * mlxcpld_led_pdata -
 * @pdev - platform device pointer
 * @led - led class device pointer
 * @trigger - trigger class device pointer
 * @profile - system configuration profile
 * @num_led_instances - number of system triggers
 * @lock - device access lock
**/
struct mlxcpld_led_pdata {
	struct platform_device *pdev;
	struct mlxcpld_led_priv *pled;
	struct mlxcpld_led_profile *profile;
	int num_led_instances;
	spinlock_t lock;
};
static struct mlxcpld_led_pdata *mlxcpld_led;

/* Default profile fit the next Mellanox systems:
 * "msx6710", "msx6720", "msb7700", "msn2700", "msx1410",
 * "msn2410", "msb7800", "msn2740"
 */
struct mlxcpld_led_profile mlxcpld_led_default_profile[] = {
	{
		0x21, 0xf0, LED_GREEN_STATIC_ON, LED_FULL, "fan1:green",
	},
	{
		0x21, 0xf0, LED_RED_STATIC_ON, LED_OFF, "fan1:red",
	},
	{
		0x21, 0x0f, LED_GREEN_STATIC_ON, LED_FULL, "fan2:green",
	},
	{
		0x21, 0x0f, LED_RED_STATIC_ON, LED_OFF, "fan2:red",
	},
	{
		0x22, 0xf0, LED_GREEN_STATIC_ON, LED_FULL, "fan3:green",
	},
	{
		0x22, 0xf0, LED_RED_STATIC_ON, LED_OFF, "fan3:red",
	},
	{
		0x22, 0x0f, LED_GREEN_STATIC_ON, LED_FULL, "fan4:green",
	},
	{
		0x22, 0x0f, LED_RED_STATIC_ON, LED_OFF, "fan4:red",
	},
	{
		0x20, 0x0f, LED_GREEN_STATIC_ON, LED_FULL, "psu:green",
	},
	{
		0x20, 0x0f, LED_RED_STATIC_ON, LED_OFF, "psu:red",
	},
	{
		0x20, 0xf0, LED_GREEN_STATIC_ON, LED_FULL, "status:green",
	},
	{
		0x20, 0xf0, LED_RED_STATIC_ON, LED_OFF, "status:red",
	},
};

/* Profile fit the Mellanox systems based on "msn2100" */
struct mlxcpld_led_profile mlxcpld_led_msn2100_profile[] = {
	{
		0x21, 0xf0, LED_GREEN_STATIC_ON, LED_FULL, "fan:green",
	},
	{
		0x21, 0xf0, LED_RED_STATIC_ON, LED_OFF, "fan:red",
	},
	{
		0x23, 0xf0, LED_GREEN_STATIC_ON, LED_FULL, "psu1:green",
	},
	{
		0x23, 0xf0, LED_RED_STATIC_ON, LED_OFF, "psu1:red",
	},
	{
		0x23, 0x0f, LED_GREEN_STATIC_ON, LED_FULL, "psu2:green",
	},
	{
		0x23, 0x0f, LED_RED_STATIC_ON, LED_OFF, "psu2:red",
	},
	{
		0x20, 0xf0, LED_GREEN_STATIC_ON, LED_FULL, "status:green",
	},
	{
		0x20, 0xf0, LED_RED_STATIC_ON, LED_OFF, "status:red",
	},
	{
		0x24, 0xf0, LED_GREEN_STATIC_ON, LED_OFF,  "uid:blue",
	},
};

enum mlxcpld_led_platform_types {
	mlxcpld_led_platform_default,
	mlxcpld_led_platform_msn2100,
};

const char *mlx_product_names[] = {
	"DEFAULT",
	"MSN2100",
};

static enum
mlxcpld_led_platform_types mlxcpld_led_platform_check_sys_type(void)
{
	const char *mlx_product_name;
	int i;

	mlx_product_name = dmi_get_system_info(DMI_PRODUCT_NAME);
	if (mlx_product_name == NULL)
		return mlxcpld_led_platform_default;

	for (i = 1;  i < ARRAY_SIZE(mlx_product_names); i++) {
		if (strstr(mlx_product_name, mlx_product_names[i]))
			return i;
	}

	return mlxcpld_led_platform_default;
}

static void mlxcpld_led_bus_access_func(u16 base, u8 offset, u8 rw_flag,
					u8 *data)
{
	u32 addr = base + offset;

	if (rw_flag == 0)
		outb(*data, addr);
	else
		*data = inb(addr);
}

static void mlxcpld_led_store_hw(u8 mask, u8 off, u8 vset)
{
	u8 tmask, val;

	spin_lock(&mlxcpld_led->lock);
	tmask = (mask == 0xf0) ? vset : (vset << 4);
	mlxcpld_led_bus_access_func(MLXPLAT_CPLD_LPC_REG_BASE_ADRR, off, 1,
				    &val);
	val = (val & mask) | tmask;
	mlxcpld_led_bus_access_func(MLXPLAT_CPLD_LPC_REG_BASE_ADRR, off, 0,
				    &val);
	spin_unlock(&mlxcpld_led->lock);
}

static void mlxcpld_led_brightness(struct led_classdev *led,
				   enum led_brightness value)
{
	struct mlxcpld_led_priv *pled = cdev_to_priv(led);

	switch (value) {
	case LED_FULL:
	case LED_HALF:
	default:
		mlxcpld_led_store_hw(pled->param.mask, pled->param.offset,
				     pled->param.base_color);
		break;
	case LED_OFF:
		mlxcpld_led_store_hw(pled->param.mask, pled->param.offset,
				     LED_IS_OFF);
		break;
	}
}

static int mlxcpld_led_blink(struct led_classdev *led,
			     unsigned long *delay_on,
			     unsigned long *delay_off)
{
	struct mlxcpld_led_priv *pled = cdev_to_priv(led);

	/* SW blinking is not supported.
	 * HW supports two types of blinking: full (6KHz) and half (3KHz).
	 * Defaul value is 3KHz, which is set for blink request.
	 */
	mlxcpld_led_store_hw(pled->param.mask, pled->param.offset,
			     pled->param.base_color + 1);

	return 0;
}

static int mlxcpld_led_config(struct device *dev,
			      struct mlxcpld_led_pdata *cpld)
{
	int err = 0, i;

	cpld->pled = devm_kzalloc(dev, sizeof(struct mlxcpld_led_priv) *
				  cpld->num_led_instances, GFP_KERNEL);
	if (!cpld->pled)
		return -ENOMEM;

	for (i = 0; i < cpld->num_led_instances; i++) {
		cpld->pled[i].cdev.name = cpld->profile[i].name;
		cpld->pled[i].cdev.brightness = cpld->profile[i].brightness;
		cpld->pled[i].cdev.max_brightness = 1;
		cpld->pled[i].cdev.brightness_set = mlxcpld_led_brightness;
		cpld->pled[i].cdev.blink_set = mlxcpld_led_blink;
		cpld->pled[i].cdev.flags = LED_CORE_SUSPENDRESUME;
		err = devm_led_classdev_register(dev, &cpld->pled[i].cdev);
		if (err) {
			devm_kfree(dev, cpld->pled);
			return err;
		}

		cpld->pled[i].param.offset = mlxcpld_led->profile[i].offset;
		cpld->pled[i].param.mask = mlxcpld_led->profile[i].mask;
		cpld->pled[i].param.base_color =
					mlxcpld_led->profile[i].base_color;
		switch (mlxcpld_led->profile[i].brightness) {
		case LED_HALF:
		case LED_FULL:
			mlxcpld_led_brightness(&cpld->pled[i].cdev,
					mlxcpld_led->profile[i].brightness);
			break;
		default:
			break;
		}
	}

	return err;
}

static int __init mlxcpld_led_probe(struct platform_device *pdev)
{
	enum mlxcpld_led_platform_types mlxcpld_led_plat =
					mlxcpld_led_platform_check_sys_type();

	mlxcpld_led = devm_kzalloc(&pdev->dev, sizeof(*mlxcpld_led),
				   GFP_KERNEL);
	if (!mlxcpld_led)
		return -ENOMEM;
	mlxcpld_led->pdev = pdev;

	switch (mlxcpld_led_plat) {
	case mlxcpld_led_platform_msn2100:
		mlxcpld_led->profile = mlxcpld_led_msn2100_profile;
		mlxcpld_led->num_led_instances =
				ARRAY_SIZE(mlxcpld_led_msn2100_profile);
		break;

	default:
		mlxcpld_led->profile = mlxcpld_led_default_profile;
		mlxcpld_led->num_led_instances =
				ARRAY_SIZE(mlxcpld_led_default_profile);
		break;
	}

	spin_lock_init(&mlxcpld_led->lock);
	platform_set_drvdata(pdev, mlxcpld_led);

	return mlxcpld_led_config(&pdev->dev, mlxcpld_led);
}

static struct platform_driver mlxcpld_led_driver = {
	.driver = {
		.name	= KBUILD_MODNAME,
	},
};

static int __init mlxcpld_led_init(void)
{
	struct platform_device *pdev;
	int err;

	pdev = platform_device_register_simple(KBUILD_MODNAME, -1, NULL, 0);
	if (!pdev) {
		pr_err("Device allocation failed\n");
		return -ENOMEM;
	}

	err = platform_driver_probe(&mlxcpld_led_driver, mlxcpld_led_probe);
	if (err) {
		pr_err("Probe platform driver failed\n");
		platform_device_unregister(pdev);
	}

	return err;
}

static void __exit mlxcpld_led_exit(void)
{
	platform_device_unregister(mlxcpld_led->pdev);
	platform_driver_unregister(&mlxcpld_led_driver);
}

module_init(mlxcpld_led_init);
module_exit(mlxcpld_led_exit);

MODULE_AUTHOR("Vadim Pasternak (vadimp@mellanox.com)");
MODULE_DESCRIPTION("Mellanox board LED driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:leds-mlxcpld");
