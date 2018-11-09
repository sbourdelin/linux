/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2018, Broadcom */

#ifndef _XHCI_BRCM_H
#define _XHCI_BRCM_H

#if IS_ENABLED(CONFIG_USB_XHCI_BRCM)
int xhci_plat_brcm_init_quirk(struct usb_hcd *hcd);
#else
static inline int xhci_plat_brcm_init_quirk(struct usb_hcd *hcd)
{
	return 0;
}
#endif
#endif /* _XHCI_BRCM_H */

