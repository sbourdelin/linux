// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2018, Broadcom */

#include <linux/usb.h>
#include <linux/usb/hcd.h>

#include "xhci.h"

int xhci_plat_brcm_init_quirk(struct usb_hcd *hcd)
{
	struct xhci_hcd	*xhci = hcd_to_xhci(hcd);

	xhci->quirks |= XHCI_RESET_ON_RESUME;
	hcd->suspend_without_phy_exit = 1;
	return 0;
}

