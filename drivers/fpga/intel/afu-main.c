/*
 * Driver for Intel FPGA Accelerated Function Unit (AFU)
 *
 * Copyright (C) 2017 Intel Corporation, Inc.
 *
 * Authors:
 *   Wu Hao <hao.wu@intel.com>
 *   Xiao Guangrong <guangrong.xiao@linux.intel.com>
 *   Joseph Grecco <joe.grecco@intel.com>
 *   Enno Luebbers <enno.luebbers@intel.com>
 *   Tim Whisonant <tim.whisonant@intel.com>
 *   Ananda Ravuri <ananda.ravuri@intel.com>
 *   Henry Mitchel <henry.mitchel@intel.com>
 *
 * This work is licensed under a dual BSD/GPLv2 license. When using or
 * redistributing this file, you may do so under either license. See the
 * LICENSE.BSD file under this directory for the BSD license and see
 * the COPYING file in the top-level directory for the GPLv2 license.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/intel-fpga.h>

#include "feature-dev.h"

static ssize_t
id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int id = fpga_port_id(to_platform_device(dev));

	return scnprintf(buf, PAGE_SIZE, "%d\n", id);
}
static DEVICE_ATTR_RO(id);

static const struct attribute *port_hdr_attrs[] = {
	&dev_attr_id.attr,
	NULL,
};

static int port_hdr_init(struct platform_device *pdev, struct feature *feature)
{
	dev_dbg(&pdev->dev, "PORT HDR Init.\n");

	fpga_port_reset(pdev);

	return sysfs_create_files(&pdev->dev.kobj, port_hdr_attrs);
}

static void port_hdr_uinit(struct platform_device *pdev,
					struct feature *feature)
{
	dev_dbg(&pdev->dev, "PORT HDR UInit.\n");

	sysfs_remove_files(&pdev->dev.kobj, port_hdr_attrs);
}

static long
port_hdr_ioctl(struct platform_device *pdev, struct feature *feature,
					unsigned int cmd, unsigned long arg)
{
	long ret;

	switch (cmd) {
	case FPGA_PORT_RESET:
		if (!arg)
			ret = fpga_port_reset(pdev);
		else
			ret = -EINVAL;
		break;
	default:
		dev_dbg(&pdev->dev, "%x cmd not handled", cmd);
		ret = -ENODEV;
	}

	return ret;
}

struct feature_ops port_hdr_ops = {
	.init = port_hdr_init,
	.uinit = port_hdr_uinit,
	.ioctl = port_hdr_ioctl,
};

static struct feature_driver port_feature_drvs[] = {
	{
		.name = PORT_FEATURE_HEADER,
		.ops = &port_hdr_ops,
	},
	{
		.ops = NULL,
	}
};

static int afu_open(struct inode *inode, struct file *filp)
{
	struct platform_device *fdev = fpga_inode_to_feature_dev(inode);
	struct feature_platform_data *pdata;
	int ret;

	pdata = dev_get_platdata(&fdev->dev);
	if (WARN_ON(!pdata))
		return -ENODEV;

	ret = feature_dev_use_begin(pdata);
	if (ret)
		return ret;

	dev_dbg(&fdev->dev, "Device File Open\n");
	filp->private_data = fdev;

	return 0;
}

static int afu_release(struct inode *inode, struct file *filp)
{
	struct platform_device *pdev = filp->private_data;
	struct feature_platform_data *pdata = dev_get_platdata(&pdev->dev);

	dev_dbg(&pdev->dev, "Device File Release\n");

	fpga_port_reset(pdev);
	feature_dev_use_end(pdata);
	return 0;
}

static long afu_ioctl_check_extension(struct feature_platform_data *pdata,
				     unsigned long arg)
{
	/* No extension support for now */
	return 0;
}

static long afu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct platform_device *pdev = filp->private_data;
	struct feature_platform_data *pdata = dev_get_platdata(&pdev->dev);
	struct feature *f;
	long ret;

	dev_dbg(&pdev->dev, "%s cmd 0x%x\n", __func__, cmd);

	switch (cmd) {
	case FPGA_GET_API_VERSION:
		return FPGA_API_VERSION;
	case FPGA_CHECK_EXTENSION:
		return afu_ioctl_check_extension(pdata, arg);
	default:
		/*
		 * Let sub-feature's ioctl function to handle the cmd
		 * Sub-feature's ioctl returns -ENODEV when cmd is not
		 * handled in this sub feature, and returns 0 and other
		 * error code if cmd is handled.
		 */
		fpga_dev_for_each_feature(pdata, f)
			if (f->ops && f->ops->ioctl) {
				ret = f->ops->ioctl(pdev, f, cmd, arg);
				if (ret == -ENODEV)
					continue;
				else
					return ret;
			}
	}

	return -EINVAL;
}

static const struct file_operations afu_fops = {
	.owner = THIS_MODULE,
	.open = afu_open,
	.release = afu_release,
	.unlocked_ioctl = afu_ioctl,
};

static int afu_probe(struct platform_device *pdev)
{
	int ret;

	dev_dbg(&pdev->dev, "%s\n", __func__);

	ret = fpga_dev_feature_init(pdev, port_feature_drvs);
	if (ret)
		return ret;

	ret = fpga_register_dev_ops(pdev, &afu_fops, THIS_MODULE);
	if (ret)
		fpga_dev_feature_uinit(pdev);

	return ret;
}

static int afu_remove(struct platform_device *pdev)
{
	dev_dbg(&pdev->dev, "%s\n", __func__);

	fpga_dev_feature_uinit(pdev);
	fpga_unregister_dev_ops(pdev);
	return 0;
}

static struct platform_driver afu_driver = {
	.driver	= {
		.name    = "intel-fpga-port",
	},
	.probe   = afu_probe,
	.remove  = afu_remove,
};

module_platform_driver(afu_driver);

MODULE_DESCRIPTION("Intel FPGA Accelerated Function Unit driver");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS("platform:intel-fpga-port");
