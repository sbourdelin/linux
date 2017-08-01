/*
 * Driver for the Intel P-Unit Mailbox IPC mechanism
 *
 * (C) Copyright 2015 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * The heart of the P-Unit is the Foxton microcontroller and its firmware,
 * which provide mailbox interface for power management usage.
 */

#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <asm/intel_punit_ipc.h>
#include <asm/intel_ipc_dev.h>

/* IPC Mailbox registers */
#define OFFSET_DATA_LOW		0x0
#define OFFSET_DATA_HIGH	0x4
/* bit field of interface register */
#define	CMD_RUN			BIT(31)
#define	CMD_PARA1_SHIFT		8
#define	CMD_PARA2_SHIFT		16

#define CMD_TIMEOUT_SECONDS	1

/* IPC PUNIT commands */
#define	IPC_DEV_PUNIT_CMD_STATUS_ERR_MASK	GENMASK(7, 0)
#define	IPC_DEV_PUNIT_CMD_STATUS_BUSY		BIT(31)

enum {
	BASE_DATA = 0,
	BASE_IFACE,
	BASE_MAX,
};

typedef struct {
	struct device *dev;
	/* base of interface and data registers */
	void __iomem *base[RESERVED_IPC][BASE_MAX];
	struct intel_ipc_dev *ipc_dev[RESERVED_IPC];
	IPC_TYPE type;
} IPC_DEV;

static IPC_DEV *punit_ipcdev;

const char *ipc_dev_name[RESERVED_IPC] = {
	"punit_bios_ipc",
	"punit_gtd_ipc",
	"punit_isp_ipc"
};

/**
 * intel_punit_ipc_simple_command() - Simple IPC command
 * @cmd:	IPC command code.
 * @para1:	First 8bit parameter, set 0 if not used.
 * @para2:	Second 8bit parameter, set 0 if not used.
 *
 * Send a IPC command to P-Unit when there is no data transaction
 *
 * Return:	IPC error code or 0 on success.
 */
int intel_punit_ipc_simple_command(int cmd, int para1, int para2)
{
	IPC_DEV *ipcdev = punit_ipcdev;
	IPC_TYPE type;
	u32 val;

	type = (cmd & IPC_PUNIT_CMD_TYPE_MASK) >> IPC_TYPE_OFFSET;

	val = cmd & ~IPC_PUNIT_CMD_TYPE_MASK;
	val |= CMD_RUN | para2 << CMD_PARA2_SHIFT | para1 << CMD_PARA1_SHIFT;

	return ipc_dev_simple_cmd(ipcdev->ipc_dev[type], val);
}
EXPORT_SYMBOL(intel_punit_ipc_simple_command);

/**
 * intel_punit_ipc_command() - IPC command with data and pointers
 * @cmd:	IPC command code.
 * @para1:	First 8bit parameter, set 0 if not used.
 * @para2:	Second 8bit parameter, set 0 if not used.
 * @in:		Input data, 32bit for BIOS cmd, two 32bit for GTD and ISPD.
 * @out:	Output data.
 *
 * Send a IPC command to P-Unit with data transaction
 *
 * Return:	IPC error code or 0 on success.
 */
int intel_punit_ipc_command(u32 cmd, u32 para1, u32 para2, u32 *in, u32 *out)
{
	IPC_DEV *ipcdev = punit_ipcdev;
	IPC_TYPE type;
	u32 val, len;

	type = (cmd & IPC_PUNIT_CMD_TYPE_MASK) >> IPC_TYPE_OFFSET;
	if (type == GTDRIVER_IPC || type == ISPDRIVER_IPC)
		len = 2;
	else
		len = 1;

	val = cmd & ~IPC_PUNIT_CMD_TYPE_MASK;
	val |= CMD_RUN | para2 << CMD_PARA2_SHIFT | para1 << CMD_PARA1_SHIFT;

	return ipc_dev_raw_cmd(ipcdev->ipc_dev[type], val, (u8*)in, len * 4,
			out, len, 0, 0);
}
EXPORT_SYMBOL_GPL(intel_punit_ipc_command);

static int intel_punit_get_bars(struct platform_device *pdev)
{
	struct resource *res;
	void __iomem *addr;

	/*
	 * The following resources are required
	 * - BIOS_IPC BASE_DATA
	 * - BIOS_IPC BASE_IFACE
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	addr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(addr))
		return PTR_ERR(addr);
	punit_ipcdev->base[BIOS_IPC][BASE_DATA] = addr;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	addr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(addr))
		return PTR_ERR(addr);
	punit_ipcdev->base[BIOS_IPC][BASE_IFACE] = addr;

	/*
	 * The following resources are optional
	 * - ISPDRIVER_IPC BASE_DATA
	 * - ISPDRIVER_IPC BASE_IFACE
	 * - GTDRIVER_IPC BASE_DATA
	 * - GTDRIVER_IPC BASE_IFACE
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	if (res) {
		addr = devm_ioremap_resource(&pdev->dev, res);
		if (!IS_ERR(addr))
			punit_ipcdev->base[ISPDRIVER_IPC][BASE_DATA] = addr;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 3);
	if (res) {
		addr = devm_ioremap_resource(&pdev->dev, res);
		if (!IS_ERR(addr))
			punit_ipcdev->base[ISPDRIVER_IPC][BASE_IFACE] = addr;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 4);
	if (res) {
		addr = devm_ioremap_resource(&pdev->dev, res);
		if (!IS_ERR(addr))
			punit_ipcdev->base[GTDRIVER_IPC][BASE_DATA] = addr;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 5);
	if (res) {
		addr = devm_ioremap_resource(&pdev->dev, res);
		if (!IS_ERR(addr))
			punit_ipcdev->base[GTDRIVER_IPC][BASE_IFACE] = addr;
	}

	return 0;
}

static int punit_ipc_err_code(int status)
{
	return (status & IPC_DEV_PUNIT_CMD_STATUS_ERR_MASK);
}

static int punit_ipc_busy_check(int status)
{
	return status | IPC_DEV_PUNIT_CMD_STATUS_BUSY;
}

static struct intel_ipc_dev *intel_punit_ipc_dev_create(struct device *dev,
		const char *devname,
		int irq,
		void __iomem *base,
		void __iomem *data)
{
	struct intel_ipc_dev_ops *ops;
	struct intel_ipc_dev_cfg *cfg;

        cfg = devm_kzalloc(dev, sizeof(*cfg), GFP_KERNEL);
        if (!cfg)
                return ERR_PTR(-ENOMEM);

	ops = devm_kzalloc(dev, sizeof(*ops), GFP_KERNEL);
	if (!ops)
		return ERR_PTR(-ENOMEM);

	/* set IPC dev ops */
	ops->to_err_code = punit_ipc_err_code;
	ops->busy_check = punit_ipc_busy_check;

	if (irq > 0)
	        cfg->mode = IPC_DEV_MODE_IRQ;
	else
	        cfg->mode = IPC_DEV_MODE_POLLING;

	cfg->chan_type = IPC_CHANNEL_IA_PUNIT;
	cfg->irq = irq;
	cfg->irqflags = IRQF_NO_SUSPEND | IRQF_SHARED;
	cfg->base = base;
	cfg->wrbuf_reg = data;
	cfg->rbuf_reg = data;
	cfg->status_reg = base;
	cfg->cmd_reg = base;

	return devm_intel_ipc_dev_create(dev, devname, cfg, ops);
}

static int intel_punit_ipc_probe(struct platform_device *pdev)
{
	int irq, ret, i;

	punit_ipcdev = devm_kzalloc(&pdev->dev,
				    sizeof(*punit_ipcdev), GFP_KERNEL);
	if (!punit_ipcdev)
		return -ENOMEM;

	platform_set_drvdata(pdev, punit_ipcdev);

	irq = platform_get_irq(pdev, 0);

	ret = intel_punit_get_bars(pdev);
	if (ret)
		return ret;

	for (i = 0; i < RESERVED_IPC; i++) {
		punit_ipcdev->ipc_dev[i] = intel_punit_ipc_dev_create(
				&pdev->dev,
				ipc_dev_name[i],
				irq,
				punit_ipcdev->base[i][BASE_IFACE],
				punit_ipcdev->base[i][BASE_DATA]);

		if (IS_ERR(punit_ipcdev->ipc_dev[i])) {
			dev_err(&pdev->dev, "%s create failed\n",
					ipc_dev_name[i]);
			return PTR_ERR(punit_ipcdev->ipc_dev[i]);
		}
	}

	punit_ipcdev->dev = &pdev->dev;

	return ret;

}

static const struct acpi_device_id punit_ipc_acpi_ids[] = {
	{ "INT34D4", 0 },
	{ }
};

static struct platform_driver intel_punit_ipc_driver = {
	.probe = intel_punit_ipc_probe,
	.driver = {
		.name = "intel_punit_ipc",
		.acpi_match_table = ACPI_PTR(punit_ipc_acpi_ids),
	},
};

static int __init intel_punit_ipc_init(void)
{
	return platform_driver_register(&intel_punit_ipc_driver);
}

static void __exit intel_punit_ipc_exit(void)
{
	platform_driver_unregister(&intel_punit_ipc_driver);
}

MODULE_AUTHOR("Zha Qipeng <qipeng.zha@intel.com>");
MODULE_DESCRIPTION("Intel P-Unit IPC driver");
MODULE_LICENSE("GPL v2");

/* Some modules are dependent on this, so init earlier */
fs_initcall(intel_punit_ipc_init);
module_exit(intel_punit_ipc_exit);
