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

#include <linux/usb/composite.h>
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

/**
 * cdns3_ep0_delegate_req - Returns status of handling setup packet
 * Setup is handled by gadget driver
 * @priv_dev: extended gadget object
 * @ctrl_req: pointer to received setup packet
 *
 * Returns zero on success or negative value on failure
 */
static int cdns3_ep0_delegate_req(struct cdns3_device *priv_dev,
				  struct usb_ctrlrequest *ctrl_req)
{
	int ret;

	spin_unlock(&priv_dev->lock);
	priv_dev->setup_pending = 1;
	ret = priv_dev->gadget_driver->setup(&priv_dev->gadget, ctrl_req);
	priv_dev->setup_pending = 0;
	spin_lock(&priv_dev->lock);
	return ret;
}

static void cdns3_prepare_setup_packet(struct cdns3_device *priv_dev)
{
	priv_dev->ep0_data_dir = 0;
	cdns3_ep0_run_transfer(priv_dev, priv_dev->setup_dma, 8, 0);
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

/**
 * cdns3_req_ep0_set_configuration - Handling of SET_CONFIG standard USB request
 * @priv_dev: extended gadget object
 * @ctrl_req: pointer to received setup packet
 *
 * Returns 0 if success, 0x7FFF on deferred status stage, error code on error
 */
static int cdns3_req_ep0_set_configuration(struct cdns3_device *priv_dev,
					   struct usb_ctrlrequest *ctrl_req)
{
	enum usb_device_state device_state = priv_dev->gadget.state;
	struct cdns3_endpoint *priv_ep, *temp_ep;
	u32 config = le16_to_cpu(ctrl_req->wValue);
	int result = 0;

	switch (device_state) {
	case USB_STATE_ADDRESS:
		/* Configure non-control EPs */
		list_for_each_entry_safe(priv_ep, temp_ep,
					 &priv_dev->ep_match_list,
					 ep_match_pending_list)
			cdns3_ep_config(priv_ep);

		result = cdns3_ep0_delegate_req(priv_dev, ctrl_req);

		if (result)
			return result;

		if (config) {
			cdns3_set_hw_configuration(priv_dev);
		} else {
			cdns3_gadget_unconfig(priv_dev);
			usb_gadget_set_state(&priv_dev->gadget,
					     USB_STATE_ADDRESS);
		}
		break;
	case USB_STATE_CONFIGURED:
		result = cdns3_ep0_delegate_req(priv_dev, ctrl_req);

		if (!config && !result) {
			cdns3_gadget_unconfig(priv_dev);
			usb_gadget_set_state(&priv_dev->gadget,
					     USB_STATE_ADDRESS);
		}
		break;
	default:
		result = -EINVAL;
	}

	return result;
}

/**
 * cdns3_req_ep0_set_address - Handling of SET_ADDRESS standard USB request
 * @priv_dev: extended gadget object
 * @ctrl_req: pointer to received setup packet
 *
 * Returns 0 if success, error code on error
 */
static int cdns3_req_ep0_set_address(struct cdns3_device *priv_dev,
				     struct usb_ctrlrequest *ctrl_req)
{
	enum usb_device_state device_state = priv_dev->gadget.state;
	u32 reg;
	u32 addr;

	addr = le16_to_cpu(ctrl_req->wValue);

	if (addr > DEVICE_ADDRESS_MAX) {
		dev_err(&priv_dev->dev,
			"Device address (%d) cannot be greater than %d\n",
			addr, DEVICE_ADDRESS_MAX);
		return -EINVAL;
	}

	if (device_state == USB_STATE_CONFIGURED) {
		dev_err(&priv_dev->dev, "USB device already configured\n");
		return -EINVAL;
	}

	reg = readl(&priv_dev->regs->usb_cmd);

	writel(reg | USB_CMD_FADDR(addr) | USB_CMD_SET_ADDR,
	       &priv_dev->regs->usb_cmd);

	usb_gadget_set_state(&priv_dev->gadget,
			     (addr ? USB_STATE_ADDRESS : USB_STATE_DEFAULT));

	cdns3_prepare_setup_packet(priv_dev);

	writel(EP_CMD_ERDY | EP_CMD_REQ_CMPL, &priv_dev->regs->ep_cmd);

	return 0;
}

/**
 * cdns3_req_ep0_get_status - Handling of GET_STATUS standard USB request
 * @priv_dev: extended gadget object
 * @ctrl_req: pointer to received setup packet
 *
 * Returns 0 if success, error code on error
 */
static int cdns3_req_ep0_get_status(struct cdns3_device *priv_dev,
				    struct usb_ctrlrequest *ctrl)
{
	__le16 *response_pkt;
	u16 usb_status = 0;
	u32 recip;
	u32 reg;

	recip = ctrl->bRequestType & USB_RECIP_MASK;

	switch (recip) {
	case USB_RECIP_DEVICE:
		/* self powered */
		usb_status |= priv_dev->gadget.is_selfpowered;

		if (priv_dev->gadget.speed != USB_SPEED_SUPER)
			break;

		reg = readl(&priv_dev->regs->usb_sts);

		if (USB_STS_U2ENS(reg))
			usb_status |= BIT(USB_DEV_STAT_U1_ENABLED);

		if (USB_STS_U2ENS(reg))
			usb_status |= BIT(USB_DEV_STAT_U2_ENABLED);

		if (priv_dev->wake_up_flag)
			usb_status |= BIT(USB_DEVICE_REMOTE_WAKEUP);
		break;
	case USB_RECIP_INTERFACE:
		return cdns3_ep0_delegate_req(priv_dev, ctrl);
	case USB_RECIP_ENDPOINT:
		/* check if endpoint is stalled */
		cdns3_select_ep(priv_dev, ctrl->wIndex);
		if (EP_STS_STALL(readl(&priv_dev->regs->ep_sts)))
			usb_status =  BIT(USB_ENDPOINT_HALT);
		break;
	default:
		return -EINVAL;
	}

	response_pkt = (__le16 *)priv_dev->setup;
	*response_pkt = cpu_to_le16(usb_status);

	cdns3_ep0_run_transfer(priv_dev, priv_dev->setup_dma,
			       sizeof(*response_pkt), 1);
	return 0;
}

static int cdns3_ep0_feature_handle_device(struct cdns3_device *priv_dev,
					   struct usb_ctrlrequest *ctrl,
					   int set)
{
	enum usb_device_state state;
	enum usb_device_speed speed;
	int ret = 0;
	u32 wValue;
	u32 wIndex;
	u16 tmode;

	wValue = le16_to_cpu(ctrl->wValue);
	wIndex = le16_to_cpu(ctrl->wIndex);
	state = priv_dev->gadget.state;
	speed = priv_dev->gadget.speed;

	switch (ctrl->wValue) {
	case USB_DEVICE_REMOTE_WAKEUP:
		priv_dev->wake_up_flag = !!set;
		break;
	case USB_DEVICE_U1_ENABLE:
		if (state != USB_STATE_CONFIGURED || speed != USB_SPEED_SUPER)
			return -EINVAL;

		cdns3_set_register_bit(&priv_dev->regs->usb_conf,
				       (set) ? USB_CONF_U1EN : USB_CONF_U1DS);
		break;
	case USB_DEVICE_U2_ENABLE:
		if (state != USB_STATE_CONFIGURED || speed != USB_SPEED_SUPER)
			return -EINVAL;

		cdns3_set_register_bit(&priv_dev->regs->usb_conf,
				       (set) ? USB_CONF_U2EN : USB_CONF_U2DS);
		break;
	case USB_DEVICE_LTM_ENABLE:
		ret = -EINVAL;
		break;
	case USB_DEVICE_TEST_MODE:
		if (state != USB_STATE_CONFIGURED || speed > USB_SPEED_HIGH)
			return -EINVAL;

		tmode = le16_to_cpu(ctrl->wIndex);

		if (!set || (tmode & 0xff) != 0)
			return -EINVAL;

		switch (tmode >> 8) {
		case TEST_J:
		case TEST_K:
		case TEST_SE0_NAK:
		case TEST_PACKET:
			cdns3_set_register_bit(&priv_dev->regs->usb_cmd,
					       USB_CMD_STMODE |
					       USB_STS_TMODE_SEL(tmode - 1));
			break;
		default:
			ret = -EINVAL;
		}
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int cdns3_ep0_feature_handle_intf(struct cdns3_device *priv_dev,
					 struct usb_ctrlrequest *ctrl,
					 int set)
{
	u32 wValue;
	int ret = 0;

	wValue = le16_to_cpu(ctrl->wValue);

	switch (wValue) {
	case USB_INTRF_FUNC_SUSPEND:
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int cdns3_ep0_feature_handle_endpoint(struct cdns3_device *priv_dev,
					     struct usb_ctrlrequest *ctrl,
					     int set)
{
	struct cdns3_endpoint *priv_ep;
	int ret = 0;
	u8 index;

	index = cdns3_ep_addr_to_index(ctrl->wIndex);
	priv_ep = priv_dev->eps[index];

	cdns3_select_ep(priv_dev, ctrl->wIndex);

	if (le16_to_cpu(ctrl->wValue) != USB_ENDPOINT_HALT)
		return -EINVAL;

	if (set) {
		writel(EP_CMD_SSTALL, &priv_dev->regs->ep_cmd);
		priv_ep->flags |= EP_STALL;
	} else {
		struct usb_request *request;

		if (priv_dev->eps[index]->flags & EP_WEDGE) {
			cdns3_select_ep(priv_dev, 0x00);
			return 0;
		}

		writel(EP_CMD_CSTALL | EP_CMD_EPRST, &priv_dev->regs->ep_cmd);

		/* wait for EPRST cleared */
		ret = cdns3_handshake(&priv_dev->regs->ep_cmd,
				      EP_CMD_EPRST, 0, 100);
		if (ret)
			return -EINVAL;

		priv_ep->flags &= ~EP_STALL;

		request = cdns3_next_request(&priv_ep->request_list);
		if (request)
			cdns3_ep_run_transfer(priv_ep, request);
	}
	return ret;
}

/**
 * cdns3_req_ep0_handle_feature -
 * Handling of GET/SET_FEATURE standard USB request
 *
 * @priv_dev: extended gadget object
 * @ctrl_req: pointer to received setup packet
 * @set: must be set to 1 for SET_FEATURE request
 *
 * Returns 0 if success, error code on error
 */
static int cdns3_req_ep0_handle_feature(struct cdns3_device *priv_dev,
					struct usb_ctrlrequest *ctrl,
					int set)
{
	int ret = 0;
	u32 recip;

	recip = ctrl->bRequestType & USB_RECIP_MASK;

	switch (recip) {
	case USB_RECIP_DEVICE:
		ret = cdns3_ep0_feature_handle_device(priv_dev, ctrl, set);
		break;
	case USB_RECIP_INTERFACE:
		ret = cdns3_ep0_feature_handle_intf(priv_dev, ctrl, set);
		break;
	case USB_RECIP_ENDPOINT:
		ret = cdns3_ep0_feature_handle_endpoint(priv_dev, ctrl, set);
		break;
	default:
		return -EINVAL;
	}

	if (!ret)
		writel(EP_CMD_ERDY | EP_CMD_REQ_CMPL, &priv_dev->regs->ep_cmd);

	return ret;
}

/**
 * cdns3_req_ep0_set_sel - Handling of SET_SEL standard USB request
 * @priv_dev: extended gadget object
 * @ctrl_req: pointer to received setup packet
 *
 * Returns 0 if success, error code on error
 */
static int cdns3_req_ep0_set_sel(struct cdns3_device *priv_dev,
				 struct usb_ctrlrequest *ctrl_req)
{
	if (priv_dev->gadget.state < USB_STATE_ADDRESS)
		return -EINVAL;

	if (ctrl_req->wLength != 6) {
		dev_err(&priv_dev->dev, "Set SEL should be 6 bytes, got %d\n",
			ctrl_req->wLength);
		return -EINVAL;
	}

	priv_dev->ep0_data_dir = 0;
	cdns3_ep0_run_transfer(priv_dev, priv_dev->setup_dma, 6, 1);
	return 0;
}

/**
 * cdns3_req_ep0_set_isoch_delay -
 * Handling of GET_ISOCH_DELAY standard USB request
 * @priv_dev: extended gadget object
 * @ctrl_req: pointer to received setup packet
 *
 * Returns 0 if success, error code on error
 */
static int cdns3_req_ep0_set_isoch_delay(struct cdns3_device *priv_dev,
					 struct usb_ctrlrequest *ctrl_req)
{
	if (ctrl_req->wIndex || ctrl_req->wLength)
		return -EINVAL;

	priv_dev->isoch_delay = ctrl_req->wValue;
	writel(EP_CMD_ERDY | EP_CMD_REQ_CMPL, &priv_dev->regs->ep_cmd);
	return 0;
}

/**
 * cdns3_ep0_standard_request - Handling standard USB requests
 * @priv_dev: extended gadget object
 * @ctrl_req: pointer to received setup packet
 *
 * Returns 0 if success, error code on error
 */
static int cdns3_ep0_standard_request(struct cdns3_device *priv_dev,
				      struct usb_ctrlrequest *ctrl_req)
{
	int ret;

	switch (ctrl_req->bRequest) {
	case USB_REQ_SET_ADDRESS:
		ret = cdns3_req_ep0_set_address(priv_dev, ctrl_req);
		break;
	case USB_REQ_SET_CONFIGURATION:
		ret = cdns3_req_ep0_set_configuration(priv_dev, ctrl_req);
		break;
	case USB_REQ_GET_STATUS:
		ret = cdns3_req_ep0_get_status(priv_dev, ctrl_req);
		break;
	case USB_REQ_CLEAR_FEATURE:
		ret = cdns3_req_ep0_handle_feature(priv_dev, ctrl_req, 0);
		break;
	case USB_REQ_SET_FEATURE:
		ret = cdns3_req_ep0_handle_feature(priv_dev, ctrl_req, 1);
		break;
	case USB_REQ_SET_SEL:
		ret = cdns3_req_ep0_set_sel(priv_dev, ctrl_req);
		break;
	case USB_REQ_SET_ISOCH_DELAY:
		ret = cdns3_req_ep0_set_isoch_delay(priv_dev, ctrl_req);
		break;
	default:
		ret = cdns3_ep0_delegate_req(priv_dev, ctrl_req);
		break;
	}

	return ret;
}

static void __pending_setup_status_handler(struct cdns3_device *priv_dev)
{
	struct usb_request *request = priv_dev->pending_status_request;

	if (priv_dev->status_completion_no_call && request &&
	    request->complete) {
		request->complete(priv_dev->gadget.ep0, request);
		priv_dev->status_completion_no_call = 0;
	}
}

void cdns3_pending_setup_status_handler(struct work_struct *work)
{
	struct cdns3_device *priv_dev = container_of(work, struct cdns3_device,
			pending_status_wq);
	unsigned long flags;

	spin_lock_irqsave(&priv_dev->lock, flags);
	__pending_setup_status_handler(priv_dev);
	spin_unlock_irqrestore(&priv_dev->lock, flags);
}

/**
 * cdns3_ep0_setup_phase - Handling setup USB requests
 * @priv_dev: extended gadget object
 */
static void cdns3_ep0_setup_phase(struct cdns3_device *priv_dev)
{
	struct usb_ctrlrequest *ctrl = priv_dev->setup;
	int result;

	if ((ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD)
		result = cdns3_ep0_standard_request(priv_dev, ctrl);
	else
		result = cdns3_ep0_delegate_req(priv_dev, ctrl);

	if (result != 0 && result != USB_GADGET_DELAYED_STATUS) {
		dev_dbg(&priv_dev->dev, "STALL(00) %d\n", result);
		/* set_stall on ep0 */
		cdns3_select_ep(priv_dev, 0x00);
		writel(EP_CMD_SSTALL, &priv_dev->regs->ep_cmd);
		writel(EP_CMD_ERDY | EP_CMD_REQ_CMPL, &priv_dev->regs->ep_cmd);
	}
}

static void cdns3_transfer_completed(struct cdns3_device *priv_dev)
{
	if (priv_dev->ep0_request) {
		usb_gadget_unmap_request_by_dev(priv_dev->sysdev,
						priv_dev->ep0_request,
						priv_dev->ep0_data_dir);

		priv_dev->ep0_request->actual =
			TRB_LEN(le32_to_cpu(priv_dev->trb_ep0->length));

		dev_dbg(&priv_dev->dev, "Ep0 completion length %d\n",
			priv_dev->ep0_request->actual);
		list_del_init(&priv_dev->ep0_request->list);
	}

	if (priv_dev->ep0_request &&
	    priv_dev->ep0_request->complete) {
		spin_unlock(&priv_dev->lock);
		priv_dev->ep0_request->complete(priv_dev->gadget.ep0,
						priv_dev->ep0_request);

		priv_dev->ep0_request = NULL;
		spin_lock(&priv_dev->lock);
	}

	cdns3_prepare_setup_packet(priv_dev);
	writel(EP_CMD_REQ_CMPL, &priv_dev->regs->ep_cmd);
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
