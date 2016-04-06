/*
 * Generic ADC thermal driver
 *
 * Copyright (C) 2016 NVIDIA CORPORATION. All rights reserved.
 *
 * Author: Laxman Dewangan <ldewangan@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/iio/consumer.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/thermal.h>

struct gadc_thermal_info {
	struct device *dev;
	struct thermal_zone_device *tz_dev;
	struct iio_channel *channel;
	s32 lower_temp;
	s32 upper_temp;
	u32 step_temp;
	int nsteps;
	u32 *lookup_table;
};

static int gadc_thermal_read_channel(struct gadc_thermal_info *gti, int *val)
{
	int ret;

	ret = iio_read_channel_processed(gti->channel, val);
	if (ret < 0)
		ret = iio_read_channel_raw(gti->channel, val);
	if (ret < 0)
		return ret;

	return ret;
}

static int gadc_thermal_adc_to_temp(struct gadc_thermal_info *gti, int val)
{
	int temp, adc_hi, adc_lo;
	int i;

	for (i = 0; i < gti->nsteps; i++) {
		if (val >= gti->lookup_table[i])
			break;
	}

	if (i == 0) {
		temp = gti->lower_temp;
	} else if (i >= (gti->nsteps - 1)) {
		temp = gti->upper_temp;
	} else {
		adc_hi = gti->lookup_table[i - 1];
		adc_lo = gti->lookup_table[i];
		temp = (gti->lower_temp + i * gti->step_temp);
		temp -= ((val - adc_lo) * 1000) / (adc_hi - adc_lo);
	}

	return temp;
}

static int gadc_thermal_get_temp(void *data, int *temp)
{
	struct gadc_thermal_info *gti = data;
	int val;
	int ret;

	ret = gadc_thermal_read_channel(gti, &val);
	if (ret < 0) {
		dev_err(gti->dev, "IIO channel read failed %d\n", ret);
		return ret;
	}
	*temp = gadc_thermal_adc_to_temp(gti, val);

	return 0;
}

static const struct thermal_zone_of_device_ops gadc_thermal_ops = {
	.get_temp = gadc_thermal_get_temp,
};

static int gadc_thermal_read_linear_lookup_table(struct device *dev,
						 struct gadc_thermal_info *gti)
{
	struct device_node *np = dev->of_node;
	u32 range_temp;
	int ret;

	ret = of_property_read_s32(np, "lower-temperature", &gti->lower_temp);
	if (ret < 0) {
		dev_err(dev, "Lower temp not found\n");
		return ret;
	}

	ret = of_property_read_s32(np, "upper-temperature", &gti->upper_temp);
	if (ret < 0) {
		dev_err(dev, "Upper temp not found\n");
		return ret;
	}

	ret = of_property_read_u32(np, "step-temperature", &gti->step_temp);
	if (ret < 0) {
		dev_err(dev, "Steps temp not found\n");
		return ret;
	}

	range_temp = abs(gti->upper_temp - gti->lower_temp);
	if (range_temp % gti->step_temp) {
		dev_err(dev, "Steps does not meet with lower/upper\n");
		return -EINVAL;
	}

	gti->nsteps = range_temp / gti->step_temp;

	gti->lookup_table = devm_kzalloc(dev, sizeof(*gti->lookup_table) *
					 gti->nsteps, GFP_KERNEL);
	if (!gti->lookup_table)
		return -ENOMEM;

	ret = of_property_read_u32_array(np, "temperature-lookup-table",
					 gti->lookup_table, gti->nsteps);
	if (ret < 0) {
		dev_err(dev, "Failed to read temperature lookup table: %d\n",
			ret);
		return ret;
	}

	return 0;
}

static int gadc_thermal_probe(struct platform_device *pdev)
{
	struct gadc_thermal_info *gti;
	int ret;

	if (!pdev->dev.of_node) {
		dev_err(&pdev->dev, "Only DT based supported\n");
		return -ENODEV;
	}

	gti = devm_kzalloc(&pdev->dev, sizeof(*gti), GFP_KERNEL);
	if (!gti)
		return -ENOMEM;

	ret = gadc_thermal_read_linear_lookup_table(&pdev->dev, gti);
	if (ret < 0)
		return ret;

	gti->dev = &pdev->dev;
	platform_set_drvdata(pdev, gti);

	gti->channel = iio_channel_get(&pdev->dev, "sensor-channel");
	if (IS_ERR(gti->channel)) {
		ret = PTR_ERR(gti->channel);
		dev_err(&pdev->dev, "IIO channel not found: %d\n", ret);
		return ret;
	}

	gti->tz_dev = devm_thermal_zone_of_sensor_register(&pdev->dev, 0,
				gti, &gadc_thermal_ops);
	if (IS_ERR(gti->tz_dev)) {
		ret = PTR_ERR(gti->tz_dev);
		dev_err(&pdev->dev, "Thermal zone sensor register failed: %d\n",
			ret);
		goto sensor_fail;
	}

	return 0;

sensor_fail:
	iio_channel_release(gti->channel);
	return ret;
}

static int gadc_thermal_remove(struct platform_device *pdev)
{
	struct gadc_thermal_info *gti = platform_get_drvdata(pdev);

	iio_channel_release(gti->channel);

	return 0;
}

static const struct of_device_id of_adc_thermal_match[] = {
	{ .compatible = "generic-adc-thermal", },
	{},
};
MODULE_DEVICE_TABLE(of, of_adc_thermal_match);

static struct platform_driver gadc_thermal_driver = {
	.driver = {
		.name = "generic-adc-thermal",
		.of_match_table = of_adc_thermal_match,
	},
	.probe = gadc_thermal_probe,
	.remove = gadc_thermal_remove,
};

module_platform_driver(gadc_thermal_driver);

MODULE_AUTHOR("Laxman Dewangan <ldewangan@nvidia.com>");
MODULE_DESCRIPTION("Generic ADC thermal driver using IIO framework with DT");
MODULE_LICENSE("GPL v2");
