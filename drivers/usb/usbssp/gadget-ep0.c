// SPDX-License-Identifier: GPL-2.0
/*
 * USBSSP device controller driver
 *
 * Copyright (C) 2018 Cadence.
 *
 * Author: Pawel Laszczak
 * Some code borrowed from the Linux XHCI driver.
 */

#include <linux/list.h>
#include <linux/usb/gadget.h>
#include <linux/usb/composite.h>
#include "gadget-trace.h"

static void usbssp_ep0_stall(struct usbssp_udc *usbssp_data)
{
	struct usbssp_ep *dep;
	int ret = 0;

	dep = &usbssp_data->devs.eps[0];
	if (usbssp_data->three_stage_setup) {
		usbssp_dbg(usbssp_data, "Send STALL on Data Stage\n");
		ret =  usbssp_halt_endpoint(usbssp_data, dep, true);

		/*
		 * Finishing SETUP transfer by removing request
		 * from pending list
		 */
		if (!list_empty(&dep->pending_list)) {
			struct usbssp_request	*req;

			req = next_request(&dep->pending_list);
			usbssp_giveback_request_in_irq(usbssp_data,
					req->td, -ECONNRESET);
			dep->ep_state = USBSSP_EP_ENABLED;
		}
	} else {
		usbssp_dbg(usbssp_data, "Send STALL on Status Stage\n");
		dep->ep_state |= EP0_HALTED_STATUS;
		usbssp_status_stage(usbssp_data);
	}
	usbssp_data->delayed_status = false;
}

static int usbssp_ep0_delegate_req(struct usbssp_udc *usbssp_data,
				   struct usb_ctrlrequest *ctrl)
{
	int ret;

	usbssp_dbg(usbssp_data, "Delagate request to gadget driver\n");
	spin_unlock(&usbssp_data->irq_thread_lock);

	ret = usbssp_data->gadget_driver->setup(&usbssp_data->gadget, ctrl);
	spin_lock(&usbssp_data->irq_thread_lock);

	return ret;
}

static int usbssp_ep0_set_config(struct usbssp_udc *usbssp_data,
				 struct usb_ctrlrequest *ctrl)
{
	enum usb_device_state state = usbssp_data->gadget.state;
	u32 cfg;
	int ret;

	cfg = le16_to_cpu(ctrl->wValue);
	switch (state) {
	case USB_STATE_DEFAULT:
		usbssp_err(usbssp_data,
			"Error: Set Config request from Default state\n");
		return -EINVAL;
	case USB_STATE_ADDRESS:
		usbssp_dbg(usbssp_data,
			"Set Configuration from addressed state\n");
		ret = usbssp_ep0_delegate_req(usbssp_data, ctrl);
		/* if the cfg matches and the cfg is non zero */
		if (cfg && (!ret || (ret == USB_GADGET_DELAYED_STATUS))) {
			/*
			 * only change state if set_config has already
			 * been processed. If gadget driver returns
			 * USB_GADGET_DELAYED_STATUS, we will wait
			 * to change the state on the next usbssp_enqueue()
			 */
			if (ret == 0) {
				usbssp_info(usbssp_data,
					"Device has been configured\n");
				usb_gadget_set_state(&usbssp_data->gadget,
					USB_STATE_CONFIGURED);
			}
		}
		break;
	case USB_STATE_CONFIGURED:
		usbssp_dbg(usbssp_data,
			"Set Configuration from Configured state\n");
		ret = usbssp_ep0_delegate_req(usbssp_data, ctrl);
		if (!cfg && !ret) {
			usbssp_info(usbssp_data, "reconfigured device\n");
			usb_gadget_set_state(&usbssp_data->gadget,
					USB_STATE_ADDRESS);
		}
		break;
	default:
		usbssp_err(usbssp_data,
			   "Set Configuration - incorrect device state\n");
		ret = -EINVAL;
	}
	return ret;
}

static int usbssp_ep0_set_address(struct usbssp_udc *usbssp_data,
				  struct usb_ctrlrequest *ctrl)
{
	enum usb_device_state state = usbssp_data->gadget.state;
	u32 addr;
	unsigned int slot_state;
	struct usbssp_slot_ctx *slot_ctx;
	int dev_state = 0;

	addr = le16_to_cpu(ctrl->wValue);
	if (addr > 127) {
		usbssp_err(usbssp_data, "invalid device address %d\n", addr);
		return -EINVAL;
	}

	slot_ctx = usbssp_get_slot_ctx(usbssp_data, usbssp_data->devs.out_ctx);
	dev_state = GET_SLOT_STATE(le32_to_cpu(slot_ctx->dev_state));

	if (state == USB_STATE_CONFIGURED) {
		usbssp_err(usbssp_data,
				"can't SetAddress() from Configured State\n");
		return -EINVAL;
	}

	usbssp_data->device_address = le16_to_cpu(ctrl->wValue);

	slot_ctx = usbssp_get_slot_ctx(usbssp_data, usbssp_data->devs.out_ctx);
	slot_state = GET_SLOT_STATE(le32_to_cpu(slot_ctx->dev_state));

	if (slot_state == SLOT_STATE_ADDRESSED) {
		/*Reset Device Command*/
		usbssp_data->defered_event &= ~EVENT_USB_RESET;
		queue_work(usbssp_data->bottom_irq_wq,
			&usbssp_data->bottom_irq);
		usbssp_reset_device(usbssp_data);
	}
	/*set device address*/
	usbssp_address_device(usbssp_data);

	if (addr)
		usb_gadget_set_state(&usbssp_data->gadget, USB_STATE_ADDRESS);
	else
		usb_gadget_set_state(&usbssp_data->gadget, USB_STATE_DEFAULT);
	return 0;
}

int usbssp_status_stage(struct usbssp_udc *usbssp_data)
{
	struct usbssp_ring *ep_ring;
	int ret;
	struct usbssp_ep *dep;

	dep = &usbssp_data->devs.eps[0];
	ep_ring = usbssp_data->devs.eps[0].ring;

	usbssp_dbg(usbssp_data, "Enqueue Status Stage\n");
	usbssp_data->ep0state = USBSSP_EP0_STATUS_PHASE;
	usbssp_data->usb_req_ep0_in.request.length = 0;
	ret = usbssp_enqueue(usbssp_data->usb_req_ep0_in.dep,
			&usbssp_data->usb_req_ep0_in);
	return ret;
}


static int usbssp_ep0_handle_feature_u1(struct usbssp_udc *usbssp_data,
					enum usb_device_state state, int set)
{
	__le32 __iomem *port_regs;
	u32 temp;

	if (state != USB_STATE_CONFIGURED)
		usbssp_err(usbssp_data,
			"Error: can't change U1 - incorrect device state\n");
		return -EINVAL;

	if ((usbssp_data->gadget.speed  != USB_SPEED_SUPER) &&
	    (usbssp_data->gadget.speed  != USB_SPEED_SUPER_PLUS))
		usbssp_err(usbssp_data,
			"Error: U1 is supported only for SS and SSP\n");
		return -EINVAL;

	port_regs = usbssp_get_port_io_addr(usbssp_data);

	temp = readl(port_regs+PORTPMSC);
	temp &= ~PORT_U1_TIMEOUT_MASK;

	if (set)
		temp |= PORT_U1_TIMEOUT(1);
	else
		temp |= PORT_U1_TIMEOUT(0);

	usbssp_info(usbssp_data, "U1 %s\n", set ? "enabled" : "disabled");
	writel(temp, port_regs+PORTPMSC);

	usbssp_status_stage(usbssp_data);
	return 0;
}

static int usbssp_ep0_handle_feature_u2(struct usbssp_udc *usbssp_data,
					enum usb_device_state state, int set)
{
	__le32 __iomem *port_regs;
	u32 temp;

	if (state != USB_STATE_CONFIGURED) {
		usbssp_err(usbssp_data,
			   "Error: can't change U2 - incorrect device state\n");
		return -EINVAL;
	}
	if ((usbssp_data->gadget.speed  != USB_SPEED_SUPER) &&
	    (usbssp_data->gadget.speed  != USB_SPEED_SUPER_PLUS)) {
		usbssp_err(usbssp_data,
			   "Error: U2 is supported only for SS and SSP\n");
		return -EINVAL;
	}

	port_regs = usbssp_get_port_io_addr(usbssp_data);
	temp = readl(port_regs+PORTPMSC);
	temp &= ~PORT_U1_TIMEOUT_MASK;

	if (set)
		temp |= PORT_U2_TIMEOUT(1);
	else
		temp |= PORT_U2_TIMEOUT(0);

	writel(temp, port_regs+PORTPMSC);
	usbssp_info(usbssp_data, "U2 %s\n", set ? "enabled" : "disabled");

	usbssp_status_stage(usbssp_data);
	return 0;
}

static int usbssp_ep0_handle_feature_test(struct usbssp_udc *usbssp_data,
					  enum usb_device_state state,
					  u32 wIndex, int set)
{
	int test_mode;
	__le32 __iomem *port_regs;
	u32 temp;
	unsigned long flags;
	int retval;

	if (usbssp_data->port_major_revision == 0x03)
		return -EINVAL;

	usbssp_info(usbssp_data, "Test mode; %d\n", wIndex);

	port_regs = usbssp_get_port_io_addr(usbssp_data);


	test_mode = (wIndex & 0xff00) >> 8;

	temp = readl(port_regs);
	temp = usbssp_port_state_to_neutral(temp);

	if (test_mode > TEST_FORCE_EN || test_mode < TEST_J) {
		/* "stall" on error */
		retval = -EPIPE;
	}

	usbssp_status_stage(usbssp_data);
	retval = usbssp_enter_test_mode(usbssp_data, test_mode, &flags);
	usbssp_exit_test_mode(usbssp_data);

	return 0;
}

static int usbssp_ep0_handle_feature_device(struct usbssp_udc *usbssp_data,
		struct usb_ctrlrequest *ctrl, int set)
{
	enum usb_device_state state;
	u32 wValue;
	u32 wIndex;
	int ret = 0;

	wValue = le16_to_cpu(ctrl->wValue);
	wIndex = le16_to_cpu(ctrl->wIndex);
	state = usbssp_data->gadget.state;

	switch (wValue) {
	case USB_DEVICE_REMOTE_WAKEUP:
		usbssp_data->remote_wakeup_allowed = (set) ? 1 : 0;
		break;
	/*
	 * 9.4.1 says only only for SS, in AddressState only for
	 * default control pipe
	 */
	case USB_DEVICE_U1_ENABLE:
		ret = usbssp_ep0_handle_feature_u1(usbssp_data, state, set);
		break;
	case USB_DEVICE_U2_ENABLE:
		ret = usbssp_ep0_handle_feature_u2(usbssp_data, state, set);
		break;
	case USB_DEVICE_LTM_ENABLE:
		ret = -EINVAL;
		break;
	case USB_DEVICE_TEST_MODE:
		ret = usbssp_ep0_handle_feature_test(usbssp_data, state,
				wIndex, set);
		break;
	default:
		usbssp_err(usbssp_data, "%s Feature Request not supported\n",
				(set) ? "Set" : "Clear");
		ret = -EINVAL;
	}

	return ret;
}

static int usbssp_ep0_handle_feature_intf(struct usbssp_udc *usbssp_data,
					  struct usb_ctrlrequest *ctrl,
					  int set)
{
	u32 wValue;
	int ret = 0;

	wValue = le16_to_cpu(ctrl->wValue);

	switch (wValue) {
	case USB_INTRF_FUNC_SUSPEND:
		/*TODO: suspend device */
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int usbssp_ep0_handle_feature_endpoint(struct usbssp_udc *usbssp_data,
		struct usb_ctrlrequest *ctrl, int set)
{
	struct usbssp_ep *dep;
	u32 wValue, wIndex;
	unsigned int ep_index = 0;
	struct usbssp_ring *ep_ring;
	struct usbssp_td *td;

	wValue = le16_to_cpu(ctrl->wValue);
	wIndex = le16_to_cpu(ctrl->wIndex);
	ep_index = ((wIndex & USB_ENDPOINT_NUMBER_MASK) << 1);

	if ((wIndex & USB_ENDPOINT_DIR_MASK) == USB_DIR_OUT)
		ep_index -= 1;

	dep =  &usbssp_data->devs.eps[ep_index];
	ep_ring = dep->ring;

	switch (wValue) {
	case USB_ENDPOINT_HALT:
		if (set == 0 && (dep->ep_state & USBSSP_EP_WEDGE))
			break;

		usbssp_halt_endpoint(usbssp_data, dep,  set);

		td = list_first_entry(&ep_ring->td_list, struct usbssp_td,
				td_list);

		usbssp_cleanup_halted_endpoint(usbssp_data, ep_index,
				ep_ring->stream_id, td,
				EP_HARD_RESET);
		break;
	default:
		usbssp_warn(usbssp_data, "WARN Incorrect wValue %04x\n",
				wValue);
		return -EINVAL;
	}
	return 0;
}

int usbssp_ep0_handle_feature(struct usbssp_udc *usbssp_data,
		struct usb_ctrlrequest *ctrl, int set)
{
	u32 recip;
	int ret;

	recip = ctrl->bRequestType & USB_RECIP_MASK;

	switch (recip) {
	case USB_RECIP_DEVICE:
		ret = usbssp_ep0_handle_feature_device(usbssp_data, ctrl, set);
		break;
	case USB_RECIP_INTERFACE:
		ret = usbssp_ep0_handle_feature_intf(usbssp_data, ctrl, set);
		break;
	case USB_RECIP_ENDPOINT:
		ret = usbssp_ep0_handle_feature_endpoint(usbssp_data,
				ctrl, set);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int usbssp_ep0_set_sel(struct usbssp_udc *usbssp_data,
			      struct usb_ctrlrequest *ctrl)
{
	struct usbssp_ep *dep;
	enum usb_device_state state = usbssp_data->gadget.state;
	u16 wLength;
	int ret = 0;

	if (state == USB_STATE_DEFAULT)
		return -EINVAL;

	wLength = le16_to_cpu(ctrl->wLength);

	if (wLength != 6) {
		usbssp_err(usbssp_data, "Set SEL should be 6 bytes, got %d\n",
				wLength);
		return -EINVAL;
	}

	/*
	 * To handle Set SEL we need to receive 6 bytes from Host. So let's
	 * queue a usb_request for 6 bytes.
	 */
	dep = &usbssp_data->devs.eps[0];

	usbssp_data->usb_req_ep0_in.request.length = 0x6;
	usbssp_data->usb_req_ep0_in.request.buf = usbssp_data->setup_buf;

	ret = usbssp_enqueue(usbssp_data->usb_req_ep0_in.dep,
			&usbssp_data->usb_req_ep0_in);
	if (ret) {
		usbssp_err(usbssp_data, "Error in  Set Sel\n");
		return ret;
	}
	return 0;
}

static int usbssp_ep0_std_request(struct usbssp_udc *usbssp_data,
				  struct usb_ctrlrequest *ctrl)
{
	int ret = 0;

	usbssp_data->bos_event_detected = 0;

	switch (ctrl->bRequest) {
	case USB_REQ_GET_STATUS:
		usbssp_info(usbssp_data, "Request GET_STATUS\n");
		/*TODO:*/
		//ret = usbssp_ep0_handle_status(usbssp_data, ctrl);
		break;
	case USB_REQ_CLEAR_FEATURE:
		usbssp_info(usbssp_data, "Request CLEAR_FEATURE\n");
		ret = usbssp_ep0_handle_feature(usbssp_data, ctrl, 0);
		break;
	case USB_REQ_SET_FEATURE:
		usbssp_info(usbssp_data, "Request SET_FEATURE\n");
		ret = usbssp_ep0_handle_feature(usbssp_data, ctrl, 1);
		break;
	case USB_REQ_SET_ADDRESS:
		usbssp_info(usbssp_data, "Request SET_ADDRESS\n");
		ret = usbssp_ep0_set_address(usbssp_data, ctrl);
		break;
	case USB_REQ_SET_CONFIGURATION:
		usbssp_info(usbssp_data, "Request SET_CONFIGURATION\n");
		ret = usbssp_ep0_set_config(usbssp_data, ctrl);
		break;
	case USB_REQ_SET_SEL:
		usbssp_info(usbssp_data, "Request SET_SEL\n");
		ret = usbssp_ep0_set_sel(usbssp_data, ctrl);
		break;
	case USB_REQ_SET_ISOCH_DELAY:
		usbssp_info(usbssp_data, "Request SET_ISOCH_DELAY\n");
		/*TODO:*/
		//ret = usbssp_ep0_set_isoch_delay(usbssp_data, ctrl);
		break;
	default:
		if ((le16_to_cpu(ctrl->wValue) >> 8) == USB_DT_BOS &&
		    ctrl->bRequest == USB_REQ_GET_DESCRIPTOR) {
			/*
			 * It will be handled after Status Stage phase
			 * in usbssp_gadget_giveback
			 */
			usbssp_data->bos_event_detected = true;
		}
		ret = usbssp_ep0_delegate_req(usbssp_data, ctrl);
		break;
	}
	return ret;
}

int usbssp_setup_analyze(struct usbssp_udc *usbssp_data)
{
	int ret = -EINVAL;
	struct usb_ctrlrequest *ctrl = &usbssp_data->setup;
	u32 len = 0;
	struct usbssp_device *priv_dev;

	ctrl = &usbssp_data->setup;

	usbssp_info(usbssp_data,
			"SETUP BRT: %02x BR: %02x V: %04x I: %04x L: %04x\n",
			ctrl->bRequestType, ctrl->bRequest,
			le16_to_cpu(ctrl->wValue), le16_to_cpu(ctrl->wIndex),
			le16_to_cpu(ctrl->wLength));

	if (!usbssp_data->gadget_driver)
		goto out;

	priv_dev = &usbssp_data->devs;

	/*
	 * First of all, if endpoint 0 was halted driver has to
	 * recovery it.
	 */
	if (priv_dev->eps[0].ep_state & EP_HALTED) {
		usbssp_dbg(usbssp_data,
			"Ep0 Halted - restoring to nomral state\n");
		usbssp_halt_endpoint(usbssp_data, &priv_dev->eps[0], 0);
	}

	/*
	 * Finishing previous SETUP transfer by removing request from
	 * list and informing upper layer
	 */
	if (!list_empty(&priv_dev->eps[0].pending_list)) {
		struct usbssp_request	*req;

		usbssp_dbg(usbssp_data,
				"Deleting previous Setup transaction\n");
		req = next_request(&priv_dev->eps[0].pending_list);
		usbssp_dequeue(&priv_dev->eps[0], req);
	}

	len = le16_to_cpu(ctrl->wLength);
	if (!len) {
		usbssp_data->three_stage_setup = false;
		usbssp_data->ep0_expect_in = false;
	} else {
		usbssp_data->three_stage_setup = true;
		usbssp_data->ep0_expect_in =
				!!(ctrl->bRequestType & USB_DIR_IN);
	}

	if ((ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD)
		ret = usbssp_ep0_std_request(usbssp_data, ctrl);
	else
		ret = usbssp_ep0_delegate_req(usbssp_data, ctrl);

	if (ret == USB_GADGET_DELAYED_STATUS) {
		usbssp_dbg(usbssp_data, "Status Stage delayed\n");
		usbssp_data->delayed_status = true;
	}

out:
	if (ret < 0)
		usbssp_ep0_stall(usbssp_data);

	return ret;
}
