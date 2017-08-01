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
#include <linux/acpi.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/mfd/core.h>

#include <asm/intel_pmc_ipc.h>
#include <asm/intel_ipc_dev.h>

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

enum {
	PMC_IPC_PUNIT_MFD_BLOCK,
	PMC_IPC_WATCHDOG_MFD_BLOCK,
	PMC_IPC_TELEMETRY_MFD_BLOCK,
	PMC_IPC_MAX_MFD_BLOCK
};

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
	bool irq_mode;
	int irq;
	int cmd;
	struct completion cmd_complete;

	/* gcr */
	void __iomem *gcr_mem_base;
	bool has_gcr_regs;

	/* Telemetry */
	u8 telem_res_inval;
} ipcdev;

/* Prevent concurrent calls to the PMC */
static DEFINE_MUTEX(ipclock);

static inline u64 gcr_data_readq(u32 offset)
{
	return readq(ipcdev.gcr_mem_base + offset);
}

static inline int is_gcr_valid(u32 offset)
{
	if (!ipcdev.has_gcr_regs)
		return -EACCES;

	if (offset > PLAT_RESOURCE_GCR_SIZE)
		return -EINVAL;

	return 0;
}

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
	int ret;

	mutex_lock(&ipclock);

	ret = is_gcr_valid(offset);
	if (ret < 0) {
		mutex_unlock(&ipclock);
		return ret;
	}

	*data = readl(ipcdev.gcr_mem_base + offset);

	mutex_unlock(&ipclock);

	return 0;
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
	int ret;

	mutex_lock(&ipclock);

	ret = is_gcr_valid(offset);
	if (ret < 0) {
		mutex_unlock(&ipclock);
		return ret;
	}

	writel(data, ipcdev.gcr_mem_base + offset);

	mutex_unlock(&ipclock);

	return 0;
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
	u32 new_val;
	int ret = 0;

	mutex_lock(&ipclock);

	ret = is_gcr_valid(offset);
	if (ret < 0)
		goto gcr_ipc_unlock;

	new_val = readl(ipcdev.gcr_mem_base + offset);

	new_val &= ~mask;
	new_val |= val & mask;

	writel(new_val, ipcdev.gcr_mem_base + offset);

	new_val = readl(ipcdev.gcr_mem_base + offset);

	/* check whether the bit update is successful */
	if ((new_val & mask) != (val & mask)) {
		ret = -EIO;
		goto gcr_ipc_unlock;
	}

gcr_ipc_unlock:
	mutex_unlock(&ipclock);
	return ret;
}
EXPORT_SYMBOL_GPL(intel_pmc_gcr_update);

static int update_no_reboot_bit(void *priv, bool set)
{
	u32 value = set ? PMC_CFG_NO_REBOOT_EN : PMC_CFG_NO_REBOOT_DIS;

	return intel_pmc_gcr_update(PMC_GCR_PMC_CFG_REG,
				    PMC_CFG_NO_REBOOT_MASK, value);
}

/**
 * intel_pmc_ipc_simple_command() - Simple IPC command
 * @cmd:	IPC command code.
 * @sub:	IPC command sub type.
 *
 * Send a simple IPC command to PMC when don't need to specify
 * input/output data and source/dest pointers.
 *
 * Return:	an IPC error code or 0 on success.
 */
int intel_pmc_ipc_simple_command(int cmd, int sub)
{
	return ipc_dev_simple_cmd(ipcdev.pmc_ipc_dev,
			sub << IPC_CMD_SUBCMD | cmd);
}
EXPORT_SYMBOL_GPL(intel_pmc_ipc_simple_command);


/**
 * intel_pmc_ipc_command() -  IPC command with input/output data
 * @cmd:	IPC command code.
 * @sub:	IPC command sub type.
 * @in:		input data of this IPC command.
 * @inlen:	input data length in bytes.
 * @out:	output data of this IPC command.
 * @outlen:	output data length in dwords.
 *
 * Send an IPC command to PMC with input/output data.
 *
 * Return:	an IPC error code or 0 on success.
 */
int intel_pmc_ipc_command(u32 cmd, u32 sub, u8 *in, u32 inlen,
			  u32 *out, u32 outlen)
{
	cmd = (inlen << IPC_CMD_SIZE) | (sub << IPC_CMD_SUBCMD) | cmd;

	if (inlen > IPC_DATA_BUFFER_SIZE || outlen > IPC_DATA_BUFFER_SIZE / 4)
		return -EINVAL;

	return ipc_dev_raw_cmd(ipcdev.pmc_ipc_dev, cmd, in, inlen, out,
			outlen, 0, 0);

}
EXPORT_SYMBOL_GPL(intel_pmc_ipc_command);

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

	cfg = devm_kzalloc(pmc_dev, sizeof(*cfg), GFP_KERNEL);
	if (!cfg)
		return ERR_PTR(-ENOMEM);

	ops = devm_kzalloc(pmc_dev, sizeof(*ops), GFP_KERNEL);
	if (!ops)
		return ERR_PTR(-ENOMEM);

	/* set IPC dev ops */
	ops->to_err_code = pmc_ipc_err_code;
	ops->busy_check = pmc_ipc_busy_check;
	ops->enable_msi = pmc_ipc_enable_msi;

	/* set cfg options */
	if (irq > 0)
		cfg->mode = IPC_DEV_MODE_IRQ;
	else
		cfg->mode = IPC_DEV_MODE_POLLING;

	cfg->chan_type = IPC_CHANNEL_IA_PMC;
	cfg->irq = irq;
	cfg->use_msi = true;
	cfg->base = base;
	cfg->wrbuf_reg = cfg->base + IPC_DEV_PMC_WRBUF_OFFSET;
	cfg->rbuf_reg = cfg->base + IPC_DEV_PMC_RBUF_OFFSET;
	cfg->sptr_reg = cfg->base + IPC_DEV_PMC_SPTR_OFFSET;
	cfg->dptr_reg = cfg->base + IPC_DEV_PMC_DPTR_OFFSET;
	cfg->status_reg = cfg->base + IPC_DEV_PMC_STATUS_OFFSET;
	cfg->cmd_reg = cfg->base;

	return devm_intel_ipc_dev_create(pmc_dev, "intel_pmc_ipc",
			cfg, ops);
}

static int ipc_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	resource_size_t pci_resource;
	int ret;
	int len;

	ipcdev.dev = &pci_dev_get(pdev)->dev;

	ipcdev.dev = &pdev->dev;

	ret = pci_enable_device(pdev);
	if (ret)
		goto release_device;

	ret = pci_request_regions(pdev, "intel_pmc_ipc");
	if (ret)
		goto disable_device;

	pci_resource = pci_resource_start(pdev, 0);
	len = pci_resource_len(pdev, 0);
	if (!pci_resource || !len) {
		dev_err(&pdev->dev, "Failed to get resource\n");
		ret = -ENOMEM;
		goto free_pci_resources;
	}

	ipcdev.ipc_base = devm_ioremap_nocache(&pdev->dev, pci_resource, len);
	if (!ipcdev.ipc_base) {
		dev_err(&pdev->dev, "Failed to ioremap ipc base\n");
		ret = -ENOMEM;
		goto free_pci_resources;
	}

	ipcdev.pmc_ipc_dev = intel_pmc_ipc_dev_create(&pdev->dev,
			ipcdev.ipc_base, pdev->irq);
	if (IS_ERR(ipcdev.pmc_ipc_dev)) {
		dev_err(ipcdev.dev,
				"Failed to create PMC IPC device\n");
		ret = PTR_ERR(ipcdev.pmc_ipc_dev);
		goto free_pci_resources;
	}

	return 0;

free_pci_resources:
	pci_release_regions(pdev);
disable_device:
	pci_disable_device(pdev);
release_device:
	pci_dev_put(pdev);

	return ret;
}

static void ipc_pci_remove(struct pci_dev *pdev)
{
	pci_release_regions(pdev);
	pci_dev_put(pdev);
	ipcdev.dev = NULL;
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
	.remove = ipc_pci_remove,
};

static ssize_t intel_pmc_ipc_simple_cmd_store(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t count)
{
	int subcmd;
	int cmd;
	int ret;

	ret = sscanf(buf, "%d %d", &cmd, &subcmd);
	if (ret != 2) {
		dev_err(dev, "Error args\n");
		return -EINVAL;
	}

	ret = intel_pmc_ipc_simple_command(cmd, subcmd);
	if (ret) {
		dev_err(dev, "command %d error with %d\n", cmd, ret);
		return ret;
	}
	return (ssize_t)count;
}

static ssize_t intel_pmc_ipc_northpeak_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	unsigned long val;
	int subcmd;
	int ret;

	if (kstrtoul(buf, 0, &val))
		return -EINVAL;

	if (val)
		subcmd = 1;
	else
		subcmd = 0;
	ret = intel_pmc_ipc_simple_command(PMC_IPC_NORTHPEAK_CTRL, subcmd);
	if (ret) {
		dev_err(dev, "command north %d error with %d\n", subcmd, ret);
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

static struct resource punit_ipc_resources[] = {
	/* Punit BIOS */
	{
		.flags = IORESOURCE_MEM,
	},
	{
		.flags = IORESOURCE_MEM,
	},
	/* Punit ISP */
	{
		.flags = IORESOURCE_MEM,
	},
	{
		.flags = IORESOURCE_MEM,
	},
	/* Punit GTD */
	{
		.flags = IORESOURCE_MEM,
	},
	{
		.flags = IORESOURCE_MEM,
	},
};

static struct resource watchdog_ipc_resources[] = {
	/* ACPI - TCO */
	{
		.flags = IORESOURCE_IO,
	},
	/* ACPI - SMI */
	{
		.flags = IORESOURCE_IO,
	},
};

static struct itco_wdt_platform_data tco_info = {
	.name = "Apollo Lake SoC",
	.version = 5,
	.no_reboot_priv = &ipcdev,
	.update_no_reboot_bit = update_no_reboot_bit,
};

static struct resource telemetry_ipc_resources[] = {
	/*Telemetry*/
	{
		.flags = IORESOURCE_MEM,
	},
	{
		.flags = IORESOURCE_MEM,
	},
};

static int ipc_create_pmc_devices(struct platform_device *pdev)
{
	u8 n = 0;
	struct mfd_cell *pmc_mfd_cells;

	pmc_mfd_cells = devm_kzalloc(&pdev->dev,
			(sizeof(*pmc_mfd_cells) * PMC_IPC_MAX_MFD_BLOCK),
			GFP_KERNEL);
	if (!pmc_mfd_cells)
		return -ENOMEM;

	/* Create PUNIT IPC MFD cell */
	pmc_mfd_cells[n].name = PUNIT_DEVICE_NAME;
	pmc_mfd_cells[n].id = -1;
	pmc_mfd_cells[n].num_resources = ARRAY_SIZE(punit_ipc_resources);
	pmc_mfd_cells[n].resources = punit_ipc_resources;
	pmc_mfd_cells[n].ignore_resource_conflicts = 1;
	n++;

	/* If we have ACPI based watchdog use that instead, othewise create
	 * a MFD cell for iTCO watchdog*/
	if (!acpi_has_watchdog()) {
		pmc_mfd_cells[n].name = TCO_DEVICE_NAME;
		pmc_mfd_cells[n].id = -1;
		pmc_mfd_cells[n].platform_data = &tco_info;
		pmc_mfd_cells[n].pdata_size = sizeof(tco_info);
		pmc_mfd_cells[n].num_resources =
			ARRAY_SIZE(watchdog_ipc_resources);
		pmc_mfd_cells[n].resources = watchdog_ipc_resources;
		pmc_mfd_cells[n].ignore_resource_conflicts = 1;
		n++;
	}

	if (!ipcdev.telem_res_inval) {
		pmc_mfd_cells[n].name = TELEMETRY_DEVICE_NAME;
		pmc_mfd_cells[n].id = -1;
		pmc_mfd_cells[n].num_resources =
			ARRAY_SIZE(telemetry_ipc_resources);
		pmc_mfd_cells[n].resources = telemetry_ipc_resources;
		pmc_mfd_cells[n].ignore_resource_conflicts = 1;
		n++;
	}

	return devm_mfd_add_devices(&pdev->dev, -1, pmc_mfd_cells, n, NULL,
				    0, NULL);
}

static int ipc_plat_get_res(struct platform_device *pdev)
{
	struct resource *res;
	void __iomem *addr;
	int mindex, pindex = 0;

	/* Get iTCO watchdog resources */
	res = platform_get_resource(pdev, IORESOURCE_IO,
				    PLAT_RESOURCE_ACPI_IO_INDEX);
	if (!res) {
		dev_err(&pdev->dev, "Failed to get io resource\n");
		return -ENXIO;
	}

	watchdog_ipc_resources[0].start = res->start + TCO_BASE_OFFSET;
	watchdog_ipc_resources[0].end = res->start +
		TCO_BASE_OFFSET + TCO_REGS_SIZE - 1;
	watchdog_ipc_resources[1].start = res->start + SMI_EN_OFFSET;
	watchdog_ipc_resources[1].end = res->start +
		SMI_EN_OFFSET + SMI_EN_SIZE - 1;

	dev_info(&pdev->dev, "watchdog res 0: %pR\n",
			&watchdog_ipc_resources[0]);
	dev_info(&pdev->dev, "watchdog res 1: %pR\n",
			&watchdog_ipc_resources[1]);

	for (mindex = 0; mindex <= PLAT_RESOURCE_GTD_IFACE_INDEX; mindex++) {

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
			memcpy(&punit_ipc_resources[pindex], res,
					sizeof(*res));
			dev_info(&pdev->dev, "PUNIT memory res: %pR\n",
					&punit_ipc_resources[pindex]);
			pindex++;
			break;
		/* Get Telemetry resources */
		case PLAT_RESOURCE_TELEM_SSRAM_INDEX:
			if (!res) {
				dev_warn(&pdev->dev,
					"Failed to get telemtry sram res\n");
				ipcdev.telem_res_inval = 1;
				continue;
			}
			telemetry_ipc_resources[0].start = res->start +
				TELEM_PUNIT_SSRAM_OFFSET;
			telemetry_ipc_resources[0].end = res->start +
				TELEM_PUNIT_SSRAM_OFFSET + TELEM_SSRAM_SIZE - 1;
			telemetry_ipc_resources[1].start = res->start +
				TELEM_PMC_SSRAM_OFFSET;
			telemetry_ipc_resources[1].end = res->start +
				TELEM_PMC_SSRAM_OFFSET + TELEM_SSRAM_SIZE - 1;

			dev_info(&pdev->dev, "telemetry punit ssram res: %pR\n",
					&telemetry_ipc_resources[0]);
			dev_info(&pdev->dev, "telemetry pmc ssram res: %pR\n",
					&telemetry_ipc_resources[1]);
			break;
		/* Get IPC resources */
		case PLAT_RESOURCE_IPC_INDEX:
			if (!res) {
				dev_err(&pdev->dev,
					"Failed to get IPC resources\n");
				return -ENXIO;
			}
			res->end = (res->start + PLAT_RESOURCE_IPC_SIZE +
				    PLAT_RESOURCE_GCR_SIZE - 1);
			addr = devm_ioremap_resource(&pdev->dev, res);
			if (IS_ERR(addr)) {
				dev_err(&pdev->dev,
					"PMC I/O memory remapping failed\n");
				return PTR_ERR(addr);
			}
			ipcdev.ipc_base = addr;
			ipcdev.gcr_mem_base = addr + PLAT_RESOURCE_GCR_OFFSET;
			dev_info(&pdev->dev, "PMC IPC resource %pR\n", res);
			break;
		};
	}

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
	u64 deep, shlw;

	if (!ipcdev.has_gcr_regs)
		return -EACCES;

	deep = gcr_data_readq(PMC_GCR_TELEM_DEEP_S0IX_REG);
	shlw = gcr_data_readq(PMC_GCR_TELEM_SHLW_S0IX_REG);

	*data = S0IX_RESIDENCY_IN_USECS(deep, shlw);

	return 0;
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

	ipcdev.dev = &pdev->dev;

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
			ipcdev.ipc_base, irq);
	if (IS_ERR(ipcdev.pmc_ipc_dev)) {
		dev_err(&pdev->dev, "Failed to create PMC IPC device\n");
		return PTR_ERR(ipcdev.pmc_ipc_dev);
	}

	ipcdev.has_gcr_regs = true;

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
