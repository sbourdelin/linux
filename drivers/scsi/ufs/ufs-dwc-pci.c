/*
 * UFS Host driver for Synopsys Designware Core
 *
 * Copyright (C) 2015-2016 Synopsys, Inc. (www.synopsys.com)
 *
 * Authors: Joao Pinto <jpinto@synopsys.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "ufshcd.h"
#include "ufshcd-dwc.h"

#include <linux/pci.h>
#include <linux/pm_runtime.h>

#ifdef CONFIG_PM
/**
 * ufs_dw_pci_suspend - suspend power management function
 * @pdev: pointer to PCI device handle
 * @state: power state
 *
 * Returns 0 if successful
 * Returns non-zero otherwise
 */
static int ufs_dw_pci_suspend(struct device *dev)
{
	return ufshcd_system_suspend(dev_get_drvdata(dev));
}

/**
 * ufs_dw_pci_resume - resume power management function
 * @pdev: pointer to PCI device handle
 *
 * Returns 0 if successful
 * Returns non-zero otherwise
 */
static int ufs_dw_pci_resume(struct device *dev)
{
	return ufshcd_system_resume(dev_get_drvdata(dev));
}

static int ufs_dw_pci_runtime_suspend(struct device *dev)
{
	return ufshcd_runtime_suspend(dev_get_drvdata(dev));
}
static int ufs_dw_pci_runtime_resume(struct device *dev)
{
	return ufshcd_runtime_resume(dev_get_drvdata(dev));
}
static int ufs_dw_pci_runtime_idle(struct device *dev)
{
	return ufshcd_runtime_idle(dev_get_drvdata(dev));
}
#else /* !CONFIG_PM */
#define ufs_dw_pci_suspend	NULL
#define ufs_dw_pci_resume	NULL
#define ufs_dw_pci_runtime_suspend	NULL
#define ufs_dw_pci_runtime_resume	NULL
#define ufs_dw_pci_runtime_idle	NULL
#endif /* CONFIG_PM */

/**
 * struct ufs_hba_dwc_vops - UFS DWC specific variant operations
 */
static struct ufs_hba_variant_ops ufs_dwc_pci_hba_vops = {
	.name                   = "ufshcd-dwc-pci",
	.link_startup_notify	= ufshcd_dwc_link_startup_notify,
};

/**
 * ufs_dw_pci_shutdown - main function to put the controller in reset state
 * @pdev: pointer to PCI device handle
 */
static void ufs_dw_pci_shutdown(struct pci_dev *pdev)
{
	ufshcd_shutdown((struct ufs_hba *)pci_get_drvdata(pdev));
}

/**
 * ufs_dw_pci_remove - de-allocate PCI/SCSI host and host memory space
 *		data structure memory
 * @pdev - pointer to PCI handle
 */
static void ufs_dw_pci_remove(struct pci_dev *pdev)
{
	struct ufs_hba *hba = pci_get_drvdata(pdev);

	pm_runtime_forbid(&pdev->dev);
	pm_runtime_get_noresume(&pdev->dev);
	ufshcd_remove(hba);
}

/**
 * ufs_dw_pci_probe - probe routine of the driver
 * @pdev: pointer to PCI device handle
 * @id: PCI device id
 *
 * Returns 0 on success, non-zero value on failure
 */
static int
ufs_dw_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct ufs_hba *hba;
	void __iomem *mmio_base;
	int err;

	err = pcim_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "pcim_enable_device failed\n");
		return err;
	}

	pci_set_master(pdev);

	err = pcim_iomap_regions(pdev, 1 << 0, UFSHCD);
	if (err < 0) {
		dev_err(&pdev->dev, "request and iomap failed\n");
		return err;
	}

	mmio_base = pcim_iomap_table(pdev)[0];

	err = ufshcd_alloc_host(&pdev->dev, &hba);
	if (err) {
		dev_err(&pdev->dev, "Allocation failed\n");
		return err;
	}

	INIT_LIST_HEAD(&hba->clk_list_head);

	hba->vops = &ufs_dwc_pci_hba_vops;

	err = ufshcd_init(hba, mmio_base, pdev->irq);
	if (err) {
		dev_err(&pdev->dev, "Initialization failed\n");
		return err;
	}

	pci_set_drvdata(pdev, hba);
	pm_runtime_put_noidle(&pdev->dev);
	pm_runtime_allow(&pdev->dev);

	return 0;
}

static const struct dev_pm_ops ufs_dw_pci_pm_ops = {
	.suspend	= ufs_dw_pci_suspend,
	.resume		= ufs_dw_pci_resume,
	.runtime_suspend = ufs_dw_pci_runtime_suspend,
	.runtime_resume  = ufs_dw_pci_runtime_resume,
	.runtime_idle    = ufs_dw_pci_runtime_idle,
};

static const struct pci_device_id ufs_dw_pci_tbl[] = {
	{ PCI_VENDOR_ID_SYNOPSYS, 0xB101, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ PCI_VENDOR_ID_SYNOPSYS, 0xB102, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ }	/* terminate list */
};

MODULE_DEVICE_TABLE(pci, ufs_dw_pci_tbl);

static struct pci_driver ufs_dw_pci_driver = {
	.name = UFSHCD,
	.id_table = ufs_dw_pci_tbl,
	.probe = ufs_dw_pci_probe,
	.remove = ufs_dw_pci_remove,
	.shutdown = ufs_dw_pci_shutdown,
	.driver = {
		.pm = &ufs_dw_pci_pm_ops
	},
};

module_pci_driver(ufs_dw_pci_driver);

MODULE_AUTHOR("Joao Pinto <Joao.Pinto@synopsys.com>");
MODULE_DESCRIPTION("DesignWare UFS host controller PCI glue driver");
MODULE_LICENSE("Dual BSD/GPL");
