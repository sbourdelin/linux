/*
 * Driver for Intel FPGA Management Engine (FME)
 *
 * Copyright (C) 2017 Intel Corporation, Inc.
 *
 * Authors:
 *   Kang Luwei <luwei.kang@intel.com>
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

static ssize_t ports_num_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct feature_fme_header *fme_hdr
		= get_feature_ioaddr_by_index(dev, FME_FEATURE_ID_HEADER);
	struct feature_fme_capability fme_capability;

	fme_capability.csr = readq(&fme_hdr->capability);

	return scnprintf(buf, PAGE_SIZE, "%d\n", fme_capability.num_ports);
}
static DEVICE_ATTR_RO(ports_num);

static ssize_t bitstream_id_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct feature_fme_header *fme_hdr
		= get_feature_ioaddr_by_index(dev, FME_FEATURE_ID_HEADER);
	u64 bitstream_id = readq(&fme_hdr->bitstream_id);

	return scnprintf(buf, PAGE_SIZE, "0x%llx\n",
				(unsigned long long)bitstream_id);
}
static DEVICE_ATTR_RO(bitstream_id);

static ssize_t bitstream_metadata_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct feature_fme_header *fme_hdr
		= get_feature_ioaddr_by_index(dev, FME_FEATURE_ID_HEADER);
	u64 bitstream_md = readq(&fme_hdr->bitstream_md);

	return scnprintf(buf, PAGE_SIZE, "0x%llx\n",
				(unsigned long long)bitstream_md);
}
static DEVICE_ATTR_RO(bitstream_metadata);

static const struct attribute *fme_hdr_attrs[] = {
	&dev_attr_ports_num.attr,
	&dev_attr_bitstream_id.attr,
	&dev_attr_bitstream_metadata.attr,
	NULL,
};

static int fme_hdr_init(struct platform_device *pdev, struct feature *feature)
{
	struct feature_fme_header *fme_hdr = feature->ioaddr;
	int ret;

	dev_dbg(&pdev->dev, "FME HDR Init.\n");
	dev_dbg(&pdev->dev, "FME cap %llx.\n",
				(unsigned long long)fme_hdr->capability.csr);

	ret = sysfs_create_files(&pdev->dev.kobj, fme_hdr_attrs);
	if (ret)
		return ret;

	return 0;
}

static void fme_hdr_uinit(struct platform_device *pdev, struct feature *feature)
{
	dev_dbg(&pdev->dev, "FME HDR UInit.\n");
	sysfs_remove_files(&pdev->dev.kobj, fme_hdr_attrs);
}

struct feature_ops fme_hdr_ops = {
	.init = fme_hdr_init,
	.uinit = fme_hdr_uinit,
};

static struct feature_driver fme_feature_drvs[] = {
	{
		.name = FME_FEATURE_HEADER,
		.ops = &fme_hdr_ops,
	},
	{
		.ops = NULL,
	},
};

static long fme_ioctl_check_extension(struct feature_platform_data *pdata,
				     unsigned long arg)
{
	/* No extension support for now */
	return 0;
}

static int fme_open(struct inode *inode, struct file *filp)
{
	struct platform_device *fdev = fpga_inode_to_feature_dev(inode);
	struct feature_platform_data *pdata = dev_get_platdata(&fdev->dev);
	int ret;

	if (WARN_ON(!pdata))
		return -ENODEV;

	ret = feature_dev_use_begin(pdata);
	if (ret)
		return ret;

	dev_dbg(&fdev->dev, "Device File Open\n");
	filp->private_data = pdata;
	return 0;
}

static int fme_release(struct inode *inode, struct file *filp)
{
	struct feature_platform_data *pdata = filp->private_data;
	struct platform_device *pdev = pdata->dev;

	dev_dbg(&pdev->dev, "Device File Release\n");
	feature_dev_use_end(pdata);
	return 0;
}

static long fme_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct feature_platform_data *pdata = filp->private_data;
	struct platform_device *pdev = pdata->dev;
	struct feature *f;
	long ret;

	dev_dbg(&pdev->dev, "%s cmd 0x%x\n", __func__, cmd);

	switch (cmd) {
	case FPGA_GET_API_VERSION:
		return FPGA_API_VERSION;
	case FPGA_CHECK_EXTENSION:
		return fme_ioctl_check_extension(pdata, arg);
	default:
		/*
		 * Let sub-feature's ioctl function to handle the cmd
		 * Sub-feature's ioctl returns -ENODEV when cmd is not
		 * handled in this sub feature, and returns 0 and other
		 * error code if cmd is handled.
		 */
		fpga_dev_for_each_feature(pdata, f) {
			if (f->ops && f->ops->ioctl) {
				ret = f->ops->ioctl(pdev, f, cmd, arg);
				if (ret == -ENODEV)
					continue;
				else
					return ret;
			}
		}
	}

	return -EINVAL;
}

static const struct file_operations fme_fops = {
	.owner		= THIS_MODULE,
	.open		= fme_open,
	.release	= fme_release,
	.unlocked_ioctl = fme_ioctl,
};

static int fme_probe(struct platform_device *pdev)
{
	int ret;

	ret = fpga_dev_feature_init(pdev, fme_feature_drvs);
	if (ret)
		goto exit;

	ret = fpga_register_dev_ops(pdev, &fme_fops, THIS_MODULE);
	if (ret)
		goto feature_uinit;

	return 0;

feature_uinit:
	fpga_dev_feature_uinit(pdev);
exit:
	return ret;
}

static int fme_remove(struct platform_device *pdev)
{
	fpga_dev_feature_uinit(pdev);
	fpga_unregister_dev_ops(pdev);
	return 0;
}

static struct platform_driver fme_driver = {
	.driver	= {
		.name    = FPGA_FEATURE_DEV_FME,
	},
	.probe   = fme_probe,
	.remove  = fme_remove,
};

module_platform_driver(fme_driver);

MODULE_DESCRIPTION("Intel FPGA Management Engine driver");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS("platform:intel-fpga-fme");
