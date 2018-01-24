// SPDX-License-Identifier: GPL-2.0+
/*
 * Xilinx Zynq MPSoC Firmware layer
 *
 *  Copyright (C) 2014-2018 Xilinx, Inc.
 *
 *  Jolly Shah <jollys@xilinx.com>
 *  Rajan Vaja <rajanv@xilinx.com>
 */

#include <linux/compiler.h>
#include <linux/of.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/platform_device.h>

#include <linux/firmware/xilinx/zynqmp/firmware.h>

static ssize_t read_register(char *buf, u32 ioctl_id, u32 reg)
{
	int ret;
	u32 ret_payload[PAYLOAD_ARG_CNT];
	const struct zynqmp_eemi_ops *eemi_ops = get_eemi_ops();

	if (!eemi_ops || !eemi_ops->ioctl)
		return -EFAULT;

	ret = eemi_ops->ioctl(0, ioctl_id, reg, 0, ret_payload);
	if (ret)
		return ret;

	return sprintf(buf, "0x%x\n", ret_payload[1]);
}

static ssize_t write_register(const char *buf, size_t count, u32 read_ioctl,
			      u32 write_ioctl, u32 reg)
{
	char *kern_buff, *inbuf, *tok;
	long mask, value;
	int ret;
	u32 ret_payload[PAYLOAD_ARG_CNT];
	const struct zynqmp_eemi_ops *eemi_ops = get_eemi_ops();

	if (!eemi_ops || !eemi_ops->ioctl)
		return -EFAULT;

	kern_buff = kzalloc(count, GFP_KERNEL);
	if (!kern_buff)
		return -ENOMEM;

	ret = strlcpy(kern_buff, buf, count);
	if (ret < 0) {
		ret = -EFAULT;
		goto err;
	}

	inbuf = kern_buff;

	/* Read the write mask */
	tok = strsep(&inbuf, " ");
	if (!tok) {
		ret = -EFAULT;
		goto err;
	}

	ret = kstrtol(tok, 16, &mask);
	if (ret) {
		ret = -EFAULT;
		goto err;
	}

	/* Read the write value */
	tok = strsep(&inbuf, " ");
	if (!tok) {
		ret = -EFAULT;
		goto err;
	}

	ret = kstrtol(tok, 16, &value);
	if (ret) {
		ret = -EFAULT;
		goto err;
	}

	ret = eemi_ops->ioctl(0, read_ioctl, reg, 0, ret_payload);
	if (ret) {
		ret = -EFAULT;
		goto err;
	}
	ret_payload[1] &= ~mask;
	value &= mask;
	value |= ret_payload[1];

	ret = eemi_ops->ioctl(0, write_ioctl, reg, value, NULL);
	if (ret)
		ret = -EFAULT;

err:
	kfree(kern_buff);
	if (ret)
		return ret;

	return count;
}

/**
 * ggs_show - Show global general storage (ggs) sysfs attribute
 * @dev: Device structure
 * @attr: Device attribute structure
 * @buf: Requested available shutdown_scope attributes string
 * @reg: Register number
 *
 * Return:Number of bytes printed into the buffer.
 *
 * Helper function for viewing a ggs register value.
 *
 * User-space interface for viewing the content of the ggs0 register.
 * cat /sys/devices/platform/firmware/ggs0
 */
static ssize_t ggs_show(struct device *dev,
			struct device_attribute *attr,
			char *buf,
			u32 reg)
{
	return read_register(buf, IOCTL_READ_GGS, reg);
}

/**
 * ggs_store - Store global general storage (ggs) sysfs attribute
 * @dev: Device structure
 * @attr: Device attribute structure
 * @buf: User entered shutdown_scope attribute string
 * @count: Size of buf
 * @reg: Register number
 *
 * Return: count argument if request succeeds, the corresponding
 * error code otherwise
 *
 * Helper function for storing a ggs register value.
 *
 * For example, the user-space interface for storing a value to the
 * ggs0 register:
 * echo 0xFFFFFFFF 0x1234ABCD > /sys/devices/platform/firmware/ggs0
 */
static ssize_t ggs_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf,
			 size_t count,
			 u32 reg)
{
	if (!dev || !attr || !buf || !count || reg >= GSS_NUM_REGS)
		return -EINVAL;

	return write_register(buf, count, IOCTL_READ_GGS, IOCTL_WRITE_GGS, reg);
}

/* GGS register show functions */
#define GGS0_SHOW(N) \
	ssize_t ggs##N##_show(struct device *dev, \
			 struct device_attribute *attr, \
			 char *buf) \
	{ \
		return ggs_show(dev, attr, buf, N); \
	}

static GGS0_SHOW(0);
static GGS0_SHOW(1);
static GGS0_SHOW(2);
static GGS0_SHOW(3);

/* GGS register store function */
#define GGS0_STORE(N) \
	ssize_t ggs##N##_store(struct device *dev, \
				   struct device_attribute *attr, \
				   const char *buf, \
				   size_t count) \
	{ \
		return ggs_store(dev, attr, buf, count, N); \
	}

#define CREATE_GGS_DEVICE(dev, N) \
do { \
	if (device_create_file(dev, &dev_attr_ggs##N)) \
		dev_err(dev, "unable to create ggs%d attribute\n", N); \
} while (0)

static GGS0_STORE(0);
static GGS0_STORE(1);
static GGS0_STORE(2);
static GGS0_STORE(3);

/**
 * pggs_show - Show persistent global general storage (pggs) sysfs attribute
 * @dev: Device structure
 * @attr: Device attribute structure
 * @buf: Requested available shutdown_scope attributes string
 * @reg: Register number
 *
 * Return:Number of bytes printed into the buffer.
 *
 * Helper function for viewing a pggs register value.
 */
static ssize_t pggs_show(struct device *dev,
			 struct device_attribute *attr,
			 char *buf,
			 u32 reg)
{
	return read_register(buf, IOCTL_READ_PGGS, reg);
}

/**
 * pggs_store - Store persistent global general storage (pggs) sysfs attribute
 * @dev: Device structure
 * @attr: Device attribute structure
 * @buf: User entered shutdown_scope attribute string
 * @count: Size of buf
 * @reg: Register number
 *
 * Return: count argument if request succeeds, the corresponding
 * error code otherwise
 *
 * Helper function for storing a pggs register value.
 */
static ssize_t pggs_store(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf,
			  size_t count,
			  u32 reg)
{
	return write_register(buf, count, IOCTL_READ_PGGS,
			      IOCTL_WRITE_PGGS, reg);
}

#define PGGS0_SHOW(N) \
	ssize_t pggs##N##_show(struct device *dev, \
			 struct device_attribute *attr, \
			 char *buf) \
	{ \
		return pggs_show(dev, attr, buf, N); \
	}

#define PGGS0_STORE(N) \
	ssize_t pggs##N##_store(struct device *dev, \
				   struct device_attribute *attr, \
				   const char *buf, \
				   size_t count) \
	{ \
		return pggs_store(dev, attr, buf, count, N); \
	}

/* PGGS register show functions */
static PGGS0_SHOW(0);
static PGGS0_SHOW(1);
static PGGS0_SHOW(2);
static PGGS0_SHOW(3);

/* PGGS register store functions */
static PGGS0_STORE(0);
static PGGS0_STORE(1);
static PGGS0_STORE(2);
static PGGS0_STORE(3);

/* GGS register device attributes */
static DEVICE_ATTR_RW(ggs0);
static DEVICE_ATTR_RW(ggs1);
static DEVICE_ATTR_RW(ggs2);
static DEVICE_ATTR_RW(ggs3);

/* PGGS register device attributes */
static DEVICE_ATTR_RW(pggs0);
static DEVICE_ATTR_RW(pggs1);
static DEVICE_ATTR_RW(pggs2);
static DEVICE_ATTR_RW(pggs3);

static struct attribute *attrs[] = {
	&dev_attr_ggs0.attr,
	&dev_attr_ggs1.attr,
	&dev_attr_ggs2.attr,
	&dev_attr_ggs3.attr,
	&dev_attr_pggs0.attr,
	&dev_attr_pggs1.attr,
	&dev_attr_pggs2.attr,
	&dev_attr_pggs3.attr,
	NULL,
};

static const struct attribute_group attr_group = {
	.attrs = attrs,
	NULL,
};

int zynqmp_pm_ggs_init(struct device *dev)
{
	return sysfs_create_group(&dev->kobj, &attr_group);
}
