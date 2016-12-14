/*
 * Eberspächer Flexcard PMC II Misc Device
 *
 * Copyright (c) 2014 - 2016, Linutronix GmbH
 * Author: Benedikt Spranger <b.spranger@linutronix.de>
 *         Holger Dengler <dengler@linutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

#include <linux/mfd/flexcard.h>

#define FLEXCARD_MAX_NAME	16

struct flexcard_misc {
	char				name[FLEXCARD_MAX_NAME];
	struct miscdevice		dev;
	struct platform_device		*pdev;
	struct fc_bar0_conf __iomem	*conf;
	struct fc_bar0_nf __iomem	*nf;
};

static ssize_t fw_version_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct flexcard_misc *priv = dev_get_drvdata(dev);
	union {
		struct fc_version ver;
		u32 reg;
	} fw_ver;

	fw_ver.reg = readl(&priv->conf->fc_fw_ver);
	return sprintf(buf, "%02x.%02x.%02x\n",
		       fw_ver.ver.maj, fw_ver.ver.min, fw_ver.ver.dev);
}
static DEVICE_ATTR_RO(fw_version);

static ssize_t hw_version_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct flexcard_misc *priv = dev_get_drvdata(dev);
	union {
		struct fc_version ver;
		u32 reg;
	} hw_ver;

	hw_ver.reg = readl(&priv->conf->fc_hw_ver);
	return sprintf(buf, "%02x.%02x.%02x\n",
		       hw_ver.ver.maj, hw_ver.ver.min, hw_ver.ver.dev);
}
static DEVICE_ATTR_RO(hw_version);

static ssize_t serialno_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct flexcard_misc *priv = dev_get_drvdata(dev);
	u64 fc_sn;

	fc_sn = readq(&priv->conf->fc_sn);
	return sprintf(buf, "%lld\n", fc_sn);
}
static DEVICE_ATTR_RO(serialno);

static ssize_t tiny_stat_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct flexcard_misc *priv = dev_get_drvdata(dev);

	return sprintf(buf, "0x%x\n", readl(&priv->conf->tiny_stat));
}
static DEVICE_ATTR_RO(tiny_stat);

static ssize_t can_dat_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct flexcard_misc *priv = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", readl(&priv->conf->can_dat_cnt));
}
static DEVICE_ATTR_RO(can_dat);

static ssize_t can_err_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct flexcard_misc *priv = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", readl(&priv->conf->can_err_cnt));
}
static DEVICE_ATTR_RO(can_err);

static ssize_t fc_data_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct flexcard_misc *priv = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", readl(&priv->conf->fc_data_cnt));
}
static DEVICE_ATTR_RO(fc_data);

static ssize_t fr_rx_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct flexcard_misc *priv = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", readl(&priv->conf->fr_rx_cnt));
}
static DEVICE_ATTR_RO(fr_rx);

static ssize_t fr_tx_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct flexcard_misc *priv = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", readl(&priv->conf->fr_tx_cnt));
}
static DEVICE_ATTR_RO(fr_tx);

static ssize_t nmv_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct flexcard_misc *priv = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", readl(&priv->conf->nmv_cnt));
}
static DEVICE_ATTR_RO(nmv);

static ssize_t info_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct flexcard_misc *priv = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", readl(&priv->conf->info_cnt));
}
static DEVICE_ATTR_RO(info);

static ssize_t stat_trg_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct flexcard_misc *priv = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", readl(&priv->conf->stat_trg_cnt));
}
static DEVICE_ATTR_RO(stat_trg);

static ssize_t nf_show(struct device *dev,
		       struct device_attribute *attr, char *buf)
{
	struct flexcard_misc *priv = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", readl(&priv->nf->nf_cnt));
}
static DEVICE_ATTR_RO(nf);

static ssize_t uid_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct flexcard_misc *priv = dev_get_drvdata(dev);
	u32 uid;
	int ret;

	ret = kstrtou32(buf, 0, &uid);
	if (ret)
		return ret;

	writel(uid, &priv->conf->fc_uid);
	return count;
}

static ssize_t uid_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct flexcard_misc *priv = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", readl(&priv->conf->fc_uid));
}
static DEVICE_ATTR(uid, 0644, uid_show, uid_store);

static struct attribute *flexcard_misc_dev_attrs[] = {
	&dev_attr_fw_version.attr,
	&dev_attr_hw_version.attr,
	&dev_attr_serialno.attr,
	&dev_attr_tiny_stat.attr,
	&dev_attr_can_dat.attr,
	&dev_attr_can_err.attr,
	&dev_attr_fc_data.attr,
	&dev_attr_fr_rx.attr,
	&dev_attr_fr_tx.attr,
	&dev_attr_nmv.attr,
	&dev_attr_info.attr,
	&dev_attr_stat_trg.attr,
	&dev_attr_nf.attr,
	&dev_attr_uid.attr,
	NULL,
};

static const struct attribute_group flexcard_misc_dev_group = {
	.attrs = flexcard_misc_dev_attrs,
};

static int flexcard_misc_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long offset, vsize, psize, addr;
	struct flexcard_misc *priv;
	struct resource *res;

	priv = container_of(filp->private_data, struct flexcard_misc, dev);
	if (!priv)
		return -EINVAL;

	if (vma->vm_flags & (VM_WRITE | VM_EXEC))
		return -EPERM;

	res = platform_get_resource(priv->pdev, IORESOURCE_MEM, 0);
	offset = vma->vm_pgoff << PAGE_SHIFT;
	if (offset > resource_size(res)) {
		dev_err(&priv->pdev->dev,
			"mmap offset out of resource range\n");
		return -EINVAL;
	}

	vsize = vma->vm_end - vma->vm_start;
	psize = round_up(resource_size(res) - offset, PAGE_SIZE);
	addr = (res->start + offset) >> PAGE_SHIFT;
	if (vsize > psize) {
		dev_err(&priv->pdev->dev,
			"requested mmap mapping too large\n");
		return -EINVAL;
	}

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	return io_remap_pfn_range(vma, vma->vm_start, addr, vsize,
				  vma->vm_page_prot);
}

static const struct file_operations flexcard_misc_fops = {
	.owner		= THIS_MODULE,
	.open		= nonseekable_open,
	.mmap		= flexcard_misc_mmap,
	.llseek		= no_llseek,
};

static int flexcard_misc_iomap(struct platform_device *pdev)
{
	struct flexcard_misc *priv = platform_get_drvdata(pdev);
	struct resource *res;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENXIO;

	priv->conf = ioremap_nocache(res->start, resource_size(res));
	if (!priv->conf)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res) {
		ret = -ENXIO;
		goto out;
	}

	priv->nf = ioremap_nocache(res->start, resource_size(res));
	if (!priv->nf) {
		ret = -ENOMEM;
		goto out;
	}

	return 0;
out:
	iounmap(priv->conf);
	return ret;
}

static int flexcard_misc_probe(struct platform_device *pdev)
{
	struct flexcard_misc *priv;
	struct device *this_device;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	ret = flexcard_misc_iomap(pdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to map resource: %d\n", ret);
		return ret;
	}

	snprintf(priv->name, sizeof(priv->name),
		 "flexcard%d", pdev->id);
	priv->dev.name = priv->name;
	priv->dev.minor = MISC_DYNAMIC_MINOR;
	priv->dev.fops = &flexcard_misc_fops;
	priv->dev.parent = &pdev->dev;
	priv->pdev = pdev;

	ret = misc_register(&priv->dev);
	if (ret) {
		dev_err(&pdev->dev, "unable to register miscdevice: %d\n", ret);
		return ret;
	}

	this_device = priv->dev.this_device;
	dev_set_drvdata(this_device, priv);

	ret = sysfs_create_group(&this_device->kobj,
				 &flexcard_misc_dev_group);
	if (ret) {
		dev_err(&pdev->dev,
			"failed to create sysfs attributes: %d\n", ret);
		goto out;
	}

	return 0;

out:
	misc_deregister(&priv->dev);
	return ret;
}

static int flexcard_misc_remove(struct platform_device *pdev)
{
	struct flexcard_misc *priv = platform_get_drvdata(pdev);
	struct device *this_device = priv->dev.this_device;

	sysfs_remove_group(&this_device->kobj,
			   &flexcard_misc_dev_group);
	misc_deregister(&priv->dev);

	return 0;
}

static struct platform_driver flexcard_misc_driver = {
	.probe		= flexcard_misc_probe,
	.remove		= flexcard_misc_remove,
	.driver		= {
		.name   = "flexcard-misc",
	},
};

module_platform_driver(flexcard_misc_driver);

MODULE_AUTHOR("Holger Dengler <dengler@linutronix.de>");
MODULE_AUTHOR("Benedikt Spranger <b.spranger@linutronix.de>");
MODULE_DESCRIPTION("Eberspaecher Flexcard PMC II Misc Driver");
MODULE_LICENSE("GPL v2");
