// SPDX-License-Identifier: GPL-2.0
/*
 * USBSSP device controller driver
 *
 * Copyright (C) 2018 Cadence.
 *
 * Author: Pawel Laszczak
 * Some code borrowed from the Linux XHCI driver.
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/usb/phy.h>
#include <linux/slab.h>
#include <linux/acpi.h>

#include "gadget.h"

#define DRIVER_AUTHOR "Pawel Laszczak"
#define DRIVER_DESC "USBSSP Device Controller (USBSSP) Driver"

#ifdef CONFIG_OF

static const struct of_device_id usbssp_dev_of_match[] = {
	{
		.compatible = "Cadence, usbssp-dev",
	},
	{},
};
MODULE_DEVICE_TABLE(of, usbssp_dev_of_match);
#endif

int usbssp_is_platform(void)
{
	return 1;
}

static int usbssp_plat_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct usbssp_udc *usbssp_data;
	int ret = 0;
	int irq;
	struct device *sysdev;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "Incorrect IRQ number\n");
		return -ENODEV;
	}

	usbssp_data = devm_kzalloc(dev, sizeof(*usbssp_data), GFP_KERNEL);
	if (!usbssp_data)
		return -ENOMEM;

	for (sysdev = &pdev->dev; sysdev; sysdev = sysdev->parent) {
		if (is_of_node(sysdev->fwnode) ||
		    is_acpi_device_node(sysdev->fwnode))
			break;
#ifdef CONFIG_PCI
		else if (sysdev->bus == &pci_bus_type)
			break;
#endif
	}

	if (!sysdev)
		sysdev = &pdev->dev;

	/* Try to set 64-bit DMA first */
	if (WARN_ON(!dev->dma_mask))
		/* Platform did not initialize dma_mask */
		ret = dma_coerce_mask_and_coherent(dev,
				DMA_BIT_MASK(64));
	else
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));

	/* If seting 64-bit DMA mask fails, fall back to 32-bit DMA mask */
	if (ret) {
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret)
			return ret;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	usbssp_data->regs = devm_ioremap_resource(dev, res);

	if (IS_ERR(usbssp_data->regs)) {
		ret = PTR_ERR(usbssp_data->regs);
		return ret;
	}

	usbssp_data->rsrc_start = res->start;
	usbssp_data->rsrc_len = resource_size(res);

	ret = devm_request_irq(dev, irq, usbssp_irq, IRQF_SHARED,
			dev_name(dev), usbssp_data);

	if (ret < 0)
		return ret;

	usbssp_data->irq = irq;
	usbssp_data->dev = dev;
	platform_set_drvdata(pdev, usbssp_data);
	ret = usbssp_gadget_init(usbssp_data);

	return ret;
}

static int usbssp_plat_remove(struct platform_device *pdev)
{
	int ret = 0;
	struct usbssp_udc *usbssp_data;

	usbssp_data = (struct usbssp_udc *)platform_get_drvdata(pdev);
	ret = usbssp_gadget_exit(usbssp_data);
	return ret;

}

static int __maybe_unused usbssp_plat_suspend(struct device *dev)
{
	struct usbssp_udc *usbssp_data = dev_get_drvdata(dev);

	return usbssp_suspend(usbssp_data, device_may_wakeup(dev));
}

static int __maybe_unused usbssp_plat_resume(struct device *dev)
{
	struct usbssp_udc *usbssp_data = dev_get_drvdata(dev);

	return usbssp_resume(usbssp_data, 0);
}

static int __maybe_unused usbssp_plat_runtime_suspend(struct device *dev)
{
	struct usbssp_udc *usbssp_data = dev_get_drvdata(dev);

	return usbssp_suspend(usbssp_data, true);
}

static int __maybe_unused usbssp_plat_runtime_resume(struct device *dev)
{
	struct usbssp_udc *usbssp_data = dev_get_drvdata(dev);

	return usbssp_resume(usbssp_data, 0);
}

static const struct dev_pm_ops usbssp_plat_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(usbssp_plat_suspend, usbssp_plat_resume)

	SET_RUNTIME_PM_OPS(usbssp_plat_runtime_suspend,
			usbssp_plat_runtime_resume,
			NULL)
};

static struct platform_driver usbssp_driver = {
	.probe	= usbssp_plat_probe,
	.remove	= usbssp_plat_remove,
	.driver	= {
		.name = "usbssp-dev",
		.pm = &usbssp_plat_pm_ops,
		.of_match_table = of_match_ptr(usbssp_dev_of_match),
	},
};

static int __init usbssp_plat_init(void)
{
	return platform_driver_register(&usbssp_driver);
}
module_init(usbssp_plat_init);

static void __exit usbssp_plat_exit(void)
{
	platform_driver_unregister(&usbssp_driver);
}
module_exit(usbssp_plat_exit);

MODULE_ALIAS("platform:usbss-gadget");
MODULE_DESCRIPTION("USBSSP' Device Controller (USBSSP) Driver");
MODULE_LICENSE("GPL");
