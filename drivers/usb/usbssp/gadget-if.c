// SPDX-License-Identifier: GPL-2.0
/*
 * USBSSP device controller driver
 *
 * Copyright (C) 2018 Cadence.
 *
 * Author: Pawel Laszczak
 * Some code borrowed from the Linux XHCI driver.
 */

#include <linux/usb/gadget.h>
#include <linux/usb/composite.h>
#include "gadget.h"

#define usbssp_g_lock(flag, save_flags) { \
	if (in_interrupt()) {\
		spin_lock_irqsave(&usbssp_data->lock, save_flags); \
	} else { \
		if (!irqs_disabled()) { \
			spin_lock_irqsave(&usbssp_data->irq_thread_lock,\
					usbssp_data->irq_thread_flag);\
			flag = 1; \
		} else \
			spin_lock(&usbssp_data->irq_thread_lock); \
	} }


#define usbssp_g_unlock(flag, save_flags) { \
	if (in_interrupt()) \
		spin_unlock_irqrestore(&usbssp_data->lock, save_flags); \
	else  { \
		if (flag) \
			spin_unlock_irqrestore(&usbssp_data->irq_thread_lock,\
					usbssp_data->irq_thread_flag);\
		else \
			spin_unlock(&usbssp_data->irq_thread_lock); \
	} }

static int usbssp_gadget_ep_enable(struct usb_ep *ep,
		const struct usb_endpoint_descriptor *desc)
{
	struct usbssp_ep *ep_priv;
	struct usbssp_udc *usbssp_data;
	int ret = 0;
	int irq_disabled_locally = 0;
	unsigned long flags = 0;

	if (!ep || !desc || desc->bDescriptorType != USB_DT_ENDPOINT) {
		pr_err("invalid parameters\n");
		return -EINVAL;
	}

	ep_priv = to_usbssp_ep(ep);
	usbssp_data = ep_priv->usbssp_data;

	if (!desc->wMaxPacketSize) {
		usbssp_dbg(usbssp_data, "missing wMaxPacketSize\n");
		return -EINVAL;
	}




	if (ep_priv->ep_state & USBSSP_EP_ENABLED) {
		usbssp_dbg(usbssp_data, "%s is already enabled\n",
			   ep_priv->name);
		return -EINVAL;
	}

	usbssp_g_lock(irq_disabled_locally, flags);
	ret = usbssp_add_endpoint(usbssp_data,  ep_priv);
	if (ret < 0)
		goto finish;

	ep_priv->ep_state |= USBSSP_EP_ENABLED;

	/*Update bandwidth information*/
	ret = usbssp_check_bandwidth(usbssp_data, &usbssp_data->gadget);

	if (ret < 0)
		ep_priv->ep_state &= ~USBSSP_EP_ENABLED;

finish:
	usbssp_dbg(usbssp_data, "%s enable endpoint %s\n", ep_priv->name,
			(ret == 0) ? "success" : "failed");
	usbssp_g_unlock(irq_disabled_locally, flags);
	return ret;
}

int usbssp_gadget_ep_disable(struct usb_ep *ep)
{
	struct usbssp_ep *ep_priv;
	struct usbssp_udc *usbssp_data;
	int ep_index = 0;
	int ret;
	int irq_disabled_locally = 0;
	unsigned long flags = 0;
	struct usbssp_request  *req_priv;

	ep_priv = to_usbssp_ep(ep);
	usbssp_data = ep_priv->usbssp_data;
	ep_index = usbssp_get_endpoint_index(ep_priv->endpoint.desc);

	if (!(ep_priv->ep_state & USBSSP_EP_ENABLED)) {
		usbssp_dbg(usbssp_data, "%s is already disabled\n",
				ep_priv->name);
		return -EINVAL;
	}

	usbssp_g_lock(irq_disabled_locally, flags);

	ep_priv->ep_state |= USBSSP_EP_DISABLE_PENDING;

	/*dequeue all USB request from endpoint*/
	list_for_each_entry(req_priv, &ep_priv->pending_list, list) {
		usbssp_dequeue(ep_priv, req_priv);
	}

	ret = usbssp_drop_endpoint(usbssp_data, &usbssp_data->gadget, ep_priv);
	if (ret)
		goto finish;

	ret = usbssp_check_bandwidth(usbssp_data, &usbssp_data->gadget);
	if (ret)
		goto finish;

	ep_priv->ep_state &= ~USBSSP_EP_ENABLED;

finish:
	ep_priv->ep_state &= ~USBSSP_EP_DISABLE_PENDING;
	usbssp_dbg(usbssp_data, "%s disable endpoint %s\n", ep_priv->name,
			(ret == 0) ? "success" : "failed");

	usbssp_g_unlock(irq_disabled_locally, flags);
	return ret;
}

static struct usb_request *usbssp_gadget_ep_alloc_request(struct usb_ep *ep,
							  gfp_t gfp_flags)
{
	struct usbssp_request *req_priv;
	struct usbssp_ep *ep_priv = to_usbssp_ep(ep);

	req_priv = kzalloc(sizeof(*req_priv), gfp_flags);
	if (!req_priv)
		return NULL;

	req_priv->epnum = ep_priv->number;
	req_priv->dep  = ep_priv;

	trace_usbssp_alloc_request(&req_priv->request);
	return &req_priv->request;
}

static void usbssp_gadget_ep_free_request(struct usb_ep *ep,
		struct usb_request *request)
{
	struct usbssp_request *req_priv = to_usbssp_request(request);

	trace_usbssp_free_request(&req_priv->request);
	kfree(req_priv);
}

static int  usbssp_gadget_ep_queue(struct usb_ep *ep,
				   struct usb_request *request,
				   gfp_t gfp_flags)
{
	struct usbssp_ep *ep_priv = to_usbssp_ep(ep);
	struct usbssp_request *req_priv = to_usbssp_request(request);
	struct usbssp_udc *usbssp_data = ep_priv->usbssp_data;
	unsigned long flags = 0;
	int irq_disabled_locally = 0;
	int ret;

	if (!ep_priv->endpoint.desc) {
		usbssp_err(usbssp_data,
				"%s: can't queue to disabled endpoint\n",
				ep_priv->name);
		return -ESHUTDOWN;
	}

	if ((ep_priv->ep_state & USBSSP_EP_DISABLE_PENDING ||
			!(ep_priv->ep_state & USBSSP_EP_ENABLED))) {
		dev_err(usbssp_data->dev,
				"%s: can't queue to disabled endpoint\n",
				ep_priv->name);
		ret = -ESHUTDOWN;
		goto out;
	}

	usbssp_g_lock(irq_disabled_locally, flags);
	ret =  usbssp_enqueue(ep_priv, req_priv);
	usbssp_g_unlock(irq_disabled_locally, flags);
out:
	return ret;
}

static int usbssp_gadget_ep_dequeue(struct usb_ep *ep,
				    struct usb_request *request)
{
	struct usbssp_ep *ep_priv = to_usbssp_ep(ep);
	struct usbssp_request *req_priv = to_usbssp_request(request);
	struct usbssp_udc *usbssp_data = ep_priv->usbssp_data;
	unsigned long flags = 0;
	int ret;
	int irq_disabled_locally = 0;

	if (!ep_priv->endpoint.desc) {
		usbssp_err(usbssp_data,
				"%s: can't queue to disabled endpoint\n",
				ep_priv->name);
		return -ESHUTDOWN;
	}

	usbssp_g_lock(irq_disabled_locally, flags);
	ret =  usbssp_dequeue(ep_priv, req_priv);
	usbssp_g_unlock(irq_disabled_locally, flags);

	return ret;
}

static int usbssp_gadget_ep_set_halt(struct usb_ep *ep, int value)
{
	struct usbssp_ep *ep_priv = to_usbssp_ep(ep);
	struct usbssp_udc *usbssp_data = ep_priv->usbssp_data;
	int ret;
	int irq_disabled_locally = 0;
	unsigned long flags = 0;

	usbssp_g_lock(irq_disabled_locally, flags);
	ret =  usbssp_halt_endpoint(usbssp_data, ep_priv, value);
	usbssp_g_unlock(irq_disabled_locally, flags);
	return ret;
}

static int usbssp_gadget_ep_set_wedge(struct usb_ep *ep)
{
	struct usbssp_ep *ep_priv = to_usbssp_ep(ep);
	struct usbssp_udc *usbssp_data = ep_priv->usbssp_data;
	unsigned long flags = 0;
	int ret;
	int irq_disabled_locally = 0;

	usbssp_g_lock(irq_disabled_locally, flags);
	ep_priv->ep_state |= USBSSP_EP_WEDGE;
	ret =  usbssp_halt_endpoint(usbssp_data, ep_priv, 1);
	usbssp_g_unlock(irq_disabled_locally, flags);

	return ret;
}

static const struct usb_ep_ops usbssp_gadget_ep0_ops = {
	.enable		= usbssp_gadget_ep_enable,
	.disable	= usbssp_gadget_ep_disable,
	.alloc_request	= usbssp_gadget_ep_alloc_request,
	.free_request	= usbssp_gadget_ep_free_request,
	.queue		= usbssp_gadget_ep_queue,
	.dequeue	= usbssp_gadget_ep_dequeue,
	.set_halt	= usbssp_gadget_ep_set_halt,
	.set_wedge	= usbssp_gadget_ep_set_wedge,
};

static const struct usb_ep_ops usbssp_gadget_ep_ops = {
	.enable		= usbssp_gadget_ep_enable,
	.disable	= usbssp_gadget_ep_disable,
	.alloc_request	= usbssp_gadget_ep_alloc_request,
	.free_request	= usbssp_gadget_ep_free_request,
	.queue		= usbssp_gadget_ep_queue,
	.dequeue	= usbssp_gadget_ep_dequeue,
	.set_halt	= usbssp_gadget_ep_set_halt,
	.set_wedge	= usbssp_gadget_ep_set_wedge,
};

void usbssp_gadget_giveback(struct usbssp_ep *ep_priv,
			    struct usbssp_request *req_priv, int status)
{
	struct usbssp_udc *usbssp_data = ep_priv->usbssp_data;

	list_del(&req_priv->list);

	if (req_priv->request.status == -EINPROGRESS)
		req_priv->request.status = status;

	usb_gadget_unmap_request_by_dev(usbssp_data->dev,
			&req_priv->request, req_priv->direction);

	trace_usbssp_request_giveback(&req_priv->request);

	if (in_interrupt())
		spin_unlock(&usbssp_data->lock);
	else
		spin_unlock(&usbssp_data->irq_thread_lock);

	if (req_priv != &usbssp_data->usb_req_ep0_in) {
		usb_gadget_giveback_request(&ep_priv->endpoint,
				&req_priv->request);
	}

	if (in_interrupt())
		spin_lock(&usbssp_data->lock);
	else
		spin_lock(&usbssp_data->irq_thread_lock);

}

static struct usb_endpoint_descriptor usbssp_gadget_ep0_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bmAttributes =		USB_ENDPOINT_XFER_CONTROL,
};

static int usbssp_gadget_start(struct usb_gadget *g,
			       struct usb_gadget_driver *driver)
{
	struct usbssp_udc *usbssp_data = gadget_to_usbssp(g);
	int ret = 0;

	if (usbssp_data->gadget_driver) {
		usbssp_err(usbssp_data, "%s is already bound to %s\n",
				usbssp_data->gadget.name,
				usbssp_data->gadget_driver->driver.name);
		ret = -EBUSY;
		goto err1;
	}

	usbssp_data->gadget_driver = driver;

	if (pm_runtime_active(usbssp_data->dev)) {
		usbssp_gadget_ep0_desc.wMaxPacketSize = cpu_to_le16(512);
		usbssp_data->ep0state = USBSSP_EP0_UNCONNECTED;
		ret = usbssp_run(usbssp_data);
		if (ret < 0)
			goto err1;
	}
	return 0;
err1:
	return ret;
}

static int usbssp_gadget_stop(struct usb_gadget *g)
{
	unsigned long flags = 0;
	int irq_disabled_locally = 0;
	struct usbssp_udc *usbssp_data  = gadget_to_usbssp(g);

	usbssp_g_lock(irq_disabled_locally, flags);
	if (pm_runtime_suspended(usbssp_data->dev))
		goto out;

	usbssp_free_dev(usbssp_data);
	usbssp_stop(usbssp_data);
out:
	usbssp_data->gadget_driver = NULL;
	usbssp_g_unlock(irq_disabled_locally, flags);

	return 0;
}

static int usbssp_gadget_get_frame(struct usb_gadget *g)
{
	struct usbssp_udc *usbssp_data = gadget_to_usbssp(g);

	return usbssp_get_frame(usbssp_data);
}

static int usbssp_gadget_wakeup(struct usb_gadget *g)
{
	struct usbssp_udc *usbssp_data = gadget_to_usbssp(g);
	unsigned long flags = 0;
	int irq_disabled_locally = 0;
	__le32 __iomem *port_regs;
	u32 temp;

	if (!usbssp_data->port_remote_wakeup)
		return -EINVAL;

	if (!usbssp_data->port_suspended)
		return -EINVAL;

	usbssp_g_lock(irq_disabled_locally, flags);

	port_regs = usbssp_get_port_io_addr(usbssp_data);
	temp = readl(port_regs+PORTPMSC);

	if (!(temp & PORT_RWE))
		return 0;

	temp = readl(port_regs + PORTSC);

	temp &= ~PORT_PLS_MASK;
	writel(temp, port_regs + PORTPMSC);
	usbssp_g_unlock(irq_disabled_locally, flags);
	return 0;
}

static int usbssp_gadget_set_selfpowered(struct usb_gadget *g,
		int is_selfpowered)
{
	unsigned long flags = 0;
	int irq_disabled_locally = 0;
	struct usbssp_udc *usbssp_data = gadget_to_usbssp(g);

	usbssp_g_lock(irq_disabled_locally, flags);

	g->is_selfpowered = !!is_selfpowered;
	usbssp_g_unlock(irq_disabled_locally, flags);

	return 0;
}

static const struct usb_gadget_ops usbssp_gadget_ops = {
	.get_frame		= usbssp_gadget_get_frame,
	.wakeup			= usbssp_gadget_wakeup,
	.set_selfpowered	= usbssp_gadget_set_selfpowered,
	.udc_start		= usbssp_gadget_start,
	.udc_stop		= usbssp_gadget_stop,
};

int usbssp_gadget_init_endpoint(struct usbssp_udc *usbssp_data)
{
	int i = 0;
	struct usbssp_ep *ep_priv;

	usbssp_data->num_endpoints = USBSSP_ENDPOINTS_NUM;
	INIT_LIST_HEAD(&usbssp_data->gadget.ep_list);

	for (i = 1; i < usbssp_data->num_endpoints; i++) {
		bool direction = i & 1;  /*start from OUT endpoint*/
		u8 epnum = (i >>  1);

		ep_priv = &usbssp_data->devs.eps[i-1];
		ep_priv->usbssp_data = usbssp_data;
		ep_priv->number = epnum;
		ep_priv->direction = direction;  /*0 for OUT, 1 for IN*/

		snprintf(ep_priv->name, sizeof(ep_priv->name), "ep%d%s", epnum,
				(ep_priv->direction) ? "in"  : "out");

		ep_priv->endpoint.name = ep_priv->name;

		if (ep_priv->number < 2) {
			ep_priv->endpoint.desc = &usbssp_gadget_ep0_desc;
			ep_priv->endpoint.comp_desc = NULL;
		}

		if (epnum == 0) {  //EP0 is  bidirectional endpoint
			usb_ep_set_maxpacket_limit(&ep_priv->endpoint, 512);
			usbssp_dbg(usbssp_data,
				"Initializing %s, MaxPack: %04x Type: Ctrl\n",
				ep_priv->name, 512);
			ep_priv->endpoint.maxburst = 1;
			ep_priv->endpoint.ops = &usbssp_gadget_ep0_ops;
			ep_priv->endpoint.caps.type_control = true;

			usbssp_data->usb_req_ep0_in.epnum = ep_priv->number;
			usbssp_data->usb_req_ep0_in.dep  = ep_priv;

			if (!epnum)
				usbssp_data->gadget.ep0 = &ep_priv->endpoint;
		} else {
			usb_ep_set_maxpacket_limit(&ep_priv->endpoint, 1024);
			ep_priv->endpoint.maxburst = 15;
			ep_priv->endpoint.ops = &usbssp_gadget_ep_ops;
			list_add_tail(&ep_priv->endpoint.ep_list,
					&usbssp_data->gadget.ep_list);
			ep_priv->endpoint.caps.type_iso = true;
			ep_priv->endpoint.caps.type_bulk = true;
			ep_priv->endpoint.caps.type_int = true;

		}

		ep_priv->endpoint.caps.dir_in = direction;
		ep_priv->endpoint.caps.dir_out = !direction;

		usbssp_dbg(usbssp_data, "Init %s, MaxPack: %04x SupType:"
				" INT/BULK/ISOC , SupDir %s\n",
				ep_priv->name, 1024,
				(ep_priv->endpoint.caps.dir_in) ? "IN" : "OUT");

		INIT_LIST_HEAD(&ep_priv->pending_list);
	}
	return 0;
}

void usbssp_gadget_free_endpoint(struct usbssp_udc *usbssp_data)
{
	int i;
	struct usbssp_ep *ep_priv;

	for (i = 0; i < usbssp_data->num_endpoints; i++) {
		ep_priv = &usbssp_data->devs.eps[i];

		if (ep_priv->number != 0)
			list_del(&ep_priv->endpoint.ep_list);
	}
}

static void usbssp_disconnect_gadget(struct usbssp_udc *usbssp_data)
{
	if (usbssp_data->gadget_driver &&
	    usbssp_data->gadget_driver->disconnect) {
		spin_unlock(&usbssp_data->irq_thread_lock);
		usbssp_data->gadget_driver->disconnect(&usbssp_data->gadget);
		spin_lock(&usbssp_data->irq_thread_lock);
	}
}

void usbssp_suspend_gadget(struct usbssp_udc *usbssp_data)
{
	if (usbssp_data->gadget_driver && usbssp_data->gadget_driver->suspend) {
		spin_unlock(&usbssp_data->lock);
		usbssp_data->gadget_driver->suspend(&usbssp_data->gadget);
		spin_lock(&usbssp_data->lock);
	}
}

void usbssp_resume_gadget(struct usbssp_udc *usbssp_data)
{
	if (usbssp_data->gadget_driver && usbssp_data->gadget_driver->resume) {
		spin_unlock(&usbssp_data->lock);
		usbssp_data->gadget_driver->resume(&usbssp_data->gadget);
		spin_lock(&usbssp_data->lock);
	}
}

static void usbssp_reset_gadget(struct usbssp_udc *usbssp_data)
{
	if (!usbssp_data->gadget_driver)
		return;

	if (usbssp_data->gadget.speed != USB_SPEED_UNKNOWN) {
		spin_unlock(&usbssp_data->lock);
		usb_gadget_udc_reset(&usbssp_data->gadget,
				usbssp_data->gadget_driver);
		spin_lock(&usbssp_data->lock);
	}
}

void usbssp_gadget_disconnect_interrupt(struct usbssp_udc *usbssp_data)
{
	usbssp_disconnect_gadget(usbssp_data);
}


void usbssp_gadget_reset_interrupt(struct usbssp_udc *usbssp_data)
{
	usbssp_reset_gadget(usbssp_data);
	switch (usbssp_data->gadget.speed) {
	case USB_SPEED_SUPER_PLUS:
		usbssp_gadget_ep0_desc.wMaxPacketSize = cpu_to_le16(512);
		usbssp_data->gadget.ep0->maxpacket = 512;
		break;
	case USB_SPEED_SUPER:
		usbssp_gadget_ep0_desc.wMaxPacketSize = cpu_to_le16(512);
		usbssp_data->gadget.ep0->maxpacket = 512;
		break;
	case USB_SPEED_HIGH:
		usbssp_gadget_ep0_desc.wMaxPacketSize = cpu_to_le16(64);
		usbssp_data->gadget.ep0->maxpacket = 64;
		break;
	case USB_SPEED_FULL:
		usbssp_gadget_ep0_desc.wMaxPacketSize = cpu_to_le16(64);
		usbssp_data->gadget.ep0->maxpacket = 64;
		break;
	case USB_SPEED_LOW:
		usbssp_gadget_ep0_desc.wMaxPacketSize = cpu_to_le16(8);
		usbssp_data->gadget.ep0->maxpacket = 8;
		break;
	default:
		break;
	}
}
