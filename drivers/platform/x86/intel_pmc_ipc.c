/*
 * intel_pmc_ipc.c: Driver for the Intel PMC IPC mechanism
 *
 * (C) Copyright 2014-2015 Intel Corporation
 *
 * This driver is based on Intel SCU IPC driver(intel_scu_opc.c) by
 *     Sreedhara DS <sreedhara.ds@intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 *
 * PMC running in ARC processor communicates with other entity running in IA
 * core through IPC mechanism which in turn messaging between IA core ad PMC.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/pm.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/pm_qos.h>
#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/sched.h>
#include <linux/atomic.h>
#include <linux/notifier.h>
#include <linux/suspend.h>
#include <linux/spinlock.h>
#include <linux/acpi.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/mfd/core.h>
#include <linux/regmap.h>

#include <asm/intel_pmc_ipc.h>

#include <linux/platform_data/itco_wdt.h>

/*
 * IPC registers
 * The IA write to IPC_CMD command register triggers an interrupt to the ARC,
 * The ARC handles the interrupt and services it, writing optional data to
 * the IPC1 registers, updates the IPC_STS response register with the status.
 */
#define		IPC_CMD_SIZE		16
#define		IPC_CMD_SUBCMD		12

/* Residency with clock rate at 19.2MHz to usecs */
#define S0IX_RESIDENCY_IN_USECS(d, s)		\
({						\
	u64 result = 10ull * ((d) + (s));	\
	do_div(result, 192);			\
	result;					\
})

/*
 * 16-byte buffer for sending data associated with IPC command.
 */
#define IPC_DATA_BUFFER_SIZE	16

/* exported resources from IFWI */
#define PLAT_RESOURCE_IPC_INDEX		0
#define PLAT_RESOURCE_IPC_SIZE		0x1000
#define PLAT_RESOURCE_GCR_OFFSET	0x1000
#define PLAT_RESOURCE_GCR_SIZE		0x1000
#define PLAT_RESOURCE_BIOS_DATA_INDEX	1
#define PLAT_RESOURCE_BIOS_IFACE_INDEX	2
#define PLAT_RESOURCE_TELEM_SSRAM_INDEX	3
#define PLAT_RESOURCE_ISP_DATA_INDEX	4
#define PLAT_RESOURCE_ISP_IFACE_INDEX	5
#define PLAT_RESOURCE_GTD_DATA_INDEX	6
#define PLAT_RESOURCE_GTD_IFACE_INDEX	7
#define PLAT_RESOURCE_MEM_MAX_INDEX	8
#define PLAT_RESOURCE_ACPI_IO_INDEX	0

/*
 * BIOS does not create an ACPI device for each PMC function,
 * but exports multiple resources from one ACPI device(IPC) for
 * multiple functions. This driver is responsible to create a
 * platform device and to export resources for those functions.
 */
#define TCO_DEVICE_NAME			"iTCO_wdt"
#define SMI_EN_OFFSET			0x40
#define SMI_EN_SIZE			4
#define TCO_BASE_OFFSET			0x60
#define TCO_REGS_SIZE			16
#define PUNIT_DEVICE_NAME		"intel_punit_ipc"
#define TELEMETRY_DEVICE_NAME		"intel_telemetry"
#define TELEM_SSRAM_SIZE		240
#define TELEM_PMC_SSRAM_OFFSET		0x1B00
#define TELEM_PUNIT_SSRAM_OFFSET	0x1A00

/* PMC register bit definitions */

/* PMC_CFG_REG bit masks */
#define PMC_CFG_NO_REBOOT_MASK		(1 << 4)
#define PMC_CFG_NO_REBOOT_EN		(1 << 4)
#define PMC_CFG_NO_REBOOT_DIS		(0 << 4)

/* IPC PMC commands */
#define	IPC_DEV_PMC_CMD_MSI			BIT(8)
#define	IPC_DEV_PMC_CMD_SIZE			16
#define	IPC_DEV_PMC_CMD_SUBCMD			12
#define	IPC_DEV_PMC_CMD_STATUS			BIT(2)
#define	IPC_DEV_PMC_CMD_STATUS_IRQ		BIT(2)
#define	IPC_DEV_PMC_CMD_STATUS_ERR		BIT(1)
#define	IPC_DEV_PMC_CMD_STATUS_ERR_MASK		GENMASK(7, 0)
#define	IPC_DEV_PMC_CMD_STATUS_BUSY		BIT(0)

/*IPC PMC reg offsets */
#define IPC_DEV_PMC_STATUS_OFFSET		0x04
#define IPC_DEV_PMC_SPTR_OFFSET			0x08
#define IPC_DEV_PMC_DPTR_OFFSET			0x0C
#define IPC_DEV_PMC_WRBUF_OFFSET		0x80
#define IPC_DEV_PMC_RBUF_OFFSET			0x90

static struct intel_pmc_ipc_dev {
	struct device *dev;
	struct intel_ipc_dev *pmc_ipc_dev;
	struct intel_ipc_dev_ops ops;
	struct intel_ipc_dev_cfg cfg;
	void __iomem *ipc_base;

	/* gcr */
	void __iomem *gcr_mem_base;
	struct regmap *gcr_regs;

} ipcdev;

static struct regmap_config pmc_regmap_config = {
        .reg_bits = 32,
        .reg_stride = 4,
        .val_bits = 32,
};

static struct regmap_config gcr_regmap_config = {
        .reg_bits = 32,
        .reg_stride = 4,
        .val_bits = 32,
	.fast_io = true,
	.max_register = PLAT_RESOURCE_GCR_SIZE,
};

/**
 * intel_pmc_gcr_read() - Read PMC GCR register
 * @offset:	offset of GCR register from GCR address base
 * @data:	data pointer for storing the register output
 *
 * Reads the PMC GCR register of given offset.
 *
 * Return:	negative value on error or 0 on success.
 */
int intel_pmc_gcr_read(u32 offset, u32 *data)
{
	struct intel_pmc_ipc_dev *pmc = &ipcdev;

	if (!pmc->gcr_regs)
		return -EACCES;

	return regmap_read(pmc->gcr_regs, offset, data);
}
EXPORT_SYMBOL_GPL(intel_pmc_gcr_read);

/**
 * intel_pmc_gcr_write() - Write PMC GCR register
 * @offset:	offset of GCR register from GCR address base
 * @data:	register update value
 *
 * Writes the PMC GCR register of given offset with given
 * value.
 *
 * Return:	negative value on error or 0 on success.
 */
int intel_pmc_gcr_write(u32 offset, u32 data)
{
	struct intel_pmc_ipc_dev *pmc = &ipcdev;

	if (!pmc->gcr_regs)
		return -EACCES;

	return regmap_write(pmc->gcr_regs, offset, data);
}
EXPORT_SYMBOL_GPL(intel_pmc_gcr_write);

/**
 * intel_pmc_gcr_update() - Update PMC GCR register bits
 * @offset:	offset of GCR register from GCR address base
 * @mask:	bit mask for update operation
 * @val:	update value
 *
 * Updates the bits of given GCR register as specified by
 * @mask and @val.
 *
 * Return:	negative value on error or 0 on success.
 */
int intel_pmc_gcr_update(u32 offset, u32 mask, u32 val)
{
	struct intel_pmc_ipc_dev *pmc = &ipcdev;

	if (!pmc->gcr_regs)
		return -EACCES;

	return regmap_update_bits(pmc->gcr_regs, offset, mask, val);
}
EXPORT_SYMBOL_GPL(intel_pmc_gcr_update);

static int update_no_reboot_bit(void *priv, bool set)
{
	u32 value = set ? PMC_CFG_NO_REBOOT_EN : PMC_CFG_NO_REBOOT_DIS;

	return intel_pmc_gcr_update(PMC_GCR_PMC_CFG_REG,
				    PMC_CFG_NO_REBOOT_MASK, value);
}

static int pre_simple_cmd_fn(u32 *cmd_list, u32 cmdlen)
{
	if (!cmd_list || cmdlen != PMC_PARAM_LEN)
		return -EINVAL;

	cmd_list[0] |= (cmd_list[1] << IPC_CMD_SUBCMD);

	return 0;
}

static int pre_raw_cmd_fn(u32 *cmd_list, u32 cmdlen, u8 *in, u32 inlen,
		u32 *out, u32 outlen, u32 dptr, u32 sptr)
{
	int ret;

	if (inlen > IPC_DATA_BUFFER_SIZE || outlen > IPC_DATA_BUFFER_SIZE/4)
		return -EINVAL;

	ret = pre_simple_cmd_fn(cmd_list, cmdlen);
	if (ret < 0)
		return ret;

	cmd_list[0] |= (inlen << IPC_CMD_SIZE);

	return 0;
}

static int pmc_ipc_err_code(int status)
{
	return ((status >> IPC_DEV_PMC_CMD_SIZE) &
			IPC_DEV_PMC_CMD_STATUS_ERR_MASK);
}

static int pmc_ipc_busy_check(int status)
{
	return status | IPC_DEV_PMC_CMD_STATUS_BUSY;
}

static u32 pmc_ipc_enable_msi(u32 cmd)
{
	return cmd | IPC_DEV_PMC_CMD_MSI;
}

static struct intel_ipc_dev *intel_pmc_ipc_dev_create(
		struct device *pmc_dev,
		void __iomem *base,
		int irq)
{
	struct intel_ipc_dev_ops *ops;
	struct intel_ipc_dev_cfg *cfg;
	struct regmap *cmd_regs;

	cfg = devm_kzalloc(pmc_dev, sizeof(*cfg), GFP_KERNEL);
	if (!cfg)
		return ERR_PTR(-ENOMEM);

	ops = devm_kzalloc(pmc_dev, sizeof(*ops), GFP_KERNEL);
	if (!ops)
		return ERR_PTR(-ENOMEM);

        cmd_regs = devm_regmap_init_mmio_clk(pmc_dev, NULL, base,
			&pmc_regmap_config);
        if (IS_ERR(cmd_regs)) {
                dev_err(pmc_dev, "cmd_regs regmap init failed\n");
                return ERR_CAST(cmd_regs);;
        }

	/* set IPC dev ops */
	ops->to_err_code = pmc_ipc_err_code;
	ops->busy_check = pmc_ipc_busy_check;
	ops->enable_msi = pmc_ipc_enable_msi;
	ops->pre_raw_cmd_fn = pre_raw_cmd_fn;
	ops->pre_simple_cmd_fn = pre_simple_cmd_fn;

	/* set cfg options */
	if (irq > 0)
		cfg->mode = IPC_DEV_MODE_IRQ;
	else
		cfg->mode = IPC_DEV_MODE_POLLING;

	cfg->chan_type = IPC_CHANNEL_IA_PMC;
	cfg->irq = irq;
	cfg->use_msi = true;
	cfg->support_sptr = true;
	cfg->support_dptr = true;
	cfg->cmd_regs = cmd_regs;
	cfg->data_regs = cmd_regs;
	cfg->wrbuf_reg = IPC_DEV_PMC_WRBUF_OFFSET;
	cfg->rbuf_reg = IPC_DEV_PMC_RBUF_OFFSET;
	cfg->sptr_reg = IPC_DEV_PMC_SPTR_OFFSET;
	cfg->dptr_reg = IPC_DEV_PMC_DPTR_OFFSET;
	cfg->status_reg = IPC_DEV_PMC_STATUS_OFFSET;

	return devm_intel_ipc_dev_create(pmc_dev, INTEL_PMC_IPC_DEV, cfg, ops);
}

static int ipc_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int ret;
	struct intel_pmc_ipc_dev *pmc = &ipcdev;

	/* Only one PMC is supported */
	if (pmc->dev)
		return -EBUSY;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	ret = pcim_iomap_regions(pdev, 1 << 0, pci_name(pdev));
	if (ret)
		return ret;

	pmc->ipc_base =  pcim_iomap_table(pdev)[0];

	pmc->pmc_ipc_dev = intel_pmc_ipc_dev_create(&pdev->dev,
			pmc->ipc_base, pdev->irq);
	if (IS_ERR(pmc->pmc_ipc_dev)) {
		dev_err(&pdev->dev,
				"Failed to create PMC IPC device\n");
		return PTR_ERR(pmc->pmc_ipc_dev);
	}

	pmc->dev = &pdev->dev;

	pci_set_drvdata(pdev, pmc);

	return 0;
}

static const struct pci_device_id ipc_pci_ids[] = {
	{PCI_VDEVICE(INTEL, 0x0a94), 0},
	{PCI_VDEVICE(INTEL, 0x1a94), 0},
	{PCI_VDEVICE(INTEL, 0x5a94), 0},
	{ 0,}
};
MODULE_DEVICE_TABLE(pci, ipc_pci_ids);

static struct pci_driver ipc_pci_driver = {
	.name = "intel_pmc_ipc",
	.id_table = ipc_pci_ids,
	.probe = ipc_pci_probe,
};

static ssize_t intel_pmc_ipc_simple_cmd_store(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t count)
{
	struct intel_pmc_ipc_dev *pmc = dev_get_drvdata(dev);
	int cmd[2];
	int ret;

	ret = sscanf(buf, "%d %d", &cmd[0], &cmd[2]);
	if (ret != 2) {
		dev_err(dev, "Error args\n");
		return -EINVAL;
	}

	ret = ipc_dev_simple_cmd(pmc->pmc_ipc_dev, cmd, 2);
	if (ret) {
		dev_err(dev, "command %d error with %d\n", cmd[0], ret);
		return ret;
	}
	return (ssize_t)count;
}

static ssize_t intel_pmc_ipc_northpeak_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct intel_pmc_ipc_dev *pmc = dev_get_drvdata(dev);
	unsigned long val;
	int cmd[2] = {PMC_IPC_NORTHPEAK_CTRL, 0};
	int ret;

	if (kstrtoul(buf, 0, &val))
		return -EINVAL;

	if (val)
		cmd[1] = 1;

	ret = ipc_dev_simple_cmd(pmc->pmc_ipc_dev, cmd, 2);
	if (ret) {
		dev_err(dev, "command north %d error with %d\n", cmd[1], ret);
		return ret;
	}

	return (ssize_t)count;
}

static DEVICE_ATTR(simplecmd, S_IWUSR,
		   NULL, intel_pmc_ipc_simple_cmd_store);
static DEVICE_ATTR(northpeak, S_IWUSR,
		   NULL, intel_pmc_ipc_northpeak_store);

static struct attribute *intel_ipc_attrs[] = {
	&dev_attr_northpeak.attr,
	&dev_attr_simplecmd.attr,
	NULL
};

static const struct attribute_group intel_ipc_group = {
	.attrs = intel_ipc_attrs,
};

static struct itco_wdt_platform_data tco_info = {
	.name = "Apollo Lake SoC",
	.version = 5,
	.no_reboot_priv = &ipcdev,
	.update_no_reboot_bit = update_no_reboot_bit,
};

static int ipc_create_punit_device(struct platform_device *pdev)
{
	struct resource *res;
	static struct resource punit_res[PLAT_RESOURCE_MEM_MAX_INDEX];
	static struct mfd_cell punit_cell;
	int mindex, pindex = 0;

	for (mindex = 0; mindex <= PLAT_RESOURCE_MEM_MAX_INDEX; mindex++) {

		res = platform_get_resource(pdev, IORESOURCE_MEM, mindex);

		switch (mindex) {
		/* Get PUNIT resources */
		case PLAT_RESOURCE_BIOS_DATA_INDEX:
		case PLAT_RESOURCE_BIOS_IFACE_INDEX:
			/* BIOS resources are required, so return error if not
			 * available */
			if (!res) {
				dev_err(&pdev->dev,
					"Failed to get punit mem resource %d\n",
					pindex);
				return -ENXIO;
			}
		case PLAT_RESOURCE_ISP_DATA_INDEX:
		case PLAT_RESOURCE_ISP_IFACE_INDEX:
		case PLAT_RESOURCE_GTD_DATA_INDEX:
		case PLAT_RESOURCE_GTD_IFACE_INDEX:
			/* if not valid resource, skip the rest of steps */
			if (!res) {
				pindex++;
				continue;
			}
			memcpy(&punit_res[pindex], res, sizeof(*res));
			punit_res[pindex].flags = IORESOURCE_MEM;
			dev_info(&pdev->dev, "PUNIT memory res: %pR\n",
					&punit_res[pindex]);
			pindex++;
			break;
		};
	}

	/* Create PUNIT IPC MFD cell */
	punit_cell.name = PUNIT_DEVICE_NAME;
	punit_cell.id = -1;
	punit_cell.num_resources = ARRAY_SIZE(punit_res);
	punit_cell.resources = punit_res;
	punit_cell.ignore_resource_conflicts = 1;

	return devm_mfd_add_devices(&pdev->dev, PLATFORM_DEVID_AUTO,
			&punit_cell, 1, NULL, 0, NULL);
}

static int ipc_create_wdt_device(struct platform_device *pdev)
{
	static struct resource wdt_ipc_res[2];
	struct resource *res;
	static struct mfd_cell wdt_cell;

	/* If we have ACPI based watchdog use that instead, othewise create
	 * a MFD cell for iTCO watchdog*/
	if (acpi_has_watchdog())
		return 0;

	/* Get iTCO watchdog resources */
	res = platform_get_resource(pdev, IORESOURCE_IO,
				    PLAT_RESOURCE_ACPI_IO_INDEX);
	if (!res) {
		dev_err(&pdev->dev, "Failed to get wdt resource\n");
		return -ENXIO;
	}

	wdt_ipc_res[0].start = res->start + TCO_BASE_OFFSET;
	wdt_ipc_res[0].end = res->start + TCO_BASE_OFFSET +
		TCO_REGS_SIZE - 1;
	wdt_ipc_res[0].flags = IORESOURCE_IO;
	wdt_ipc_res[1].start = res->start + SMI_EN_OFFSET;
	wdt_ipc_res[1].end = res->start +
		SMI_EN_OFFSET + SMI_EN_SIZE - 1;
	wdt_ipc_res[1].flags = IORESOURCE_IO;

	dev_info(&pdev->dev, "watchdog res 0: %pR\n",
			&wdt_ipc_res[0]);
	dev_info(&pdev->dev, "watchdog res 1: %pR\n",
			&wdt_ipc_res[1]);

	wdt_cell.name = TCO_DEVICE_NAME;
	wdt_cell.id = -1;
	wdt_cell.platform_data = &tco_info;
	wdt_cell.pdata_size = sizeof(tco_info);
	wdt_cell.num_resources = ARRAY_SIZE(wdt_ipc_res);
	wdt_cell.resources = wdt_ipc_res;
	wdt_cell.ignore_resource_conflicts = 1;

	return devm_mfd_add_devices(&pdev->dev, PLATFORM_DEVID_AUTO,
			&wdt_cell, 1, NULL, 0, NULL);
}

static int ipc_create_telemetry_device(struct platform_device *pdev)
{
	static struct resource telemetry_ipc_res[2];
	struct resource *res;
	static struct mfd_cell telemetry_cell;

	/* Get telemetry resources */
	res = platform_get_resource(pdev, IORESOURCE_MEM,
				    PLAT_RESOURCE_TELEM_SSRAM_INDEX);
	if (!res) {
		dev_err(&pdev->dev, "Failed to get telemetry resource\n");
		return -ENXIO;
	}

	telemetry_ipc_res[0].start = res->start +
		TELEM_PUNIT_SSRAM_OFFSET;
	telemetry_ipc_res[0].end = res->start +
		TELEM_PUNIT_SSRAM_OFFSET + TELEM_SSRAM_SIZE - 1;
	telemetry_ipc_res[0].flags = IORESOURCE_MEM;
	telemetry_ipc_res[1].start = res->start + TELEM_PMC_SSRAM_OFFSET;
	telemetry_ipc_res[1].end = res->start +
		TELEM_PMC_SSRAM_OFFSET + TELEM_SSRAM_SIZE - 1;
	telemetry_ipc_res[1].flags = IORESOURCE_MEM;

	dev_info(&pdev->dev, "Telemetry res 0: %pR\n",
			&telemetry_ipc_res[0]);
	dev_info(&pdev->dev, "Telemetry res 1: %pR\n",
			&telemetry_ipc_res[1]);

	telemetry_cell.name = TELEMETRY_DEVICE_NAME;
	telemetry_cell.id = -1;
	telemetry_cell.num_resources = ARRAY_SIZE(telemetry_ipc_res);
	telemetry_cell.resources = telemetry_ipc_res;
	telemetry_cell.ignore_resource_conflicts = 1;

	return devm_mfd_add_devices(&pdev->dev, PLATFORM_DEVID_AUTO,
			&telemetry_cell, 1, NULL, 0, NULL);
}

static int ipc_create_pmc_devices(struct platform_device *pdev)
{
	int ret;

	ret = ipc_create_punit_device(pdev);
	if (ret < 0)
		return ret;

	ret = ipc_create_wdt_device(pdev);
	if (ret < 0)
		return ret;

	ret = ipc_create_telemetry_device(pdev);
	if (ret < 0)
		return ret;

	return 0;
}

static int ipc_plat_get_res(struct platform_device *pdev)
{
	struct intel_pmc_ipc_dev *pmc = dev_get_drvdata(&pdev->dev);
	struct resource *res;
	void __iomem *addr;

	/* Get IPC resources */
	res = platform_get_resource(pdev, IORESOURCE_MEM,
			PLAT_RESOURCE_IPC_INDEX);
	if (!res) {
		dev_err(&pdev->dev, "Failed to get IPC resources\n");
		return -ENXIO;
	}

	res->end = (res->start + PLAT_RESOURCE_IPC_SIZE +
			PLAT_RESOURCE_GCR_SIZE - 1);

	addr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(addr)) {
		dev_err(&pdev->dev, "PMC I/O memory remapping failed\n");
			return PTR_ERR(addr);
	}

	pmc->ipc_base = addr;
	pmc->gcr_mem_base = addr + PLAT_RESOURCE_GCR_OFFSET;
	dev_info(&pdev->dev, "PMC IPC resource %pR\n", res);

	return 0;
}

/**
 * intel_pmc_s0ix_counter_read() - Read S0ix residency.
 * @data: Out param that contains current S0ix residency count.
 *
 * Return: an error code or 0 on success.
 */
int intel_pmc_s0ix_counter_read(u64 *data)
{
	struct intel_pmc_ipc_dev *pmc = &ipcdev;
	u64 deep, shlw;
	int ret;

	if (!pmc->gcr_regs)
		return -EACCES;

	ret = regmap_bulk_read(pmc->gcr_regs, PMC_GCR_TELEM_DEEP_S0IX_REG,
			&deep, 2);
	if (ret)
		return ret;

	ret = regmap_bulk_read(pmc->gcr_regs, PMC_GCR_TELEM_SHLW_S0IX_REG,
			&shlw, 2);
	if (ret)
		return ret;

	*data = S0IX_RESIDENCY_IN_USECS(deep, shlw);

	return ret;
}
EXPORT_SYMBOL_GPL(intel_pmc_s0ix_counter_read);

#ifdef CONFIG_ACPI
static const struct acpi_device_id ipc_acpi_ids[] = {
	{ "INT34D2", 0},
	{ }
};
MODULE_DEVICE_TABLE(acpi, ipc_acpi_ids);
#endif

static int ipc_plat_probe(struct platform_device *pdev)
{
	int ret, irq;
	struct intel_pmc_ipc_dev *pmc = &ipcdev;

	pmc->dev = &pdev->dev;

	dev_set_drvdata(&pdev->dev, pmc);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "Failed to get irq\n");
		return -EINVAL;
	}

	ret = ipc_plat_get_res(pdev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request resource\n");
		return ret;
	}

        pmc->gcr_regs = devm_regmap_init_mmio_clk(pmc->dev, NULL,
			pmc->gcr_mem_base, &gcr_regmap_config);
        if (IS_ERR(pmc->gcr_regs)) {
                dev_err(&pdev->dev, "gcr_regs regmap init failed\n");
                return PTR_ERR(pmc->gcr_regs);;
        }

	ret = ipc_create_pmc_devices(pdev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to create pmc devices\n");
		return ret;
	}

	ret = sysfs_create_group(&pdev->dev.kobj, &intel_ipc_group);
	if (ret) {
		dev_err(&pdev->dev, "Failed to create sysfs group %d\n",
			ret);
		return ret;
	}

	ipcdev.pmc_ipc_dev = intel_pmc_ipc_dev_create(&pdev->dev,
			pmc->ipc_base, irq);
	if (IS_ERR(pmc->pmc_ipc_dev)) {
		dev_err(&pdev->dev, "Failed to create PMC IPC device\n");
		return PTR_ERR(pmc->pmc_ipc_dev);
	}

	return 0;
}

static int ipc_plat_remove(struct platform_device *pdev)
{
	sysfs_remove_group(&pdev->dev.kobj, &intel_ipc_group);
	ipcdev.dev = NULL;

	return 0;
}

static struct platform_driver ipc_plat_driver = {
	.remove = ipc_plat_remove,
	.probe = ipc_plat_probe,
	.driver = {
		.name = "pmc-ipc-plat",
		.acpi_match_table = ACPI_PTR(ipc_acpi_ids),
	},
};

static int __init intel_pmc_ipc_init(void)
{
	int ret;

	ret = platform_driver_register(&ipc_plat_driver);
	if (ret) {
		pr_err("Failed to register PMC ipc platform driver\n");
		return ret;
	}
	ret = pci_register_driver(&ipc_pci_driver);
	if (ret) {
		pr_err("Failed to register PMC ipc pci driver\n");
		platform_driver_unregister(&ipc_plat_driver);
		return ret;
	}
	return ret;
}

static void __exit intel_pmc_ipc_exit(void)
{
	pci_unregister_driver(&ipc_pci_driver);
	platform_driver_unregister(&ipc_plat_driver);
}

MODULE_AUTHOR("Zha Qipeng <qipeng.zha@intel.com>");
MODULE_DESCRIPTION("Intel PMC IPC driver");
MODULE_LICENSE("GPL");

/* Some modules are dependent on this, so init earlier */
fs_initcall(intel_pmc_ipc_init);
module_exit(intel_pmc_ipc_exit);
