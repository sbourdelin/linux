/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Cadence USBSS DRD Driver.
 *
 * Copyright (C) 2017 NXP
 * Copyright (C) 2018 Cadence.
 *
 * Authors: Peter Chen <peter.chen@nxp.com>
 *          Pawel Laszczak <pawell@cadence.com>
 */
#include <linux/usb/otg.h>

#ifndef __LINUX_CDNS3_CORE_H
#define __LINUX_CDNS3_CORE_H

struct cdns3;
enum cdns3_roles {
	CDNS3_ROLE_HOST = 0,
	CDNS3_ROLE_GADGET,
	CDNS3_ROLE_END,
	CDNS3_ROLE_OTG,
};

/**
 * struct cdns3_role_driver - host/gadget role driver
 * @start: start this role
 * @stop: stop this role
 * @suspend: suspend callback for this role
 * @resume: resume callback for this role
 * @irq: irq handler for this role
 * @name: role name string (host/gadget)
 */
struct cdns3_role_driver {
	int (*start)(struct cdns3 *cdns);
	void (*stop)(struct cdns3 *cdns);
	int (*suspend)(struct cdns3 *cdns, bool do_wakeup);
	int (*resume)(struct cdns3 *cdns, bool hibernated);
	irqreturn_t (*irq)(struct cdns3 *cdns);
	const char *name;
};

#define CDNS3_NUM_OF_CLKS	5
/**
 * struct cdns3 - Representation of Cadence USB3 DRD controller.
 * @dev: pointer to Cadence device struct
 * @xhci_regs: pointer to base of xhci registers
 * @xhci_res: the resource for xhci
 * @dev_regs: pointer to base of dev registers
 * @otg_regs: pointer to base of otg registers
 * @irq: irq number for controller
 * @roles: array of supported roles for this controller
 * @role: current role
 * @host_dev: the child host device pointer for cdns3 core
 * @gadget_dev: the child gadget device pointer for cdns3 core
 * @usbphy: usbphy for this controller
 * @role_switch_wq: work queue item for role switch
 * @in_lpm: the controller in low power mode
 * @wakeup_int: the wakeup interrupt
 * @mutex: the mutex for concurrent code at driver
 * @dr_mode: requested mode of operation
 * @current_dr_role: current role of operation when in dual-role mode
 * @desired_dr_role: desired role of operation when in dual-role mode
 * @root: debugfs root folder pointer
 */
struct cdns3 {
	struct device			*dev;
	void __iomem			*xhci_regs;
	struct resource			*xhci_res;
	struct cdns3_usb_regs __iomem	*dev_regs;
	struct cdns3_otg_regs		*otg_regs;
	int irq;
	struct cdns3_role_driver	*roles[CDNS3_ROLE_END];
	enum cdns3_roles		role;
	struct device			*host_dev;
	struct device			*gadget_dev;
	struct usb_phy			*usbphy;
	struct work_struct		role_switch_wq;
	int				in_lpm:1;
	int				wakeup_int:1;
	/* mutext used in workqueue*/
	struct mutex			mutex;
	enum usb_dr_mode		dr_mode;
	enum usb_dr_mode		current_dr_mode;
	enum usb_dr_mode		desired_role;
	struct dentry			*root;
};

#endif /* __LINUX_CDNS3_CORE_H */
