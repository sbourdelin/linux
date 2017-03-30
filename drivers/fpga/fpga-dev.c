/*
 * FPGA Device Framework Driver
 *
 * Copyright (C) 2017 Intel Corporation, Inc.
 *
 * This work is licensed under a dual BSD/GPLv2 license. When using or
 * redistributing this file, you may do so under either license. See the
 * LICENSE.BSD file under drivers/fpga/intel for the BSD license and see
 * the COPYING file in the top-level directory for the GPLv2 license.
 */
#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fpga/fpga-dev.h>

static DEFINE_IDA(fpga_dev_ida);
static struct class *fpga_dev_class;

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

/**
 * fpga_dev_create - create a fpga device
 * @parent: parent device
 * @name: fpga device name
 *
 * Return fpga_dev struct for success, error code otherwise.
 */
struct fpga_dev *fpga_dev_create(struct device *parent, const char *name)
{
	struct fpga_dev *fdev;
	int id, ret = 0;

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
	fdev->dev.class = fpga_dev_class;
	fdev->dev.parent = parent;
	fdev->dev.id = id;

	ret = dev_set_name(&fdev->dev, "fpga.%d", id);
	if (ret)
		goto error_device;

	ret = device_add(&fdev->dev);
	if (ret)
		goto error_device;

	dev_dbg(fdev->dev.parent, "fpga device [%s] created\n", fdev->name);

	return fdev;

error_device:
	ida_simple_remove(&fpga_dev_ida, id);
error_kfree:
	kfree(fdev);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(fpga_dev_create);

static void fpga_dev_release(struct device *dev)
{
	struct fpga_dev *fdev = to_fpga_dev(dev);

	ida_simple_remove(&fpga_dev_ida, fdev->dev.id);
	kfree(fdev);
}

static int __init fpga_dev_class_init(void)
{
	pr_info("FPGA Device framework\n");

	fpga_dev_class = class_create(THIS_MODULE, "fpga");
	if (IS_ERR(fpga_dev_class))
		return PTR_ERR(fpga_dev_class);

	fpga_dev_class->dev_groups = fpga_dev_groups;
	fpga_dev_class->dev_release = fpga_dev_release;

	return 0;
}

static void __exit fpga_dev_class_exit(void)
{
	class_destroy(fpga_dev_class);
}

MODULE_DESCRIPTION("FPGA Device framework");
MODULE_LICENSE("Dual BSD/GPL");

subsys_initcall(fpga_dev_class_init);
module_exit(fpga_dev_class_exit);
