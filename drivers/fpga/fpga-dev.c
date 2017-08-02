/*
 * FPGA Bus Device Framework Driver
 *
 * Copyright (C) 2017 Intel Corporation, Inc.
 *
 * This work is licensed under the terms of the GNU GPL version 2. See
 * the COPYING file in the top-level directory.
 */
#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fpga/fpga-dev.h>

static DEFINE_IDA(fpga_dev_ida);
static bool is_bus_registered;

static void fpga_dev_release(struct device *dev)
{
	struct fpga_dev *fdev = to_fpga_dev(dev);

	ida_simple_remove(&fpga_dev_ida, fdev->dev.id);
	kfree(fdev);
}

static const struct device_type fpga_dev_type = {
	.name		= "fpga_dev",
	.release	= fpga_dev_release,
};

static ssize_t name_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct fpga_dev *fdev = to_fpga_dev(dev);

	return sprintf(buf, "%s\n", fdev->name);
}
static DEVICE_ATTR_RO(name);

static struct attribute *fpga_dev_attrs[] = {
	&dev_attr_name.attr,
	NULL,
};
ATTRIBUTE_GROUPS(fpga_dev);

static struct bus_type fpga_bus_type = {
	.name		= "fpga",
};

/**
 * fpga_dev_create - create a fpga device on fpga bus
 * @parent: parent device
 * @name: fpga bus device name
 *
 * Return fpga_dev struct for success, error code otherwise.
 */
struct fpga_dev *fpga_dev_create(struct device *parent, const char *name)
{
	struct fpga_dev *fdev;
	int id, ret = 0;

	if (WARN_ON(!is_bus_registered))
		return ERR_PTR(-ENODEV);

	if (!name || !strlen(name)) {
		dev_err(parent, "Attempt to register with no name!\n");
		return ERR_PTR(-EINVAL);
	}

	fdev = kzalloc(sizeof(*fdev), GFP_KERNEL);
	if (!fdev)
		return ERR_PTR(-ENOMEM);

	id = ida_simple_get(&fpga_dev_ida, 0, 0, GFP_KERNEL);
	if (id < 0) {
		ret = id;
		goto error_kfree;
	}

	fdev->name = name;

	device_initialize(&fdev->dev);
	fdev->dev.type = &fpga_dev_type;
	fdev->dev.bus = &fpga_bus_type;
	fdev->dev.groups = fpga_dev_groups;
	fdev->dev.parent = parent;
	fdev->dev.id = id;

	ret = dev_set_name(&fdev->dev, "fpga.%d", id);
	if (ret)
		goto error_device;

	ret = device_add(&fdev->dev);
	if (ret)
		goto error_device;

	dev_dbg(fdev->dev.parent, "fpga bus device [%s] created\n", fdev->name);

	return fdev;

error_device:
	ida_simple_remove(&fpga_dev_ida, id);
error_kfree:
	kfree(fdev);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(fpga_dev_create);

static int __init fpga_bus_init(void)
{
	int ret;

	pr_info("FPGA Bus Device Framework\n");

	ret = bus_register(&fpga_bus_type);
	if (ret)
		return ret;

	is_bus_registered = true;
	return 0;
}

static void __exit fpga_bus_exit(void)
{
	bus_unregister(&fpga_bus_type);
}

MODULE_DESCRIPTION("FPGA Bus Device Framework");
MODULE_LICENSE("GPL v2");

subsys_initcall(fpga_bus_init);
module_exit(fpga_bus_exit);
