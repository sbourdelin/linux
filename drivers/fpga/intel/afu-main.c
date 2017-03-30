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
#include <linux/uaccess.h>
#include <linux/intel-fpga.h>

#include "afu.h"

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

static ssize_t
afu_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct feature_platform_data *pdata = dev_get_platdata(dev);
	struct feature_port_header *port_hdr =
			get_feature_ioaddr_by_index(dev, PORT_FEATURE_ID_UAFU);
	u64 guidl;
	u64 guidh;

	mutex_lock(&pdata->lock);
	guidl = readq(&port_hdr->afu_header.guid.b[0]);
	guidh = readq(&port_hdr->afu_header.guid.b[8]);
	mutex_unlock(&pdata->lock);

	return scnprintf(buf, PAGE_SIZE, "%016llx%016llx\n", guidh, guidl);
}
static DEVICE_ATTR_RO(afu_id);

static const struct attribute *port_uafu_attrs[] = {
	&dev_attr_afu_id.attr,
	NULL
};

static int port_uafu_init(struct platform_device *pdev, struct feature *feature)
{
	struct resource *res = &pdev->resource[feature->resource_index];
	u32 flags = FPGA_REGION_READ | FPGA_REGION_WRITE | FPGA_REGION_MMAP;
	int ret;

	dev_dbg(&pdev->dev, "PORT AFU Init.\n");

	ret = afu_region_add(dev_get_platdata(&pdev->dev),
			     FPGA_PORT_INDEX_UAFU, resource_size(res),
			     res->start, flags);
	if (ret)
		return ret;

	return sysfs_create_files(&pdev->dev.kobj, port_uafu_attrs);
}

static void port_uafu_uinit(struct platform_device *pdev,
					struct feature *feature)
{
	dev_dbg(&pdev->dev, "PORT AFU UInit.\n");

	sysfs_remove_files(&pdev->dev.kobj, port_uafu_attrs);
}

struct feature_ops port_uafu_ops = {
	.init = port_uafu_init,
	.uinit = port_uafu_uinit,
};

static struct feature_driver port_feature_drvs[] = {
	{
		.name = PORT_FEATURE_HEADER,
		.ops = &port_hdr_ops,
	},
	{
		.name = PORT_FEATURE_UAFU,
		.ops = &port_uafu_ops,
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

static long
afu_ioctl_get_info(struct feature_platform_data *pdata, void __user *arg)
{
	struct fpga_port_info info;
	struct fpga_afu *afu;
	unsigned long minsz;

	minsz = offsetofend(struct fpga_port_info, num_umsgs);

	if (copy_from_user(&info, arg, minsz))
		return -EFAULT;

	if (info.argsz < minsz)
		return -EINVAL;

	mutex_lock(&pdata->lock);
	afu = fpga_pdata_get_private(pdata);
	info.flags = 0;
	info.num_regions = afu->num_regions;
	info.num_umsgs = afu->num_umsgs;
	mutex_unlock(&pdata->lock);

	if (copy_to_user(arg, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

static long
afu_ioctl_get_region_info(struct feature_platform_data *pdata, void __user *arg)
{
	struct fpga_port_region_info rinfo;
	struct fpga_afu_region region;
	unsigned long minsz;
	long ret;

	minsz = offsetofend(struct fpga_port_region_info, offset);

	if (copy_from_user(&rinfo, arg, minsz))
		return -EFAULT;

	if (rinfo.argsz < minsz || rinfo.padding)
		return -EINVAL;

	ret = afu_get_region_by_index(pdata, rinfo.index, &region);
	if (ret)
		return ret;

	rinfo.flags = region.flags;
	rinfo.size = region.size;
	rinfo.offset = region.offset;

	if (copy_to_user(arg, &rinfo, sizeof(rinfo)))
		return -EFAULT;

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
	case FPGA_PORT_GET_INFO:
		return afu_ioctl_get_info(pdata, (void __user *)arg);
	case FPGA_PORT_GET_REGION_INFO:
		return afu_ioctl_get_region_info(pdata, (void __user *)arg);
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

static int afu_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct fpga_afu_region region;
	struct platform_device *pdev = filp->private_data;
	struct feature_platform_data *pdata = dev_get_platdata(&pdev->dev);
	u64 size = vma->vm_end - vma->vm_start;
	u64 offset;
	int ret;

	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;

	offset = vma->vm_pgoff << PAGE_SHIFT;
	ret = afu_get_region_by_offset(pdata, offset, size, &region);
	if (ret)
		return ret;

	if (!(region.flags & FPGA_REGION_MMAP))
		return -EINVAL;

	if ((vma->vm_flags & VM_READ) && !(region.flags & FPGA_REGION_READ))
		return -EPERM;

	if ((vma->vm_flags & VM_WRITE) && !(region.flags & FPGA_REGION_WRITE))
		return -EPERM;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start,
			(region.phys + (offset - region.offset)) >> PAGE_SHIFT,
			size, vma->vm_page_prot);
}

static const struct file_operations afu_fops = {
	.owner = THIS_MODULE,
	.open = afu_open,
	.release = afu_release,
	.unlocked_ioctl = afu_ioctl,
	.mmap = afu_mmap,
};

static int afu_dev_init(struct platform_device *pdev)
{
	struct fpga_afu *afu;
	struct feature_platform_data *pdata = dev_get_platdata(&pdev->dev);

	afu = devm_kzalloc(&pdev->dev, sizeof(*afu), GFP_KERNEL);
	if (!afu)
		return -ENOMEM;

	afu->pdata = pdata;

	mutex_lock(&pdata->lock);
	fpga_pdata_set_private(pdata, afu);
	afu_region_init(pdata);
	mutex_unlock(&pdata->lock);
	return 0;
}

static int afu_dev_destroy(struct platform_device *pdev)
{
	struct feature_platform_data *pdata = dev_get_platdata(&pdev->dev);
	struct fpga_afu *afu;

	mutex_lock(&pdata->lock);
	afu = fpga_pdata_get_private(pdata);
	afu_region_destroy(pdata);
	fpga_pdata_set_private(pdata, NULL);
	mutex_unlock(&pdata->lock);

	devm_kfree(&pdev->dev, afu);
	return 0;
}

static int afu_probe(struct platform_device *pdev)
{
	int ret;

	dev_dbg(&pdev->dev, "%s\n", __func__);

	ret = afu_dev_init(pdev);
	if (ret)
		goto exit;

	ret = fpga_dev_feature_init(pdev, port_feature_drvs);
	if (ret)
		goto dev_destroy;

	ret = fpga_register_dev_ops(pdev, &afu_fops, THIS_MODULE);
	if (ret) {
		fpga_dev_feature_uinit(pdev);
		goto dev_destroy;
	}

	return 0;

dev_destroy:
	afu_dev_destroy(pdev);
exit:
	return ret;
}

static int afu_remove(struct platform_device *pdev)
{
	dev_dbg(&pdev->dev, "%s\n", __func__);

	fpga_dev_feature_uinit(pdev);
	fpga_unregister_dev_ops(pdev);
	afu_dev_destroy(pdev);
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
