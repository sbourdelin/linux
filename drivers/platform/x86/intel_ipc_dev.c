/*
 * intel_ipc_dev.c: Intel IPC device class driver
 *
 * (C) Copyright 2017 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 *
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/platform_data/x86/intel_ipc_dev.h>
#include <linux/regmap.h>

/* mutex to sync different ipc devices in same channel */
static struct mutex channel_lock[IPC_CHANNEL_MAX];

static void ipc_channel_lock_init(void)
{
	int i;

	for (i = 0; i < IPC_CHANNEL_MAX; i++)
		mutex_init(&channel_lock[i]);
}

static struct class intel_ipc_class = {
	.name = "intel_ipc",
	.owner = THIS_MODULE,
};

static int ipc_dev_lock(struct intel_ipc_dev *ipc_dev)
{
	int chan_type;

	if (!ipc_dev || !ipc_dev->cfg)
		return -ENODEV;

	chan_type = ipc_dev->cfg->chan_type;
	if (chan_type > IPC_CHANNEL_MAX)
		return -EINVAL;

	/* acquire channel lock */
	mutex_lock(&channel_lock[chan_type]);

	/* acquire IPC device lock */
	mutex_lock(&ipc_dev->lock);

	return 0;
}

static int ipc_dev_unlock(struct intel_ipc_dev *ipc_dev)
{
	int chan_type;

	if (!ipc_dev || !ipc_dev->cfg)
		return -ENODEV;

	chan_type = ipc_dev->cfg->chan_type;
	if (chan_type > IPC_CHANNEL_MAX)
		return -EINVAL;

	/* release IPC device lock */
	mutex_unlock(&ipc_dev->lock);

	/* release channel lock */
	mutex_unlock(&channel_lock[chan_type]);

	return 0;
}

static const char *ipc_dev_err_string(struct intel_ipc_dev *ipc_dev,
	int error)
{
	switch (error) {
	case IPC_DEV_ERR_NONE:
		return "No error";
	case IPC_DEV_ERR_CMD_NOT_SUPPORTED:
		return "Command not-supported/Invalid";
	case IPC_DEV_ERR_CMD_NOT_SERVICED:
		return "Command not-serviced/Invalid param";
	case IPC_DEV_ERR_UNABLE_TO_SERVICE:
		return "Unable-to-service/Cmd-timeout";
	case IPC_DEV_ERR_CMD_INVALID:
		return "Command-invalid/Cmd-locked";
	case IPC_DEV_ERR_CMD_FAILED:
		return "Command-failed/Invalid-VR-id";
	case IPC_DEV_ERR_EMSECURITY:
		return "Invalid Battery/VR-Error";
	case IPC_DEV_ERR_UNSIGNEDKERNEL:
		return "Unsigned kernel";
	default:
		return "Unknown Command";
	};
}

/* Helper function to send given command to IPC device */
static inline void ipc_dev_send_cmd(struct intel_ipc_dev *ipc_dev, u32 cmd)
{
	ipc_dev->cmd = cmd;

	if (ipc_dev->cfg->mode == IPC_DEV_MODE_IRQ)
		reinit_completion(&ipc_dev->cmd_complete);

	if (ipc_dev->ops->enable_msi)
		cmd = ipc_dev->ops->enable_msi(cmd);

	regmap_write(ipc_dev->cfg->cmd_regs, ipc_dev->cfg->cmd_reg, cmd);
}

static inline int ipc_dev_status_busy(struct intel_ipc_dev *ipc_dev)
{
	int status;

	regmap_read(ipc_dev->cfg->cmd_regs, ipc_dev->cfg->status_reg, &status);

	if (ipc_dev->ops->busy_check)
		return ipc_dev->ops->busy_check(status);

	return 0;
}

/* Check the status of IPC command and return err code if failed */
static int ipc_dev_check_status(struct intel_ipc_dev *ipc_dev)
{
	int loop_count = IPC_DEV_CMD_LOOP_CNT;
	int status;
	int ret = 0;

	if (ipc_dev->cfg->mode == IPC_DEV_MODE_IRQ) {
		if (!wait_for_completion_timeout(&ipc_dev->cmd_complete,
				IPC_DEV_CMD_TIMEOUT))
			ret = -ETIMEDOUT;
	} else {
		while (ipc_dev_status_busy(ipc_dev) && --loop_count)
			udelay(1);
		if (!loop_count)
			ret = -ETIMEDOUT;
	}

	if (ret < 0) {
		dev_err(&ipc_dev->dev,
				"IPC timed out, CMD=0x%x\n", ipc_dev->cmd);
		return ret;
	}

	regmap_read(ipc_dev->cfg->cmd_regs, ipc_dev->cfg->status_reg, &status);

	if (ipc_dev->ops->to_err_code)
		ret = ipc_dev->ops->to_err_code(status);

	if (ret) {
		dev_err(&ipc_dev->dev,
				"IPC failed: %s, STS=0x%x, CMD=0x%x\n",
				ipc_dev_err_string(ipc_dev, ret),
				status, ipc_dev->cmd);
		return -EIO;
	}

	return 0;
}

/**
 * ipc_dev_simple_cmd() - Send simple IPC command
 * @ipc_dev     : Reference to ipc device.
 * @cmd_list    : IPC command list.
 * @cmdlen      : Number of cmd/sub-cmds.
 *
 * Send a simple IPC command to ipc device.
 * Use this when don't need to specify input/output data
 * and source/dest pointers.
 *
 * Return:	an IPC error code or 0 on success.
 */

int ipc_dev_simple_cmd(struct intel_ipc_dev *ipc_dev, u32 *cmd_list,
		u32 cmdlen)
{
	int ret;

	if (!cmd_list)
		return -EINVAL;

	ret = ipc_dev_lock(ipc_dev);
	if (ret)
		return ret;

	/* Call custom pre-processing handler */
	if (ipc_dev->ops->pre_simple_cmd_fn) {
		ret = ipc_dev->ops->pre_simple_cmd_fn(cmd_list, cmdlen);
		if (ret)
			goto unlock_device;
	}

	ipc_dev_send_cmd(ipc_dev, cmd_list[0]);

	ret = ipc_dev_check_status(ipc_dev);

unlock_device:
	ipc_dev_unlock(ipc_dev);

	return ret;
}
EXPORT_SYMBOL_GPL(ipc_dev_simple_cmd);

/**
 * ipc_dev_cmd() - Send IPC command with data.
 * @ipc_dev     : Reference to ipc_dev.
 * @cmd_list    : Array of commands/sub-commands.
 * @cmdlen      : Number of commands.
 * @in          : Input data of this IPC command.
 * @inlen       : Input data length in dwords.
 * @out         : Output data of this IPC command.
 * @outlen      : Length of output data in dwords.
 *
 * Send an IPC command to device with input/output data.
 *
 * Return:	an IPC error code or 0 on success.
 */
int ipc_dev_cmd(struct intel_ipc_dev *ipc_dev, u32 *cmd_list, u32 cmdlen,
		u32 *in, u32 inlen, u32 *out, u32 outlen)
{
	int ret;

	if (!cmd_list || !in)
		return -EINVAL;

	ret = ipc_dev_lock(ipc_dev);
	if (ret)
		return ret;

	/* Call custom pre-processing handler. */
	if (ipc_dev->ops->pre_cmd_fn) {
		ret = ipc_dev->ops->pre_cmd_fn(cmd_list, cmdlen, in, inlen,
				out, outlen);
		if (ret)
			goto unlock_device;
	}

	/* Write inlen dwords of data to wrbuf_reg. */
	if (inlen > 0)
		regmap_bulk_write(ipc_dev->cfg->data_regs,
				ipc_dev->cfg->wrbuf_reg, in, inlen);

	ipc_dev_send_cmd(ipc_dev, cmd_list[0]);

	ret = ipc_dev_check_status(ipc_dev);

	/* Read outlen dwords of data from rbug_reg. */
	if (!ret && outlen > 0)
		regmap_bulk_read(ipc_dev->cfg->data_regs,
				ipc_dev->cfg->rbuf_reg, out, outlen);
unlock_device:
	ipc_dev_unlock(ipc_dev);

	return ret;
}
EXPORT_SYMBOL_GPL(ipc_dev_cmd);

/**
 * ipc_dev_raw_cmd() - Send IPC command with data and pointers.
 * @ipc_dev     : Reference to ipc_dev.
 * @cmd_list    : Array of commands/sub-commands.
 * @cmdlen      : Number of commands.
 * @in          : Input data of this IPC command.
 * @inlen       : Input data length in bytes.
 * @out         : Output data of this IPC command.
 * @outlen      : Length of output data in dwords.
 * @dptr        : IPC destination data address.
 * @sptr        : IPC source data address.
 *
 * Send an IPC command to device with input/output data and
 * source/dest pointers.
 *
 * Return:	an IPC error code or 0 on success.
 */

int ipc_dev_raw_cmd(struct intel_ipc_dev *ipc_dev, u32 *cmd_list, u32 cmdlen,
		u8 *in, u32 inlen, u32 *out, u32 outlen, u32 dptr, u32 sptr)
{
	int ret;
	int inbuflen = DIV_ROUND_UP(inlen, 4);
	u32 *inbuf;

	if (!cmd_list || !in)
		return -EINVAL;

	inbuf = kzalloc(inbuflen, GFP_KERNEL);
	if (!inbuf)
		return -ENOMEM;

	ret = ipc_dev_lock(ipc_dev);
	if (ret)
		return ret;

	/* Call custom pre-processing handler. */
	if (ipc_dev->ops->pre_raw_cmd_fn) {
		ret = ipc_dev->ops->pre_raw_cmd_fn(cmd_list, cmdlen, in, inlen,
				out, outlen, dptr, sptr);
		if (ret)
			goto unlock_device;
	}

	/* If supported, write DPTR register.*/
	if (ipc_dev->cfg->support_dptr)
		regmap_write(ipc_dev->cfg->cmd_regs, ipc_dev->cfg->dptr_reg,
				dptr);

	/* If supported, write SPTR register. */
	if (ipc_dev->cfg->support_sptr)
		regmap_write(ipc_dev->cfg->cmd_regs, ipc_dev->cfg->sptr_reg,
				sptr);

	memcpy(inbuf, in, inlen);

	/* Write inlen dwords of data to wrbuf_reg. */
	if (inlen > 0)
		regmap_bulk_write(ipc_dev->cfg->data_regs,
				ipc_dev->cfg->wrbuf_reg, inbuf, inbuflen);

	ipc_dev_send_cmd(ipc_dev, cmd_list[0]);

	ret = ipc_dev_check_status(ipc_dev);

	/* Read outlen dwords of data from rbug_reg. */
	if (!ret && outlen > 0)
		regmap_bulk_read(ipc_dev->cfg->data_regs,
				ipc_dev->cfg->rbuf_reg, out, outlen);
unlock_device:
	ipc_dev_unlock(ipc_dev);
	kfree(inbuf);

	return ret;
}
EXPORT_SYMBOL_GPL(ipc_dev_raw_cmd);

/* sysfs option to send simple IPC commands from userspace */
static ssize_t ipc_dev_cmd_reg_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct intel_ipc_dev *ipc_dev = dev_get_drvdata(dev);
	u32 cmd;
	int ret;

	ret = sscanf(buf, "%d", &cmd);
	if (ret != 1) {
		dev_err(dev, "Error args\n");
		return -EINVAL;
	}

	ret = ipc_dev_simple_cmd(ipc_dev, &cmd, 1);
	if (ret) {
		dev_err(dev, "command 0x%x error with %d\n", cmd, ret);
		return ret;
	}
	return (ssize_t)count;
}

static DEVICE_ATTR(send_cmd, S_IWUSR, NULL, ipc_dev_cmd_reg_store);

static struct attribute *ipc_dev_attrs[] = {
	&dev_attr_send_cmd.attr,
	NULL
};

static const struct attribute_group ipc_dev_group = {
	.attrs = ipc_dev_attrs,
};

static const struct attribute_group *ipc_dev_groups[] = {
	&ipc_dev_group,
	NULL,
};

/* IPC device IRQ handler */
static irqreturn_t ipc_dev_irq_handler(int irq, void *dev_id)
{
	struct intel_ipc_dev *ipc_dev = (struct intel_ipc_dev *)dev_id;

	complete(&ipc_dev->cmd_complete);

	return IRQ_HANDLED;
}

static void devm_intel_ipc_dev_release(struct device *dev, void *res)
{
	struct intel_ipc_dev *ipc_dev = *(struct intel_ipc_dev **)res;

	if (!ipc_dev)
		return;

	device_del(&ipc_dev->dev);

	kfree(ipc_dev);
}

static int match_name(struct device *dev, const void *data)
{
        if (!dev_name(dev))
                return 0;

        return !strcmp(dev_name(dev), (char *)data);
}

/**
 * intel_ipc_dev_get() - Get Intel IPC device from name.
 * @dev_name    : Name of the IPC device.
 *
 * Return       : ERR_PTR/NULL or intel_ipc_dev pointer on success.
 */
struct intel_ipc_dev *intel_ipc_dev_get(const char *dev_name)
{
        struct device *dev;

	if (!dev_name)
		return ERR_PTR(-EINVAL);

	dev = class_find_device(&intel_ipc_class, NULL, dev_name, match_name);
	if (dev)
		put_device(dev);

	return dev_get_drvdata(dev);
}
EXPORT_SYMBOL_GPL(intel_ipc_dev_get);

/**
 * devm_intel_ipc_dev_create() - Create IPC device
 * @dev		: IPC parent device.
 * @devname	: Name of the IPC device.
 * @cfg		: IPC device configuration.
 * @ops		: IPC device ops.
 *
 * Resource managed API to create IPC device with
 * given configuration.
 *
 * Return	: IPC device pointer or ERR_PTR(error code).
 */
struct intel_ipc_dev *devm_intel_ipc_dev_create(struct device *dev,
		const char *devname,
		struct intel_ipc_dev_cfg *cfg,
		struct intel_ipc_dev_ops *ops)
{
	struct intel_ipc_dev **ptr, *ipc_dev;
	int ret;

	if (!dev && !devname && !cfg)
		return ERR_PTR(-EINVAL);

	if (!intel_ipc_dev_get(devname)) {
		dev_err(dev, "IPC device %s already exist\n", devname);
		return ERR_PTR(-EINVAL);
	}

	ptr = devres_alloc(devm_intel_ipc_dev_release, sizeof(*ptr),
			GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	ipc_dev = kzalloc(sizeof(*ipc_dev), GFP_KERNEL);
	if (!ipc_dev) {
		ret = -ENOMEM;
		goto err_dev_create;
	}

	ipc_dev->dev.class = &intel_ipc_class;
	ipc_dev->dev.parent = dev;
	ipc_dev->dev.groups = ipc_dev_groups;
	ipc_dev->cfg = cfg;
	ipc_dev->ops = ops;

	mutex_init(&ipc_dev->lock);
	init_completion(&ipc_dev->cmd_complete);
	dev_set_drvdata(&ipc_dev->dev, ipc_dev);
	dev_set_name(&ipc_dev->dev, devname);
	device_initialize(&ipc_dev->dev);

	ret = device_add(&ipc_dev->dev);
	if (ret < 0) {
		dev_err(&ipc_dev->dev, "%s device create failed\n",
				__func__);
		ret = -ENODEV;
		goto err_dev_add;
	}

	if (ipc_dev->cfg->mode == IPC_DEV_MODE_IRQ) {
		if (devm_request_irq(&ipc_dev->dev,
				ipc_dev->cfg->irq,
				ipc_dev_irq_handler,
				ipc_dev->cfg->irqflags,
				dev_name(&ipc_dev->dev),
				ipc_dev)) {
			dev_err(&ipc_dev->dev,
					"Failed to request irq\n");
			goto err_irq_request;
		}
	}

	*ptr = ipc_dev;

	devres_add(dev, ptr);

	return ipc_dev;

err_irq_request:
	device_del(&ipc_dev->dev);
err_dev_add:
	kfree(ipc_dev);
err_dev_create:
	devres_free(ptr);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(devm_intel_ipc_dev_create);

static int __init intel_ipc_init(void)
{
	ipc_channel_lock_init();
	return class_register(&intel_ipc_class);
}

static void __exit intel_ipc_exit(void)
{
	class_unregister(&intel_ipc_class);
}
subsys_initcall(intel_ipc_init);
module_exit(intel_ipc_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kuppuswamy Sathyanarayanan<sathyanarayanan.kuppuswamy@linux.intel.com>");
MODULE_DESCRIPTION("Intel IPC device class driver");
