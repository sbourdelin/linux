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

#include <linux/dma-mapping.h>
#include <linux/usb/gadget.h>

#include "core.h"
#include "gadget-export.h"
#include "gadget.h"

/**
 * cdns3_handshake - spin reading  until handshake completes or fails
 * @ptr: address of device controller register to be read
 * @mask: bits to look at in result of read
 * @done: value of those bits when handshake succeeds
 * @usec: timeout in microseconds
 *
 * Returns negative errno, or zero on success
 *
 * Success happens when the "mask" bits have the specified value (hardware
 * handshake done). There are two failure modes: "usec" have passed (major
 * hardware flakeout), or the register reads as all-ones (hardware removed).
 */
int cdns3_handshake(void __iomem *ptr, u32 mask, u32 done, int usec)
{
	u32	result;

	do {
		result = readl(ptr);
		if (result == ~(u32)0)	/* card removed */
			return -ENODEV;
		result &= mask;
		if (result == done)
			return 0;
		udelay(1);
		usec--;
	} while (usec > 0);
	return -ETIMEDOUT;
}

/**
 * cdns3_set_register_bit - set bit in given register.
 * @ptr: address of device controller register to be read and changed
 * @mask: bits requested to set
 */
void cdns3_set_register_bit(void __iomem *ptr, u32 mask)
{
	mask = readl(ptr) | mask;
	writel(mask, ptr);
}

/**
 * cdns3_ep_reg_pos_to_index - Macro converts bit position of ep_ists register
 * to index of endpoint object in cdns3_device.eps[] container
 * @i: bit position of endpoint for which endpoint object is required
 *
 * Remember that endpoint container doesn't contain default endpoint
 */
static u8 cdns3_ep_reg_pos_to_index(int i)
{
	return ((i / 16) + (((i % 16) - 2) * 2));
}

/**
 * cdns3_next_request - returns next request from list
 * @list: list containing requests
 *
 * Returns request or NULL if no requests in list
 */
struct usb_request *cdns3_next_request(struct list_head *list)
{
	if (list_empty(list))
		return NULL;
	return list_first_entry(list, struct usb_request, list);
}

/**
 * select_ep - selects endpoint
 * @priv_dev:  extended gadget object
 * @ep: endpoint address
 */
void cdns3_select_ep(struct cdns3_device *priv_dev, u32 ep)
{
	if (priv_dev->selected_ep == ep)
		return;

	dev_dbg(&priv_dev->dev, "Ep sel: 0x%02x\n", ep);
	priv_dev->selected_ep = ep;
	writel(ep, &priv_dev->regs->ep_sel);
	/* memory barrier for selecting endpoint. */
	wmb();
}

/**
 * cdns3_allocate_trb_pool - Allocates TRB's pool for selected endpoint
 * @priv_ep:  endpoint object
 *
 * Function will return 0 on success or -ENOMEM on allocation error
 */
static int cdns3_allocate_trb_pool(struct cdns3_endpoint *priv_ep)
{
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	struct cdns3_trb *link_trb;

	priv_ep->trb_pool = dma_zalloc_coherent(priv_dev->sysdev,
						TRB_SIZE * TRBS_PER_SEGMENT,
						&priv_ep->trb_pool_dma,
						GFP_DMA);
	if (!priv_ep->trb_pool)
		return -ENOMEM;

	priv_ep->aligned_buff = dma_alloc_coherent(priv_dev->sysdev,
						   CDNS3_UNALIGNED_BUF_SIZE,
						   &priv_ep->aligned_dma_addr,
						   GFP_DMA);
	if (!priv_ep->aligned_buff) {
		dma_free_coherent(priv_dev->sysdev,
				  TRB_SIZE * TRBS_PER_SEGMENT,
				  priv_ep->trb_pool, priv_ep->trb_pool_dma);
		priv_ep->trb_pool = NULL;

		return -ENOMEM;
	}

	/* Initialize the last TRB as Link TRB */
	link_trb = (priv_ep->trb_pool + TRBS_PER_SEGMENT - 1);
	link_trb->buffer = TRB_BUFFER(priv_ep->trb_pool_dma);
	link_trb->control = TRB_CYCLE | TRB_TYPE(TRB_LINK) |
			    TRB_CHAIN | TRB_TOGGLE;

	return 0;
}

static void cdns3_free_trb_pool(struct cdns3_endpoint *priv_ep)
{
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;

	dma_free_coherent(priv_dev->sysdev,
			  TRB_SIZE * TRBS_PER_SEGMENT,
			  priv_ep->trb_pool, priv_ep->trb_pool_dma);
	priv_ep->trb_pool = NULL;

	dma_free_coherent(priv_dev->sysdev, CDNS3_UNALIGNED_BUF_SIZE,
			  priv_ep->aligned_buff, priv_ep->aligned_dma_addr);
	priv_ep->aligned_buff = NULL;
}

/**
 * cdns3_data_flush - flush data at onchip buffer
 * @priv_ep: endpoint object
 *
 * Endpoint must be selected before call to this function
 *
 * Returns zero on success or negative value on failure
 */
static int cdns3_data_flush(struct cdns3_endpoint *priv_ep)
{
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;

	writel(EP_CMD_DFLUSH, &priv_dev->regs->ep_cmd);

	/* wait for DFLUSH cleared */
	return cdns3_handshake(&priv_dev->regs->ep_cmd, EP_CMD_DFLUSH, 0, 100);
}

/**
 * cdns3_ep_stall_flush - Stalls and flushes selected endpoint
 * @priv_ep: endpoint object
 *
 * Endpoint must be selected before call to this function
 */
static void cdns3_ep_stall_flush(struct cdns3_endpoint *priv_ep)
{
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;

	writel(EP_CMD_DFLUSH | EP_CMD_ERDY | EP_CMD_SSTALL,
	       &priv_dev->regs->ep_cmd);

	/* wait for DFLUSH cleared */
	cdns3_handshake(&priv_dev->regs->ep_cmd, EP_CMD_DFLUSH, 0, 100);
	priv_ep->flags |= EP_STALL;
}

/**
 * cdns3_gadget_unconfig - reset device configuration
 * @priv_dev: extended gadget object
 */
void cdns3_gadget_unconfig(struct cdns3_device *priv_dev)
{
	/* RESET CONFIGURATION */
	writel(USB_CONF_CFGRST, &priv_dev->regs->usb_conf);

	cdns3_enable_l1(priv_dev, 0);
	priv_dev->hw_configured_flag = 0;
	priv_dev->onchip_mem_allocated_size = 0;
	priv_dev->out_mem_is_allocated = 0;
}

void cdns3_enable_l1(struct cdns3_device *priv_dev, int enable)
{
	if (enable)
		writel(USB_CONF_L1EN, &priv_dev->regs->usb_conf);
	else
		writel(USB_CONF_L1DS, &priv_dev->regs->usb_conf);
}

static enum usb_device_speed cdns3_get_speed(struct cdns3_device *priv_dev)
{
	u32 reg;

	reg = readl(&priv_dev->regs->usb_sts);

	if (DEV_SUPERSPEED(reg))
		return USB_SPEED_SUPER;
	else if (DEV_HIGHSPEED(reg))
		return USB_SPEED_HIGH;
	else if (DEV_FULLSPEED(reg))
		return USB_SPEED_FULL;
	else if (DEV_LOWSPEED(reg))
		return USB_SPEED_LOW;
	return USB_SPEED_UNKNOWN;
}

/**
 * cdns3_gadget_giveback - call struct usb_request's ->complete callback
 * @priv_ep: The endpoint to whom the request belongs to
 * @priv_req: The request we're giving back
 * @status: completion code for the request
 *
 * Must be called with controller's lock held and interrupts disabled. This
 * function will unmap @req and call its ->complete() callback to notify upper
 * layers that it has completed.
 */
void cdns3_gadget_giveback(struct cdns3_endpoint *priv_ep,
			   struct cdns3_request *priv_req,
			   int status)
{
	//TODO: Implements this function.
}

/**
 * cdns3_ep_run_transfer - start transfer on no-default endpoint hardware
 * @priv_ep: endpoint object
 *
 * Returns zero on success or negative value on failure
 */
int cdns3_ep_run_transfer(struct cdns3_endpoint *priv_ep,
			  struct usb_request *request)
{
	return 0;
}

static void cdns3_transfer_completed(struct cdns3_device *priv_dev,
				     struct cdns3_endpoint *priv_ep)
{
	//TODO: Implements this function.
}

/**
 * cdns3_check_ep_interrupt_proceed - Processes interrupt related to endpoint
 * @priv_ep: endpoint object
 *
 * Returns 0
 */
static int cdns3_check_ep_interrupt_proceed(struct cdns3_endpoint *priv_ep)
{
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	struct cdns3_usb_regs __iomem *regs;
	u32 ep_sts_reg;

	regs = priv_dev->regs;

	cdns3_select_ep(priv_dev, priv_ep->endpoint.address);
	ep_sts_reg = readl(&regs->ep_sts);

	if (ep_sts_reg & EP_STS_TRBERR)
		writel(EP_STS_TRBERR, &regs->ep_sts);

	if (ep_sts_reg & EP_STS_ISOERR)
		writel(EP_STS_ISOERR, &regs->ep_sts);

	if (ep_sts_reg & EP_STS_OUTSMM)
		writel(EP_STS_OUTSMM, &regs->ep_sts);

	if (ep_sts_reg & EP_STS_NRDY)
		writel(EP_STS_NRDY, &regs->ep_sts);

	if ((ep_sts_reg & EP_STS_IOC) || (ep_sts_reg & EP_STS_ISP)) {
		writel(EP_STS_IOC | EP_STS_ISP, &regs->ep_sts);
		cdns3_transfer_completed(priv_dev, priv_ep);
	}

	if (ep_sts_reg & EP_STS_DESCMIS)
		writel(EP_STS_DESCMIS, &regs->ep_sts);

	return 0;
}

/**
 * cdns3_check_usb_interrupt_proceed - Processes interrupt related to device
 * @priv_dev: extended gadget object
 * @usb_ists: bitmap representation of device's reported interrupts
 * (usb_ists register value)
 */
static void cdns3_check_usb_interrupt_proceed(struct cdns3_device *priv_dev,
					      u32 usb_ists)
{
	struct cdns3_usb_regs __iomem *regs;
	int speed = 0;

	regs = priv_dev->regs;

	/* Connection detected */
	if (usb_ists & (USB_ISTS_CON2I | USB_ISTS_CONI)) {
		writel(USB_ISTS_CON2I | USB_ISTS_CONI, &regs->usb_ists);
		speed = cdns3_get_speed(priv_dev);

		dev_dbg(&priv_dev->dev, "Connection detected at speed: %s %d\n",
			usb_speed_string(speed), speed);

		priv_dev->gadget.speed = speed;
		priv_dev->is_connected = 1;
		usb_gadget_set_state(&priv_dev->gadget, USB_STATE_POWERED);
		cdns3_ep0_config(priv_dev);
	}

	/* SS Disconnection detected */
	if (usb_ists & (USB_ISTS_DIS2I | USB_ISTS_DISI)) {
		dev_dbg(&priv_dev->dev, "Disconnection detected\n");

		writel(USB_ISTS_DIS2I | USB_ISTS_DISI, &regs->usb_ists);
		if (priv_dev->gadget_driver &&
		    priv_dev->gadget_driver->disconnect) {
			spin_unlock(&priv_dev->lock);
			priv_dev->gadget_driver->disconnect(&priv_dev->gadget);
			spin_lock(&priv_dev->lock);
		}
		priv_dev->gadget.speed = USB_SPEED_UNKNOWN;
		usb_gadget_set_state(&priv_dev->gadget, USB_STATE_NOTATTACHED);
		priv_dev->is_connected = 0;
		cdns3_gadget_unconfig(priv_dev);
	}

	if (usb_ists & USB_ISTS_L2ENTI) {
		dev_dbg(&priv_dev->dev, "Device suspended\n");
		writel(USB_ISTS_L2ENTI, &regs->usb_ists);
	}

	/* Exit from standby mode on L2 exit (Suspend in HS/FS or SS) */
	if (usb_ists & USB_ISTS_L2EXTI) {
		dev_dbg(&priv_dev->dev, "[Interrupt] L2 exit detected\n");
		writel(USB_ISTS_L2EXTI, &regs->usb_ists);
	}

	/* Exit from standby mode on U3 exit (Suspend in HS/FS or SS). */
	if (usb_ists & USB_ISTS_U3EXTI) {
		dev_dbg(&priv_dev->dev, "U3 exit detected\n");
		writel(USB_ISTS_U3EXTI, &regs->usb_ists);
	}

	/* resets cases */
	if (usb_ists & (USB_ISTS_UWRESI | USB_ISTS_UHRESI | USB_ISTS_U2RESI)) {
		writel(USB_ISTS_U2RESI | USB_ISTS_UWRESI | USB_ISTS_UHRESI,
		       &regs->usb_ists);

		/*read again to check the actuall speed*/
		speed = cdns3_get_speed(priv_dev);

		dev_dbg(&priv_dev->dev, "Reset detected at speed: %s %d\n",
			usb_speed_string(speed), speed);

		usb_gadget_set_state(&priv_dev->gadget, USB_STATE_DEFAULT);
		priv_dev->gadget.speed = speed;
		cdns3_gadget_unconfig(priv_dev);
		cdns3_ep0_config(priv_dev);
	}
}

/**
 * cdns3_irq_handler - irq line interrupt handler
 * @cdns: cdns3 instance
 *
 * Returns IRQ_HANDLED when interrupt raised by USBSS_DEV,
 * IRQ_NONE when interrupt raised by other device connected
 * to the irq line
 */
static irqreturn_t cdns3_irq_handler_thread(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev;
	irqreturn_t ret = IRQ_NONE;
	unsigned long flags;
	u32 reg;

	priv_dev = container_of(cdns->gadget_dev, struct cdns3_device, dev);
	spin_lock_irqsave(&priv_dev->lock, flags);

	/* check USB device interrupt */
	reg = readl(&priv_dev->regs->usb_ists);
	if (reg) {
		dev_dbg(&priv_dev->dev, "IRQ: usb_ists: %08X\n", reg);
		cdns3_check_usb_interrupt_proceed(priv_dev, reg);
		ret = IRQ_HANDLED;
	}

	/* check endpoint interrupt */
	reg = readl(&priv_dev->regs->ep_ists);
	if (reg != 0) {
		dev_dbg(&priv_dev->dev, "IRQ ep_ists: %08X\n", reg);
	} else {
		if (USB_STS_CFGSTS(readl(&priv_dev->regs->usb_sts)))
			ret = IRQ_HANDLED;
		goto irqend;
	}

	/* handle default endpoint OUT */
	if (reg & EP_ISTS_EP_OUT0) {
		cdns3_check_ep0_interrupt_proceed(priv_dev, 0);
		ret = IRQ_HANDLED;
	}

	/* handle default endpoint IN */
	if (reg & EP_ISTS_EP_IN0) {
		cdns3_check_ep0_interrupt_proceed(priv_dev, 1);
		ret = IRQ_HANDLED;
	}

	/* check if interrupt from non default endpoint, if no exit */
	reg &= ~(EP_ISTS_EP_OUT0 | EP_ISTS_EP_IN0);
	if (!reg)
		goto irqend;

	do {
		unsigned int bit_pos = ffs(reg);
		u32 bit_mask = 1 << (bit_pos - 1);
		int index;

		index = cdns3_ep_reg_pos_to_index(bit_pos);
		cdns3_check_ep_interrupt_proceed(priv_dev->eps[index]);
		reg &= ~bit_mask;
		ret = IRQ_HANDLED;
	} while (reg);

irqend:
	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return ret;
}

/* Find correct direction for HW endpoint according to description */
static int cdns3_ep_dir_is_correct(struct usb_endpoint_descriptor *desc,
				   struct cdns3_endpoint *priv_ep)
{
	return (priv_ep->endpoint.caps.dir_in && usb_endpoint_dir_in(desc)) ||
	       (priv_ep->endpoint.caps.dir_out && usb_endpoint_dir_out(desc));
}

static struct cdns3_endpoint *cdns3_find_available_ss_ep(struct cdns3_device *priv_dev,
							 struct usb_endpoint_descriptor *desc)
{
	struct usb_ep *ep;
	struct cdns3_endpoint *priv_ep;

	list_for_each_entry(ep, &priv_dev->gadget.ep_list, ep_list) {
		unsigned long num;
		int ret;
		/* ep name pattern likes epXin or epXout */
		char c[2] = {ep->name[2], '\0'};

		ret = kstrtoul(c, 10, &num);
		if (ret)
			return ERR_PTR(ret);

		priv_ep = ep_to_cdns3_ep(ep);
		if (cdns3_ep_dir_is_correct(desc, priv_ep)) {
			if (!(priv_ep->flags & EP_USED)) {
				priv_ep->num  = num;
				priv_ep->flags |= EP_USED;
				return priv_ep;
			}
		}
	}
	return ERR_PTR(-ENOENT);
}

static struct usb_ep *cdns3_gadget_match_ep(struct usb_gadget *gadget,
					    struct usb_endpoint_descriptor *desc,
					    struct usb_ss_ep_comp_descriptor *comp_desc)
{
	struct cdns3_device *priv_dev = gadget_to_cdns3_device(gadget);
	struct cdns3_endpoint *priv_ep;
	unsigned long flags;

	priv_ep = cdns3_find_available_ss_ep(priv_dev, desc);
	if (IS_ERR(priv_ep)) {
		dev_err(&priv_dev->dev, "no available ep\n");
		return NULL;
	}

	dev_dbg(&priv_dev->dev, "match endpoint: %s\n", priv_ep->name);

	spin_lock_irqsave(&priv_dev->lock, flags);
	priv_ep->endpoint.desc = desc;
	priv_ep->dir  = usb_endpoint_dir_in(desc) ? USB_DIR_IN : USB_DIR_OUT;
	priv_ep->type = usb_endpoint_type(desc);

	list_add_tail(&priv_ep->ep_match_pending_list,
		      &priv_dev->ep_match_list);
	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return &priv_ep->endpoint;
}

/**
 * cdns3_gadget_ep_enable Enable endpoint
 * @ep: endpoint object
 * @desc: endpoint descriptor
 *
 * Returns 0 on success, error code elsewhere
 */
static int cdns3_gadget_ep_enable(struct usb_ep *ep,
				  const struct usb_endpoint_descriptor *desc)
{
	struct cdns3_endpoint *priv_ep;
	struct cdns3_device *priv_dev;
	unsigned long flags;
	int ret;

	priv_ep = ep_to_cdns3_ep(ep);
	priv_dev = priv_ep->cdns3_dev;

	if (!ep || !desc || desc->bDescriptorType != USB_DT_ENDPOINT) {
		dev_err(&priv_dev->dev, "usbss: invalid parameters\n");
		return -EINVAL;
	}

	if (!desc->wMaxPacketSize) {
		dev_err(&priv_dev->dev, "usbss: missing wMaxPacketSize\n");
		return -EINVAL;
	}

	if (dev_WARN_ONCE(&priv_dev->dev, priv_ep->flags & EP_ENABLED,
			  "%s is already enabled\n", priv_ep->name))
		return 0;

	ret = cdns3_allocate_trb_pool(priv_ep);
	if (ret)
		return ret;

	dev_dbg(&priv_dev->dev, "Enabling endpoint: %s\n", ep->name);
	spin_lock_irqsave(&priv_dev->lock, flags);
	cdns3_select_ep(priv_dev, desc->bEndpointAddress);
	writel(EP_CMD_EPRST, &priv_dev->regs->ep_cmd);

	ret = cdns3_handshake(&priv_dev->regs->ep_cmd,
			      EP_CMD_EPRST, 0, 100);

	cdns3_set_register_bit(&priv_dev->regs->ep_cfg, EP_CFG_ENABLE);

	ep->desc = desc;
	priv_ep->flags &= ~(EP_PENDING_REQUEST | EP_STALL);
	priv_ep->flags |= EP_ENABLED | EP_UPDATE_EP_TRBADDR;
	priv_ep->enqueue = 0;
	priv_ep->dequeue = 0;
	priv_ep->pcs = 1;
	priv_ep->ccs = 1;
	/* one TRB is reserved for link TRB used in DMULT mode*/
	priv_ep->free_trbs = TRBS_PER_SEGMENT - 1;

	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return 0;
}

/**
 * cdns3_gadget_ep_disable Disable endpoint
 * @ep: endpoint object
 *
 * Returns 0 on success, error code elsewhere
 */
static int cdns3_gadget_ep_disable(struct usb_ep *ep)
{
	struct cdns3_endpoint *priv_ep;
	struct cdns3_device *priv_dev;
	unsigned long flags;
	int ret = 0;
	struct usb_request *request;
	u32 ep_cfg;

	if (!ep) {
		pr_debug("usbss: invalid parameters\n");
		return -EINVAL;
	}

	priv_ep = ep_to_cdns3_ep(ep);
	priv_dev = priv_ep->cdns3_dev;

	if (dev_WARN_ONCE(&priv_dev->dev, !(priv_ep->flags & EP_ENABLED),
			  "%s is already disabled\n", priv_ep->name))
		return 0;

	spin_lock_irqsave(&priv_dev->lock, flags);
	if (!priv_dev->start_gadget) {
		dev_dbg(&priv_dev->dev,
			"Disabling endpoint at disconnection: %s\n", ep->name);
		spin_unlock_irqrestore(&priv_dev->lock, flags);
		return 0;
	}

	dev_dbg(&priv_dev->dev, "Disabling endpoint: %s\n", ep->name);

	cdns3_select_ep(priv_dev, ep->desc->bEndpointAddress);
	ret = cdns3_data_flush(priv_ep);
	while (!list_empty(&priv_ep->request_list)) {
		request = cdns3_next_request(&priv_ep->request_list);

		cdns3_gadget_giveback(priv_ep, to_cdns3_request(request),
				      -ESHUTDOWN);
	}

	ep_cfg = readl(&priv_dev->regs->ep_cfg);
	ep_cfg &= ~EP_CFG_ENABLE;
	writel(ep_cfg, &priv_dev->regs->ep_cfg);
	ep->desc = NULL;
	priv_ep->flags &= ~EP_ENABLED;

	spin_unlock_irqrestore(&priv_dev->lock, flags);

	return ret;
}

/**
 * cdns3_gadget_ep_alloc_request Allocates request
 * @ep: endpoint object associated with request
 * @gfp_flags: gfp flags
 *
 * Returns allocated request address, NULL on allocation error
 */
struct usb_request *cdns3_gadget_ep_alloc_request(struct usb_ep *ep,
						  gfp_t gfp_flags)
{
	struct cdns3_request *priv_req;

	priv_req = kzalloc(sizeof(*priv_req), gfp_flags);
	if (!priv_req)
		return NULL;

	return &priv_req->request;
}

/**
 * cdns3_gadget_ep_free_request Free memory occupied by request
 * @ep: endpoint object associated with request
 * @request: request to free memory
 */
void cdns3_gadget_ep_free_request(struct usb_ep *ep,
				  struct usb_request *request)
{
	struct cdns3_request *priv_req = to_cdns3_request(request);

	kfree(priv_req);
}

/**
 * cdns3_gadget_ep_queue Transfer data on endpoint
 * @ep: endpoint object
 * @request: request object
 * @gfp_flags: gfp flags
 *
 * Returns 0 on success, error code elsewhere
 */
static int __cdns3_gadget_ep_queue(struct usb_ep *ep,
				   struct usb_request *request,
				   gfp_t gfp_flags)
{
	struct cdns3_endpoint *priv_ep = ep_to_cdns3_ep(ep);
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	int ret = 0;

	request->actual = 0;
	request->status = -EINPROGRESS;

	dev_dbg(&priv_dev->dev, "Queuing to endpoint: %s\n", priv_ep->name);

	ret = usb_gadget_map_request_by_dev(priv_dev->sysdev, request,
					    usb_endpoint_dir_in(ep->desc));

	if (ret)
		return ret;

	if (!cdns3_ep_run_transfer(priv_ep, request))
		list_add_tail(&request->list, &priv_ep->request_list);

	return ret;
}

static int cdns3_gadget_ep_queue(struct usb_ep *ep, struct usb_request *request,
				 gfp_t gfp_flags)
{
	struct cdns3_endpoint *priv_ep = ep_to_cdns3_ep(ep);
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	struct usb_request *zlp_request;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&priv_dev->lock, flags);
	ret = __cdns3_gadget_ep_queue(ep, request, gfp_flags);

	if (ret == 0 && request->zero && request->length &&
	    (request->length % ep->maxpacket == 0)) {
		zlp_request = cdns3_gadget_ep_alloc_request(ep, GFP_ATOMIC);
		zlp_request->buf = priv_dev->zlp_buf;
		zlp_request->length = 0;

		dev_dbg(&priv_dev->dev, "Queuing ZLP for endpoint: %s\n",
			priv_ep->name);
		ret = __cdns3_gadget_ep_queue(ep, zlp_request, gfp_flags);
	}

	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return ret;
}

/**
 * cdns3_gadget_ep_dequeue Remove request from transfer queue
 * @ep: endpoint object associated with request
 * @request: request object
 *
 * Returns 0 on success, error code elsewhere
 */
int cdns3_gadget_ep_dequeue(struct usb_ep *ep,
			    struct usb_request *request)
{
	struct cdns3_endpoint *priv_ep = ep_to_cdns3_ep(ep);
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	struct usb_request *req, *req_temp;
	unsigned long flags;
	int ret = 0;

	if (!ep || !request || !ep->desc)
		return -EINVAL;

	spin_lock_irqsave(&priv_dev->lock, flags);
	dev_dbg(&priv_dev->dev, "Dequeue from %s\n", ep->name);

	cdns3_select_ep(priv_dev, ep->desc->bEndpointAddress);
	if (priv_dev->start_gadget)
		ret = cdns3_data_flush(priv_ep);

	list_for_each_entry_safe(req, req_temp, &priv_ep->request_list, list) {
		if (request == req) {
			cdns3_gadget_giveback(priv_ep,
					      to_cdns3_request(request),
					      -ECONNRESET);
			break;
		}
	}

	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return ret;
}

/**
 * cdns3_gadget_ep_set_halt Sets/clears stall on selected endpoint
 * @ep: endpoint object to set/clear stall on
 * @value: 1 for set stall, 0 for clear stall
 *
 * Returns 0 on success, error code elsewhere
 */
int cdns3_gadget_ep_set_halt(struct usb_ep *ep, int value)
{
	struct cdns3_endpoint *priv_ep = ep_to_cdns3_ep(ep);
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	unsigned long flags;
	int ret = 0;

	if (!(priv_ep->flags & EP_ENABLED))
		return -EPERM;

	/* if actual transfer is pending defer setting stall on this endpoint */
	if ((priv_ep->flags & EP_PENDING_REQUEST) && value) {
		priv_ep->flags |= EP_STALL;
		return 0;
	}

	dev_dbg(&priv_dev->dev, "Halt endpoint %s\n", priv_ep->name);

	spin_lock_irqsave(&priv_dev->lock, flags);

	cdns3_select_ep(priv_dev, ep->desc->bEndpointAddress);
	if (value) {
		cdns3_ep_stall_flush(priv_ep);
	} else {
		priv_ep->flags &= ~EP_WEDGE;
		writel(EP_CMD_CSTALL | EP_CMD_EPRST, &priv_dev->regs->ep_cmd);

		/* wait for EPRST cleared */
		ret = cdns3_handshake(&priv_dev->regs->ep_cmd,
				      EP_CMD_EPRST, 0, 100);
		priv_ep->flags &= ~EP_STALL;
	}

	priv_ep->flags &= ~EP_PENDING_REQUEST;
	spin_unlock_irqrestore(&priv_dev->lock, flags);

	return ret;
}

extern const struct usb_ep_ops cdns3_gadget_ep0_ops;

static const struct usb_ep_ops cdns3_gadget_ep_ops = {
	.enable = cdns3_gadget_ep_enable,
	.disable = cdns3_gadget_ep_disable,
	.alloc_request = cdns3_gadget_ep_alloc_request,
	.free_request = cdns3_gadget_ep_free_request,
	.queue = cdns3_gadget_ep_queue,
	.dequeue = cdns3_gadget_ep_dequeue,
	.set_halt = cdns3_gadget_ep_set_halt,
	.set_wedge = cdns3_gadget_ep_set_wedge,
};

/**
 * cdns3_gadget_get_frame Returns number of actual ITP frame
 * @gadget: gadget object
 *
 * Returns number of actual ITP frame
 */
static int cdns3_gadget_get_frame(struct usb_gadget *gadget)
{
	struct cdns3_device *priv_dev = gadget_to_cdns3_device(gadget);

	return readl(&priv_dev->regs->usb_iptn);
}

static int cdns3_gadget_wakeup(struct usb_gadget *gadget)
{
	return 0;
}

static int cdns3_gadget_set_selfpowered(struct usb_gadget *gadget,
					int is_selfpowered)
{
	struct cdns3_device *priv_dev = gadget_to_cdns3_device(gadget);
	unsigned long flags;

	spin_lock_irqsave(&priv_dev->lock, flags);
	gadget->is_selfpowered = !!is_selfpowered;
	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return 0;
}

static int cdns3_gadget_pullup(struct usb_gadget *gadget, int is_on)
{
	struct cdns3_device *priv_dev = gadget_to_cdns3_device(gadget);

	if (!priv_dev->start_gadget)
		return 0;

	if (is_on)
		writel(USB_CONF_DEVEN, &priv_dev->regs->usb_conf);
	else
		writel(USB_CONF_DEVDS, &priv_dev->regs->usb_conf);

	return 0;
}

static void cdns3_gadget_config(struct cdns3_device *priv_dev)
{
	struct cdns3_usb_regs __iomem *regs = priv_dev->regs;

	cdns3_ep0_config(priv_dev);

	/* enable interrupts for endpoint 0 (in and out) */
	writel(EP_IEN_EP_OUT0 | EP_IEN_EP_IN0, &regs->ep_ien);

	/* enable generic interrupt*/
	writel(USB_IEN_INIT, &regs->usb_ien);
	writel(USB_CONF_CLK2OFFDS | USB_CONF_L1DS, &regs->usb_conf);
	writel(USB_CONF_U1DS | USB_CONF_U2DS, &regs->usb_conf);
	writel(USB_CONF_DMULT, &regs->usb_conf);
	writel(USB_CONF_DEVEN, &regs->usb_conf);
}

/**
 * cdns3_gadget_udc_start Gadget start
 * @gadget: gadget object
 * @driver: driver which operates on this gadget
 *
 * Returns 0 on success, error code elsewhere
 */
static int cdns3_gadget_udc_start(struct usb_gadget *gadget,
				  struct usb_gadget_driver *driver)
{
	struct cdns3_device *priv_dev = gadget_to_cdns3_device(gadget);
	unsigned long flags;

	if (priv_dev->gadget_driver) {
		dev_err(&priv_dev->dev, "%s is already bound to %s\n",
			priv_dev->gadget.name,
			priv_dev->gadget_driver->driver.name);
		return -EBUSY;
	}

	spin_lock_irqsave(&priv_dev->lock, flags);
	priv_dev->gadget_driver = driver;
	if (!priv_dev->start_gadget)
		goto unlock;

	cdns3_gadget_config(priv_dev);
unlock:
	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return 0;
}

/**
 * cdns3_gadget_udc_stop Stops gadget
 * @gadget: gadget object
 *
 * Returns 0
 */
static int cdns3_gadget_udc_stop(struct usb_gadget *gadget)
{
	struct cdns3_device *priv_dev = gadget_to_cdns3_device(gadget);
	struct cdns3_endpoint *priv_ep, *temp_ep;
	u32 bEndpointAddress;
	struct usb_ep *ep;
	int ret = 0;
	int i;

	priv_dev->gadget_driver = NULL;
	list_for_each_entry_safe(priv_ep, temp_ep, &priv_dev->ep_match_list,
				 ep_match_pending_list) {
		list_del(&priv_ep->ep_match_pending_list);
		priv_ep->flags &= ~EP_USED;
	}

	priv_dev->onchip_mem_allocated_size = 0;
	priv_dev->out_mem_is_allocated = 0;
	priv_dev->gadget.speed = USB_SPEED_UNKNOWN;

	for (i = 0; i < priv_dev->ep_nums ; i++)
		cdns3_free_trb_pool(priv_dev->eps[i]);

	if (!priv_dev->start_gadget)
		return 0;

	list_for_each_entry(ep, &priv_dev->gadget.ep_list, ep_list) {
		priv_ep = ep_to_cdns3_ep(ep);
		bEndpointAddress = priv_ep->num | priv_ep->dir;
		cdns3_select_ep(priv_dev, bEndpointAddress);
		writel(EP_CMD_EPRST, &priv_dev->regs->ep_cmd);
		ret = cdns3_handshake(&priv_dev->regs->ep_cmd,
				      EP_CMD_EPRST, 0, 100);
	}

	/* disable interrupt for device */
	writel(0, &priv_dev->regs->usb_ien);
	writel(USB_CONF_DEVDS, &priv_dev->regs->usb_conf);

	return ret;
}

static const struct usb_gadget_ops cdns3_gadget_ops = {
	.get_frame = cdns3_gadget_get_frame,
	.wakeup = cdns3_gadget_wakeup,
	.set_selfpowered = cdns3_gadget_set_selfpowered,
	.pullup = cdns3_gadget_pullup,
	.udc_start = cdns3_gadget_udc_start,
	.udc_stop = cdns3_gadget_udc_stop,
	.match_ep = cdns3_gadget_match_ep,
};

/**
 * cdns3_init_ep Initializes software endpoints of gadget
 * @cdns3: extended gadget object
 *
 * Returns 0 on success, error code elsewhere
 */
static int cdns3_init_ep(struct cdns3_device *priv_dev)
{
	u32 ep_enabled_reg, iso_ep_reg;
	struct cdns3_endpoint *priv_ep;
	int found_endpoints = 0;
	int ep_dir, ep_number;
	u32 ep_mask;
	int i;

	/* Read it from USB_CAP3 to USB_CAP5 */
	ep_enabled_reg = readl(&priv_dev->regs->usb_cap3);
	iso_ep_reg = readl(&priv_dev->regs->usb_cap4);

	dev_dbg(&priv_dev->dev, "Initializing non-zero endpoints\n");

	for (i = 0; i < USB_SS_ENDPOINTS_MAX_COUNT; i++) {
		ep_number = (i / 2) + 1;
		ep_dir = i % 2;
		ep_mask = BIT((16 * ep_dir) + ep_number);

		if (!(ep_enabled_reg & ep_mask))
			continue;

		priv_ep = devm_kzalloc(&priv_dev->dev, sizeof(*priv_ep),
				       GFP_KERNEL);
		if (!priv_ep)
			return -ENOMEM;

		/* set parent of endpoint object */
		priv_ep->cdns3_dev = priv_dev;
		priv_dev->eps[found_endpoints++] = priv_ep;

		snprintf(priv_ep->name, sizeof(priv_ep->name), "ep%d%s",
			 ep_number, !!ep_dir ? "in" : "out");
		priv_ep->endpoint.name = priv_ep->name;

		usb_ep_set_maxpacket_limit(&priv_ep->endpoint,
					   ENDPOINT_MAX_PACKET_LIMIT);
		priv_ep->endpoint.max_streams = ENDPOINT_MAX_STREAMS;
		priv_ep->endpoint.ops = &cdns3_gadget_ep_ops;
		if (ep_dir)
			priv_ep->endpoint.caps.dir_in = 1;
		else
			priv_ep->endpoint.caps.dir_out = 1;

		if (iso_ep_reg & ep_mask)
			priv_ep->endpoint.caps.type_iso = 1;

		priv_ep->endpoint.caps.type_bulk = 1;
		priv_ep->endpoint.caps.type_int = 1;
		priv_ep->endpoint.maxburst = CDNS3_EP_BUF_SIZE - 1;

		dev_info(&priv_dev->dev, "Initialized  %s support: %s %s\n",
			 priv_ep->name,
			 priv_ep->endpoint.caps.type_bulk ? "BULK, INT" : "",
			 priv_ep->endpoint.caps.type_iso ? "ISO" : "");

		list_add_tail(&priv_ep->endpoint.ep_list,
			      &priv_dev->gadget.ep_list);
		INIT_LIST_HEAD(&priv_ep->request_list);
		INIT_LIST_HEAD(&priv_ep->ep_match_pending_list);
	}

	priv_dev->ep_nums = found_endpoints;
	return 0;
}

static void cdns3_gadget_release(struct device *dev)
{
	struct cdns3_device *priv_dev;

	priv_dev = container_of(dev, struct cdns3_device, dev);
	kfree(priv_dev);
}

static int __cdns3_gadget_init(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev;
	struct device *dev;
	int ret;

	priv_dev = kzalloc(sizeof(*priv_dev), GFP_KERNEL);
	if (!priv_dev)
		return -ENOMEM;

	dev = &priv_dev->dev;
	dev->release = cdns3_gadget_release;
	dev->parent = cdns->dev;
	dev_set_name(dev, "gadget-cdns3");
	cdns->gadget_dev = dev;

	priv_dev->sysdev = cdns->dev;
	ret = device_register(dev);
	if (ret)
		goto err1;

	priv_dev->regs = cdns->dev_regs;

	/* fill gadget fields */
	priv_dev->gadget.max_speed = USB_SPEED_SUPER;
	priv_dev->gadget.speed = USB_SPEED_UNKNOWN;
	priv_dev->gadget.ops = &cdns3_gadget_ops;
	priv_dev->gadget.name = "usb-ss-gadget";
	priv_dev->gadget.sg_supported = 1;
	priv_dev->is_connected = 0;

	spin_lock_init(&priv_dev->lock);

	priv_dev->in_standby_mode = 1;

	/* initialize endpoint container */
	INIT_LIST_HEAD(&priv_dev->gadget.ep_list);
	INIT_LIST_HEAD(&priv_dev->ep_match_list);

	ret = cdns3_init_ep0(priv_dev);
	if (ret) {
		dev_err(dev, "Failed to create endpoint 0\n");
		ret = -ENOMEM;
		goto err2;
	}

	ret = cdns3_init_ep(priv_dev);
	if (ret) {
		dev_err(dev, "Failed to create non zero endpoints\n");
		ret = -ENOMEM;
		goto err2;
	}

	/* allocate memory for default endpoint TRB */
	priv_dev->trb_ep0 = dma_alloc_coherent(priv_dev->sysdev, 24,
					       &priv_dev->trb_ep0_dma, GFP_DMA);
	if (!priv_dev->trb_ep0) {
		dev_err(dev, "Failed to allocate memory for ep0 TRB\n");
		ret = -ENOMEM;
		goto err2;
	}

	/* allocate memory for setup packet buffer */
	priv_dev->setup = dma_alloc_coherent(priv_dev->sysdev, 8,
					     &priv_dev->setup_dma, GFP_DMA);
	if (!priv_dev->setup) {
		dev_err(dev, "Failed to allocate memory for SETUP buffer\n");
		ret = -ENOMEM;
		goto err3;
	}

	dev_dbg(dev, "Device Controller version: %08x\n",
		readl(&priv_dev->regs->usb_cap6));
	dev_dbg(dev, "USB Capabilities:: %08x\n",
		readl(&priv_dev->regs->usb_cap1));
	dev_dbg(dev, "On-Chip memory cnfiguration: %08x\n",
		readl(&priv_dev->regs->usb_cap2));

	/* add USB gadget device */
	ret = usb_add_gadget_udc(&priv_dev->dev, &priv_dev->gadget);
	if (ret < 0) {
		dev_err(dev, "Failed to register USB device controller\n");
		goto err4;
	}

	priv_dev->zlp_buf = kzalloc(ENDPOINT_ZLP_BUF_SIZE, GFP_KERNEL);
	if (!priv_dev->zlp_buf) {
		ret = -ENOMEM;
		goto err4;
	}

	return 0;
err4:
	dma_free_coherent(priv_dev->sysdev, 8, priv_dev->setup,
			  priv_dev->setup_dma);
err3:
	dma_free_coherent(priv_dev->sysdev, 20, priv_dev->trb_ep0,
			  priv_dev->trb_ep0_dma);
err2:
	device_del(dev);
err1:
	put_device(dev);
	cdns->gadget_dev = NULL;
	return ret;
}

/**
 * cdns3_gadget_remove: parent must call this to remove UDC
 *
 * cdns: cdns3 instance
 */
void cdns3_gadget_remove(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev;

	if (!cdns->roles[CDNS3_ROLE_GADGET])
		return;

	priv_dev = container_of(cdns->gadget_dev, struct cdns3_device, dev);
	usb_del_gadget_udc(&priv_dev->gadget);
	dma_free_coherent(priv_dev->sysdev, 8, priv_dev->setup,
			  priv_dev->setup_dma);
	dma_free_coherent(priv_dev->sysdev, 20, priv_dev->trb_ep0,
			  priv_dev->trb_ep0_dma);
	device_unregister(cdns->gadget_dev);
	cdns->gadget_dev = NULL;
	kfree(priv_dev->zlp_buf);
}

static int cdns3_gadget_start(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev = container_of(cdns->gadget_dev,
			struct cdns3_device, dev);
	unsigned long flags;

	pm_runtime_get_sync(cdns->dev);
	spin_lock_irqsave(&priv_dev->lock, flags);
	priv_dev->start_gadget = 1;

	if (!priv_dev->gadget_driver) {
		spin_unlock_irqrestore(&priv_dev->lock, flags);
		return 0;
	}

	cdns3_gadget_config(priv_dev);
	priv_dev->in_standby_mode = 0;
	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return 0;
}

static void __cdns3_gadget_stop(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev;
	unsigned long flags;

	priv_dev = container_of(cdns->gadget_dev, struct cdns3_device, dev);

	if (priv_dev->gadget_driver)
		priv_dev->gadget_driver->disconnect(&priv_dev->gadget);

	usb_gadget_disconnect(&priv_dev->gadget);
	spin_lock_irqsave(&priv_dev->lock, flags);
	priv_dev->gadget.speed = USB_SPEED_UNKNOWN;

	/* disable interrupt for device */
	writel(0, &priv_dev->regs->usb_ien);
	writel(USB_CONF_DEVDS, &priv_dev->regs->usb_conf);
	priv_dev->start_gadget = 0;
	spin_unlock_irqrestore(&priv_dev->lock, flags);
}

static void cdns3_gadget_stop(struct cdns3 *cdns)
{
	if (cdns->role == CDNS3_ROLE_GADGET)
		__cdns3_gadget_stop(cdns);

	pm_runtime_mark_last_busy(cdns->dev);
	pm_runtime_put_autosuspend(cdns->dev);
}

static int cdns3_gadget_suspend(struct cdns3 *cdns, bool do_wakeup)
{
	__cdns3_gadget_stop(cdns);
	return 0;
}

static int cdns3_gadget_resume(struct cdns3 *cdns, bool hibernated)
{
	struct cdns3_device *priv_dev;
	unsigned long flags;

	priv_dev = container_of(cdns->gadget_dev, struct cdns3_device, dev);
	spin_lock_irqsave(&priv_dev->lock, flags);
	priv_dev->start_gadget = 1;
	if (!priv_dev->gadget_driver) {
		spin_unlock_irqrestore(&priv_dev->lock, flags);
		return 0;
	}

	cdns3_gadget_config(priv_dev);
	priv_dev->in_standby_mode = 0;
	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return 0;
}

/**
 * cdns3_gadget_init - initialize device structure
 *
 * cdns: cdns3 instance
 *
 * This function initializes the gadget.
 */
int cdns3_gadget_init(struct cdns3 *cdns)
{
	struct cdns3_role_driver *rdrv;

	rdrv = devm_kzalloc(cdns->dev, sizeof(*rdrv), GFP_KERNEL);
	if (!rdrv)
		return -ENOMEM;

	rdrv->start	= cdns3_gadget_start;
	rdrv->stop	= cdns3_gadget_stop;
	rdrv->suspend	= cdns3_gadget_suspend;
	rdrv->resume	= cdns3_gadget_resume;
	rdrv->irq	= cdns3_irq_handler_thread;
	rdrv->name	= "gadget";
	cdns->roles[CDNS3_ROLE_GADGET] = rdrv;
	return __cdns3_gadget_init(cdns);
}
