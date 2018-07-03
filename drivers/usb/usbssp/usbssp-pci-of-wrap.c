// SPDX-License-Identifier: GPL-2.0
/*
 * USBSSP device controller driver - PCIe wrapper
 *
 * Copyright (C) 2018 Cadence.
 *
 * Author: Pawel Laszczak
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>


struct usbssp_pci {
	struct platform_device *plat_cdns;
	struct pci_dev *otg;
	struct pci_dev *hg_dev;
	struct resource res[5];
};

struct usbssp_pci usbssp;

/**
 * usbssp_pci_probe - Probe function for Cadence USB wrapper driver
 * @pdev: platform device object
 * @id: pci device id
 *
 * Returns 0 on success otherwise negative errno
 */
static int usbssp_pci_probe(struct pci_dev *pdev,
			    const struct pci_device_id *id)
{
	int rv = 0;
	struct platform_device *usbssp_plat;

	if (pdev->devfn > 1)
		return 0;

	if (!id)
		return -EINVAL;

	if (pcim_enable_device(pdev) < 0) {
		dev_err(&pdev->dev, "failed to enable PCI device\n");
		return -ENOMEM;
	}

	pci_set_master(pdev);

	/*
	 * function 0: device(BAR_2) + host(BAR_0)
	 * function 1: otg(BAR_0))cdns3_remove_roles
	 */
	if (pdev->devfn == 0 || pdev->devfn == 1) {
		if (!usbssp.plat_cdns) {
			usbssp_plat = platform_device_alloc("usbssp-dev",
					PLATFORM_DEVID_AUTO);
			memset(usbssp.res, 0x00, sizeof(struct resource) *
					ARRAY_SIZE(usbssp.res));
			usbssp.plat_cdns = usbssp_plat;

			if (!usbssp_plat->dev.dma_mask) {
				dma_coerce_mask_and_coherent(&usbssp_plat->dev,
					DMA_BIT_MASK(32));
			}
		}
	} else
		return -EINVAL;

	/* for GADGET device function number is 0 */
	if (pdev->devfn == 0) {
		usbssp.hg_dev = pdev;
		//device resource
		usbssp.res[0].start = pci_resource_start(pdev, 2);
		usbssp.res[0].end = pci_resource_end(pdev, 2);
		usbssp.res[0].name = "usbssp-dev-regs";
		usbssp.res[0].flags = IORESOURCE_MEM;

		/* Interrupt resource*/
		usbssp.res[1].start = pdev->irq;
		usbssp.res[1].name = "usbssp-dev-irq";
		usbssp.res[1].flags = IORESOURCE_IRQ;
	} else if (pdev->devfn == 1) {
		usbssp.otg = pdev;
		//OTG
		usbssp.res[2].start = pci_resource_start(pdev, 0);
		usbssp.res[2].end =   pci_resource_end(pdev, 0);
		usbssp.res[2].name = "otg";
		usbssp.res[2].flags = IORESOURCE_MEM;

		usbssp.res[3].start = pci_resource_start(pdev, 1);
		usbssp.res[3].end =   pci_resource_end(pdev, 1);
		usbssp.res[3].name = "debug1";
		usbssp.res[3].flags = IORESOURCE_MEM;

		usbssp.res[4].start = pci_resource_start(pdev, 2);
		usbssp.res[4].end =   pci_resource_end(pdev, 2);
		usbssp.res[4].name = "debug2";
		usbssp.res[4].flags = IORESOURCE_MEM;
	} else {
		return -EIO;
	}

	if (usbssp.otg && usbssp.hg_dev) {

		struct pci_dev *pci_hg = usbssp.hg_dev;
		struct platform_device *plat_dev = usbssp.plat_cdns;

		rv = platform_device_add_resources(plat_dev, usbssp.res,
				ARRAY_SIZE(usbssp.res));
		if (rv) {
			dev_err(&plat_dev->dev,
				"couldn't add resources to cdns device\n");
			return rv;
		}

		rv = platform_device_add(plat_dev);

		if (rv) {
			dev_err(&pci_hg->dev,
				"failed to register cdns device\n");
			platform_device_put(plat_dev);
			return rv;
		}
	}

	pci_set_drvdata(pdev, &usbssp);

	return rv;
}

void usbssp_pci_remove(struct pci_dev *pdev)
{
	struct usbssp_pci *usbssp = pci_get_drvdata(pdev);

	if (pdev->devfn > 1)
		return;

	if (pdev->devfn == 1)
		usbssp->otg = NULL;
	else if (pdev->devfn == 0)
		usbssp->hg_dev = NULL;

	if (!usbssp->hg_dev && !usbssp->otg)
		platform_device_unregister(usbssp->plat_cdns);
}

#if CONFIG_PM || CONFIG_PM_SLEEP
static int usbssp_generic_suspend(struct usbssp_pci *usbssp, int param)
{
	/*TODO*/
	return -ENOSYS;
}

static int usbssp_generic_resume(struct usbssp_pci *usbssp, int param)
{
	/*TODO*/
	return -ENOSYS;
}

#endif /*CONFIG_PM || CONFIG_PM_SLEEP*/

#ifdef CONFIG_PM
static int usbssp_runtime_suspend(struct device *dev)
{
	struct usbssp_pci		*usbssp = dev_get_drvdata(dev);

	return usbssp_generic_suspend(usbssp, 0);
}

static int usbssp_runtime_resume(struct device *dev)
{
	struct usbssp_pci		*usbssp = dev_get_drvdata(dev);

	return usbssp_generic_resume(usbssp, 0);
}
#endif /* CONFIG_PM */

#ifdef CONFIG_PM_SLEEP
static int usbssp_pci_suspend(struct device *dev)
{
	struct usbssp_pci		*usbssp = dev_get_drvdata(dev);

	return usbssp_generic_suspend(usbssp, 0);
}

static int usbssp_pci_resume(struct device *dev)
{
	struct usbssp_pci		*usbssp = dev_get_drvdata(dev);

	return usbssp_generic_resume(usbssp, 0);
}
#endif /* CONFIG_PM_SLEEP */

static const struct dev_pm_ops usbssp_pci_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(usbssp_runtime_suspend, usbssp_runtime_resume)
	SET_RUNTIME_PM_OPS(usbssp_pci_suspend, usbssp_pci_resume,
		NULL)
};

#define PCI_VENDOR_ID_CDZ 0x17CD
static const struct pci_device_id usbssp_pci_ids[] = {
	{PCI_VDEVICE(CDZ, 0x0100), },
	{ /* terminate list */}
};

MODULE_DEVICE_TABLE(pci, usbssp_pci_ids);

static struct pci_driver usbssp_pci_driver = {
	.name = "usbssp-pci",
	.id_table = &usbssp_pci_ids[0],
	.probe = usbssp_pci_probe,
	.remove = usbssp_pci_remove,
	.driver		= {
		.pm	= &usbssp_pci_dev_pm_ops,
	}
};

MODULE_AUTHOR("Pawel Laszczak <pawell@cadence.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Cadence USBSSP PCI Glue Layer");

module_pci_driver(usbssp_pci_driver);


