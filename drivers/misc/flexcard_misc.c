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

	return 0;
}

static int flexcard_misc_remove(struct platform_device *pdev)
{
	struct flexcard_misc *priv = platform_get_drvdata(pdev);

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
