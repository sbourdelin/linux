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
#include <asm/intel_ipc_dev.h>

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

static int __init intel_ipc_init(void)
{
	ipc_channel_lock_init();
	return class_register(&intel_ipc_class);
}

static void __exit intel_ipc_exit(void)
{
	class_unregister(&intel_ipc_class);
}

static int ipc_dev_lock(struct intel_ipc_dev *ipc_dev)
{
	int chan_type = ipc_dev->cfg->chan_type;

	if (!ipc_dev)
		return -ENODEV;

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
	int chan_type = ipc_dev->cfg->chan_type;

	if (!ipc_dev)
		return -ENODEV;

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

/* Helper function to read IPC device status register */
static inline u32 ipc_dev_read_status(struct intel_ipc_dev *ipc_dev)
{
	return readl(ipc_dev->cfg->status_reg);
}

/* Helper function to write 32 bits to IPC device data register */
static inline void ipc_dev_write_datal(struct intel_ipc_dev *ipc_dev,
		u32 data, u32 offset)
{
	writel(data, ipc_dev->cfg->wrbuf_reg + offset);
}

/* Helper function to read 32 bits from IPC device data register */
static inline u32 ipc_dev_read_datal(struct intel_ipc_dev *ipc_dev,
		u32 offset)
{
	return readl(ipc_dev->cfg->rbuf_reg + offset);
}

/* Helper function to send given command to IPC device */
static inline void ipc_dev_send_cmd(struct intel_ipc_dev *ipc_dev,
		u32 cmd)
{
	ipc_dev->cmd = cmd;

	if (ipc_dev->cfg->mode == IPC_DEV_MODE_IRQ)
		reinit_completion(&ipc_dev->cmd_complete);

	if (ipc_dev->ops->enable_msi)
		cmd = ipc_dev->ops->enable_msi(cmd);

	writel(cmd, ipc_dev->cfg->cmd_reg);
}

static inline int ipc_dev_status_busy(struct intel_ipc_dev *ipc_dev)
{
	int status = ipc_dev_read_status(ipc_dev);

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

	status = ipc_dev_read_status(ipc_dev);

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
 * @ipc_dev	: Reference to ipc device.
 * @cmd		: IPC command code.
 *
 * Send a simple IPC command to ipc device.
 * Use this when don't need to specify input/output data
 * and source/dest pointers.
 *
 * Return:	an IPC error code or 0 on success.
 */

int ipc_dev_simple_cmd(struct intel_ipc_dev *ipc_dev, u32 cmd)
{
	int ret;

	ipc_dev_lock(ipc_dev);
	ipc_dev_send_cmd(ipc_dev, cmd);
	ret = ipc_dev_check_status(ipc_dev);
	ipc_dev_unlock(ipc_dev);

	return ret;
}
EXPORT_SYMBOL_GPL(ipc_dev_simple_cmd);

/**
 * ipc_dev_raw_cmd() - IPC command with data and pointers
 * @ipc_dev	: reference to ipc_dev.
 * @cmd		: IPC command code.
 * @in		: input data of this IPC command.
 * @inlen	: input data length in bytes.
 * @out		: output data of this IPC command.
 * @outlen	: output data length in dwords.
 * @sptr	: data writing to SPTR register. Use 0 if want to skip.
 * @dptr	: data writing to DPTR register. Use 0 if want to skip.
 *
 * Send an IPC command to device with input/output data and
 * source/dest pointers.
 *
 * Return:	an IPC error code or 0 on success.
 */

int ipc_dev_raw_cmd(struct intel_ipc_dev *ipc_dev, u32 cmd,
		u8 *in, u32 inlen, u32 *out,
		u32 outlen, u32 dptr, u32 sptr)
{
	u32 wbuf[4] = { 0 };
	int ret;
	int i;

	ipc_dev_lock(ipc_dev);

	memcpy(wbuf, in, inlen);

	/* write if dptr_reg is valid */
	if (ipc_dev->cfg->dptr_reg)
		writel(dptr, ipc_dev->cfg->dptr_reg);

	/* write if sptr_reg is valid */
	if (ipc_dev->cfg->sptr_reg)
		writel(sptr, ipc_dev->cfg->sptr_reg);

	/* The input data register is 32bit register and inlen
	 * is in Byte */
	for (i = 0; i < ((inlen + 3) / 4); i++)
		ipc_dev_write_datal(ipc_dev, wbuf[i], 4 * i);

	ipc_dev_send_cmd(ipc_dev, cmd);

	ret = ipc_dev_check_status(ipc_dev);

	/* The out data register is 32bit register and outlen
	 * is in 32 bit */
	if (!ret) {
		for (i = 0; i < outlen; i++)
			*out++ = ipc_dev_read_datal(ipc_dev, 4 * i);
	}

	ipc_dev_unlock(ipc_dev);

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

	ret = ipc_dev_simple_cmd(ipc_dev, cmd);
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

subsys_initcall(intel_ipc_init);
module_exit(intel_ipc_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kuppuswamy Sathyanarayanan<sathyanarayanan.kuppuswamy@linux.intel.com>");
MODULE_DESCRIPTION("Intel IPC device class driver");
