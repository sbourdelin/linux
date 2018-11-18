// SPDX-License-Identifier: GPL-2.0
/*
 * Cadence USBSS DRD Driver.
 *
 * Copyright (C) 2018 Cadence.
 *
 * Author: Peter Chen <peter.chen@nxp.com>
 *         Pawel Laszczak <pawell@cadence.com>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>

#include "gadget.h"
#include "core.h"
#include "host-export.h"
#include "gadget-export.h"
#include "drd.h"

static inline struct cdns3_role_driver *cdns3_get_current_role_driver(struct cdns3 *cdns)
{
	WARN_ON(cdns->role >= CDNS3_ROLE_END || !cdns->roles[cdns->role]);
	return cdns->roles[cdns->role];
}

static inline int cdns3_role_start(struct cdns3 *cdns, enum cdns3_roles role)
{
	int ret;

	if (role >= CDNS3_ROLE_END)
		return 0;

	if (!cdns->roles[role])
		return -ENXIO;

	mutex_lock(&cdns->mutex);
	cdns->role = role;
	ret = cdns->roles[role]->start(cdns);
	mutex_unlock(&cdns->mutex);
	return ret;
}

static inline void cdns3_role_stop(struct cdns3 *cdns)
{
	enum cdns3_roles role = cdns->role;

	if (role == CDNS3_ROLE_END)
		return;

	mutex_lock(&cdns->mutex);
	cdns->roles[role]->stop(cdns);
	cdns->role = CDNS3_ROLE_END;
	mutex_unlock(&cdns->mutex);
}

static enum cdns3_roles cdns3_get_role(struct cdns3 *cdns)
{
	if (cdns->roles[CDNS3_ROLE_HOST] && cdns->roles[CDNS3_ROLE_GADGET]) {
		if (cdns3_is_host(cdns))
			return CDNS3_ROLE_HOST;
		if (cdns3_is_device(cdns))
			return CDNS3_ROLE_GADGET;
	}
	return cdns->roles[CDNS3_ROLE_HOST]
		? CDNS3_ROLE_HOST
		: CDNS3_ROLE_GADGET;
}

/**
 * cdns3_core_init_role - initialize role of operation
 * @cdns: Pointer to cdns3 structure
 *
 * Returns 0 on success otherwise negative errno
 */
static int cdns3_core_init_role(struct cdns3 *cdns)
{
	struct device *dev = cdns->dev;
	enum usb_dr_mode dr_mode;

	dr_mode = usb_get_dr_mode(dev);
	cdns->role = CDNS3_ROLE_END;

	/*
	 * If driver can't read mode by means of usb_get_dr_mdoe function then
	 * chooses mode according with Kernel configuration. This setting
	 * can be restricted later depending on strap pin configuration.
	 */
	if (dr_mode == USB_DR_MODE_UNKNOWN) {
		if (IS_ENABLED(CONFIG_USB_CDNS3_HOST) &&
		    IS_ENABLED(CONFIG_USB_CDNS3_GADGET))
			dr_mode = USB_DR_MODE_OTG;
		else if (IS_ENABLED(CONFIG_USB_CDNS3_HOST))
			dr_mode = USB_DR_MODE_HOST;
		else if (IS_ENABLED(CONFIG_USB_CDNS3_GADGET))
			dr_mode = USB_DR_MODE_PERIPHERAL;
	}

	if (dr_mode == USB_DR_MODE_OTG || dr_mode == USB_DR_MODE_HOST) {
		if (cdns3_host_init(cdns))
			dev_info(dev, "doesn't support host\n");
	}

	if (dr_mode == USB_DR_MODE_OTG || dr_mode == USB_DR_MODE_PERIPHERAL) {
		if (cdns3_gadget_init(cdns))
			dev_info(dev, "doesn't support gadget\n");
	}

	if (!cdns->roles[CDNS3_ROLE_HOST] && !cdns->roles[CDNS3_ROLE_GADGET]) {
		dev_err(dev, "no supported roles\n");
		return -ENODEV;
	}

	cdns->dr_mode = dr_mode;
	return 0;
}

/**
 * cdns3_irq - interrupt handler for cdns3 core device
 *
 * @irq: irq number for cdns3 core device
 * @data: structure of cdns3
 *
 * Returns IRQ_HANDLED or IRQ_NONE
 */
static irqreturn_t cdns3_irq(int irq, void *data)
{
	struct cdns3 *cdns = data;
	irqreturn_t ret = IRQ_NONE;

	if (cdns->dr_mode == USB_DR_MODE_OTG) {
		ret = cdns3_drd_irq(cdns);
		if (ret == IRQ_HANDLED)
			return ret;
	}

	/* Handle device/host interrupt */
	if (cdns->role != CDNS3_ROLE_END)
		ret = cdns3_get_current_role_driver(cdns)->irq(cdns);

	return ret;
}

static void cdns3_remove_roles(struct cdns3 *cdns)
{
	cdns3_gadget_remove(cdns);
	cdns3_host_remove(cdns);
}

static int cdns3_do_role_switch(struct cdns3 *cdns, enum cdns3_roles role)
{
	enum cdns3_roles current_role;
	int ret = 0;

	current_role = cdns->role;

	if (role == CDNS3_ROLE_END)
		return 0;

	dev_dbg(cdns->dev, "Switching role");

	ret = cdns3_role_start(cdns, role);
	if (ret) {
		/* Back to current role */
		dev_err(cdns->dev, "set %d has failed, back to %d\n",
			role, current_role);
		ret = cdns3_role_start(cdns, current_role);
	}

	return ret;
}

/**
 * cdns3_role_switch - work queue handler for role switch
 *
 * @work: work queue item structure
 *
 * Handles below events:
 * - Role switch for dual-role devices
 * - CDNS3_ROLE_GADGET <--> CDNS3_ROLE_END for peripheral-only devices
 */
static void cdns3_role_switch(struct work_struct *work)
{
	enum cdns3_roles role = CDNS3_ROLE_END;
	struct cdns3 *cdns;
	bool device, host;

	cdns = container_of(work, struct cdns3, role_switch_wq);

	host = cdns3_is_host(cdns);
	device = cdns3_is_device(cdns);

	if (host)
		role = CDNS3_ROLE_HOST;
	else if (device)
		role = CDNS3_ROLE_GADGET;

	if (cdns->desired_dr_mode == cdns->current_dr_mode &&
	    cdns->role == role)
		return;

	pm_runtime_get_sync(cdns->dev);
	cdns3_role_stop(cdns);

	if (cdns->desired_dr_mode != cdns->current_dr_mode) {
		cdns3_drd_update_mode(cdns);
		host = cdns3_is_host(cdns);
		device = cdns3_is_device(cdns);
	}

	if (host) {
		if (cdns->roles[CDNS3_ROLE_HOST])
			cdns3_do_role_switch(cdns, CDNS3_ROLE_HOST);
		pm_runtime_put_sync(cdns->dev);
		return;
	}

	if (device)
		cdns3_do_role_switch(cdns, CDNS3_ROLE_GADGET);
	else
		cdns3_do_role_switch(cdns, CDNS3_ROLE_END);

	pm_runtime_put_sync(cdns->dev);
}

/**
 * cdns3_probe - probe for cdns3 core device
 * @pdev: Pointer to cdns3 core platform device
 *
 * Returns 0 on success otherwise negative errno
 */
static int cdns3_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource	*res;
	struct cdns3 *cdns;
	void __iomem *regs;
	int ret;

	cdns = devm_kzalloc(dev, sizeof(*cdns), GFP_KERNEL);
	if (!cdns)
		return -ENOMEM;

	cdns->dev = dev;

	platform_set_drvdata(pdev, cdns);

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!res) {
		dev_err(dev, "missing IRQ\n");
		return -ENODEV;
	}
	cdns->irq = res->start;

	/*
	 * Request memory region
	 * region-0: xHCI
	 * region-1: Peripheral
	 * region-2: OTG registers
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	regs = devm_ioremap_resource(dev, res);

	if (IS_ERR(regs))
		return PTR_ERR(regs);
	cdns->xhci_regs = regs;
	cdns->xhci_res = res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(regs))
		return PTR_ERR(regs);
	cdns->dev_regs	= regs;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(regs))
		return PTR_ERR(regs);
	cdns->otg_regs = regs;

	mutex_init(&cdns->mutex);

	cdns->phy = devm_phy_get(dev, "cdns3,usbphy");
	if (IS_ERR(cdns->phy)) {
		dev_info(dev, "no generic phy found\n");
		cdns->phy = NULL;
		/*
		 * fall through here!
		 * if no generic phy found, phy init
		 * should be done under boot!
		 */
	} else {
		phy_init(cdns->phy);
	}

	ret = cdns3_core_init_role(cdns);
	if (ret)
		goto err1;

	INIT_WORK(&cdns->role_switch_wq, cdns3_role_switch);
	if (ret)
		goto err2;

	ret = cdns3_drd_init(cdns);
	if (ret)
		goto err2;

	cdns->role = cdns3_get_role(cdns);

	ret = devm_request_irq(dev, cdns->irq, cdns3_irq, IRQF_SHARED,
			       dev_name(dev), cdns);

	if (ret)
		goto err2;

	ret = cdns3_role_start(cdns, cdns->role);
	if (ret) {
		dev_err(dev, "can't start %s role\n",
			cdns3_get_current_role_driver(cdns)->name);
		goto err2;
	}

	device_set_wakeup_capable(dev, true);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	/*
	 * The controller needs less time between bus and controller suspend,
	 * and we also needs a small delay to avoid frequently entering low
	 * power mode.
	 */
	pm_runtime_set_autosuspend_delay(dev, 20);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_use_autosuspend(dev);
	dev_dbg(dev, "Cadence USB3 core: probe succeed\n");

	return 0;

err2:
	cdns3_remove_roles(cdns);
err1:
	return ret;
}

/**
 * cdns3_remove - unbind drd driver and clean up
 * @pdev: Pointer to Linux platform device
 *
 * Returns 0 on success otherwise negative errno
 */
static int cdns3_remove(struct platform_device *pdev)
{
	struct cdns3 *cdns = platform_get_drvdata(pdev);

	pm_runtime_get_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);
	cdns3_remove_roles(cdns);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id of_cdns3_match[] = {
	{ .compatible = "cdns,usb3" },
	{ },
};
MODULE_DEVICE_TABLE(of, of_cdns3_match);
#endif

#ifdef CONFIG_PM

#ifdef CONFIG_PM_SLEEP
static int cdns3_suspend(struct device *dev)
{
	//TODO: implements this function
	return 0;
}

static int cdns3_resume(struct device *dev)
{
	//TODO: implements this function
	return 0;
}
#endif /* CONFIG_PM_SLEEP */
static int cdns3_runtime_suspend(struct device *dev)
{	//TODO: implements this function
	return 0;
}

static int cdns3_runtime_resume(struct device *dev)
{
	//TODO: implements this function
	return 0;
}
#endif /* CONFIG_PM */

static const struct dev_pm_ops cdns3_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(cdns3_suspend, cdns3_resume)
	SET_RUNTIME_PM_OPS(cdns3_runtime_suspend, cdns3_runtime_resume, NULL)
};

static struct platform_driver cdns3_driver = {
	.probe		= cdns3_probe,
	.remove		= cdns3_remove,
	.driver		= {
		.name	= "cdns-usb3",
		.of_match_table	= of_match_ptr(of_cdns3_match),
		.pm	= &cdns3_pm_ops,
	},
};

static int __init cdns3_driver_platform_register(void)
{
	cdns3_host_driver_init();
	return platform_driver_register(&cdns3_driver);
}
module_init(cdns3_driver_platform_register);

static void __exit cdns3_driver_platform_unregister(void)
{
	platform_driver_unregister(&cdns3_driver);
}
module_exit(cdns3_driver_platform_unregister);

MODULE_ALIAS("platform:cdns3");
MODULE_AUTHOR("Pawel Laszczak <pawell@cadence.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Cadence USB3 DRD Controller Driver");
