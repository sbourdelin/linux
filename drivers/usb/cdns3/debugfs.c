// SPDX-License-Identifier: GPL-2.0
/*
 * Cadence USBSS DRD Controller DebugFS filer.
 *
 * Copyright (C) 2018 Cadence.
 *
 * Author: Pawel Laszczak <pawell@cadence.com>
 */

#include <linux/types.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include "core.h"
#include "gadget.h"

static int cdns3_mode_show(struct seq_file *s, void *unused)
{
	struct cdns3		*cdns = s->private;

	switch (cdns->role) {
	case CDNS3_ROLE_HOST:
		seq_puts(s, "host\n");
		break;
	case CDNS3_ROLE_GADGET:
		seq_puts(s, "device\n");
		break;
	case CDNS3_ROLE_OTG:
	case CDNS3_ROLE_END:
		seq_puts(s, "otg\n");
		break;
	default:
		seq_puts(s, "UNKNOWN mode\n");
	}

	return 0;
}

static int cdns3_mode_open(struct inode *inode, struct file *file)
{
	return single_open(file, cdns3_mode_show, inode->i_private);
}

static ssize_t cdns3_mode_write(struct file *file,
				const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct seq_file		*s = file->private_data;
	struct cdns3		*cdns = s->private;
	u32			mode = 0;
	char			buf[32];

	if (copy_from_user(&buf, ubuf, min_t(size_t, sizeof(buf) - 1, count)))
		return -EFAULT;

	if (!strncmp(buf, "host", 4))
		mode = USB_DR_MODE_HOST;

	if (!strncmp(buf, "device", 6))
		mode = USB_DR_MODE_PERIPHERAL;

	if (!strncmp(buf, "otg", 3))
		mode = USB_DR_MODE_OTG;

	cdns->desired_role = mode;
	queue_work(system_freezable_wq, &cdns->role_switch_wq);
	return count;
}

static const struct file_operations cdns3_mode_fops = {
	.open			= cdns3_mode_open,
	.write			= cdns3_mode_write,
	.read			= seq_read,
	.llseek			= seq_lseek,
	.release		= single_release,
};

void cdns3_debugfs_init(struct cdns3 *cdns)
{
	struct dentry *root;

	root = debugfs_create_dir(dev_name(cdns->dev), NULL);
	cdns->root = root;
	if (IS_ENABLED(CONFIG_USB_CDNS3_GADGET) &&
	    IS_ENABLED(CONFIG_USB_CDNS3_HOST))
		debugfs_create_file("mode", 0644, root, cdns,
				    &cdns3_mode_fops);
}

void cdns3_debugfs_exit(struct cdns3 *cdns)
{
	debugfs_remove_recursive(cdns->root);
}
