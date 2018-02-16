// SPDX-License-Identifier: GPL-2.0
/**
 * pci-debugfs.c - DesignWare USB3 DRD Controller PCI DebugFS file
 *
 * Copyright (C) 2018 Synopsys, Inc.
 *
 * Authors: Felipe Balbi <balbi@ti.com>,
 *	    Sebastian Andrzej Siewior <bigeasy@linutronix.de>
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/delay.h>
#include <linux/uaccess.h>

#include <linux/usb/ch9.h>

#include "dwc3-pci.h"

static void dwc3_params_init(struct dwc3_pci *dwc)
{
	dwc->params.maximum_speed = USB_SPEED_UNKNOWN;
}

static int dwc3_param_set_maxspeed(struct dwc3_pci *dwc)
{
	struct device *dev = &dwc->pci->dev;
	struct dwc3_params *params = &dwc->params;
	const char *speed = usb_speed_string(params->maximum_speed);
	struct property_entry property = PROPERTY_ENTRY_STRING("maximum-speed",
							       speed);

	switch (params->maximum_speed) {
	case USB_SPEED_LOW:
	case USB_SPEED_FULL:
	case USB_SPEED_HIGH:
	case USB_SPEED_SUPER:
	case USB_SPEED_SUPER_PLUS:
		return dwc3_pci_add_one_property(dwc, property);
	default:
		dev_err(dev, "Invalid speed: %d\n", params->maximum_speed);
		return -EINVAL;
	}

	return 0;
}

static int dwc3_params_set(struct dwc3_pci *dwc)
{
	int ret;
	struct dwc3_params *params = &dwc->params;

	if (params->maximum_speed != USB_SPEED_UNKNOWN) {
		ret = dwc3_param_set_maxspeed(dwc);
		if (ret)
			return ret;
	}

	return 0;
}

static ssize_t dwc3_start_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct dwc3_pci *dwc = s->private;
	struct device *dev = &dwc->pci->dev;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&dwc->lock, flags);
	if (dwc->dwc3)
		goto fail;

	ret = dwc3_params_set(dwc);
	if (ret)
		goto fail;

	ret = dwc3_pci_add_platform_device(dwc);
	if (ret) {
		dev_err(dev, "failed to register dwc3 device\n");
		goto fail;
	}
	spin_unlock_irqrestore(&dwc->lock, flags);

	return count;

fail:
	spin_unlock_irqrestore(&dwc->lock, flags);
	return count;
}

static int dwc3_start_show(struct seq_file *s, void *unused)
{
	return 0;
}

static int dwc3_start_open(struct inode *inode, struct file *file)
{
	return single_open(file, dwc3_start_show, inode->i_private);
}

static const struct file_operations dwc3_start_fops = {
	.open = dwc3_start_open,
	.write = dwc3_start_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

void dwc3_pci_debugfs_init(struct dwc3_pci *dwc)
{
	struct dentry *root;
	struct dentry *file;
	struct pci_dev *pci = dwc->pci;

	dwc3_params_init(dwc);

	root = debugfs_create_dir(dev_name(&pci->dev), NULL);
	if (IS_ERR_OR_NULL(root)) {
		dev_err(&pci->dev, "Can't create debugfs root\n");
		return;
	}

	dwc->root = root;

	file = debugfs_create_u8("maxspeed", 0644, root,
				 &dwc->params.maximum_speed);
	if (!file)
		dev_dbg(&pci->dev, "Can't create maxspeed attribute\n");

	file = debugfs_create_file("start", 0200, root, dwc, &dwc3_start_fops);
	if (!file)
		dev_dbg(&pci->dev, "Can't create debugfs start\n");
}

void dwc3_pci_debugfs_exit(struct dwc3_pci *dwc)
{
	debugfs_remove_recursive(dwc->root);
}
