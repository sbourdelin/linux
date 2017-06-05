/*
 * Universal Flash Storage Intel Host controller PCI driver
 *
 * Copyright (c) 2017, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include "ufshcd.h"
#include <linux/pci.h>
#include <linux/pm_runtime.h>

static int ufs_intel_disable_lcc(struct ufs_hba *hba)
{
	u32 attr = UIC_ARG_MIB(PA_LOCAL_TX_LCC_ENABLE);
	u32 lcc_enable = 0;

	ufshcd_dme_get(hba, attr, &lcc_enable);
	if (lcc_enable)
		ufshcd_dme_set(hba, attr, 0);

	return 0;
}

static int ufs_intel_link_startup_notify(struct ufs_hba *hba,
					 enum ufs_notify_change_status status)
{
	int err = 0;

	switch (status) {
	case PRE_CHANGE:
		err = ufs_intel_disable_lcc(hba);
		break;
	case POST_CHANGE:
		break;
	default:
		break;
	}

	return err;
}

static struct ufs_hba_variant_ops ufs_intel_hba_vops = {
	.name                   = "intel",
	.link_startup_notify	= ufs_intel_link_startup_notify,
};

#ifdef CONFIG_PM_SLEEP
static int ufs_intel_suspend(struct device *dev)
{
	return ufshcd_system_suspend(dev_get_drvdata(dev));
}

static int ufs_intel_resume(struct device *dev)
{
	return ufshcd_system_resume(dev_get_drvdata(dev));
}
#endif /* !CONFIG_PM_SLEEP */

#ifdef CONFIG_PM
static int ufs_intel_runtime_suspend(struct device *dev)
{
	return ufshcd_runtime_suspend(dev_get_drvdata(dev));
}

static int ufs_intel_runtime_resume(struct device *dev)
{
	return ufshcd_runtime_resume(dev_get_drvdata(dev));
}

static int ufs_intel_runtime_idle(struct device *dev)
{
	return ufshcd_runtime_idle(dev_get_drvdata(dev));
}
#endif /* !CONFIG_PM */

static void ufs_intel_shutdown(struct pci_dev *pdev)
{
	ufshcd_shutdown((struct ufs_hba *)pci_get_drvdata(pdev));
}

static void ufs_intel_remove(struct pci_dev *pdev)
{
	struct ufs_hba *hba = pci_get_drvdata(pdev);

	pm_runtime_forbid(&pdev->dev);
	pm_runtime_get_noresume(&pdev->dev);
	ufshcd_remove(hba);
	ufshcd_dealloc_host(hba);
}

static int ufs_intel_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct ufs_hba *hba;
	void __iomem *mmio_base;
	int err;

	dev_info(&pdev->dev, "UFS controller found [%04x:%04x]\n",
		 (int)pdev->vendor, (int)pdev->device);

	err = pcim_enable_device(pdev);
	if (err)
		return err;

	pci_set_master(pdev);

	err = pcim_iomap_regions(pdev, 1 << 0, UFSHCD);
	if (err < 0)
		return err;

	mmio_base = pcim_iomap_table(pdev)[0];

	err = ufshcd_alloc_host(&pdev->dev, &hba);
	if (err)
		return err;

	hba->vops = &ufs_intel_hba_vops;

	err = ufshcd_init(hba, mmio_base, pdev->irq);
	if (err) {
		dev_err(&pdev->dev, "Initialization failed\n");
		goto out_dealloc;
	}

	pci_set_drvdata(pdev, hba);
	pm_runtime_put_noidle(&pdev->dev);
	pm_runtime_allow(&pdev->dev);

	return 0;

out_dealloc:
	ufshcd_dealloc_host(hba);
	return err;
}

static const struct dev_pm_ops ufs_intel_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(ufs_intel_suspend,
				ufs_intel_resume)
	SET_RUNTIME_PM_OPS(ufs_intel_runtime_suspend,
			   ufs_intel_runtime_resume,
			   ufs_intel_runtime_idle)
};

#define PCI_CLASS_STORAGE_UFSHCI 0x010901

#define UFSHCD_INTEL_PCI_UFSHCI_DEVICE() { \
	.vendor      = PCI_VENDOR_ID_INTEL, \
	.device      = PCI_ANY_ID, \
	.subvendor   = PCI_ANY_ID, \
	.subdevice   = PCI_ANY_ID, \
	.class       = PCI_CLASS_STORAGE_UFSHCI, \
	.class_mask  = ~0, \
}

static const struct pci_device_id ufs_intel_tbl[] = {
	UFSHCD_INTEL_PCI_UFSHCI_DEVICE(),
	{ }	/* terminate list */
};
MODULE_DEVICE_TABLE(pci, ufs_intel_tbl);

static struct pci_driver ufs_intel_driver = {
	.name       = "ufshcd-intel-pci",
	.id_table   = ufs_intel_tbl,
	.probe      = ufs_intel_probe,
	.remove     = ufs_intel_remove,
	.shutdown   = ufs_intel_shutdown,
	.driver     = {
		.pm = &ufs_intel_pm_ops
	},
};

module_pci_driver(ufs_intel_driver);

MODULE_AUTHOR("Szymon Mielczarek <szymonx.mielczarek@intel.com>");
MODULE_DESCRIPTION("Intel UFS host controller PCI glue driver");
MODULE_LICENSE("GPL v2");
