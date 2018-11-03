// SPDX-License-Identifier: GPL-2.0
/*
 * Cadence USBSS DRD Driver - gadget side.
 *
 * Copyright (C) 2018 Cadence Design Systems.
 * Copyright (C) 2017 NXP
 *
 * Authors: Pawel Jez <pjez@cadence.com>,
 *          Pawel Laszczak <pawell@cadence.com>
 *	    Peter Chen <peter.chen@nxp.com>
 */

#include "gadget.h"

static struct usb_endpoint_descriptor cdns3_gadget_ep0_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bmAttributes =	USB_ENDPOINT_XFER_CONTROL,
};

static void cdns3_prepare_setup_packet(struct cdns3_device *priv_dev)
{
	//TODO: Implements this function
}

/**
 * cdns3_gadget_ep_set_wedge Set wedge on selected endpoint
 * @ep: endpoint object
 *
 * Returns 0
 */
int cdns3_gadget_ep_set_wedge(struct usb_ep *ep)
{
	struct cdns3_endpoint *priv_ep = ep_to_cdns3_ep(ep);
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;

	dev_dbg(&priv_dev->dev, "Wedge for %s\n", ep->name);
	cdns3_gadget_ep_set_halt(ep, 1);
	priv_ep->flags |= EP_WEDGE;

	return 0;
}

/**
 * cdns3_ep0_config - Configures default endpoint
 * @priv_dev: extended gadget object
 *
 * Functions sets parameters: maximal packet size and enables interrupts
 */
void cdns3_ep0_config(struct cdns3_device *priv_dev)
{
	struct cdns3_usb_regs __iomem *regs;
	u32 max_packet_size = 64;

	regs = priv_dev->regs;

	if (priv_dev->gadget.speed == USB_SPEED_SUPER)
		max_packet_size = 512;

	if (priv_dev->ep0_request) {
		list_del_init(&priv_dev->ep0_request->list);
		priv_dev->ep0_request = NULL;
	}

	priv_dev->gadget.ep0->maxpacket = max_packet_size;
	cdns3_gadget_ep0_desc.wMaxPacketSize = cpu_to_le16(max_packet_size);

	/* init ep out */
	cdns3_select_ep(priv_dev, USB_DIR_OUT);

	writel(EP_CFG_ENABLE | EP_CFG_MAXPKTSIZE(max_packet_size),
	       &regs->ep_cfg);

	writel(EP_STS_EN_SETUPEN | EP_STS_EN_DESCMISEN | EP_STS_EN_TRBERREN,
	       &regs->ep_sts_en);

	/* init ep in */
	cdns3_select_ep(priv_dev, USB_DIR_IN);

	writel(EP_CFG_ENABLE | EP_CFG_MAXPKTSIZE(max_packet_size),
	       &regs->ep_cfg);

	writel(EP_STS_EN_SETUPEN | EP_STS_EN_TRBERREN, &regs->ep_sts_en);

	cdns3_set_register_bit(&regs->usb_conf, USB_CONF_U1DS | USB_CONF_U2DS);
	cdns3_prepare_setup_packet(priv_dev);
}

/**
 * cdns3_init_ep0 Initializes software endpoint 0 of gadget
 * @cdns3: extended gadget object
 *
 * Returns 0 on success, error code elsewhere
 */
int cdns3_init_ep0(struct cdns3_device *priv_dev)
{
	struct cdns3_endpoint *ep0;

	ep0 = devm_kzalloc(&priv_dev->dev, sizeof(struct cdns3_endpoint),
			   GFP_KERNEL);

	if (!ep0)
		return -ENOMEM;

	ep0->cdns3_dev = priv_dev;
	sprintf(ep0->name, "ep0");

	/* fill linux fields */
	//TODO: implements cdns3_gadget_ep0_ops object
	//ep0->endpoint.ops = &cdns3_gadget_ep0_ops;
	ep0->endpoint.maxburst = 1;
	usb_ep_set_maxpacket_limit(&ep0->endpoint, ENDPOINT0_MAX_PACKET_LIMIT);
	ep0->endpoint.address = 0;
	ep0->endpoint.caps.type_control = 1;
	ep0->endpoint.caps.dir_in = 1;
	ep0->endpoint.caps.dir_out = 1;
	ep0->endpoint.name = ep0->name;
	ep0->endpoint.desc = &cdns3_gadget_ep0_desc;
	priv_dev->gadget.ep0 = &ep0->endpoint;
	INIT_LIST_HEAD(&ep0->request_list);

	return 0;
}
