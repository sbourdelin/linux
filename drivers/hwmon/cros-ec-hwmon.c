/*
 * Copyright (c) 2017, National Instruments Corp.
 *
 * Chromium EC Fan speed and temperature sensor driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/spi/spi.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/of_platform.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/bitops.h>
#include <linux/mfd/cros_ec.h>

struct cros_ec_hwmon_priv {
	struct cros_ec_device *ec;
	struct device *hwmon_dev;

	struct attribute **attrs;

	struct attribute_group attr_group;
	const struct attribute_group *groups[2];
};

#define KELVIN_TO_MILLICELSIUS(x) (((x) - 273) * 1000)

static int __cros_ec_hwmon_probe_fans(struct cros_ec_hwmon_priv *priv)
{
	int err, idx;
	uint16_t data;

	for (idx = 0; idx < EC_FAN_SPEED_ENTRIES; idx++) {
		err = cros_ec_read_mapped_mem16(priv->ec,
					       EC_MEMMAP_FAN + 2 * idx,
					       &data);
		if (err)
			return err;

		if (data == EC_FAN_SPEED_NOT_PRESENT)
			break;
	}

	return idx;
}

static int __cros_ec_hwmon_probe_temps(struct cros_ec_hwmon_priv *priv)
{
	uint8_t data;
	int err, idx;

	err = cros_ec_read_mapped_mem8(priv->ec, EC_MEMMAP_THERMAL_VERSION,
				       &data);

	/* if we have a read error, or EC_MEMMAP_THERMAL_VERSION is not set,
	 * most likely we don't have temperature sensors ...
	 */
	if (err || !data)
		return 0;

	for (idx = 0; idx < EC_TEMP_SENSOR_ENTRIES; idx++) {
		err = cros_ec_read_mapped_mem8(priv->ec,
					       EC_MEMMAP_TEMP_SENSOR + idx,
					       &data);
		if (err)
			return idx;

		/* this assumes that they're all good up to idx */
		switch (data) {
		case EC_TEMP_SENSOR_NOT_PRESENT:
		case EC_TEMP_SENSOR_ERROR:
		case EC_TEMP_SENSOR_NOT_POWERED:
		case EC_TEMP_SENSOR_NOT_CALIBRATED:
			return idx;
		default:
			continue;
		};
	}

	return idx;
}

static ssize_t cros_ec_hwmon_read_fan_rpm(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	uint16_t data;
	int err;
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	struct cros_ec_hwmon_priv *priv = dev_get_drvdata(dev);

	err = cros_ec_read_mapped_mem16(priv->ec,
					EC_MEMMAP_FAN + 2 * sattr->index,
					&data);
	if (err)
		return err;

	return sprintf(buf, "%d\n", data);
}

static ssize_t cros_ec_hwmon_read_temp(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	uint8_t data;
	int err, tmp;

	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	struct cros_ec_hwmon_priv *priv = dev_get_drvdata(dev);

	err = cros_ec_read_mapped_mem8(priv->ec,
				       EC_MEMMAP_TEMP_SENSOR + 1 * sattr->index,
				       &data);
	if (err)
		return err;

	switch (data) {
	case EC_TEMP_SENSOR_NOT_PRESENT:
	case EC_TEMP_SENSOR_ERROR:
	case EC_TEMP_SENSOR_NOT_POWERED:
	case EC_TEMP_SENSOR_NOT_CALIBRATED:
		dev_info(priv->ec->dev, "Failure: result=%d\n", data);
		return -EIO;
	}

	/* make sure we don't overflow when adding offset*/
	tmp = data + EC_TEMP_SENSOR_OFFSET;

	return sprintf(buf, "%d\n", KELVIN_TO_MILLICELSIUS(tmp));
}

static int cros_ec_hwmon_probe(struct platform_device *pdev)
{
	struct cros_ec_device *ec = dev_get_drvdata(pdev->dev.parent);
	struct cros_ec_hwmon_priv *ec_hwmon;
	struct sensor_device_attribute *attr;
	int num_fans, num_temps, i;

	ec_hwmon = devm_kzalloc(&pdev->dev, sizeof(*ec_hwmon), GFP_KERNEL);
	if (!ec_hwmon)
		return -ENOMEM;
	ec_hwmon->ec = ec;

	num_fans = __cros_ec_hwmon_probe_fans(ec_hwmon);
	if (num_fans < 0)
		return num_fans;

	num_temps = __cros_ec_hwmon_probe_temps(ec_hwmon);
	if (num_fans < 0)
		return num_temps;

	ec_hwmon->attrs = devm_kzalloc(&pdev->dev,
				       sizeof(*ec_hwmon->attrs) *
				       (num_fans + num_temps + 1),
				       GFP_KERNEL);
	if (!ec_hwmon->attrs)
		return -ENOMEM;

	for (i = 0; i < num_fans; i++) {
		attr = devm_kzalloc(&pdev->dev, sizeof(*attr), GFP_KERNEL);
		if (!attr)
			return -ENOMEM;
		sysfs_attr_init(&attr->dev_attr.attr);
		attr->dev_attr.attr.name = devm_kasprintf(&pdev->dev,
							  GFP_KERNEL,
							  "fan%d_input",
							  i);
		if (!attr->dev_attr.attr.name)
			return -ENOMEM;

		attr->dev_attr.show = cros_ec_hwmon_read_fan_rpm;
		attr->dev_attr.attr.mode = S_IRUGO;
		attr->index = i;
		ec_hwmon->attrs[i] = &attr->dev_attr.attr;

	}

	for (i = 0; i < num_temps; i++) {
		attr = devm_kzalloc(&pdev->dev, sizeof(*attr), GFP_KERNEL);
		if (!attr)
			return -ENOMEM;
		sysfs_attr_init(&attr->dev_attr.attr);
		attr->dev_attr.attr.name = devm_kasprintf(&pdev->dev,
							  GFP_KERNEL,
							  "temp%d_input",
							  i);
		if (!attr->dev_attr.attr.name)
			return -ENOMEM;

		attr->dev_attr.show = cros_ec_hwmon_read_temp;
		attr->dev_attr.attr.mode = S_IRUGO;
		attr->index = i;
		ec_hwmon->attrs[i + num_fans] = &attr->dev_attr.attr;

	}

	ec_hwmon->attr_group.attrs = ec_hwmon->attrs;
	ec_hwmon->groups[0] = &ec_hwmon->attr_group;

	ec_hwmon->hwmon_dev = devm_hwmon_device_register_with_groups(&pdev->dev,
		    "ec_hwmon", ec_hwmon, ec_hwmon->groups);

	if (IS_ERR(ec_hwmon->hwmon_dev))
		return PTR_ERR(ec_hwmon->hwmon_dev);

	platform_set_drvdata(pdev, ec_hwmon);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id cros_ec_hwmon_of_match[] = {
	{ .compatible = "google,cros-ec-hwmon" },
	{},
};
MODULE_DEVICE_TABLE(of, cros_ec_hwmon_of_match);
#endif

static struct platform_driver cros_ec_hwmon_driver = {
	.probe = cros_ec_hwmon_probe,
	.driver = {
		.name = "cros-ec-hwmon",
		.of_match_table = of_match_ptr(cros_ec_hwmon_of_match),
	},
};
module_platform_driver(cros_ec_hwmon_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("ChromeOS EC Hardware Monitor driver");
MODULE_ALIAS("platform:cros-ec-hwmon");
MODULE_AUTHOR("Moritz Fischer <mdf@kernel.org>");
