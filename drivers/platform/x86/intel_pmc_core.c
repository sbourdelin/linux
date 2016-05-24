/*
 * Intel Core SoC Power Management Controller Driver
 *
 * Copyright (c) 2016, Intel Corporation.
 * All Rights Reserved.
 * Author: Rajneesh Bhardwaj (rajneesh.bhardwaj@intel.com)
 * Author: Vishwanath Somayaji (vishwanath.somayaji@intel.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/seq_file.h>

#include <asm/cpu_device_id.h>
#include <asm/pmc_core.h>

#include "intel_pmc_core.h"

static struct pmc_dev pmc;

static const struct pci_device_id pmc_pci_ids[] = {
	{ PCI_VDEVICE(INTEL, SPT_PMC_PCI_DEVICE_ID), (kernel_ulong_t)NULL },
	{ 0, },
};
MODULE_DEVICE_TABLE(pci, pmc_pci_ids);

static inline u32 pmc_core_reg_read(struct pmc_dev *pmc, int reg_offset)
{
	return readl(pmc->regmap + reg_offset);
}

static inline u32 pmc_core_adjust_slp_s0_step(u32 value)
{
	return value * SPT_PMC_SLP_S0_RES_COUNTER_STEP;
}

/**
 * intel_pmc_slp_s0_counter_read() - Read slp_s0 residency.
 * @data: Out param that contains current slp_s0 count.
 *
 * This API currently supports Intel Skylake SoC and Sunrise
 * Point Platform Controller Hub. Future platform support
 * should be added for platforms that support low power modes
 * beyond Package C10 state.
 *
 * SLP_S0_RESIDENCY counter counts in 100 us granularity per
 * step hence function populates the multiplied value in out
 * parameter @data.
 *
 * Return: an error code or 0 on success.
 */
int intel_pmc_slp_s0_counter_read(u32 *data)
{
	struct pmc_dev *pmcdev = &pmc;
	u32 value;

	if (!pmcdev->has_slp_s0_res)
		return -EACCES;

	value = pmc_core_reg_read(pmcdev, SPT_PMC_SLP_S0_RES_COUNTER_OFFSET);
	*data = pmc_core_adjust_slp_s0_step(value);

	return 0;
}
EXPORT_SYMBOL_GPL(intel_pmc_slp_s0_counter_read);

#if IS_ENABLED(CONFIG_DEBUG_FS)
static int pmc_core_dev_state_show(struct seq_file *s, void *unused)
{
	struct pmc_dev *pmcdev = s->private;
	u32 counter_val;

	counter_val = pmc_core_reg_read(pmcdev,
					SPT_PMC_SLP_S0_RES_COUNTER_OFFSET);
	seq_printf(s, "%u\n", pmc_core_adjust_slp_s0_step(counter_val));

	return 0;
}

static int pmc_core_dev_state_open(struct inode *inode, struct file *file)
{
	return single_open(file, pmc_core_dev_state_show, inode->i_private);
}

static const struct file_operations pmc_core_dev_state_ops = {
	.open           = pmc_core_dev_state_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release        = single_release,
};

static void pmc_core_dbgfs_unregister(struct pmc_dev *pmc)
{
	debugfs_remove_recursive(pmc->dbgfs_dir);
}

static int pmc_core_dbgfs_register(struct pmc_dev *pmc)
{
	struct dentry *dir, *file;
	int ret = 0;

	dir = debugfs_create_dir("pmc_core", NULL);
	if (!dir)
		return -ENOMEM;

	pmc->dbgfs_dir = dir;
	file = debugfs_create_file("slp_s0_residency_usec", S_IFREG | S_IRUGO,
				   dir, pmc, &pmc_core_dev_state_ops);

	if (!file) {
		pmc_core_dbgfs_unregister(pmc);
		ret = -ENODEV;
	}
	return ret;
}
#else
static inline int pmc_core_dbgfs_register(struct pmc_dev *pmc)
{
	return 0; /* nothing to register */
}

static inline void pmc_core_dbgfs_unregister(struct pmc_dev *pmc)
{
	/* nothing to deregister */
}
#endif /* CONFIG_DEBUG_FS */

static const struct x86_cpu_id intel_pmc_core_ids[] = {
	{ X86_VENDOR_INTEL, 6, 0x4e, X86_FEATURE_MWAIT,
		(kernel_ulong_t)NULL}, /* Skylake CPUID Signature */
	{ X86_VENDOR_INTEL, 6, 0x5e, X86_FEATURE_MWAIT,
		(kernel_ulong_t)NULL}, /* Skylake CPUID Signature */
	{}
};
MODULE_DEVICE_TABLE(x86cpu, intel_pmc_core_ids);

static int pmc_core_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct device *ptr_dev = &dev->dev;
	struct pmc_dev *pmcdev = &pmc;
	const struct x86_cpu_id *cpu_id;
	int err;

	cpu_id = x86_match_cpu(intel_pmc_core_ids);
	if (!cpu_id) {
		dev_dbg(&dev->dev, "PMC Core: cpuid mismatch.\n");
		return -EINVAL;
	}

	err = pcim_enable_device(dev);
	if (err < 0) {
		dev_dbg(&dev->dev, "PMC Core: failed to enable Power Management Controller.\n");
		return err;
	}

	err = pci_read_config_dword(dev,
				    SPT_PMC_BASE_ADDR_OFFSET,
				    &pmcdev->base_addr);
	if (err < 0) {
		dev_dbg(&dev->dev, "PMC Core: failed to read pci config space.\n");
		return err;
	}
	dev_dbg(&dev->dev, "PMC Core: PWRMBASE is %#x\n", pmcdev->base_addr);

	pmcdev->regmap = devm_ioremap_nocache(ptr_dev,
					      pmcdev->base_addr,
					      SPT_PMC_MMIO_REG_LEN);
	if (!pmcdev->regmap) {
		dev_dbg(&dev->dev, "PMC Core: ioremap failed.\n");
		return -ENOMEM;
	}

	err = pmc_core_dbgfs_register(pmcdev);
	if (err < 0) {
		dev_err(&dev->dev, "PMC Core: debugfs register failed.\n");
		return err;
	}

	pmc.has_slp_s0_res = true;
	return 0;
}

static void intel_pmc_core_remove(struct pci_dev *pdev)
{
	pmc_core_dbgfs_unregister(&pmc);
}

static struct pci_driver intel_pmc_core_driver = {
	.name = "intel_pmc_core",
	.id_table = pmc_pci_ids,
	.probe = pmc_core_probe,
	.remove = intel_pmc_core_remove,
};
module_pci_driver(intel_pmc_core_driver);

MODULE_AUTHOR("Rajneesh Bhardwaj <rajneesh.bhardwaj@intel.com>");
MODULE_AUTHOR("Vishwanath Somayaji <vishwanath.somayaji@intel.com>");
MODULE_DESCRIPTION("Intel CORE SoC Power Management Controller Interface");
MODULE_LICENSE("GPL v2");
