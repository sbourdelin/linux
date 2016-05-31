/*
 * PCI glue for ISHTP provider device (ISH) driver
 *
 * Copyright (c) 2014-2016, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/miscdevice.h>
#define CREATE_TRACE_POINTS
#include <trace/events/intel_ish.h>
#include "ishtp-dev.h"
#include "hw-ish.h"

static const struct pci_device_id ish_pci_tbl[] = {
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, CHV_DEVICE_ID)},
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, BXT_Ax_DEVICE_ID)},
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, BXT_Bx_DEVICE_ID)},
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, BXTP_Ax_DEVICE_ID)},
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, SPT_Ax_DEVICE_ID)},
	{0, }
};
MODULE_DEVICE_TABLE(pci, ish_pci_tbl);

static void ish_event_tracer(struct ishtp_device *dev, char *format, ...)
{
	if (trace_ishtp_dump_enabled()) {
		va_list args;
		char tmp_buf[100];

		va_start(args, format);
		vsnprintf(tmp_buf, sizeof(tmp_buf), format, args);
		va_end(args);

		trace_ishtp_dump(tmp_buf);
	}
}

static int ish_init(struct ishtp_device *dev)
{
	int ret;

	dev_set_drvdata(dev->devc, dev);

	init_waitqueue_head(&dev->suspend_wait);

	/* Register ishtp bus */
	ret = ishtp_cl_bus_init();
	if (ret) {
		dev_err(dev->devc, "ISH: Init hw failed.\n");
		return ret;
	}

	/* Set the state of ISH HW to start */
	ret = ish_hw_start(dev);
	if (ret) {
		dev_err(dev->devc, "ISH: Init hw failed.\n");
		goto bus_unreg;
	}

	/* Start the inter process communication to ISH processor */
	ret = ishtp_start(dev);
	if (ret) {
		dev_err(dev->devc, "ISHTP: Protocol init failed.\n");
		goto bus_unreg;
	}

	return 0;

bus_unreg:
	ishtp_cl_bus_exit();

	return ret;
}

static int ish_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct ishtp_device *dev;
	struct ish_hw *hw;
	int	ret;

	/* enable pci dev */
	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "ISH: Failed to enable PCI device\n");
		return ret;
	}

	/* set PCI host mastering */
	pci_set_master(pdev);

	/* pci request regions for ISH driver */
	ret = pci_request_regions(pdev, KBUILD_MODNAME);
	if (ret) {
		dev_err(&pdev->dev, "ISH: Failed to get PCI regions\n");
		goto disable_device;
	}

	/* allocates and initializes the ISH dev structure */
	dev = ish_dev_init(pdev);
	if (!dev) {
		ret = -ENOMEM;
		goto release_regions;
	}
	hw = to_ish_hw(dev);
	dev->print_log = ish_event_tracer;

	/* mapping IO device memory */
	hw->mem_addr = pci_iomap(pdev, 0, 0);
	if (!hw->mem_addr) {
		dev_err(&pdev->dev, "ISH: mapping I/O range failure\n");
		ret = -ENOMEM;
		goto free_device;
	}

	dev->pdev = pdev;

	/*
	 * PCI quirk: prevent from being put into D3 state. ISH has internal
	 * power management logic to transition to low power state based
	 * on the usage. So no explicit action is required to change the
	 * state to D3.
	 */
	pdev->dev_flags |= PCI_DEV_FLAGS_NO_D3;


	/* request and enable interrupt */
	ret = request_irq(pdev->irq, ish_irq_handler, IRQF_NO_SUSPEND,
			  KBUILD_MODNAME, dev);
	if (ret) {
		dev_err(&pdev->dev, "ISH: request IRQ failure (%d)\n",
			pdev->irq);
		goto free_device;
	}

	ret = ish_init(dev);
	if (ret)
		goto free_device;

	return 0;

free_device:
	pci_iounmap(pdev, hw->mem_addr);
	kfree(dev);
release_regions:
	pci_release_regions(pdev);
disable_device:
	pci_disable_device(pdev);
	dev_err(&pdev->dev, "ISH: PCI driver initialization failed.\n");

	return ret;
}

static int ish_suspend(struct device *device)
{
	struct pci_dev *pdev = to_pci_dev(device);
	struct ishtp_device *dev = pci_get_drvdata(pdev);

	enable_irq_wake(pdev->irq);
	/*
	 * If previous suspend hasn't been asnwered then ISH is likely dead,
	 * don't attempt nested notification
	 */
	if (dev->suspend_flag)
		return	0;

	dev->suspend_flag = 1;
	ishtp_send_suspend(dev);

	/* 25 ms should be enough for live ISH to flush all IPC buf */
	if (dev->suspend_flag)
		wait_event_timeout(dev->suspend_wait, !dev->suspend_flag,
				   msecs_to_jiffies(25));

	return 0;
}

static int ish_resume(struct device *device)
{
	struct pci_dev *pdev = to_pci_dev(device);
	struct ishtp_device *dev = pci_get_drvdata(pdev);

	disable_irq_wake(pdev->irq);
	ishtp_send_resume(dev);
	dev->suspend_flag = 0;

	return 0;
}

#ifdef CONFIG_PM
static const struct dev_pm_ops ish_pm_ops = {
	.suspend = ish_suspend,
	.resume = ish_resume,
};
#define ISHTP_ISH_PM_OPS	(&ish_pm_ops)
#else
#define ISHTP_ISH_PM_OPS	NULL
#endif

static struct pci_driver ish_driver = {
	.name = KBUILD_MODNAME,
	.id_table = ish_pci_tbl,
	.probe = ish_probe,
	.driver.pm = ISHTP_ISH_PM_OPS,
};

static int __init ish_driver_init(void)
{
	return pci_register_driver(&ish_driver);
}
device_initcall(ish_driver_init);

/* Primary author */
MODULE_AUTHOR("Daniel Drubin <daniel.drubin@intel.com>");
/* Adoption to upstream Linux kernel */
MODULE_AUTHOR("Srinivas Pandruvada <srinivas.pandruvada@linux.intel.com>");

MODULE_DESCRIPTION("Intel(R) Integrated Sensor Hub PCI Device Driver");
MODULE_LICENSE("GPL");

