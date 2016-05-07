/*
 * dptf_power:  DPTF platform power driver
 * Copyright (c) 2016, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/platform_device.h>
#include "battery.h"

#define DPTF_POWER_SHOW(name, object) \
static ssize_t name##_show(struct device *dev,\
			   struct device_attribute *attr,\
			   char *buf)\
{\
	struct acpi_device *acpi_dev = to_acpi_device(dev);\
	unsigned long long val;\
	acpi_status status;\
\
	status = acpi_evaluate_integer(acpi_dev->handle, #object,\
				       NULL, &val);\
	if (ACPI_SUCCESS(status))\
		return sprintf(buf, "%llu\n", val);\
	else\
		return -EINVAL;\
}

DPTF_POWER_SHOW(max_platform_power, PMAX)
DPTF_POWER_SHOW(platform_power_source, PSRC)
DPTF_POWER_SHOW(adapter_rating, ARTG)
DPTF_POWER_SHOW(charger_type, CTYP)
DPTF_POWER_SHOW(battery_steady_power, PBSS)
DPTF_POWER_SHOW(power_sampling_period, DPSP)


static DEVICE_ATTR_RO(max_platform_power);
static DEVICE_ATTR_RO(platform_power_source);
static DEVICE_ATTR_RO(adapter_rating);
static DEVICE_ATTR_RO(battery_steady_power);
static DEVICE_ATTR_RO(power_sampling_period);
static DEVICE_ATTR_RO(charger_type);

static struct attribute *dptf_power_attrs[] = {
	&dev_attr_max_platform_power.attr,
	&dev_attr_platform_power_source.attr,
	&dev_attr_adapter_rating.attr,
	&dev_attr_charger_type.attr,
	&dev_attr_battery_steady_power.attr,
	&dev_attr_power_sampling_period.attr,
	NULL
};

static struct attribute_group dptf_power_attribute_group = {
	.attrs = dptf_power_attrs,
	.name = "dptf_power"
};

static void dptf_power_notify(acpi_handle handle, u32 event, void *data)
{
	struct acpi_device *device = data;

	acpi_battery_common_notify(device, event);
}

static int dptf_power_add(struct platform_device *pdev)
{
	struct acpi_device *acpi_dev;
	acpi_status status;
	unsigned long long ptype;
	int result;

#if IS_ENABLED(CONFIG_ACPI_BATTERY)
	/* Don't register two battery devices with power supply class */
	return -ENODEV;
#endif
	acpi_dev = ACPI_COMPANION(&(pdev->dev));
	if (!acpi_dev)
		return -ENODEV;

	status = acpi_evaluate_integer(acpi_dev->handle, "PTYP", NULL, &ptype);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	if (ptype != 0x11)
		return -ENODEV;


	result = acpi_battery_common_add(acpi_dev);
	if (result)
		return result;

	result = sysfs_create_group(&acpi_dev->dev.kobj,
				    &dptf_power_attribute_group);
	if (result)
		goto error_remove_battery;

	result = acpi_install_notify_handler(acpi_dev->handle,
					     ACPI_DEVICE_NOTIFY,
					     dptf_power_notify,
					     (void *)acpi_dev);
	if (result)
		goto error_remove_sysfs;

	platform_set_drvdata(pdev, acpi_dev);

	return 0;

error_remove_sysfs:
	sysfs_remove_group(&acpi_dev->dev.kobj, &dptf_power_attribute_group);
error_remove_battery:
	acpi_battery_common_remove(acpi_dev);
	return result;
}

static int dptf_power_remove(struct platform_device *pdev)
{
	struct acpi_device *acpi_dev = platform_get_drvdata(pdev);

	acpi_remove_notify_handler(acpi_dev->handle, ACPI_DEVICE_NOTIFY,
				   dptf_power_notify);
	sysfs_remove_group(&acpi_dev->dev.kobj, &dptf_power_attribute_group);
	acpi_battery_common_remove(acpi_dev);

	return 0;
}

static const struct acpi_device_id int3407_device_ids[] = {
	{"INT3407", 0},
	{"", 0},
};
MODULE_DEVICE_TABLE(acpi, int3407_device_ids);

static struct platform_driver dptf_power_driver = {
	.probe = dptf_power_add,
	.remove = dptf_power_remove,
	.driver = {
		.name = "DPTF Platform Power",
		.acpi_match_table = int3407_device_ids,
	},
};

module_platform_driver(dptf_power_driver);

MODULE_AUTHOR("Srinivas Pandruvada <srinivas.pandruvada@linux.intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("ACPI DPTF platform power driver");
