/**
 * Marvell Bluetooth driver: sysfs related functions
 *
 * Copyright (C) 2015, Marvell International Ltd.
 *
 * This software file (the "File") is distributed by Marvell International
 * Ltd. under the terms of the GNU General Public License Version 2, June 1991
 * (the "License").  You may use, redistribute and/or modify this File in
 * accordance with the terms and conditions of the License, a copy of which
 * is available on the worldwide web at
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt.
 *
 * THE FILE IS DISTRIBUTED AS-IS, WITHOUT WARRANTY OF ANY KIND, AND THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE
 * ARE EXPRESSLY DISCLAIMED.  The License provides additional details about
 * this warranty disclaimer.
 */

#include <linux/slab.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "btmrvl_drv.h"

static ssize_t
btmrvl_sysfs_show_hscfgcmd(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct hci_dev *hdev = container_of(dev, struct hci_dev, dev);
	struct btmrvl_private *priv = hci_get_drvdata(hdev);

	return snprintf(buf, PAGE_SIZE, "%d\n", priv->btmrvl_dev.hscfgcmd);
}

static ssize_t
btmrvl_sysfs_store_hscfgcmd(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	long ret;
	u8 result;
	struct hci_dev *hdev = container_of(dev, struct hci_dev, dev);
	struct btmrvl_private *priv = hci_get_drvdata(hdev);

	ret = kstrtou8(buf, 10, &result);
	if (ret)
		return ret;

	if (!priv)
		return -EINVAL;

	priv->btmrvl_dev.hscfgcmd = result;
	if (priv->btmrvl_dev.hscfgcmd) {
		btmrvl_prepare_command(priv);
		wake_up_interruptible(&priv->main_thread.wait_q);
	}

	return count;
}

static DEVICE_ATTR(hscfgcmd, S_IRUGO | S_IWUSR,
		   btmrvl_sysfs_show_hscfgcmd,
		   btmrvl_sysfs_store_hscfgcmd);

static ssize_t
btmrvl_sysfs_show_gpiogap(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct hci_dev *hdev = container_of(dev, struct hci_dev, dev);
	struct btmrvl_private *priv = hci_get_drvdata(hdev);

	return snprintf(buf, PAGE_SIZE, "0x%x\n", priv->btmrvl_dev.gpio_gap);
}

static ssize_t
btmrvl_sysfs_store_gpiogap(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	long ret;
	u16 result;
	struct hci_dev *hdev = container_of(dev, struct hci_dev, dev);
	struct btmrvl_private *priv = hci_get_drvdata(hdev);

	ret = kstrtou16(buf, 10, &result);
	if (ret)
		return ret;

	priv->btmrvl_dev.gpio_gap = result;
	return count;
}

static DEVICE_ATTR(gpiogap, S_IRUGO | S_IWUSR,
		   btmrvl_sysfs_show_gpiogap,
		   btmrvl_sysfs_store_gpiogap);

static struct attribute *btmrvl_dev_attrs[] = {
	&dev_attr_hscfgcmd.attr,
	&dev_attr_gpiogap.attr,
	NULL,
};

static struct attribute_group btmrvl_dev_attr_group = {
	.attrs = btmrvl_dev_attrs,
};

static const struct attribute_group *btmrvl_dev_attr_groups[] = {
	&btmrvl_dev_attr_group,
	NULL,
};

int btmrvl_sysfs_register(struct btmrvl_private *priv)
{
	return sysfs_create_groups(&priv->btmrvl_dev.hcidev->dev.kobj,
				   btmrvl_dev_attr_groups);
}

void btmrvl_sysfs_unregister(struct btmrvl_private *priv)
{
	sysfs_remove_groups(&priv->btmrvl_dev.hcidev->dev.kobj,
			    btmrvl_dev_attr_groups);
}
