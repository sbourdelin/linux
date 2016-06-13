/*
 * Intel MID Power Management Unit device driver
 *
 * Copyright (C) 2016, Intel Corporation
 *
 * Author: Andy Shevchenko <andriy.shevchenko@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/types.h>

#include <asm/intel-mid.h>

/* Registers */
#define PM_STS		0x00
#define PM_CMD		0x04
#define PM_ICS		0x08
#define PM_WKC(x)	(0x10 + (x) * 4)
#define PM_WKS(x)	(0x18 + (x) * 4)
#define PM_SSC(x)	(0x20 + (x) * 4)
#define PM_SSS(x)	(0x30 + (x) * 4)

/* Bits in PM_STS */
#define PM_STS_BUSY		(1 << 8)

/* Bits in PM_CMD */
#define PM_CMD_CMD(x)		((x) << 0)
#define PM_CMD_IOC		(1 << 8)
#define PM_CMD_D3cold		(1 << 21)

/* List of commands */
#define CMD_SET_CFG		0x01

/* Bits in PM_ICS */
#define PM_ICS_INT_STATUS(x)	((x) & 0xff)
#define PM_ICS_IE		(1 << 8)
#define PM_ICS_IP		(1 << 9)
#define PM_ICS_SW_INT_STS	(1 << 10)

/* List of interrupts */
#define INT_INVALID		0
#define INT_CMD_COMPLETE	1
#define INT_CMD_ERR		2
#define INT_WAKE_EVENT		3
#define INT_LSS_POWER_ERR	4
#define INT_S0iX_MSG_ERR	5
#define INT_NO_C6		6
#define INT_TRIGGER_ERR		7
#define INT_INACTIVITY		8

/* South Complex devices */
#define LSS_MAX_SHARED_DEVS		4
#define LSS_MAX_DEVS			64

#define LSS_WS_BITS			1	/* wake state width */
#define LSS_PWS_BITS			2	/* power state width */

/* Supported device IDs */
#define PCI_DEVICE_ID_TANGIER		0x11a1

struct mid_pmu_dev {
	struct pci_dev *pdev;
	pci_power_t state;
};

struct mid_pmu {
	struct device *dev;
	void __iomem *regs;
	int irq;
	bool available;

	struct mutex lock;
	struct mid_pmu_dev lss[LSS_MAX_DEVS][LSS_MAX_SHARED_DEVS];
};

static struct mid_pmu *midpmu;

static u32 mid_pmu_get_state(struct mid_pmu *pmu, int reg)
{
	return readl(pmu->regs + PM_SSS(reg));
}

static void mid_pmu_set_state(struct mid_pmu *pmu, int reg, u32 value)
{
	writel(value, pmu->regs + PM_SSC(reg));
}

static void mid_pmu_set_wake(struct mid_pmu *pmu, int reg, u32 value)
{
	writel(value, pmu->regs + PM_WKC(reg));
}

static void mid_pmu_interrupt_disable(struct mid_pmu *pmu)
{
	writel(~PM_ICS_IE, pmu->regs + PM_ICS);
}

static bool mid_pmu_is_busy(struct mid_pmu *pmu)
{
	return !!(readl(pmu->regs + PM_STS) & PM_STS_BUSY);
}

/* Wait 500ms that the latest PMU command finished */
static int mid_pmu_wait(struct mid_pmu *pmu)
{
	unsigned int count = 500000;
	bool busy;

	do {
		busy = mid_pmu_is_busy(pmu);
		if (!busy)
			return 0;
		udelay(1);
	} while (--count);

	return -EBUSY;
}

static int mid_pmu_wait_for_cmd(struct mid_pmu *pmu, u8 cmd)
{
	writel(PM_CMD_CMD(cmd), pmu->regs + PM_CMD);
	return mid_pmu_wait(pmu);
}

static int __update_power_state(struct mid_pmu *pmu, int reg, int bit, int new)
{
	int curstate;
	u32 power;
	int ret;

	/* Check if the device is already in desired state */
	power = mid_pmu_get_state(pmu, reg);
	curstate = (power >> bit) & 3;
	if (curstate == new)
		return 0;

	/* Update the power state */
	mid_pmu_set_state(pmu, reg, (power & ~(3 << bit)) | (new << bit));

	/* Send command to SCU */
	ret = mid_pmu_wait_for_cmd(pmu, CMD_SET_CFG);
	if (ret)
		return ret;

	/* Check if the device is already in desired state */
	power = mid_pmu_get_state(pmu, reg);
	curstate = (power >> bit) & 3;
	if (curstate != new)
		return -EAGAIN;

	return 0;
}

static pci_power_t __find_weakest_power_state(struct mid_pmu_dev *lss,
					      struct pci_dev *pdev,
					      pci_power_t state)
{
	pci_power_t weakest = PCI_D3hot;
	unsigned int j;

	/* Find device in cache or first free cell */
	for (j = 0; j < LSS_MAX_SHARED_DEVS; j++)
		if (lss[j].pdev == pdev || !lss[j].pdev)
			break;

	/* Store the desired state in cache */
	if (j < LSS_MAX_SHARED_DEVS) {
		lss[j].pdev = pdev;
		lss[j].state = state;
	} else {
		dev_WARN(&pdev->dev, "No room for device in PMU LSS cache\n");
		weakest = state;
	}

	/* Find the power state we may use */
	for (j = 0; j < LSS_MAX_SHARED_DEVS; j++)
		if (lss[j].state < weakest)
			weakest = lss[j].state;

	return weakest;
}

static int __set_power_state(struct mid_pmu *pmu, struct pci_dev *pdev,
			     pci_power_t state, int id, int reg, int bit)
{
	const char *name;
	int ret;

	state = __find_weakest_power_state(pmu->lss[id], pdev, state);
	name = pci_power_name(state);

	ret = __update_power_state(pmu, reg, bit, (__force int)state);
	if (ret) {
		dev_warn(&pdev->dev, "Can't set power state %s: %d\n", name, ret);
		return ret;
	}

	dev_vdbg(&pdev->dev, "Set power state %s\n", name);
	return 0;
}

static int mid_pmu_set_power_state(struct mid_pmu *pmu, struct pci_dev *pdev,
				   pci_power_t state)
{
	int id, reg, bit;
	int ret;

	id = intel_mid_pmu_get_lss_id(pdev);
	if (id < 0)
		return id;

	reg = (id * LSS_PWS_BITS) / 32;
	bit = (id * LSS_PWS_BITS) % 32;

	/* We support states between PCI_D0 and PCI_D3hot */
	if (state < PCI_D0)
		state = PCI_D0;
	if (state > PCI_D3hot)
		state = PCI_D3hot;

	mutex_lock(&pmu->lock);
	ret = __set_power_state(pmu, pdev, state, id, reg, bit);
	mutex_unlock(&pmu->lock);
	return ret;
}

int intel_mid_pci_set_power_state(struct pci_dev *pdev, pci_power_t state)
{
	struct mid_pmu *pmu = midpmu;
	int ret = 0;

	might_sleep();

	if (pmu && pmu->available)
		ret = mid_pmu_set_power_state(pmu, pdev, state);
	dev_vdbg(&pdev->dev, "set_power_state() returns %d\n", ret);

	return 0;
}
EXPORT_SYMBOL_GPL(intel_mid_pci_set_power_state);

int intel_mid_pmu_get_lss_id(struct pci_dev *pdev)
{
	int vndr;
	u8 id;

	/*
	 * Mapping to PMU index is kept in the Logical SubSystem ID byte of
	 * Vendor capability.
	 */
	vndr = pci_find_capability(pdev, PCI_CAP_ID_VNDR);
	if (!vndr)
		return -EINVAL;

	/* Read the Logical SubSystem ID byte */
	pci_read_config_byte(pdev, vndr + INTEL_MID_PMU_LSS_OFFSET, &id);
	if (!(id & INTEL_MID_PMU_LSS_TYPE))
		return -ENODEV;

	id &= ~INTEL_MID_PMU_LSS_TYPE;
	if (id >= LSS_MAX_DEVS)
		return -ERANGE;

	return id;
}

static irqreturn_t mid_pmu_irq_handler(int irq, void *dev_id)
{
	struct mid_pmu *pmu = dev_id;
	u32 ics;

	ics = readl(pmu->regs + PM_ICS);
	if (!(ics & PM_ICS_IP))
		return IRQ_NONE;

	writel(ics | PM_ICS_IP, pmu->regs + PM_ICS);

	dev_warn(pmu->dev, "Unexpected IRQ: %#x\n", PM_ICS_INT_STATUS(ics));
	return IRQ_HANDLED;
}

struct mid_pmu_device_info {
	int (*set_initial_state)(struct mid_pmu *pmu);
};

static int mid_pmu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct mid_pmu_device_info *info = (void *)id->driver_data;
	struct device *dev = &pdev->dev;
	struct mid_pmu *pmu;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "error: could not enable device\n");
		return ret;
	}

	ret = pcim_iomap_regions(pdev, 1 << 0, pci_name(pdev));
	if (ret) {
		dev_err(&pdev->dev, "I/O memory remapping failed\n");
		return ret;
	}

	pmu = devm_kzalloc(dev, sizeof(*pmu), GFP_KERNEL);
	if (!pmu)
		return -ENOMEM;

	pmu->dev = dev;
	pmu->regs = pcim_iomap_table(pdev)[0];
	pmu->irq = pdev->irq;

	mutex_init(&pmu->lock);

	/* Disable interrupts */
	mid_pmu_interrupt_disable(pmu);

	if (info && info->set_initial_state) {
		ret = info->set_initial_state(pmu);
		if (ret)
			dev_warn(dev, "Can't set initial state: %d\n", ret);
	}

	ret = devm_request_irq(dev, pdev->irq, mid_pmu_irq_handler,
			       IRQF_NO_SUSPEND, pci_name(pdev), pmu);
	if (ret)
		return ret;

	pmu->available = true;
	midpmu = pmu;

	pci_set_drvdata(pdev, pmu);
	return 0;
}

static int tng_set_initial_state(struct mid_pmu *pmu)
{
	unsigned int i, j;
	int ret;

	/* Enable wake events */
	mid_pmu_set_wake(pmu, 0, 0xffffffff);
	mid_pmu_set_wake(pmu, 1, 0xffffffff);

	/* Power off unused devices */
	mid_pmu_set_state(pmu, 0, 0xffffffff);
	mid_pmu_set_state(pmu, 1, 0xffffffff);
	mid_pmu_set_state(pmu, 2, 0xffffffff);
	mid_pmu_set_state(pmu, 3, 0xffffffff);

	/* Send command to SCU */
	ret = mid_pmu_wait_for_cmd(pmu, CMD_SET_CFG);
	if (ret)
		return ret;

	for (i = 0; i < LSS_MAX_DEVS; i++)
		for (j = 0; j < LSS_MAX_SHARED_DEVS; j++)
			pmu->lss[i][j].state = PCI_D3hot;

	return 0;
}

static const struct mid_pmu_device_info tng_info = {
	.set_initial_state = tng_set_initial_state,
};

static const struct pci_device_id mid_pmu_pci_ids[] = {
	{ PCI_VDEVICE(INTEL, PCI_DEVICE_ID_TANGIER), (kernel_ulong_t)&tng_info },
	{}
};
MODULE_DEVICE_TABLE(pci, mid_pmu_pci_ids);

static struct pci_driver mid_pmu_pci_driver = {
	.name		= "intel_mid_pmu",
	.probe		= mid_pmu_probe,
	.id_table	= mid_pmu_pci_ids,
};

builtin_pci_driver(mid_pmu_pci_driver);
