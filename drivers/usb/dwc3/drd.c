/**
 * drd.c - DesignWare USB3 DRD Controller Dual-role support
 *
 * Copyright (C) 2017 Texas Instruments Incorporated - http://www.ti.com
 *
 * Authors: Roger Quadros <rogerq@ti.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2  of
 * the License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/extcon.h>

#include "debug.h"
#include "core.h"
#include "gadget.h"

static void dwc3_drd_update(struct dwc3 *dwc)
{
	int id;
	int new_role;
	unsigned long flags;

	if (dwc->drd_prevent_change)
		return;

	id = extcon_get_state(dwc->edev, EXTCON_USB_HOST);
	/* Host means ID is 0 */
	id = !id;

	if (!id)
		new_role = DWC3_GCTL_PRTCAP_HOST;
	else
		new_role = DWC3_GCTL_PRTCAP_DEVICE;

	if (dwc->current_dr_role == new_role)
		return;

	/* stop old role */
	switch (dwc->current_dr_role) {
	case DWC3_GCTL_PRTCAP_HOST:
		dwc3_host_exit(dwc);
		break;
	case DWC3_GCTL_PRTCAP_DEVICE:
		usb_del_gadget_udc(&dwc->gadget);
		break;
	default:
		break;
	}

	/* switch PRTCAP mode. updates current_dr_role */
	spin_lock_irqsave(&dwc->lock, flags);
	dwc3_set_mode(dwc, new_role);
	spin_unlock_irqrestore(&dwc->lock, flags);

	/* start new role */
	switch (dwc->current_dr_role) {
	case DWC3_GCTL_PRTCAP_HOST:
		dwc3_host_init(dwc);
		break;
	case DWC3_GCTL_PRTCAP_DEVICE:
		dwc3_event_buffers_setup(dwc);
		usb_add_gadget_udc(dwc->dev, &dwc->gadget);
		break;
	default:
		break;
	}
}

static void dwc3_drd_work(struct work_struct *work)
{
	struct dwc3 *dwc = container_of(work, struct dwc3,
					drd_work);
	dwc3_drd_update(dwc);
}

static int dwc3_drd_notifier(struct notifier_block *nb,
			     unsigned long event, void *ptr)
{
	struct dwc3 *dwc = container_of(nb, struct dwc3, edev_nb);

	queue_work(system_power_efficient_wq, &dwc->drd_work);

	return NOTIFY_DONE;
}

int dwc3_drd_init(struct dwc3 *dwc)
{
	int ret;
	int id;
	struct device *dev = dwc->dev;

	INIT_WORK(&dwc->drd_work, dwc3_drd_work);

	if (dev->of_node) {
		if (of_property_read_bool(dev->of_node, "extcon"))
			dwc->edev = extcon_get_edev_by_phandle(dev, 0);

		if (IS_ERR(dwc->edev))
			return PTR_ERR(dwc->edev);
	} else {
		return -ENODEV;
	}

	dwc->edev_nb.notifier_call = dwc3_drd_notifier;
	ret = extcon_register_notifier(dwc->edev, EXTCON_USB_HOST,
				       &dwc->edev_nb);
	if (ret < 0) {
		dev_err(dwc->dev, "Couldn't register USB-HOST cable notifier\n");
		return -ENODEV;
	}

	/* sanity check id & vbus states */
	id = extcon_get_state(dwc->edev, EXTCON_USB_HOST);
	if (id < 0) {
		dev_err(dwc->dev, "Invalid USB cable state. ID %d", id);
		ret = -ENODEV;
		goto fail;
	}

	/* start in peripheral role by default */
	dwc3_set_mode(dwc, DWC3_GCTL_PRTCAP_DEVICE);
	ret = dwc3_gadget_init(dwc);
	if (ret)
		goto fail;

	/* check & update drd state */
	dwc3_drd_update(dwc);

	return 0;

fail:
	extcon_unregister_notifier(dwc->edev, EXTCON_USB_HOST,
				   &dwc->edev_nb);

	return ret;
}

void dwc3_drd_exit(struct dwc3 *dwc)
{
	unsigned long flags;

	spin_lock_irqsave(&dwc->lock, flags);
	dwc->drd_prevent_change = true;
	spin_unlock_irqrestore(&dwc->lock, flags);

	extcon_unregister_notifier(dwc->edev, EXTCON_USB_HOST,
				   &dwc->edev_nb);

	/* role might have changed since start, stop active controller */
	if (dwc->current_dr_role == DWC3_GCTL_PRTCAP_HOST) {
		dwc3_host_exit(dwc);
		/* Add back UDC to match dwc3_gadget_exit() */
		usb_add_gadget_udc(dwc->dev, &dwc->gadget);
	}

	dwc3_gadget_exit(dwc);
}
