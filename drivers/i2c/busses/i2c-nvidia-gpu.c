// SPDX-License-Identifier: GPL-2.0
/*
 * Nvidia GPU I2C controller Driver
 *
 * Copyright (C) 2018 NVIDIA Corporation. All rights reserved.
 * Author: Ajay Gupta <ajayg@nvidia.com>
 *
 */
#include <asm/unaligned.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>

/* I2C definitions */
#define I2C_MST_CNTL				0x0
#define I2C_MST_CNTL_GEN_START			BIT(0)
#define I2C_MST_CNTL_GEN_STOP			BIT(1)
#define I2C_MST_CNTL_CMD_NONE			(0 << 2)
#define I2C_MST_CNTL_CMD_READ			(1 << 2)
#define I2C_MST_CNTL_CMD_WRITE			(2 << 2)
#define I2C_MST_CNTL_GEN_RAB			BIT(4)
#define I2C_MST_CNTL_BURST_SIZE_SHIFT		(6)
#define I2C_MST_CNTL_GEN_NACK			BIT(28)
#define I2C_MST_CNTL_STATUS			(3 << 29)
#define I2C_MST_CNTL_STATUS_OKAY		(0 << 29)
#define I2C_MST_CNTL_STATUS_NO_ACK		(1 << 29)
#define I2C_MST_CNTL_STATUS_TIMEOUT		(2 << 29)
#define I2C_MST_CNTL_STATUS_BUS_BUSY		(3 << 29)
#define I2C_MST_CNTL_CYCLE_TRIGGER		BIT(31)

#define I2C_MST_ADDR				0x04
#define I2C_MST_ADDR_DAB			0

#define I2C_MST_I2C0_TIMING				0x08
#define I2C_MST_I2C0_TIMING_SCL_PERIOD_100KHZ		0x10e
#define I2C_MST_I2C0_TIMING_TIMEOUT_CLK_CNT		16
#define I2C_MST_I2C0_TIMING_TIMEOUT_CLK_CNT_MAX		255
#define I2C_MST_I2C0_TIMING_TIMEOUT_CHECK		BIT(24)

#define I2C_MST_DATA					0x0c

#define I2C_MST_HYBRID_PADCTL				0x20
#define I2C_MST_HYBRID_PADCTL_MODE_I2C			BIT(0)
#define I2C_MST_HYBRID_PADCTL_I2C_SCL_INPUT_RCV		BIT(14)
#define I2C_MST_HYBRID_PADCTL_I2C_SDA_INPUT_RCV		BIT(15)

struct gpu_i2c_dev {
	struct pci_dev *pci_dev;
	void __iomem *regs;
	struct i2c_adapter adapter;
	struct i2c_client *client;
	struct mutex mutex;	/* to sync read/write */
	bool do_start;
};

static void enable_i2c_bus(struct gpu_i2c_dev *i2cd)
{
	u32 val;

	/* enable I2C */
	val = readl(i2cd->regs + I2C_MST_HYBRID_PADCTL);
	val |= I2C_MST_HYBRID_PADCTL_MODE_I2C |
		I2C_MST_HYBRID_PADCTL_I2C_SCL_INPUT_RCV |
		I2C_MST_HYBRID_PADCTL_I2C_SDA_INPUT_RCV;
	writel(val, i2cd->regs + I2C_MST_HYBRID_PADCTL);

	/* enable 100KHZ mode */
	val = I2C_MST_I2C0_TIMING_SCL_PERIOD_100KHZ;
	val |= (I2C_MST_I2C0_TIMING_TIMEOUT_CLK_CNT_MAX
	    << I2C_MST_I2C0_TIMING_TIMEOUT_CLK_CNT);
	val |= I2C_MST_I2C0_TIMING_TIMEOUT_CHECK;
	writel(val, i2cd->regs + I2C_MST_I2C0_TIMING);
}

static int i2c_check_status(struct gpu_i2c_dev *i2cd)
{
	unsigned long target = jiffies + msecs_to_jiffies(1000);
	int status = -EIO;
	u32 val;

	do {
		val = readl(i2cd->regs + I2C_MST_CNTL);
		if ((val & I2C_MST_CNTL_CYCLE_TRIGGER) !=
				I2C_MST_CNTL_CYCLE_TRIGGER)
			break;
		if ((val & I2C_MST_CNTL_STATUS) !=
				I2C_MST_CNTL_STATUS_BUS_BUSY)
			break;
		usleep_range(1000, 2000);
	} while (time_is_after_jiffies(target));

	if (time_is_before_jiffies(target))
		return status;

	val = readl(i2cd->regs + I2C_MST_CNTL);
	switch (val & I2C_MST_CNTL_STATUS) {
	case I2C_MST_CNTL_STATUS_OKAY:
		status = 0;
		break;
	case I2C_MST_CNTL_STATUS_NO_ACK:
		status = -EIO;
		break;
	case I2C_MST_CNTL_STATUS_TIMEOUT:
		status = -ETIME;
		break;
	case I2C_MST_CNTL_STATUS_BUS_BUSY:
		status = -EBUSY;
		break;
	default:
		break;
	}
	return status;
}

static int i2c_read(struct gpu_i2c_dev *i2cd, u8 *data, u16 len)
{
	int status;
	u32 val = 0;

	val |= I2C_MST_CNTL_GEN_START | I2C_MST_CNTL_GEN_STOP |
		I2C_MST_CNTL_CMD_READ | (len << I2C_MST_CNTL_BURST_SIZE_SHIFT) |
		I2C_MST_CNTL_CYCLE_TRIGGER | I2C_MST_CNTL_GEN_NACK;
	val &= ~I2C_MST_CNTL_GEN_RAB;
	writel(val, i2cd->regs + I2C_MST_CNTL);

	status = i2c_check_status(i2cd);
	if (status < 0)
		return status;

	val = readl(i2cd->regs + I2C_MST_DATA);
	switch (len) {
	case 1:
		data[0] = val;
		break;
	case 2:
		put_unaligned_be16(val, data);
		break;
	case 3:
		put_unaligned_be16(val >> 8, data);
		data[2] = val;
		break;
	case 4:
		put_unaligned_be32(val, data);
		break;
	default:
		break;
	}
	return status;
}

static int i2c_start(struct gpu_i2c_dev *i2cd, u16 addr)
{
	u32 val;

	val = addr << I2C_MST_ADDR_DAB;
	writel(val, i2cd->regs + I2C_MST_ADDR);

	val = I2C_MST_CNTL_GEN_START | I2C_MST_CNTL_CMD_NONE |
		I2C_MST_CNTL_GEN_NACK;
	val &= ~(I2C_MST_CNTL_GEN_STOP | I2C_MST_CNTL_GEN_RAB);
	writel(val, i2cd->regs + I2C_MST_CNTL);

	return i2c_check_status(i2cd);
}

static int i2c_stop(struct gpu_i2c_dev *i2cd)
{
	u32 val;

	val = I2C_MST_CNTL_GEN_STOP | I2C_MST_CNTL_CMD_NONE |
		I2C_MST_CNTL_GEN_NACK;
	val &= ~(I2C_MST_CNTL_GEN_START | I2C_MST_CNTL_GEN_RAB);
	writel(val, i2cd->regs + I2C_MST_CNTL);

	return i2c_check_status(i2cd);
}

static int i2c_write(struct gpu_i2c_dev *i2cd, u8 data)
{
	u32 val;

	writel(data, i2cd->regs + I2C_MST_DATA);

	val = I2C_MST_CNTL_CMD_WRITE | (1 << I2C_MST_CNTL_BURST_SIZE_SHIFT) |
		I2C_MST_CNTL_GEN_NACK;
	val &= ~(I2C_MST_CNTL_GEN_START | I2C_MST_CNTL_GEN_STOP
		| I2C_MST_CNTL_GEN_RAB);
	writel(val, i2cd->regs + I2C_MST_CNTL);

	return i2c_check_status(i2cd);
}

static int gpu_i2c_master_xfer(struct i2c_adapter *adap,
			       struct i2c_msg *msgs, int num)
{
	struct gpu_i2c_dev *i2cd = i2c_get_adapdata(adap);
	struct device *dev = &i2cd->pci_dev->dev;
	int sts;
	int i, j;

	mutex_lock(&i2cd->mutex);
	for (i = 0; i < num; i++) {
		if (msgs[i].flags & I2C_M_RD) {
			sts = i2c_read(i2cd, msgs[i].buf, msgs[i].len);
			if (sts < 0) {
				dev_err(dev, "i2c_read error %x", sts);
				break;
			}
			i2cd->do_start = true;
		} else if (msgs[i].flags & I2C_M_STOP) {
			sts = i2c_stop(i2cd);
			if (sts < 0) {
				dev_err(dev, "i2c_stop error %x", sts);
				goto unlock;
			}
			i2cd->do_start = true;
		} else {
			if (i2cd->do_start) {
				sts = i2c_start(i2cd, msgs[i].addr);
				if (sts < 0) {
					dev_err(dev, "i2c_start error %x", sts);
					goto unlock;
				}
				sts = i2c_write(i2cd, msgs[i].addr << 1);
				if (sts < 0) {
					dev_err(dev, "i2c_write error %x", sts);
					goto stop;
				}
				i2cd->do_start = false;
			}
			for (j = 0; j < msgs[i].len; j++) {
				sts = i2c_write(i2cd, *(msgs[i].buf + j));
				if (sts < 0) {
					dev_err(dev, "i2c_write error %x", sts);
					goto stop;
				}
			}
		}
	}
	goto unlock;
stop:
	sts = i2c_stop(i2cd);
	if (sts < 0)
		dev_err(dev, "i2c_stop error %x", sts);
unlock:
	mutex_unlock(&i2cd->mutex);
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

#define PCI_CLASS_SERIAL_UNKNOWN	0x0c80
static const struct pci_device_id gpu_i2c_ids[] = {
	{ PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID, PCI_ANY_ID, PCI_ANY_ID,
		PCI_CLASS_SERIAL_UNKNOWN << 8, 0xffffff00},
	{ }
};
MODULE_DEVICE_TABLE(pci, gpu_i2c_ids);

static int gpu_i2c_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct i2c_board_info gpu_i2c_ucsi_board_info = {
		I2C_BOARD_INFO("ccgx-ucsi", 0x8)
	};
	struct gpu_i2c_dev *i2cd;
	int status;

	i2cd = devm_kzalloc(&dev->dev, sizeof(struct gpu_i2c_dev), GFP_KERNEL);
	if (!i2cd)
		return -ENOMEM;

	i2cd->pci_dev = dev;
	pci_set_drvdata(dev, i2cd);

	status = pcim_enable_device(dev);
	if (status < 0) {
		dev_err(&dev->dev, "pcim_enable_device failed - %d\n", status);
		return status;
	}

	pci_set_master(dev);

	i2cd->regs = pcim_iomap(dev, 0, 0);
	if (!i2cd->regs) {
		dev_err(&dev->dev, "pcim_iomap failed\n");
		return -ENOMEM;
	}

	status = pci_alloc_irq_vectors(dev, 1, 1, PCI_IRQ_MSI);
	if (status < 0) {
		dev_err(&dev->dev, "pci_alloc_irq_vectors err - %d\n", status);
		return status;
	}

	i2cd->do_start = true;
	mutex_init(&i2cd->mutex);
	enable_i2c_bus(i2cd);

	i2c_set_adapdata(&i2cd->adapter, i2cd);
	i2cd->adapter.owner = THIS_MODULE;
	strlcpy(i2cd->adapter.name, "NVIDIA GPU I2C adapter",
		sizeof(i2cd->adapter.name));
	i2cd->adapter.algo = &gpu_i2c_algorithm;
	i2cd->adapter.dev.parent = &dev->dev;
	status = i2c_add_adapter(&i2cd->adapter);
	if (status < 0) {
		dev_err(&dev->dev, "i2c_add_adapter failed - %d\n", status);
		goto del_adapter;
	}

	gpu_i2c_ucsi_board_info.irq = dev->irq;
	i2cd->client = i2c_new_device(&i2cd->adapter, &gpu_i2c_ucsi_board_info);

	if (!i2cd->client) {
		dev_err(&dev->dev, "i2c_new_device failed - %d\n", status);
		status = -ENODEV;
		goto del_adapter;
	}

	pm_runtime_put_noidle(&dev->dev);
	pm_runtime_allow(&dev->dev);

	return 0;

del_adapter:
	i2c_del_adapter(&i2cd->adapter);
	pci_free_irq_vectors(dev);
	return status;
}

static void gpu_i2c_remove(struct pci_dev *dev)
{
	struct gpu_i2c_dev *i2cd = pci_get_drvdata(dev);

	i2c_del_adapter(&i2cd->adapter);
	pci_free_irq_vectors(dev);
}

static int gpu_i2c_resume(struct device *dev)
{
	struct gpu_i2c_dev *i2cd = pci_get_drvdata(to_pci_dev(dev));

	enable_i2c_bus(i2cd);
	return 0;
}

static int gpu_i2c_idle(struct device *dev)
{
	struct gpu_i2c_dev *i2cd = pci_get_drvdata(to_pci_dev(dev));

	if (!mutex_trylock(&i2cd->mutex)) {
		dev_info(dev, "-EBUSY\n");
		return -EBUSY;
	}
	mutex_unlock(&i2cd->mutex);

	return 0;
}

UNIVERSAL_DEV_PM_OPS(gpu_i2c_driver_pm, NULL, gpu_i2c_resume, gpu_i2c_idle);

static struct pci_driver gpu_i2c_driver = {
	.name		= "nvidia-gpu",
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
