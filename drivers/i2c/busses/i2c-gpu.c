// SPDX-License-Identifier: GPL-2.0
/*
 * Nvidia GPU I2C controller Driver
 *
 * Copyright (C) 2018 NVIDIA Corporation. All rights reserved.
 * Author: Ajay Gupta <ajayg@nvidia.com>
 *
 */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>

/* STATUS definitions  */
#define STATUS_SUCCESS			0
#define STATUS_UNSUCCESSFUL		0x80000000UL
#define STATUS_TIMEOUT			0x80000001UL
#define STATUS_IO_DEVICE_ERROR		0x80000002UL
#define STATUS_IO_TIMEOUT		0x80000004UL
#define STATUS_IO_PREEMPTED		0x80000008UL

/* Cypress Type-C controllers (CCGx) device */
#define CCGX_I2C_DEV_ADDRESS		0x08

/* I2C definitions */
#define I2C_MST_CNTL				0x00
#define I2C_MST_CNTL_GEN_START			(1 << 0)
#define I2C_MST_CNTL_GEN_STOP			(1 << 1)
#define I2C_MST_CNTL_CMD_NONE			(0 << 2)
#define I2C_MST_CNTL_CMD_READ			(1 << 2)
#define I2C_MST_CNTL_CMD_WRITE			(2 << 2)
#define I2C_MST_CNTL_CMD_RESET			(3 << 2)
#define I2C_MST_CNTL_GEN_RAB			(1 << 4)
#define I2C_MST_CNTL_BURST_SIZE_SHIFT		(6)
#define I2C_MST_CNTL_GEN_NACK			(1 << 28)
#define I2C_MST_CNTL_STATUS			(3 << 29)
#define I2C_MST_CNTL_STATUS_OKAY		(0 << 29)
#define I2C_MST_CNTL_STATUS_NO_ACK		(1 << 29)
#define I2C_MST_CNTL_STATUS_TIMEOUT		(2 << 29)
#define I2C_MST_CNTL_STATUS_BUS_BUSY		(3 << 29)
#define I2C_MST_CNTL_CYCLE_TRIGGER		(1 << 31)

#define I2C_MST_ADDR				0x04
#define I2C_MST_ADDR_DAB			0

#define I2C_MST_I2C0_TIMING				0x08
#define I2C_MST_I2C0_TIMING_SCL_PERIOD_100KHZ		(0x10e << 0)
#define I2C_MST_I2C0_TIMING_SCL_PERIOD_200KHZ		(0x087 << 0)
#define I2C_MST_I2C0_TIMING_TIMEOUT_CLK_CNT		16
#define I2C_MST_I2C0_TIMING_TIMEOUT_CLK_CNT_MAX		255
#define I2C_MST_I2C0_TIMING_TIMEOUT_CHECK		(1 << 24)

#define I2C_MST_DATA					0x0c

#define I2C_MST_HYBRID_PADCTL				0x20
#define I2C_MST_HYBRID_PADCTL_MODE_I2C			(1 << 0)
#define I2C_MST_HYBRID_PADCTL_I2C_SCL_INPUT_RCV		(1 << 14)
#define I2C_MST_HYBRID_PADCTL_I2C_SDA_INPUT_RCV		(1 << 15)

/* PCI driver data */
struct gpu_i2c_dev {
	struct pci_dev *pci_dev;
	void __iomem *regs;
	struct i2c_adapter adapter;
	struct i2c_client *client;
	struct mutex mutex;
	bool do_start;
};

static void enable_i2c_bus(struct gpu_i2c_dev *gdev)
{
	struct device *dev = &gdev->pci_dev->dev;
	u32 val;

	/* enable I2C */
	val = readl(gdev->regs + I2C_MST_HYBRID_PADCTL);
	val |= I2C_MST_HYBRID_PADCTL_MODE_I2C |
		I2C_MST_HYBRID_PADCTL_I2C_SCL_INPUT_RCV |
		I2C_MST_HYBRID_PADCTL_I2C_SDA_INPUT_RCV;

	dev_dbg(dev, "%s: %p (I2C_MST_HYBRID_PADCTL) <- %08x", __func__,
		(gdev->regs + I2C_MST_HYBRID_PADCTL), val);

	writel(val, gdev->regs + I2C_MST_HYBRID_PADCTL);

	/* enable 100KHZ mode */
	val = 0;
	val |= I2C_MST_I2C0_TIMING_SCL_PERIOD_100KHZ;
	val |= (I2C_MST_I2C0_TIMING_TIMEOUT_CLK_CNT_MAX
	    << I2C_MST_I2C0_TIMING_TIMEOUT_CLK_CNT);
	val |= I2C_MST_I2C0_TIMING_TIMEOUT_CHECK;

	dev_dbg(dev, "%s: %p (I2C_MST_I2C0_TIMING) <- %08x", __func__,
		gdev->regs + I2C_MST_I2C0_TIMING, val);
	writel(val, gdev->regs + I2C_MST_I2C0_TIMING);
}

static u32 i2c_check_status(struct gpu_i2c_dev *gdev)
{
	struct device *dev = &gdev->pci_dev->dev;
	unsigned long target = jiffies + msecs_to_jiffies(1000);
	u32 status = STATUS_UNSUCCESSFUL;
	u32 val;

	while (time_is_after_jiffies(target)) {
		val = readl(gdev->regs + I2C_MST_CNTL);
		if ((val & I2C_MST_CNTL_CYCLE_TRIGGER) !=
				I2C_MST_CNTL_CYCLE_TRIGGER)
			break;
		if ((val & I2C_MST_CNTL_STATUS) !=
				I2C_MST_CNTL_STATUS_BUS_BUSY)
			break;
		usleep_range(1000, 2000);
	}

	if (time_is_before_jiffies(target)) {
		dev_err(dev, "%si2c timeout", __func__);
		return status;
	}

	val = readl(gdev->regs + I2C_MST_CNTL);
	switch (val & I2C_MST_CNTL_STATUS) {
	case I2C_MST_CNTL_STATUS_OKAY:
		status = STATUS_SUCCESS;
		break;
	case I2C_MST_CNTL_STATUS_NO_ACK:
		status = STATUS_IO_DEVICE_ERROR;
		break;
	case I2C_MST_CNTL_STATUS_TIMEOUT:
		status = STATUS_IO_TIMEOUT;
		break;
	case I2C_MST_CNTL_STATUS_BUS_BUSY:
		status = STATUS_IO_PREEMPTED;
		break;
	default:
		break;
	}
	return status;
}

static u32 i2c_read(struct gpu_i2c_dev *gdev, u8 *data, u16 len)
{
	struct device *dev = &gdev->pci_dev->dev;
	u32 status;
	u32 val = 0;

	val |= I2C_MST_CNTL_GEN_START | I2C_MST_CNTL_GEN_STOP |
		I2C_MST_CNTL_CMD_READ | (len << I2C_MST_CNTL_BURST_SIZE_SHIFT) |
		I2C_MST_CNTL_CYCLE_TRIGGER | I2C_MST_CNTL_GEN_NACK;
	val &= ~I2C_MST_CNTL_GEN_RAB;
	writel(val, gdev->regs + I2C_MST_CNTL);

	status = i2c_check_status(gdev);
	if (status == STATUS_UNSUCCESSFUL) {
		dev_err(dev, "%s failed\n", __func__);
		return status;
	}

	val = readl(gdev->regs + I2C_MST_DATA);
	switch (len) {
	case 1:
		data[0] = (val >> 0) & 0xff;
		break;
	case 2:
		data[0] = (val >> 8) & 0xff;
		data[1] = (val >> 0) & 0xff;
		break;
	case 3:
		data[0] = (val >> 16) & 0xff;
		data[1] = (val >> 8) & 0xff;
		data[2] = (val >> 0) & 0xff;
		break;
	case 4:
		data[0] = (val >> 24) & 0xff;
		data[1] = (val >> 16) & 0xff;
		data[2] = (val >> 8) & 0xff;
		data[3] = (val >> 0) & 0xff;
		break;
	default:
		break;
	}
	return status;
}

static u32 i2c_manual_start(struct gpu_i2c_dev *gdev, u16 addr)
{
	u32 val = 0;

	val = addr << I2C_MST_ADDR_DAB;
	writel(val, gdev->regs + I2C_MST_ADDR);

	val = 0;
	val |= I2C_MST_CNTL_GEN_START | I2C_MST_CNTL_CMD_NONE |
		I2C_MST_CNTL_GEN_NACK;
	val &= ~(I2C_MST_CNTL_GEN_STOP | I2C_MST_CNTL_GEN_RAB);
	writel(val, gdev->regs + I2C_MST_CNTL);

	return i2c_check_status(gdev);
}

static u32 i2c_manual_stop(struct gpu_i2c_dev *gdev)
{
	u32 val = 0;

	val |= I2C_MST_CNTL_GEN_STOP | I2C_MST_CNTL_CMD_NONE |
		I2C_MST_CNTL_GEN_NACK;
	val &= ~(I2C_MST_CNTL_GEN_START | I2C_MST_CNTL_GEN_RAB);
	writel(val, gdev->regs + I2C_MST_CNTL);

	return i2c_check_status(gdev);
}

static u32 i2c_manual_write(struct gpu_i2c_dev *gdev, u8 data)
{
	u32 val = 0;

	writel(data, gdev->regs + I2C_MST_DATA);

	val |= I2C_MST_CNTL_CMD_WRITE | (1 << I2C_MST_CNTL_BURST_SIZE_SHIFT) |
		I2C_MST_CNTL_GEN_NACK;
	val &= ~(I2C_MST_CNTL_GEN_START | I2C_MST_CNTL_GEN_STOP
		| I2C_MST_CNTL_GEN_RAB);
	writel(val, gdev->regs + I2C_MST_CNTL);

	return i2c_check_status(gdev);
}

/* gdev i2c adapter */
static int gpu_i2c_master_xfer(struct i2c_adapter *adap,
	struct i2c_msg *msgs, int num)
{
	struct gpu_i2c_dev *gdev = i2c_get_adapdata(adap);
	struct device *dev = &gdev->pci_dev->dev;
	int retry1b = 10;
	u32 status;
	int i, j;

	dev_dbg(dev, "%s: adap %p msgs %p num %d\n", __func__, adap, msgs, num);

	mutex_lock(&gdev->mutex);

	for (i = 0; i < num; i++) {
		if (msgs[i].flags & I2C_M_RD) {
retry2:
			status = i2c_read(gdev, msgs[i].buf, msgs[i].len);
			if (status != STATUS_SUCCESS) {
				dev_err(dev,
				"%s:%d i2c_read failed %08lx", __func__,
				__LINE__, (unsigned long)status);

				if (--retry1b > 0) {
					usleep_range(10000, 11000);
					goto retry2;
				}
				break;
			}
			gdev->do_start = true;
		} else if (msgs[i].flags & I2C_M_STOP) {
			status = i2c_manual_stop(gdev);
			if (status != STATUS_SUCCESS) {
				dev_err(dev,
				"%s:%d i2c_manual_stop failed %08lx", __func__,
				__LINE__, (unsigned long)status);
				goto exit;
			}
			gdev->do_start = true;
		} else {
			dev_dbg(dev, "!I2C_M_RD start %d len %d\n",
				gdev->do_start, msgs[i].len);
			if (gdev->do_start) {
				status = i2c_manual_start(gdev, msgs[i].addr);
				if (status != STATUS_SUCCESS) {
					dev_err(dev,
					"%s:%d i2c_manual_start failed %08lx",
						__func__, __LINE__,
						(unsigned long)status);
					goto exit;
				}
				status = i2c_manual_write(gdev,
						msgs[i].addr << 1);
				if (status != STATUS_SUCCESS) {
					dev_err(dev,
					"%s:%d i2c_manual_write failed %08lx",
						__func__, __LINE__,
						(unsigned long)status);
					goto exit_stop;
				}
				gdev->do_start = false;
			}
			for (j = 0; j < msgs[i].len; j++) {
				status = i2c_manual_write(gdev,
						*(msgs[i].buf + j));
				if (status != STATUS_SUCCESS) {
					dev_err(dev,
					"%s:%d i2c_manual_write failed %08lx",
						__func__, __LINE__,
						(unsigned long)status);
					goto exit_stop;
				}
			}
		}
	}
	goto exit;
exit_stop:
	status = i2c_manual_stop(gdev);
	if (status != STATUS_SUCCESS)
		dev_err(dev, "i2c_manual_stop failed %x", status);
exit:
	mutex_unlock(&gdev->mutex);
	return i;
}

static u32 gpu_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm gpu_i2c_algorithm = {
	.master_xfer	= gpu_i2c_master_xfer,
	.functionality	= gpu_i2c_functionality,
};

static int gpu_i2c_dev_init(struct gpu_i2c_dev *gdev)
{
	gdev->do_start = true;

	/* initialize mutex */
	mutex_init(&gdev->mutex);

	/* initialize i2c */
	enable_i2c_bus(gdev);

	return 0;
}

struct i2c_board_info gpu_i2c_ucsi_board_info = {
	I2C_BOARD_INFO("i2c-gpu-ucsi", CCGX_I2C_DEV_ADDRESS),
};

#define PCI_CLASS_SERIAL_UNKNOWN	0x0c80
/* pci driver */
static const struct pci_device_id gpu_i2c_ids[] = {
	{ PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID, PCI_ANY_ID, PCI_ANY_ID,
		PCI_CLASS_SERIAL_UNKNOWN << 8, 0xffffff00},
	{ },
};
MODULE_DEVICE_TABLE(pci, gpu_i2c_ids);

static int gpu_i2c_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct gpu_i2c_dev *gdev;
	int status;

	dev_info(&dev->dev,
		"dev %p id %08x %08x sub %08x %08x class %08x %08x\n",
		dev, id->vendor, id->device, id->subvendor, id->subdevice,
		id->class, id->class_mask);

	gdev = devm_kzalloc(&dev->dev, sizeof(struct gpu_i2c_dev), GFP_KERNEL);
	if (!gdev)
		return -ENOMEM;

	gdev->pci_dev = dev;
	pci_set_drvdata(dev, gdev);

	status = pci_enable_device(dev);
	if (status < 0) {
		dev_err(&dev->dev, "pci_enable_device failed - %d\n", status);
		return status;
	}

	pci_set_master(dev);

	gdev->regs = pci_iomap(dev, 0, 0);
	if (!gdev->regs) {
		dev_err(&dev->dev, "pci_iomap failed\n");
		status = -ENOMEM;
		goto iomap_err;
	}

	status = pci_enable_msi(dev);
	if (status < 0) {
		dev_err(&dev->dev, "pci_enable_msi failed - %d\n", status);
		goto enable_msi_err;
	}

	status = gpu_i2c_dev_init(gdev);
	if (status < 0) {
		dev_err(&dev->dev, "gpu_i2c_dev_init failed - %d\n", status);
		goto i2c_init_err;
	}

	i2c_set_adapdata(&gdev->adapter, gdev);
	gdev->adapter.owner = THIS_MODULE;
	strlcpy(gdev->adapter.name, "NVIDIA GPU I2C adapter",
		sizeof(gdev->adapter.name));
	gdev->adapter.algo = &gpu_i2c_algorithm;
	gdev->adapter.dev.parent = &dev->dev;
	status = i2c_add_adapter(&gdev->adapter);
	if (status < 0) {
		dev_err(&dev->dev, "i2c_add_adapter failed - %d\n", status);
		goto add_adapter_err;
	}

	gpu_i2c_ucsi_board_info.irq = dev->irq;
	gdev->client = i2c_new_device(&gdev->adapter,
			&gpu_i2c_ucsi_board_info);

	if (!gdev->client) {
		dev_err(&dev->dev, "i2c_new_device failed - %d\n", status);
		status = -ENODEV;
		goto add_adapter_err;
	}

	dev_set_drvdata(&dev->dev, gdev);
	pm_runtime_put_noidle(&dev->dev);
	pm_runtime_allow(&dev->dev);

	return 0;

add_adapter_err:
	i2c_del_adapter(&gdev->adapter);
i2c_init_err:
	pci_disable_msi(dev);
enable_msi_err:
	pci_iounmap(dev, gdev->regs);
iomap_err:
	pci_disable_device(dev);
	return status;
}

static void gpu_i2c_remove(struct pci_dev *dev)
{
	struct gpu_i2c_dev *gdev = pci_get_drvdata(dev);

	i2c_del_adapter(&gdev->adapter);
	pci_disable_msi(dev);
	pci_iounmap(dev, gdev->regs);
}

static int gpu_i2c_suspend(struct device *dev)
{
	dev_dbg(dev, "%s\n", __func__);

	return 0;
}

static int gpu_i2c_resume(struct device *dev)
{
	struct gpu_i2c_dev *gdev = dev_get_drvdata(dev);

	dev_dbg(dev, "%s\n", __func__);

	enable_i2c_bus(gdev);

	return 0;
}

static int gpu_i2c_idle(struct device *dev)
{
	struct gpu_i2c_dev *gdev = dev_get_drvdata(dev);

	if (!mutex_trylock(&gdev->mutex)) {
		dev_info(dev, "%s: -EBUSY\n", __func__);
		return -EBUSY;
	}
	mutex_unlock(&gdev->mutex);

	return 0;
}

UNIVERSAL_DEV_PM_OPS(gpu_i2c_driver_pm, gpu_i2c_suspend, gpu_i2c_resume,
	gpu_i2c_idle);

static struct pci_driver gpu_i2c_driver = {
	.name		= "gpu_i2c_driver",
	.id_table	= gpu_i2c_ids,
	.probe		= gpu_i2c_probe,
	.remove		= gpu_i2c_remove,
	.driver		= {
		.pm	= &gpu_i2c_driver_pm,
	},
};

module_pci_driver(gpu_i2c_driver);

MODULE_AUTHOR("Ajay Gupta <ajayg@nvidia.com>");
MODULE_DESCRIPTION("Nvidia GPU I2C controller Driver");
MODULE_LICENSE("GPL v2");
