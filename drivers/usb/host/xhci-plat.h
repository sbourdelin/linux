// SPDX-License-Identifier: GPL-2.0
/*
 * xhci-plat.h - xHCI host controller driver platform Bus Glue.
 *
 * Copyright (C) 2015 Renesas Electronics Corporation
 */

#ifndef _XHCI_PLAT_H
#define _XHCI_PLAT_H

#include <linux/extcon.h>
#include <linux/usb/hcd.h>
#include <linux/workqueue.h>
#include "xhci.h"	/* for hcd_to_xhci() */

struct xhci_plat_priv {
	struct extcon_dev *edev;
	struct notifier_block nb;
	struct usb_hcd *hcd;		/* for rcar */
	struct usb_hcd *shared_hcd;	/* for rcar */
	int irq;			/* for rcar */
	unsigned long event;		/* for rcar */
	struct work_struct work;	/* for rcar */
	bool halted_by_peri;		/* for rcar */
	const char *firmware_name;
	void (*plat_start)(struct usb_hcd *);
	int (*init_quirk)(struct usb_hcd *);
	int (*resume_quirk)(struct usb_hcd *);
	int (*notifier)(struct notifier_block *nb, unsigned long event,
			void *data);
};

#define hcd_to_xhci_priv(h) ((struct xhci_plat_priv *)hcd_to_xhci(h)->priv)
#endif	/* _XHCI_PLAT_H */
