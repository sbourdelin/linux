/*
 * Power off driver for Maxim MAX77620 device.
 *
 * Copyright (c) 2014-2016, NVIDIA CORPORATION.  All rights reserved.
 *
 * Based on work by Chaitanya Bandi <bandik@nvidia.com>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any kind,
 * whether express or implied; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 */

#include <linux/errno.h>
#include <linux/mfd/max77620.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#if defined(CONFIG_ARM) || defined(CONFIG_ARM64)
#include <asm/system_misc.h>
#endif

struct max77620_power {
	struct regmap *regmap;
	struct device *dev;
};

static struct max77620_power *system_power_controller = NULL;

static void max77620_pm_power_off(void)
{
	struct max77620_power *power = system_power_controller;
	unsigned int value;
	int err;

	if (!power)
		return;

	/* clear power key interrupts */
	err = regmap_read(power->regmap, MAX77620_REG_ONOFFIRQ, &value);
	if (err < 0)
		dev_err(power->dev, "failed to clear power key interrupts: %d\n", err);

	/* clear RTC interrupts */
	/*
	err = regmap_read(power->regmap, MAX77620_REG_RTCINT, &value);
	if (err < 0)
		dev_err(power->dev, "failed to clear RTC interrupts: %d\n", err);
	*/

	/* clear TOP interrupts */
	err = regmap_read(power->regmap, MAX77620_REG_IRQTOP, &value);
	if (err < 0)
		dev_err(power->dev, "failed to clear interrupts: %d\n", err);

	err = regmap_update_bits(power->regmap, MAX77620_REG_ONOFFCNFG2,
				 MAX77620_ONOFFCNFG2_SFT_RST_WK, 0);
	if (err < 0)
		dev_err(power->dev, "failed to clear SFT_RST_WK: %d\n", err);

	err = regmap_update_bits(power->regmap, MAX77620_REG_ONOFFCNFG1,
				 MAX77620_ONOFFCNFG1_SFT_RST,
				 MAX77620_ONOFFCNFG1_SFT_RST);
	if (err < 0)
		dev_err(power->dev, "failed to set SFT_RST: %d\n", err);
}

#if defined(CONFIG_ARM) || defined(CONFIG_ARM64)
static void max77620_pm_restart(enum reboot_mode mode, const char *cmd)
{
	struct max77620_power *power = system_power_controller;
	int err;

	if (!power)
		return;

	err = regmap_update_bits(power->regmap, MAX77620_REG_ONOFFCNFG2,
				 MAX77620_ONOFFCNFG2_SFT_RST_WK,
				 MAX77620_ONOFFCNFG2_SFT_RST_WK);
	if (err < 0)
		dev_err(power->dev, "failed to set SFT_RST_WK: %d\n", err);

	err = regmap_update_bits(power->regmap, MAX77620_REG_ONOFFCNFG1,
				 MAX77620_ONOFFCNFG1_SFT_RST,
				 MAX77620_ONOFFCNFG1_SFT_RST);
	if (err < 0)
		dev_err(power->dev, "failed to set SFT_RST: %d\n", err);
}
#endif

static int max77620_poweroff_probe(struct platform_device *pdev)
{
	struct max77620_chip *max77620 = dev_get_drvdata(pdev->dev.parent);
	struct device_node *np = pdev->dev.parent->of_node;
	struct max77620_power *power;
	unsigned int value;
	int err;

	if (!of_property_read_bool(np, "system-power-controller"))
		return 0;

	power = devm_kzalloc(&pdev->dev, sizeof(*power), GFP_KERNEL);
	if (!power)
		return -ENOMEM;

	power->regmap = max77620->rmap;
	power->dev = &pdev->dev;

	err = regmap_read(power->regmap, MAX77620_REG_NVERC, &value);
	if (err < 0) {
		dev_err(power->dev, "failed to read event recorder: %d\n", err);
		return err;
	}

	dev_dbg(&pdev->dev, "event recorder: %#x\n", value);

	system_power_controller = power;
	pm_power_off = max77620_pm_power_off;
#if defined(CONFIG_ARM) || defined(CONFIG_ARM64)
	arm_pm_restart = max77620_pm_restart;
#endif

	return 0;
}

static struct platform_driver max77620_poweroff_driver = {
	.driver = {
		.name = "max77620-power",
	},
	.probe = max77620_poweroff_probe,
};
module_platform_driver(max77620_poweroff_driver);

MODULE_DESCRIPTION("Maxim MAX77620 PMIC power off and restart driver");
MODULE_AUTHOR("Thierry Reding <treding@nvidia.com>");
MODULE_ALIAS("platform:max77620-power");
MODULE_LICENSE("GPL v2");
