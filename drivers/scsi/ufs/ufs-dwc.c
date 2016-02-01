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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/delay.h>

#include "ufshcd.h"
#include "ufshcd-pltfrm.h"

/**
 * struct ufs_hba_dwc_vops - UFS DWC specific variant operations
 *
 */
static struct ufs_hba_variant_ops ufs_hba_dwc_vops = {
	.name                   = "dwc",
};

/**
 * ufs_dwc_probe()
 * @pdev: pointer to platform device structure
 *
 */
static int ufs_dwc_probe(struct platform_device *pdev)
{
	int err;
	struct device *dev = &pdev->dev;

	/* Perform generic probe */
	err = ufshcd_pltfrm_init(pdev, &ufs_hba_dwc_vops);
	if (err)
		dev_err(dev, "ufshcd_pltfrm_init() failed %d\n", err);

	return err;
}

/**
 * ufs_dwc_remove()
 * @pdev: pointer to platform device structure
 *
 */
static int ufs_dwc_remove(struct platform_device *pdev)
{
	struct ufs_hba *hba =  platform_get_drvdata(pdev);

	pm_runtime_get_sync(&(pdev)->dev);
	ufshcd_remove(hba);

	return 0;
}

static const struct of_device_id ufs_dwc_match[] = {
	{
		.compatible = "snps,ufshcd"
	},
	{ },
};
MODULE_DEVICE_TABLE(of, ufs_dwc_match);

static const struct dev_pm_ops ufs_dwc_pm_ops = {
	.suspend	= ufshcd_pltfrm_suspend,
	.resume		= ufshcd_pltfrm_resume,
	.runtime_suspend = ufshcd_pltfrm_runtime_suspend,
	.runtime_resume  = ufshcd_pltfrm_runtime_resume,
	.runtime_idle    = ufshcd_pltfrm_runtime_idle,
};

static struct platform_driver ufs_dwc_driver = {
	.probe		= ufs_dwc_probe,
	.remove		= ufs_dwc_remove,
	.shutdown = ufshcd_pltfrm_shutdown,
	.driver		= {
		.name	= "ufshcd-dwc",
		.pm	= &ufs_dwc_pm_ops,
		.of_match_table	= of_match_ptr(ufs_dwc_match),
	},
};

module_platform_driver(ufs_dwc_driver);

MODULE_ALIAS("platform:ufshcd-dwc");
MODULE_DESCRIPTION("DesignWare UFS Host platform glue driver");
MODULE_AUTHOR("Joao Pinto <Joao.Pinto@synopsys.com>");
MODULE_LICENSE("Dual BSD/GPL");
