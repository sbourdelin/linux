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

/**
 * cdns3_ep0_run_transfer - Do transfer on default endpoint hardware
 * @priv_dev: extended gadget object
 * @dma_addr: physical address where data is/will be stored
 * @length: data length
 * @erdy: set it to 1 when ERDY packet should be sent -
 *        exit from flow control state
 */
static void cdns3_ep0_run_transfer(struct cdns3_device *priv_dev,
				   dma_addr_t dma_addr,
				   unsigned int length, int erdy)
{
	struct cdns3_usb_regs __iomem *regs = priv_dev->regs;

	priv_dev->trb_ep0->buffer = TRB_BUFFER(dma_addr);
	priv_dev->trb_ep0->length = TRB_LEN(length);
	priv_dev->trb_ep0->control = TRB_CYCLE | TRB_IOC | TRB_TYPE(TRB_NORMAL);

	cdns3_select_ep(priv_dev,
			priv_dev->ep0_data_dir ? USB_DIR_IN : USB_DIR_OUT);

	writel(EP_TRADDR_TRADDR(priv_dev->trb_ep0_dma), &regs->ep_traddr);

	dev_dbg(&priv_dev->dev, "//Ding Dong ep0%s\n",
		priv_dev->ep0_data_dir ? "IN" : "OUT");

	/* TRB should be prepared before starting transfer */
	wmb();
	writel(EP_CMD_DRDY, &regs->ep_cmd);

	if (erdy)
		writel(EP_CMD_ERDY, &priv_dev->regs->ep_cmd);
}

static void cdns3_prepare_setup_packet(struct cdns3_device *priv_dev)
{
	//TODO: Implements this function
}

static void cdns3_set_hw_configuration(struct cdns3_device *priv_dev)
{
	struct cdns3_endpoint *priv_ep;
	struct usb_request *request;
	struct usb_ep *ep;
	int result = 0;

	if (priv_dev->hw_configured_flag)
		return;

	writel(USB_CONF_CFGSET, &priv_dev->regs->usb_conf);
	writel(EP_CMD_ERDY | EP_CMD_REQ_CMPL, &priv_dev->regs->ep_cmd);

	cdns3_set_register_bit(&priv_dev->regs->usb_conf,
			       USB_CONF_U1EN | USB_CONF_U2EN);

	/* wait until configuration set */
	result = cdns3_handshake(&priv_dev->regs->usb_sts,
				 USB_STS_CFGSTS_MASK, 1, 100);

	priv_dev->hw_configured_flag = 1;
	cdns3_enable_l1(priv_dev, 1);

	list_for_each_entry(ep, &priv_dev->gadget.ep_list, ep_list) {
		if (ep->enabled) {
			priv_ep = ep_to_cdns3_ep(ep);
			request = cdns3_next_request(&priv_ep->request_list);
			if (request)
				cdns3_ep_run_transfer(priv_ep, request);
		}
	}
}

static void __pending_setup_status_handler(struct cdns3_device *priv_dev)
{
	//TODO: Implements this function
}

/**
 * cdns3_ep0_setup_phase - Handling setup USB requests
 * @priv_dev: extended gadget object
 */
static void cdns3_ep0_setup_phase(struct cdns3_device *priv_dev)
{
	//TODO: Implements this function.
}

static void cdns3_transfer_completed(struct cdns3_device *priv_dev)
{
	//TODO: Implements this function
}

/**
 * cdns3_check_ep0_interrupt_proceed - Processes interrupt related to endpoint 0
 * @priv_dev: extended gadget object
 * @dir: 1 for IN direction, 0 for OUT direction
 */
void cdns3_check_ep0_interrupt_proceed(struct cdns3_device *priv_dev, int dir)
{
	struct cdns3_usb_regs __iomem *regs = priv_dev->regs;
	u32 ep_sts_reg;

	cdns3_select_ep(priv_dev, 0 | (dir ? USB_DIR_IN : USB_DIR_OUT));
	ep_sts_reg = readl(&regs->ep_sts);

	__pending_setup_status_handler(priv_dev);

	if ((ep_sts_reg & EP_STS_SETUP) && dir == 0) {
		struct usb_ctrlrequest *setup = priv_dev->setup;

		writel(EP_STS_SETUP | EP_STS_IOC | EP_STS_ISP, &regs->ep_sts);

		priv_dev->ep0_data_dir = setup->bRequestType & USB_DIR_IN;
		cdns3_ep0_setup_phase(priv_dev);
		ep_sts_reg &= ~(EP_STS_SETUP | EP_STS_IOC | EP_STS_ISP);
	}

	if (ep_sts_reg & EP_STS_TRBERR)
		writel(EP_STS_TRBERR, &priv_dev->regs->ep_sts);

	if (ep_sts_reg & EP_STS_DESCMIS) {
		writel(EP_STS_DESCMIS, &priv_dev->regs->ep_sts);

		if (dir == 0 && !priv_dev->setup_pending) {
			priv_dev->ep0_data_dir = 0;
			cdns3_ep0_run_transfer(priv_dev, priv_dev->setup_dma,
					       8, 0);
		}
	}

	if ((ep_sts_reg & EP_STS_IOC) || (ep_sts_reg & EP_STS_ISP)) {
		writel(EP_STS_IOC, &priv_dev->regs->ep_sts);
		cdns3_transfer_completed(priv_dev);
	}
}

/**
 * cdns3_gadget_ep0_enable
 * Function shouldn't be called by gadget driver,
 * endpoint 0 is allways active
 */
static int cdns3_gadget_ep0_enable(struct usb_ep *ep,
				   const struct usb_endpoint_descriptor *desc)
{
	return -EINVAL;
}

/**
 * cdns3_gadget_ep0_disable
 * Function shouldn't be called by gadget driver,
 * endpoint 0 is allways active
 */
static int cdns3_gadget_ep0_disable(struct usb_ep *ep)
{
	return -EINVAL;
}

/**
 * cdns3_gadget_ep0_set_halt
 * @ep: pointer to endpoint zero object
 * @value: 1 for set stall, 0 for clear stall
 *
 * Returns 0
 */
static int cdns3_gadget_ep0_set_halt(struct usb_ep *ep, int value)
{
	/* TODO */
	return 0;
}

/**
 * cdns3_gadget_ep0_queue Transfer data on endpoint zero
 * @ep: pointer to endpoint zero object
 * @request: pointer to request object
 * @gfp_flags: gfp flags
 *
 * Returns 0 on success, error code elsewhere
 */
static int cdns3_gadget_ep0_queue(struct usb_ep *ep,
				  struct usb_request *request,
				  gfp_t gfp_flags)
{
	struct cdns3_endpoint *priv_ep = ep_to_cdns3_ep(ep);
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	unsigned long flags;
	int erdy_sent = 0;
	int ret = 0;

	dev_dbg(&priv_dev->dev, "Queue to Ep0%s L: %d\n",
		priv_dev->ep0_data_dir ? "IN" : "OUT",
		request->length);

	/* send STATUS stage */
	if (request->length == 0 && request->zero == 0) {
		spin_lock_irqsave(&priv_dev->lock, flags);
		cdns3_select_ep(priv_dev, 0x00);

		erdy_sent = !priv_dev->hw_configured_flag;
		cdns3_set_hw_configuration(priv_dev);

		if (!erdy_sent)
			writel(EP_CMD_ERDY | EP_CMD_REQ_CMPL,
			       &priv_dev->regs->ep_cmd);

		cdns3_prepare_setup_packet(priv_dev);
		request->actual = 0;
		priv_dev->status_completion_no_call = true;
		priv_dev->pending_status_request = request;
		spin_unlock_irqrestore(&priv_dev->lock, flags);

		/*
		 * Since there is no completion interrupt for status stage,
		 * it needs to call ->completion in software after
		 * ep0_queue is back.
		 */
		queue_work(system_freezable_wq, &priv_dev->pending_status_wq);
		return 0;
	}

	spin_lock_irqsave(&priv_dev->lock, flags);
	if (!list_empty(&priv_ep->request_list)) {
		dev_err(&priv_dev->dev,
			"can't handle multiple requests for ep0\n");
		spin_unlock_irqrestore(&priv_dev->lock, flags);
		return -EOPNOTSUPP;
	}

	ret = usb_gadget_map_request_by_dev(priv_dev->sysdev, request,
					    priv_dev->ep0_data_dir);
	if (ret) {
		spin_unlock_irqrestore(&priv_dev->lock, flags);
		dev_err(&priv_dev->dev, "failed to map request\n");
		return -EINVAL;
	}

	priv_dev->ep0_request = request;
	list_add_tail(&request->list, &priv_ep->request_list);
	cdns3_ep0_run_transfer(priv_dev, request->dma, request->length, 1);
	spin_unlock_irqrestore(&priv_dev->lock, flags);

	return ret;
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

const struct usb_ep_ops cdns3_gadget_ep0_ops = {
	.enable = cdns3_gadget_ep0_enable,
	.disable = cdns3_gadget_ep0_disable,
	.alloc_request = cdns3_gadget_ep_alloc_request,
	.free_request = cdns3_gadget_ep_free_request,
	.queue = cdns3_gadget_ep0_queue,
	.dequeue = cdns3_gadget_ep_dequeue,
	.set_halt = cdns3_gadget_ep0_set_halt,
	.set_wedge = cdns3_gadget_ep_set_wedge,
};

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
	ep0->endpoint.ops = &cdns3_gadget_ep0_ops;
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
