/*
 * Thermal device driver for DA9062 and DA9061
 * Copyright (C) 2016  Dialog Semiconductor Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/regmap.h>
#include <linux/thermal.h>
#include <linux/of.h>

#include <linux/mfd/da9062/core.h>
#include <linux/mfd/da9062/registers.h>

#define DA9062_DEFAULT_POLLING_MS_PERIOD	3000
#define DA9062_MAX_POLLING_MS_PERIOD		10000
#define DA9062_MIN_POLLING_MS_PERIOD		1000

#define DA9062_MILLI_CELSIUS(t)			((t)*1000)

struct da9062_thermal_config {
	const char *name;
};

struct da9062_thermal {
	struct da9062 *hw;
	struct delayed_work work;
	struct thermal_zone_device *zone;
	enum thermal_device_mode mode;
	unsigned int polling_period;
	struct mutex lock;
	int temperature;
	int irq;
	const struct da9062_thermal_config *config;
	struct device *dev;
};

static void da9062_thermal_poll_on(struct work_struct *work)
{
	struct da9062_thermal *thermal = container_of(work,
						struct da9062_thermal,
						work.work);
	unsigned int val;
	int ret;

	/* clear E_TEMP */
	ret = regmap_write(thermal->hw->regmap,
				DA9062AA_EVENT_B,
				DA9062AA_E_TEMP_MASK);
	if (ret < 0) {
		dev_err(thermal->dev,
			"Cannot clear the TJUNC temperature status\n");
		goto err_enable_irq;
	}

	/* Now read E_TEMP again: it is acting like a status bit.
	 * If over-temperature, then this status will be true.
	 * If not over-temperature, this staus will be false.
	 */
	ret = regmap_read(thermal->hw->regmap,
			  DA9062AA_EVENT_B,
			  &val);
	if (ret < 0) {
		dev_err(thermal->dev,
			"Cannot check the TJUNC temperature status\n");
		goto err_enable_irq;
	} else {
		if (val & DA9062AA_E_TEMP_MASK) {
			mutex_lock(&thermal->lock);
			thermal->temperature = DA9062_MILLI_CELSIUS(125);
			mutex_unlock(&thermal->lock);
			thermal_zone_device_update(thermal->zone);

			schedule_delayed_work(&thermal->work,
				msecs_to_jiffies(thermal->polling_period));
			return;
		} else {
			mutex_lock(&thermal->lock);
			thermal->temperature = DA9062_MILLI_CELSIUS(0);
			mutex_unlock(&thermal->lock);
			thermal_zone_device_update(thermal->zone);
		}
	}

err_enable_irq:
	enable_irq(thermal->irq);
}

static irqreturn_t da9062_thermal_irq_handler(int irq, void *data)
{
	struct da9062_thermal *thermal = data;

	disable_irq_nosync(thermal->irq);
	schedule_delayed_work(&thermal->work, 0);

	return IRQ_HANDLED;
}

static int da9062_thermal_get_mode(struct thermal_zone_device *z,
				   enum thermal_device_mode *mode)
{
	struct da9062_thermal *thermal = z->devdata;
	*mode = thermal->mode;
	return 0;
}

static int da9062_thermal_get_trip_type(struct thermal_zone_device *z,
				int trip,
				enum thermal_trip_type *type)
{
	struct da9062_thermal *thermal = z->devdata;

	switch (trip) {
	case 0:
		*type = THERMAL_TRIP_HOT;
		break;
	default:
		dev_err(thermal->dev,
			"Driver does not support more than 1 trip-wire\n");
		return -EINVAL;
	}

	return 0;
}

static int da9062_thermal_get_trip_temp(struct thermal_zone_device *z,
					int trip,
					int *temp)
{
	struct da9062_thermal *thermal = z->devdata;

	switch (trip) {
	case 0:
		*temp = DA9062_MILLI_CELSIUS(125);
		break;
	default:
		dev_err(thermal->dev,
			"Driver does not support more than 1 trip-wire\n");
		return -EINVAL;
	}

	return 0;
}

static int da9062_thermal_notify(struct thermal_zone_device *z,
				 int trip,
				 enum thermal_trip_type type)
{
	struct da9062_thermal *thermal = z->devdata;

	switch (type) {
	case THERMAL_TRIP_HOT:
		dev_warn(thermal->dev, "Reached HOT (125degC) temperature\n");
		break;
	default:
		break;
	}

	return 0;
}

static int da9062_thermal_get_temp(struct thermal_zone_device *z,
				   int *temp)
{
	struct da9062_thermal *thermal = z->devdata;

	mutex_lock(&thermal->lock);
	*temp = thermal->temperature;
	mutex_unlock(&thermal->lock);

	return 0;
}

static struct thermal_zone_device_ops da9062_thermal_ops = {
	.get_temp	= da9062_thermal_get_temp,
	.get_mode	= da9062_thermal_get_mode,
	.get_trip_type	= da9062_thermal_get_trip_type,
	.get_trip_temp	= da9062_thermal_get_trip_temp,
	.notify		= da9062_thermal_notify,
};

static const struct da9062_thermal_config da9062_config = {
	.name = "da9062-thermal",
};

static const struct da9062_thermal_config da9061_config = {
	.name = "da9061-thermal",
};

static const struct of_device_id da9062_compatible_reg_id_table[] = {
	{ .compatible = "dlg,da9062-thermal", .data = &da9062_config },
	{ .compatible = "dlg,da9061-thermal", .data = &da9061_config },
	{ },
};

static int da9062_thermal_probe(struct platform_device *pdev)
{
	struct da9062 *chip = dev_get_drvdata(pdev->dev.parent);
	struct da9062_thermal *thermal;
	unsigned int pp_tmp = DA9062_DEFAULT_POLLING_MS_PERIOD;
	const struct of_device_id *match;
	int ret = 0;

	match = of_match_node(da9062_compatible_reg_id_table,
			      pdev->dev.of_node);
	if (!match)
		return -ENXIO;

	if (pdev->dev.of_node) {
		if (!of_property_read_u32(pdev->dev.of_node,
					"dlg,tjunc-temp-polling-period-ms",
					&pp_tmp)) {
			if (pp_tmp < DA9062_MIN_POLLING_MS_PERIOD ||
				pp_tmp > DA9062_MAX_POLLING_MS_PERIOD)
				pp_tmp = DA9062_DEFAULT_POLLING_MS_PERIOD;
		}

		dev_dbg(&pdev->dev,
			 "TJUNC temp polling period set at %d ms\n",
			 pp_tmp);
	}

	thermal = devm_kzalloc(&pdev->dev, sizeof(struct da9062_thermal),
			     GFP_KERNEL);
	if (!thermal) {
		ret = -ENOMEM;
		goto err;
	}

	thermal->config = match->data;

	INIT_DELAYED_WORK(&thermal->work, da9062_thermal_poll_on);
	mutex_init(&thermal->lock);

	thermal->zone = thermal_zone_device_register(thermal->config->name,
					1, 0, thermal,
					&da9062_thermal_ops, NULL, 0,
					0);
	if (IS_ERR(thermal->zone)) {
		dev_err(&pdev->dev, "Cannot register thermal zone device\n");
		ret = PTR_ERR(thermal->zone);
		goto err;
	}

	ret = platform_get_irq_byname(pdev, "THERMAL");
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to get platform IRQ.\n");
		goto err_zone;
	}
	thermal->irq = ret;

	ret = request_threaded_irq(thermal->irq, NULL,
				   da9062_thermal_irq_handler,
				   IRQF_TRIGGER_LOW | IRQF_ONESHOT,
				   "THERMAL", thermal);
	if (ret) {
		dev_err(&pdev->dev,
			"Failed to request thermal device IRQ.\n");
		goto err_zone;
	}

	thermal->hw = chip;
	thermal->polling_period = pp_tmp;
	thermal->mode = THERMAL_DEVICE_ENABLED;
	thermal->dev = &pdev->dev;

	platform_set_drvdata(pdev, thermal);
	return 0;

err_zone:
	thermal_zone_device_unregister(thermal->zone);
err:
	return ret;
}

static int da9062_thermal_remove(struct platform_device *pdev)
{
	struct	da9062_thermal *thermal = platform_get_drvdata(pdev);

	free_irq(thermal->irq, thermal);
	thermal_zone_device_unregister(thermal->zone);
	cancel_delayed_work_sync(&thermal->work);
	return 0;
}

static struct platform_driver da9062_thermal_driver = {
	.probe	= da9062_thermal_probe,
	.remove	= da9062_thermal_remove,
	.driver	= {
		.name	= "da9062-thermal",
		.of_match_table = da9062_compatible_reg_id_table,
	},
};

module_platform_driver(da9062_thermal_driver);

MODULE_AUTHOR("Steve Twiss, Dialog Semiconductor");
MODULE_DESCRIPTION("Thermal TJUNC device driver for Dialog DA9062 and DA9061");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:da9062-thermal");
