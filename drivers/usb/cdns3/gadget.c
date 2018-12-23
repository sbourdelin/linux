// SPDX-License-Identifier: GPL-2.0
/*
 * Cadence USBSS DRD Driver - gadget side.
 *
 * Copyright (C) 2018 Cadence Design Systems.
 * Copyright (C) 2017-2018 NXP
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

#include "trace.h"

static int __cdns3_gadget_ep_queue(struct usb_ep *ep,
				   struct usb_request *request,
				   gfp_t gfp_flags);

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
 * cdns3_ep_addr_to_index - Macro converts endpoint address to
 * index of endpoint object in cdns3_device.eps[] container
 * @ep_addr: endpoint address for which endpoint object is required
 *
 */
u8 cdns3_ep_addr_to_index(u8 ep_addr)
{
	return (((ep_addr & 0x7F)) + ((ep_addr & USB_DIR_IN) ? 16 : 0));
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
	return list_first_entry_or_null(list, struct usb_request, list);
}

/**
 * cdns3_next_priv_request - returns next request from list
 * @list: list containing requests
 *
 * Returns request or NULL if no requests in list
 */
struct cdns3_request *cdns3_next_priv_request(struct list_head *list)
{
	if (list_empty(list))
		return NULL;
	return list_first_entry_or_null(list, struct cdns3_request, list);
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

	priv_dev->selected_ep = ep;
	writel(ep, &priv_dev->regs->ep_sel);
}

dma_addr_t cdns3_trb_virt_to_dma(struct cdns3_endpoint *priv_ep,
				 struct cdns3_trb *trb)
{
	u32 offset = (char *)trb - (char *)priv_ep->trb_pool;

	return priv_ep->trb_pool_dma + offset;
}

int cdns3_ring_size(struct cdns3_endpoint *priv_ep)
{
	switch (priv_ep->type) {
	case USB_ENDPOINT_XFER_ISOC:
		return TRB_ISO_RING_SIZE;
	case USB_ENDPOINT_XFER_CONTROL:
		return TRB_CTRL_RING_SIZE;
	default:
		return TRB_RING_SIZE;
	}
}

/**
 * cdns3_allocate_trb_pool - Allocates TRB's pool for selected endpoint
 * @priv_ep:  endpoint object
 *
 * Function will return 0 on success or -ENOMEM on allocation error
 */
int cdns3_allocate_trb_pool(struct cdns3_endpoint *priv_ep)
{
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	int ring_size = cdns3_ring_size(priv_ep);
	struct cdns3_trb *link_trb;

	if (!priv_ep->trb_pool) {
		priv_ep->trb_pool = dma_zalloc_coherent(priv_dev->sysdev,
							ring_size,
							&priv_ep->trb_pool_dma,
							GFP_DMA);
		if (!priv_ep->trb_pool)
			return -ENOMEM;
	} else {
		memset(priv_ep->trb_pool, 0, ring_size);
	}

	if (!priv_ep->num)
		return 0;

	if (!priv_ep->aligned_buff) {
		void *buff = dma_alloc_coherent(priv_dev->sysdev,
						CDNS3_ALIGNED_BUF_SIZE,
						&priv_ep->aligned_dma_addr,
						GFP_DMA);

		priv_ep->aligned_buff  = buff;
		if (!priv_ep->aligned_buff) {
			dma_free_coherent(priv_dev->sysdev,
					  ring_size,
					  priv_ep->trb_pool,
					  priv_ep->trb_pool_dma);
			priv_ep->trb_pool = NULL;

			return -ENOMEM;
		}
	}

	priv_ep->num_trbs = ring_size / TRB_SIZE;
	/* Initialize the last TRB as Link TRB */
	link_trb = (priv_ep->trb_pool + (priv_ep->num_trbs - 1));
	link_trb->buffer = TRB_BUFFER(priv_ep->trb_pool_dma);
	link_trb->control = TRB_CYCLE | TRB_TYPE(TRB_LINK) |
			    TRB_CHAIN | TRB_TOGGLE;

	return 0;
}

static void cdns3_free_trb_pool(struct cdns3_endpoint *priv_ep)
{
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;

	if (priv_ep->trb_pool) {
		dma_free_coherent(priv_dev->sysdev,
				  cdns3_ring_size(priv_ep),
				  priv_ep->trb_pool, priv_ep->trb_pool_dma);
		priv_ep->trb_pool = NULL;
	}

	if (priv_ep->aligned_buff) {
		dma_free_coherent(priv_dev->sysdev, CDNS3_ALIGNED_BUF_SIZE,
				  priv_ep->aligned_buff,
				  priv_ep->aligned_dma_addr);
		priv_ep->aligned_buff = NULL;
	}
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
 * cdns3_hw_reset_eps_config - reset endpoints configuration kept by controller.
 * @priv_dev: extended gadget object
 */
void cdns3_hw_reset_eps_config(struct cdns3_device *priv_dev)
{
	writel(USB_CONF_CFGRST, &priv_dev->regs->usb_conf);

	cdns3_allow_enable_l1(priv_dev, 0);
	priv_dev->hw_configured_flag = 0;
	priv_dev->onchip_mem_allocated_size = 0;
}

/**
 * cdns3_ep_inc_trb - increment a trb index.
 * @index: Pointer to the TRB index to increment.
 * @cs: Cycle state
 * @trb_in_seg: number of TRBs in segment
 *
 * The index should never point to the link TRB. After incrementing,
 * if it is point to the link TRB, wrap around to the beginning and revert
 * cycle state bit The
 * link TRB is always at the last TRB entry.
 */
static void cdns3_ep_inc_trb(int *index, u8 *cs, int trb_in_seg)
{
	(*index)++;
	if (*index == (trb_in_seg - 1)) {
		*index = 0;
		*cs ^=  1;
	}
}

/**
 * cdns3_ep_inc_enq - increment endpoint's enqueue pointer
 * @priv_ep: The endpoint whose enqueue pointer we're incrementing
 */
static void cdns3_ep_inc_enq(struct cdns3_endpoint *priv_ep)
{
	priv_ep->free_trbs--;
	cdns3_ep_inc_trb(&priv_ep->enqueue, &priv_ep->pcs, priv_ep->num_trbs);
}

/**
 * cdns3_ep_inc_deq - increment endpoint's dequeue pointer
 * @priv_ep: The endpoint whose dequeue pointer we're incrementing
 */
static void cdns3_ep_inc_deq(struct cdns3_endpoint *priv_ep)
{
	priv_ep->free_trbs++;
	cdns3_ep_inc_trb(&priv_ep->dequeue, &priv_ep->ccs, priv_ep->num_trbs);
}

/**
 * cdns3_allow_enable_l1 - enable/disable permits to transition to L1.
 * @priv_dev: Extended gadget object
 * @enable: Enable/disable permit to transition to L1.
 *
 * If bit USB_CONF_L1EN is set and device receive Extended Token packet,
 * then controller answer with ACK handshake.
 * If bit USB_CONF_L1DS is set and device receive Extended Token packet,
 * then controller answer with NYET handshake.
 */
void cdns3_allow_enable_l1(struct cdns3_device *priv_dev, int enable)
{
	if (enable)
		writel(USB_CONF_L1EN, &priv_dev->regs->usb_conf);
	else
		writel(USB_CONF_L1DS, &priv_dev->regs->usb_conf);
}

enum usb_device_speed cdns3_get_speed(struct cdns3_device *priv_dev)
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
 * cdns3_start_all_request - add to ring all request not started
 * @priv_dev: Extended gadget object
 * @priv_ep: The endpoint for whom request will be started.
 *
 * Returns return ENOMEM if transfer ring i not enough TRBs to start
 *         all requests.
 */
static int cdns3_start_all_request(struct cdns3_device *priv_dev,
				   struct cdns3_endpoint *priv_ep)
{
	struct cdns3_request *priv_req;
	struct usb_request *request;
	int ret = 0;

	while (!list_empty(&priv_ep->deferred_req_list)) {
		request = cdns3_next_request(&priv_ep->deferred_req_list);
		priv_req = to_cdns3_request(request);

		ret = cdns3_ep_run_transfer(priv_ep, request);
		if (ret)
			return ret;

		list_del(&request->list);
		list_add_tail(&request->list,
			      &priv_ep->pending_req_list);
	}

	priv_ep->flags &= ~EP_RING_FULL;
	return ret;
}

/**
 * cdns3_descmiss_copy_data copy data from internal requests to request queued
 * by class driver.
 * @priv_ep: extended endpoint object
 * @request: request object
 */
static void cdns3_descmiss_copy_data(struct cdns3_endpoint *priv_ep,
				     struct usb_request *request)
{
	struct usb_request *descmiss_req;
	struct cdns3_request *descmiss_priv_req;

	while (!list_empty(&priv_ep->descmiss_req_list)) {
		int chunk_end;
		int length;

		descmiss_priv_req =
			cdns3_next_priv_request(&priv_ep->descmiss_req_list);
		descmiss_req = &descmiss_priv_req->request;

		/* driver can't touch pending request */
		if (descmiss_priv_req->flags & REQUEST_PENDING)
			break;

		chunk_end = descmiss_priv_req->flags & REQUEST_INTERNAL_CH;
		length = request->actual + descmiss_req->actual;

		if (length <= request->length) {
			memcpy(&((u8 *)request->buf)[request->actual],
			       descmiss_req->buf,
			       descmiss_req->actual);
			request->actual = length;
		} else {
			/* It should never occures */
			request->status = -ENOMEM;
		}

		list_del_init(&descmiss_priv_req->list);

		kfree(descmiss_req->buf);
		cdns3_gadget_ep_free_request(&priv_ep->endpoint, descmiss_req);

		if (!chunk_end)
			break;
	}
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
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	struct usb_request *request = &priv_req->request;

	list_del_init(&request->list);

	if (request->status == -EINPROGRESS)
		request->status = status;

	usb_gadget_unmap_request_by_dev(priv_dev->sysdev, request,
					priv_ep->dir);

	priv_req->flags &= ~REQUEST_PENDING;
	trace_cdns3_gadget_giveback(priv_req);

	/* WA1: */
	if (priv_ep->flags & EP_QUIRK_EXTRA_BUF_EN &&
	    priv_req->flags & REQUEST_INTERNAL) {
		struct usb_request *req;

		req = cdns3_next_request(&priv_ep->deferred_req_list);
		request = req;
		priv_ep->descmis_req = NULL;

		if (!req)
			return;

		cdns3_descmiss_copy_data(priv_ep, req);
		if (!(priv_ep->flags & EP_QUIRK_END_TRANSFER) &&
		    req->length != req->actual) {
			/* wait for next part of transfer */
			return;
		}

		if (req->status == -EINPROGRESS)
			req->status = 0;

		list_del_init(&req->list);
		cdns3_start_all_request(priv_dev, priv_ep);
	}

	/* Start all not pending request */
	if (priv_ep->flags & EP_RING_FULL)
		cdns3_start_all_request(priv_dev, priv_ep);

	if (request->complete) {
		spin_unlock(&priv_dev->lock);
		usb_gadget_giveback_request(&priv_ep->endpoint,
					    request);
		spin_lock(&priv_dev->lock);
	}

	if (request->buf == priv_dev->zlp_buf)
		cdns3_gadget_ep_free_request(&priv_ep->endpoint, request);
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
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	struct cdns3_request *priv_req;
	struct cdns3_trb *trb;
	dma_addr_t trb_dma;
	int sg_iter = 0;
	u32 first_pcs;
	int  num_trb;
	int address;
	int pcs;

	if (priv_ep->type == USB_ENDPOINT_XFER_ISOC)
		num_trb = priv_ep->interval;
	else
		num_trb = request->num_sgs ? request->num_sgs : 1;

	if (num_trb > priv_ep->free_trbs) {
		priv_ep->flags |= EP_RING_FULL;
		return -ENOBUFS;
	}

	priv_req = to_cdns3_request(request);
	address = priv_ep->endpoint.desc->bEndpointAddress;

	priv_ep->flags |= EP_PENDING_REQUEST;
	trb_dma = request->dma;

	/* must allocate buffer aligned to 8 */
	if ((request->dma % 8)) {
		if (request->length <= CDNS3_ALIGNED_BUF_SIZE) {
			memcpy(priv_ep->aligned_buff, request->buf,
			       request->length);
			trb_dma = priv_ep->aligned_dma_addr;
		} else {
			return -ENOMEM;
		}
	}

	trb = priv_ep->trb_pool + priv_ep->enqueue;
	priv_req->start_trb = priv_ep->enqueue;
	priv_req->trb = trb;

	/* prepare ring */
	if ((priv_ep->enqueue + num_trb)  >= (priv_ep->num_trbs - 1)) {
		/*updating C bt in  Link TRB before starting DMA*/
		struct cdns3_trb *link_trb = priv_ep->trb_pool +
					     (priv_ep->num_trbs - 1);
		link_trb->control = ((priv_ep->pcs) ? TRB_CYCLE : 0) |
				    TRB_TYPE(TRB_LINK) | TRB_CHAIN |
				    TRB_TOGGLE;
	}

	first_pcs = priv_ep->pcs ? TRB_CYCLE : 0;

	do {
	/* fill TRB */
		trb->buffer = TRB_BUFFER(request->num_sgs == 0
				? trb_dma : request->sg[sg_iter].dma_address);

		trb->length = TRB_BURST_LEN(16) |
		    TRB_LEN(request->num_sgs == 0 ?
				request->length : request->sg[sg_iter].length);

		trb->control = TRB_TYPE(TRB_NORMAL);
		pcs = priv_ep->pcs ? TRB_CYCLE : 0;

		/*
		 * first trb should be prepared as last to avoid processing
		 *  transfer to early
		 */
		if (sg_iter != 0)
			trb->control |= pcs;

		if (priv_ep->type == USB_ENDPOINT_XFER_ISOC  && !priv_ep->dir) {
			trb->control |= TRB_IOC | TRB_ISP;
		} else {
			/* for last element in TD or in SG list */
			if (sg_iter == (num_trb - 1) && sg_iter != 0)
				trb->control |= pcs | TRB_IOC | TRB_ISP;
		}
		++sg_iter;
		priv_req->end_trb = priv_ep->enqueue;
		cdns3_ep_inc_enq(priv_ep);
		trb = priv_ep->trb_pool + priv_ep->enqueue;
	} while (sg_iter < num_trb);

	trb = priv_req->trb;

	/*
	 * Memory barrier = Cycle Bit must be set before trb->length  and
	 * trb->buffer fields.
	 */
	wmb();

	priv_req->flags |= REQUEST_PENDING;

	/* give the TD to the consumer*/
	if (sg_iter == 1)
		trb->control |= first_pcs | TRB_IOC | TRB_ISP;
	else
		trb->control |= first_pcs;

	trace_cdns3_prepare_trb(priv_ep, priv_req->trb);
	trace_cdns3_ring(priv_ep);

	/* arm transfer on selected endpoint */
	cdns3_select_ep(priv_ep->cdns3_dev, address);

	/*
	 * For DMULT mode we can set address to transfer ring only once after
	 * enabling endpoint.
	 */
	if (priv_ep->flags & EP_UPDATE_EP_TRBADDR) {
		writel(EP_TRADDR_TRADDR(priv_ep->trb_pool_dma),
		       &priv_dev->regs->ep_traddr);
		priv_ep->flags &= ~EP_UPDATE_EP_TRBADDR;
	}

	/*clearing TRBERR and EP_STS_DESCMIS before seting DRDY*/
	writel(EP_STS_TRBERR | EP_STS_DESCMIS, &priv_dev->regs->ep_sts);
	trace_cdns3_doorbell_epx(priv_ep->name,
				 readl(&priv_dev->regs->ep_traddr));
	writel(EP_CMD_DRDY, &priv_dev->regs->ep_cmd);

	return 0;
}

void cdns3_set_hw_configuration(struct cdns3_device *priv_dev)
{
	struct cdns3_endpoint *priv_ep;
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
	cdns3_allow_enable_l1(priv_dev, 1);

	list_for_each_entry(ep, &priv_dev->gadget.ep_list, ep_list) {
		if (ep->enabled) {
			priv_ep = ep_to_cdns3_ep(ep);
			cdns3_start_all_request(priv_dev, priv_ep);
		}
	}
}

/**
 * cdns3_request_handled - check whether request has been handled by DMA
 *
 * @priv_ep: extended endpoint object.
 * @priv_req: request object for checking
 *
 * Endpoint must be selected before invoking this function.
 *
 * Returns false if request has not been handled by DMA, else returns true.
 *
 * SR - start ring
 * ER -  end ring
 * DQ = priv_ep->dequeue - dequeue position
 * EQ = priv_ep->enqueue -  enqueue position
 * ST = priv_req->start_trb - index of first TRB in transfer ring
 * ET = priv_req->end_trb - index of last TRB in transfer ring
 * CI = current_index - index of processed TRB by DMA.
 *
 * As first step, function checks if cycle bit for priv_req->start_trb is
 * correct.
 *
 * some rules:
 * 1. priv_ep->dequeue never exceed current_index.
 * 2  priv_ep->enqueue never exceed priv_ep->dequeue
 *
 * Then We can split recognition into two parts:
 * Case 1 - priv_ep->dequeue < current_index
 *      SR ... EQ ... DQ ... CI ... ER
 *      SR ... DQ ... CI ... EQ ... ER
 *
 *      Request has been handled by DMA if ST and ET is between DQ and CI.
 *
 * Case 2 - priv_ep->dequeue > current_index
 * This situation take place when CI go through the LINK TRB at the end of
 * transfer ring.
 *      SR ... CI ... EQ ... DQ ... ER
 *
 *      Request has been handled by DMA if ET is less then CI or
 *      ET is greater or equal DQ.
 */
static bool cdns3_request_handled(struct cdns3_endpoint *priv_ep,
				  struct cdns3_request *priv_req)
{
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	struct cdns3_trb *trb = priv_req->trb;
	int current_index = 0;
	int handled = 0;

	current_index = (readl(&priv_dev->regs->ep_traddr) -
			 priv_ep->trb_pool_dma) / TRB_SIZE;

	trb = &priv_ep->trb_pool[priv_req->start_trb];

	if ((trb->control  & TRB_CYCLE) != priv_ep->ccs)
		goto finish;

	if (priv_ep->dequeue < current_index) {
		if ((current_index == (priv_ep->num_trbs - 1)) &&
		    !priv_ep->dequeue)
			goto finish;

		if (priv_req->end_trb >= priv_ep->dequeue &&
		    priv_req->end_trb < current_index)
			handled = 1;
	} else if (priv_ep->dequeue  > current_index) {
		if (priv_req->end_trb  < current_index ||
		    priv_req->end_trb >= priv_ep->dequeue)
			handled = 1;
	}

finish:
	trace_cdns3_request_handled(priv_req, current_index, handled);

	return handled;
}

static void cdns3_transfer_completed(struct cdns3_device *priv_dev,
				     struct cdns3_endpoint *priv_ep)
{
	struct cdns3_request *priv_req;
	struct usb_request *request;
	struct cdns3_trb *trb;
	int current_trb;

	while (!list_empty(&priv_ep->pending_req_list)) {
		request = cdns3_next_request(&priv_ep->pending_req_list);
		priv_req = to_cdns3_request(request);

		if (!cdns3_request_handled(priv_ep, priv_req))
			return;

		if (request->dma % 8 && priv_ep->dir == USB_DIR_OUT)
			memcpy(request->buf, priv_ep->aligned_buff,
			       request->length);

		trb = priv_ep->trb_pool + priv_ep->dequeue;
		trace_cdns3_complete_trb(priv_ep, trb);

		if (trb != priv_req->trb)
			dev_warn(priv_dev->dev,
				 "request_trb=0x%p, queue_trb=0x%p\n",
				 priv_req->trb, trb);

		request->actual = TRB_LEN(le32_to_cpu(trb->length));
		current_trb = priv_req->start_trb;

		while (current_trb != priv_req->end_trb) {
			cdns3_ep_inc_deq(priv_ep);
			current_trb = priv_ep->dequeue;
		}

		cdns3_ep_inc_deq(priv_ep);
		cdns3_gadget_giveback(priv_ep, priv_req, 0);
	}
	priv_ep->flags &= ~EP_PENDING_REQUEST;
}

/**
 * cdns3_descmissing_packet - handles descriptor missing event.
 * @priv_dev: extended gadget object
 *
 * WA1: Controller for OUT endpoints has shared on-chip buffers for all incoming
 * packets, including ep0out. It's FIFO buffer, so packets must be handle by DMA
 * in correct order. If the first packet in the buffer will not be handled,
 * then the following packets directed for other endpoints and  functions
 * will be blocked.
 * Additionally the packets directed to one endpoint can block entire on-chip
 * buffers. In this case transfer to other endpoints also will blocked.
 *
 * To resolve this issue after raising the descriptor missing interrupt
 * driver prepares internal usb_request object and use it to arm DMA transfer.
 *
 * The problematic situation was observed in case when endpoint has been enabled
 * but no usb_request were queued. Driver try detects such endpoints and will
 * use this workaround only for these endpoint.
 *
 * Driver use limited number of buffer. This number can be set by macro
 * CDNS_WA1_NUM_BUFFERS.
 *
 * Such blocking situation was observed on ACM gadget. For this function
 * host send OUT data packet but ACM function is not prepared for this packet.
 *
 * It's limitation of controller but maybe this issues should be fixed in
 * function driver.
 */
static int cdns3_descmissing_packet(struct cdns3_endpoint *priv_ep)
{
	struct cdns3_request *priv_req;
	struct usb_request *request;

	if (priv_ep->flags & EP_QUIRK_EXTRA_BUF_DET) {
		priv_ep->flags &= ~EP_QUIRK_EXTRA_BUF_DET;
		priv_ep->flags |= EP_QUIRK_EXTRA_BUF_EN;
	}

	request = cdns3_gadget_ep_alloc_request(&priv_ep->endpoint,
						GFP_ATOMIC);
	if (!request)
		return -ENOMEM;

	priv_req = to_cdns3_request(request);
	priv_req->flags |= REQUEST_INTERNAL;

	/* if this field is still assigned it indicate that transfer related
	 * with this request has not been finished yet. Driver in this
	 * case simply allocate next request and assign flag REQUEST_INTERNAL_CH
	 * flag to previous one. It will indicate that current request is
	 * part of the previous one.
	 */
	if (priv_ep->descmis_req)
		priv_ep->descmis_req->flags |= REQUEST_INTERNAL_CH;

	priv_req->request.buf = kzalloc(CDNS3_DESCMIS_BUF_SIZE,
					GFP_ATOMIC);
	if (!priv_req) {
		cdns3_gadget_ep_free_request(&priv_ep->endpoint, request);
		return -ENOMEM;
	}

	priv_req->request.length = CDNS3_DESCMIS_BUF_SIZE;
	priv_ep->descmis_req = priv_req;

	__cdns3_gadget_ep_queue(&priv_ep->endpoint,
				&priv_ep->descmis_req->request,
				GFP_ATOMIC);

	return 0;
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
	u32 ep_sts_reg;

	cdns3_select_ep(priv_dev, priv_ep->endpoint.address);

	trace_cdns3_epx_irq(priv_dev, priv_ep);

	ep_sts_reg = readl(&priv_dev->regs->ep_sts);
	writel(ep_sts_reg, &priv_dev->regs->ep_sts);

	if ((ep_sts_reg & EP_STS_IOC) || (ep_sts_reg & EP_STS_ISP)) {
		if (priv_ep->flags & EP_QUIRK_EXTRA_BUF_EN) {
			if (ep_sts_reg & EP_STS_ISP)
				priv_ep->flags |= EP_QUIRK_END_TRANSFER;
			else
				priv_ep->flags &= ~EP_QUIRK_END_TRANSFER;
		}
		cdns3_transfer_completed(priv_dev, priv_ep);
	}

	/*
	 * For isochronous transfer driver completes request on IOC or on
	 * TRBERR. IOC appears only when device receive OUT data packet.
	 * If host disable stream or lost some packet then the only way to
	 * finish all queued transfer is to do it on TRBERR event.
	 */
	if ((ep_sts_reg & EP_STS_TRBERR) &&
	    priv_ep->type == USB_ENDPOINT_XFER_ISOC)
		cdns3_transfer_completed(priv_dev, priv_ep);

	/*
	 * WA1: this condition should only be meet when
	 * priv_ep->flags & EP_QUIRK_EXTRA_BUF_DET or
	 * priv_ep->flags & EP_QUIRK_EXTRA_BUF_EN.
	 * In other cases this interrupt will be disabled/
	 */
	if (ep_sts_reg & EP_STS_DESCMIS) {
		int err;

		err = cdns3_descmissing_packet(priv_ep);
		if (err)
			dev_err(priv_dev->dev,
				"Failed: No sufficient memory for DESCMIS\n");
	}

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
	int speed = 0;

	trace_cdns3_usb_irq(priv_dev, usb_ists);
	/* Connection detected */
	if (usb_ists & (USB_ISTS_CON2I | USB_ISTS_CONI)) {
		speed = cdns3_get_speed(priv_dev);
		priv_dev->gadget.speed = speed;
		usb_gadget_set_state(&priv_dev->gadget, USB_STATE_POWERED);
		cdns3_ep0_config(priv_dev);
	}

	/* Disconnection detected */
	if (usb_ists & (USB_ISTS_DIS2I | USB_ISTS_DISI)) {
		if (priv_dev->gadget_driver &&
		    priv_dev->gadget_driver->disconnect) {
			spin_unlock(&priv_dev->lock);
			priv_dev->gadget_driver->disconnect(&priv_dev->gadget);
			spin_lock(&priv_dev->lock);
		}

		priv_dev->gadget.speed = USB_SPEED_UNKNOWN;
		usb_gadget_set_state(&priv_dev->gadget, USB_STATE_NOTATTACHED);
		cdns3_hw_reset_eps_config(priv_dev);
	}

	/* reset*/
	if (usb_ists & (USB_ISTS_UWRESI | USB_ISTS_UHRESI | USB_ISTS_U2RESI)) {
		/*read again to check the actuall speed*/
		speed = cdns3_get_speed(priv_dev);
		usb_gadget_set_state(&priv_dev->gadget, USB_STATE_DEFAULT);
		priv_dev->gadget.speed = speed;
		cdns3_hw_reset_eps_config(priv_dev);
		cdns3_ep0_config(priv_dev);
	}
}

/**
 * cdns3_device_irq_handler- interrupt handler for device part of controller
 *
 * @irq: irq number for cdns3 core device
 * @data: structure of cdns3
 *
 * Returns IRQ_HANDLED or IRQ_NONE
 */
static irqreturn_t cdns3_device_irq_handler(int irq, void *data)
{
	struct cdns3_device *priv_dev;
	struct cdns3 *cdns = data;
	irqreturn_t ret = IRQ_NONE;
	unsigned long flags;
	u32 reg;

	priv_dev = cdns->gadget_dev;
	spin_lock_irqsave(&priv_dev->lock, flags);

	/* check USB device interrupt */
	reg = readl(&priv_dev->regs->usb_ists);
	writel(reg, &priv_dev->regs->usb_ists);

	if (reg) {
		cdns3_check_usb_interrupt_proceed(priv_dev, reg);
		ret = IRQ_HANDLED;
	}

	/* check endpoint interrupt */
	reg = readl(&priv_dev->regs->ep_ists);

	if (reg) {
		reg = ~reg & readl(&priv_dev->regs->ep_ien);
		/* mask deferred interrupt. */
		writel(reg, &priv_dev->regs->ep_ien);
		ret = IRQ_WAKE_THREAD;
	}

	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return ret;
}

/**
 * cdns3_device_thread_irq_handler- interrupt handler for device part
 * of controller
 *
 * @irq: irq number for cdns3 core device
 * @data: structure of cdns3
 *
 * Returns IRQ_HANDLED or IRQ_NONE
 */
static irqreturn_t cdns3_device_thread_irq_handler(int irq, void *data)
{
	struct cdns3_device *priv_dev;
	struct cdns3 *cdns = data;
	irqreturn_t ret = IRQ_NONE;
	unsigned long flags;
	u32 ep_ien;
	int bit;
	u32 reg;

	priv_dev = cdns->gadget_dev;
	spin_lock_irqsave(&priv_dev->lock, flags);

	reg = readl(&priv_dev->regs->ep_ists);
	ep_ien = reg;

	/* handle default endpoint OUT */
	if (reg & EP_ISTS_EP_OUT0) {
		cdns3_check_ep0_interrupt_proceed(priv_dev, USB_DIR_OUT);
		ret = IRQ_HANDLED;
	}

	/* handle default endpoint IN */
	if (reg & EP_ISTS_EP_IN0) {
		cdns3_check_ep0_interrupt_proceed(priv_dev, USB_DIR_IN);
		ret = IRQ_HANDLED;
	}

	/* check if interrupt from non default endpoint, if no exit */
	reg &= ~(EP_ISTS_EP_OUT0 | EP_ISTS_EP_IN0);
	if (!reg)
		goto irqend;

	for_each_set_bit(bit, (unsigned long *)&reg,
			 sizeof(u32) * BITS_PER_BYTE) {
		cdns3_check_ep_interrupt_proceed(priv_dev->eps[bit]);
		ret = IRQ_HANDLED;
	}

irqend:
	ep_ien |= readl(&priv_dev->regs->ep_ien);
	/* Unmask all handled EP interrupts */
	writel(ep_ien, &priv_dev->regs->ep_ien);
	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return ret;
}

/**
 * cdns3_ep_onchip_buffer_reserve - Try to reserve onchip buf for EP
 *
 * The real reservation will occur during write to EP_CFG register,
 * this function is used to check if the 'size' reservation is allowed.
 *
 * @priv_dev: extended gadget object
 * @size: the size (KB) for EP would like to allocate
 *
 * Return 0 if the required size can met or negative value on failure
 */
static int cdns3_ep_onchip_buffer_reserve(struct cdns3_device *priv_dev,
					  int size)
{
	u32 onchip_mem;

	priv_dev->onchip_mem_allocated_size += size;

	onchip_mem = USB_CAP2_ACTUAL_MEM_SIZE(readl(&priv_dev->regs->usb_cap2));
	if (!onchip_mem)
		onchip_mem = 256;

	/* 2KB is reserved for EP0*/
	onchip_mem -= 2;
	if (priv_dev->onchip_mem_allocated_size > onchip_mem) {
		priv_dev->onchip_mem_allocated_size -= size;
		return -EPERM;
	}

	return 0;
}

/**
 * cdns3_ep_config Configure hardware endpoint
 * @priv_ep: extended endpoint object
 */
void cdns3_ep_config(struct cdns3_endpoint *priv_ep)
{
	bool is_iso_ep = (priv_ep->type == USB_ENDPOINT_XFER_ISOC);
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;
	u32 bEndpointAddress = priv_ep->num | priv_ep->dir;
	u32 max_packet_size = 0;
	u32 ep_cfg = 0;
	int ret;

	switch (priv_ep->type) {
	case USB_ENDPOINT_XFER_INT:
		ep_cfg = EP_CFG_EPTYPE(USB_ENDPOINT_XFER_INT);
		break;
	case USB_ENDPOINT_XFER_BULK:
		ep_cfg = EP_CFG_EPTYPE(USB_ENDPOINT_XFER_BULK);
		break;
	default:
		ep_cfg = EP_CFG_EPTYPE(USB_ENDPOINT_XFER_ISOC);
	}

	switch (priv_dev->gadget.speed) {
	case USB_SPEED_FULL:
		max_packet_size = is_iso_ep ? 1023 : 64;
		break;
	case USB_SPEED_HIGH:
		max_packet_size = is_iso_ep ? 1024 : 512;
		break;
	case USB_SPEED_SUPER:
		max_packet_size = 1024;
		break;
	default:
		/* all other speed are not supported */
		return;
	}

	ret = cdns3_ep_onchip_buffer_reserve(priv_dev, CDNS3_EP_BUF_SIZE);
	if (ret) {
		dev_err(priv_dev->dev, "onchip mem is full, ep is invalid\n");
		return;
	}

	ep_cfg |= EP_CFG_MAXPKTSIZE(max_packet_size) |
		  EP_CFG_BUFFERING(CDNS3_EP_BUF_SIZE - 1) |
		  EP_CFG_MAXBURST(priv_ep->endpoint.maxburst);

	cdns3_select_ep(priv_dev, bEndpointAddress);
	writel(ep_cfg, &priv_dev->regs->ep_cfg);

	dev_dbg(priv_dev->dev, "Configure %s: with val %08x\n",
		priv_ep->name, ep_cfg);
}

/* Find correct direction for HW endpoint according to description */
static int cdns3_ep_dir_is_correct(struct usb_endpoint_descriptor *desc,
				   struct cdns3_endpoint *priv_ep)
{
	return (priv_ep->endpoint.caps.dir_in && usb_endpoint_dir_in(desc)) ||
	       (priv_ep->endpoint.caps.dir_out && usb_endpoint_dir_out(desc));
}

static struct
cdns3_endpoint *cdns3_find_available_ep(struct cdns3_device *priv_dev,
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
			if (!(priv_ep->flags & EP_CLAIMED)) {
				priv_ep->num  = num;
				return priv_ep;
			}
		}
	}

	return ERR_PTR(-ENOENT);
}

/*
 *  Cadence IP has one limitation that all endpoints must be configured
 * (Type & MaxPacketSize) before setting configuration through hardware
 * register, it means we can't change endpoints configuration after
 * set_configuration.
 *
 * This function set EP_CLAIMED flag which is added when the gadget driver
 * uses usb_ep_autoconfig to configure specific endpoint;
 * When the udc driver receives set_configurion request,
 * it goes through all claimed endpoints, and configure all endpoints
 * accordingly.
 *
 * At usb_ep_ops.enable/disable, we only enable and disable endpoint through
 * ep_cfg register which can be changed after set_configuration, and do
 * some software operation accordingly.
 */
static struct
usb_ep *cdns3_gadget_match_ep(struct usb_gadget *gadget,
			      struct usb_endpoint_descriptor *desc,
			      struct usb_ss_ep_comp_descriptor *comp_desc)
{
	struct cdns3_device *priv_dev = gadget_to_cdns3_device(gadget);
	struct cdns3_endpoint *priv_ep;
	unsigned long flags;

	priv_ep = cdns3_find_available_ep(priv_dev, desc);
	if (IS_ERR(priv_ep)) {
		dev_err(priv_dev->dev, "no available ep\n");
		return NULL;
	}

	dev_dbg(priv_dev->dev, "match endpoint: %s\n", priv_ep->name);

	spin_lock_irqsave(&priv_dev->lock, flags);
	priv_ep->endpoint.desc = desc;
	priv_ep->dir  = usb_endpoint_dir_in(desc) ? USB_DIR_IN : USB_DIR_OUT;
	priv_ep->type = usb_endpoint_type(desc);
	priv_ep->flags |= EP_CLAIMED;
	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return &priv_ep->endpoint;
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
	struct cdns3_endpoint *priv_ep = ep_to_cdns3_ep(ep);
	struct cdns3_request *priv_req;

	priv_req = kzalloc(sizeof(*priv_req), gfp_flags);
	if (!priv_req)
		return NULL;

	priv_req->priv_ep = priv_ep;

	trace_cdns3_alloc_request(priv_req);
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

	trace_cdns3_free_request(priv_req);
	kfree(priv_req);
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
	u32 reg = EP_STS_EN_TRBERREN;
	u32 bEndpointAddress;
	unsigned long flags;
	int ret;

	priv_ep = ep_to_cdns3_ep(ep);
	priv_dev = priv_ep->cdns3_dev;

	if (!ep || !desc || desc->bDescriptorType != USB_DT_ENDPOINT) {
		dev_dbg(priv_dev->dev, "usbss: invalid parameters\n");
		return -EINVAL;
	}

	if (!desc->wMaxPacketSize) {
		dev_err(priv_dev->dev, "usbss: missing wMaxPacketSize\n");
		return -EINVAL;
	}

	if (dev_WARN_ONCE(priv_dev->dev, priv_ep->flags & EP_ENABLED,
			  "%s is already enabled\n", priv_ep->name))
		return 0;

	spin_lock_irqsave(&priv_dev->lock, flags);

	priv_ep->endpoint.desc = desc;
	priv_ep->type = usb_endpoint_type(desc);
	priv_ep->interval = desc->bInterval ? BIT(desc->bInterval - 1) : 0;

	if (priv_ep->interval > ISO_MAX_INTERVAL &&
	    priv_ep->type == USB_ENDPOINT_XFER_ISOC) {
		dev_err(priv_dev->dev, "Driver is limited to %d period\n",
			ISO_MAX_INTERVAL);

		ret =  -EINVAL;
		goto exit;
	}

	ret = cdns3_allocate_trb_pool(priv_ep);

	if (ret)
		goto exit;

	bEndpointAddress = priv_ep->num | priv_ep->dir;
	cdns3_select_ep(priv_dev, bEndpointAddress);

	trace_cdns3_gadget_ep_enable(priv_ep);

	writel(EP_CMD_EPRST, &priv_dev->regs->ep_cmd);

	ret = cdns3_handshake(&priv_dev->regs->ep_cmd,
			      EP_CMD_CSTALL | EP_CMD_EPRST, 0, 100);

	/* enable interrupt for selected endpoint */
	cdns3_set_register_bit(&priv_dev->regs->ep_ien,
			       BIT(cdns3_ep_addr_to_index(bEndpointAddress)));
	/*
	 * WA1: Set flag for all not ISOC OUT endpoints. If this flag is set
	 * driver try to detect whether endpoint need additional internal
	 * buffer for unblocking on-chip FIFO buffer. This flag will be cleared
	 * if before first DESCMISS interrupt the DMA will be armed.
	 */
	if (!priv_ep->dir && priv_ep->type != USB_ENDPOINT_XFER_ISOC) {
		priv_ep->flags |= EP_QUIRK_EXTRA_BUF_DET;
		reg |= EP_STS_EN_DESCMISEN;
	}

	writel(reg, &priv_dev->regs->ep_sts_en);

	cdns3_set_register_bit(&priv_dev->regs->ep_cfg, EP_CFG_ENABLE);

	ep->desc = desc;
	priv_ep->flags &= ~(EP_PENDING_REQUEST | EP_STALL |
			    EP_QUIRK_EXTRA_BUF_EN);
	priv_ep->flags |= EP_ENABLED | EP_UPDATE_EP_TRBADDR;
	priv_ep->enqueue = 0;
	priv_ep->dequeue = 0;
	reg = readl(&priv_dev->regs->ep_sts);
	priv_ep->pcs = !!EP_STS_CCS(reg);
	priv_ep->ccs = !!EP_STS_CCS(reg);
	/* one TRB is reserved for link TRB used in DMULT mode*/
	priv_ep->free_trbs = priv_ep->num_trbs - 1;
exit:
	spin_unlock_irqrestore(&priv_dev->lock, flags);

	return ret;
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
	struct cdns3_request *priv_req;
	struct cdns3_device *priv_dev;
	struct usb_request *request;
	unsigned long flags;
	int ret = 0;
	u32 ep_cfg;

	if (!ep) {
		pr_err("usbss: invalid parameters\n");
		return -EINVAL;
	}

	priv_ep = ep_to_cdns3_ep(ep);
	priv_dev = priv_ep->cdns3_dev;

	if (dev_WARN_ONCE(priv_dev->dev, !(priv_ep->flags & EP_ENABLED),
			  "%s is already disabled\n", priv_ep->name))
		return 0;

	spin_lock_irqsave(&priv_dev->lock, flags);

	trace_cdns3_gadget_ep_disable(priv_ep);

	cdns3_select_ep(priv_dev, ep->desc->bEndpointAddress);
	ret = cdns3_data_flush(priv_ep);

	ep_cfg = readl(&priv_dev->regs->ep_cfg);
	ep_cfg &= ~EP_CFG_ENABLE;
	writel(ep_cfg, &priv_dev->regs->ep_cfg);

	while (!list_empty(&priv_ep->pending_req_list)) {
		request = cdns3_next_request(&priv_ep->pending_req_list);

		cdns3_gadget_giveback(priv_ep, to_cdns3_request(request),
				      -ESHUTDOWN);
	}

	while (!list_empty(&priv_ep->descmiss_req_list)) {
		priv_req = cdns3_next_priv_request(&priv_ep->descmiss_req_list);

		kfree(priv_req->request.buf);
		cdns3_gadget_ep_free_request(&priv_ep->endpoint,
					     &priv_req->request);
	}

	while (!list_empty(&priv_ep->deferred_req_list)) {
		request = cdns3_next_request(&priv_ep->deferred_req_list);

		cdns3_gadget_giveback(priv_ep, to_cdns3_request(request),
				      -ESHUTDOWN);
	}

	priv_ep->descmis_req = NULL;

	ep->desc = NULL;
	priv_ep->flags &= ~EP_ENABLED;

	spin_unlock_irqrestore(&priv_dev->lock, flags);

	return ret;
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
	struct cdns3_request *priv_req;
	int deferred = 0;
	int ret = 0;

	request->actual = 0;
	request->status = -EINPROGRESS;
	priv_req = to_cdns3_request(request);
	trace_cdns3_ep_queue(priv_req);

	/*
	 * WA1: if transfer was queued before DESCMISS appear than we
	 * can disable handling of DESCMISS interrupt. Driver assumes that it
	 * can disable special treatment for this endpoint.
	 */
	if (priv_ep->flags & EP_QUIRK_EXTRA_BUF_DET) {
		u32 reg;

		cdns3_select_ep(priv_dev, priv_ep->num | priv_ep->dir);
		reg = readl(&priv_dev->regs->ep_sts_en);
		priv_ep->flags &= ~EP_QUIRK_EXTRA_BUF_DET;
		reg &= EP_STS_EN_DESCMISEN;
		writel(reg, &priv_dev->regs->ep_sts_en);
	}

	/* WA1 */
	if (priv_ep->flags & EP_QUIRK_EXTRA_BUF_EN) {
		u8 pending_empty = list_empty(&priv_ep->pending_req_list);
		u8 descmiss_empty = list_empty(&priv_ep->descmiss_req_list);

		/*
		 *  DESCMISS transfer has been finished, so data will be
		 *  directly copied from internal allocated usb_request
		 *  objects.
		 */
		if (pending_empty && !descmiss_empty &&
		    !(priv_req->flags & REQUEST_INTERNAL)) {
			cdns3_descmiss_copy_data(priv_ep, request);
			list_add_tail(&request->list,
				      &priv_ep->pending_req_list);
			cdns3_gadget_giveback(priv_ep, priv_req,
					      request->status);
			return ret;
		}

		/*
		 * WA1 driver will wait for completion DESCMISS transfer,
		 * before starts new, not DESCMISS transfer.
		 */
		if (!pending_empty && !descmiss_empty)
			deferred = 1;

		if (priv_req->flags & REQUEST_INTERNAL)
			list_add_tail(&priv_req->list,
				      &priv_ep->descmiss_req_list);
	}

	ret = usb_gadget_map_request_by_dev(priv_dev->sysdev, request,
					    usb_endpoint_dir_in(ep->desc));
	if (ret)
		return ret;

	/*
	 * If hardware endpoint configuration has not been set yet then
	 * just queue request in deferred list. Transfer will be started in
	 * cdns3_set_hw_configuration.
	 */
	if (!priv_dev->hw_configured_flag)
		deferred = 1;
	else
		ret = cdns3_ep_run_transfer(priv_ep, request);

	if (ret || deferred)
		list_add_tail(&request->list, &priv_ep->deferred_req_list);
	else
		list_add_tail(&request->list, &priv_ep->pending_req_list);

	return ret;
}

static int cdns3_gadget_ep_queue(struct usb_ep *ep, struct usb_request *request,
				 gfp_t gfp_flags)
{
	struct usb_request *zlp_request;
	struct cdns3_endpoint *priv_ep;
	struct cdns3_device *priv_dev;
	unsigned long flags;
	int ret;

	if (!request || !ep)
		return -EINVAL;

	priv_ep = ep_to_cdns3_ep(ep);
	priv_dev = priv_ep->cdns3_dev;

	spin_lock_irqsave(&priv_dev->lock, flags);

	ret = __cdns3_gadget_ep_queue(ep, request, gfp_flags);

	if (ret == 0 && request->zero && request->length &&
	    (request->length % ep->maxpacket == 0)) {
		struct cdns3_request *priv_req;

		zlp_request = cdns3_gadget_ep_alloc_request(ep, GFP_ATOMIC);
		zlp_request->buf = priv_dev->zlp_buf;
		zlp_request->length = 0;

		priv_req = to_cdns3_request(zlp_request);
		priv_req->flags |= REQUEST_ZLP;

		dev_dbg(priv_dev->dev, "Queuing ZLP for endpoint: %s\n",
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
	struct cdns3_request *priv_req;
	unsigned long flags;
	int ret = 0;

	if (!ep || !request || !ep->desc)
		return -EINVAL;

	spin_lock_irqsave(&priv_dev->lock, flags);

	priv_req = to_cdns3_request(request);

	trace_cdns3_ep_dequeue(priv_req);

	cdns3_select_ep(priv_dev, ep->desc->bEndpointAddress);

	list_for_each_entry_safe(req, req_temp, &priv_ep->pending_req_list,
				 list) {
		if (request == req)
			goto found;
	}

	list_for_each_entry_safe(req, req_temp, &priv_ep->deferred_req_list,
				 list) {
		if (request == req)
			goto found;
	}

found:
	if (request == req)
		cdns3_gadget_giveback(priv_ep, priv_req, -ECONNRESET);

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

	spin_lock_irqsave(&priv_dev->lock, flags);

	/* if actual transfer is pending defer setting stall on this endpoint */
	if ((priv_ep->flags & EP_PENDING_REQUEST) && value) {
		priv_ep->flags |= EP_STALL;
		goto finish;
	}

	dev_dbg(priv_dev->dev, "Halt endpoint %s\n", priv_ep->name);

	cdns3_select_ep(priv_dev, ep->desc->bEndpointAddress);
	if (value) {
		cdns3_ep_stall_flush(priv_ep);
	} else {
		priv_ep->flags &= ~EP_WEDGE;
		writel(EP_CMD_CSTALL | EP_CMD_EPRST, &priv_dev->regs->ep_cmd);

		/* wait for EPRST cleared */
		ret = cdns3_handshake(&priv_dev->regs->ep_cmd,
				      EP_CMD_EPRST, 0, 100);
		if (unlikely(ret)) {
			dev_err(priv_dev->dev,
				"Clearing halt condition failed for %s\n",
				priv_ep->name);
			goto finish;

		} else {
			priv_ep->flags &= ~EP_STALL;
		}
	}

	priv_ep->flags &= ~EP_PENDING_REQUEST;
finish:
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
	priv_dev->is_selfpowered = !!is_selfpowered;
	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return 0;
}

static int cdns3_gadget_pullup(struct usb_gadget *gadget, int is_on)
{
	struct cdns3_device *priv_dev = gadget_to_cdns3_device(gadget);

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
	writel(USB_CONF_DMULT, &regs->usb_conf);
	cdns3_gadget_pullup(&priv_dev->gadget, 1);
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

	spin_lock_irqsave(&priv_dev->lock, flags);
	priv_dev->gadget_driver = driver;
	cdns3_gadget_config(priv_dev);
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
	struct cdns3_endpoint *priv_ep;
	u32 bEndpointAddress;
	struct usb_ep *ep;
	int ret = 0;

	priv_dev->gadget_driver = NULL;

	priv_dev->onchip_mem_allocated_size = 0;
	priv_dev->gadget.speed = USB_SPEED_UNKNOWN;

	list_for_each_entry(ep, &priv_dev->gadget.ep_list, ep_list) {
		priv_ep = ep_to_cdns3_ep(ep);
		bEndpointAddress = priv_ep->num | priv_ep->dir;
		cdns3_select_ep(priv_dev, bEndpointAddress);
		writel(EP_CMD_EPRST, &priv_dev->regs->ep_cmd);
		ret = cdns3_handshake(&priv_dev->regs->ep_cmd,
				      EP_CMD_EPRST, 0, 100);
		cdns3_free_trb_pool(priv_ep);
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

static void cdns3_free_all_eps(struct cdns3_device *priv_dev)
{
	int i;

	/*ep0 OUT point to ep0 IN*/
	priv_dev->eps[16] = NULL;

	cdns3_free_trb_pool(priv_dev->eps[0]);

	for (i = 0; i < CDNS3_ENDPOINTS_MAX_COUNT; i++)
		if (priv_dev->eps[i])
			devm_kfree(priv_dev->dev, priv_dev->eps[i]);
}

/**
 * cdns3_init_eps Initializes software endpoints of gadget
 * @cdns3: extended gadget object
 *
 * Returns 0 on success, error code elsewhere
 */
static int cdns3_init_eps(struct cdns3_device *priv_dev)
{
	u32 ep_enabled_reg, iso_ep_reg;
	struct cdns3_endpoint *priv_ep;
	int ep_dir, ep_number;
	u32 ep_mask;
	int ret = 0;
	int i;

	/* Read it from USB_CAP3 to USB_CAP5 */
	ep_enabled_reg = readl(&priv_dev->regs->usb_cap3);
	iso_ep_reg = readl(&priv_dev->regs->usb_cap4);

	dev_dbg(priv_dev->dev, "Initializing non-zero endpoints\n");

	for (i = 0; i < CDNS3_ENDPOINTS_MAX_COUNT; i++) {
		ep_dir = i >> 4;	/* i div 16 */
		ep_number = i & 0xF;	/* i % 16 */
		ep_mask = BIT(i);

		if (!(ep_enabled_reg & ep_mask))
			continue;

		if (ep_dir && !ep_number) {
			priv_dev->eps[i] = priv_dev->eps[0];
			continue;
		}

		priv_ep = devm_kzalloc(priv_dev->dev, sizeof(*priv_ep),
				       GFP_KERNEL);
		if (!priv_ep) {
			ret = -ENOMEM;
			goto err;
		}

		/* set parent of endpoint object */
		priv_ep->cdns3_dev = priv_dev;
		priv_dev->eps[i] = priv_ep;
		priv_ep->num = ep_number;
		priv_ep->dir = ep_dir ? USB_DIR_IN : USB_DIR_OUT;

		if (!ep_number) {
			ret = cdns3_init_ep0(priv_dev, priv_ep);
			if (ret) {
				dev_err(priv_dev->dev, "Failed to init ep0\n");
				goto err;
			}
		} else {
			snprintf(priv_ep->name, sizeof(priv_ep->name), "ep%d%s",
				 ep_number, !!ep_dir ? "in" : "out");
			priv_ep->endpoint.name = priv_ep->name;

			usb_ep_set_maxpacket_limit(&priv_ep->endpoint,
						   CDNS3_EP_MAX_PACKET_LIMIT);
			priv_ep->endpoint.max_streams = CDNS3_EP_MAX_STREAMS;
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

			list_add_tail(&priv_ep->endpoint.ep_list,
				      &priv_dev->gadget.ep_list);
		}

		priv_ep->flags = 0;

		dev_info(priv_dev->dev, "Initialized  %s support: %s %s\n",
			 priv_ep->name,
			 priv_ep->endpoint.caps.type_bulk ? "BULK, INT" : "",
			 priv_ep->endpoint.caps.type_iso ? "ISO" : "");

		INIT_LIST_HEAD(&priv_ep->pending_req_list);
		INIT_LIST_HEAD(&priv_ep->deferred_req_list);
		INIT_LIST_HEAD(&priv_ep->descmiss_req_list);
	}

	return 0;
err:
	cdns3_free_all_eps(priv_dev);
	return -ENOMEM;
}

static void cdns3_gadget_disable(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev;

	priv_dev = cdns->gadget_dev;

	if (priv_dev->gadget_driver)
		priv_dev->gadget_driver->disconnect(&priv_dev->gadget);

	usb_gadget_disconnect(&priv_dev->gadget);
	priv_dev->gadget.speed = USB_SPEED_UNKNOWN;
}

void cdns3_gadget_exit(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev;

	priv_dev = cdns->gadget_dev;

	cdns3_gadget_disable(cdns);

	devm_free_irq(cdns->dev, cdns->irq, cdns);

	pm_runtime_mark_last_busy(cdns->dev);
	pm_runtime_put_autosuspend(cdns->dev);

	usb_del_gadget_udc(&priv_dev->gadget);

	cdns3_free_all_eps(priv_dev);

	dma_free_coherent(priv_dev->sysdev, 8, priv_dev->setup_buf,
			  priv_dev->setup_dma);

	kfree(priv_dev->zlp_buf);
	kfree(priv_dev);
	cdns->gadget_dev = NULL;
}

static int cdns3_gadget_start(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev;
	u32 max_speed;
	int ret;

	priv_dev = kzalloc(sizeof(*priv_dev), GFP_KERNEL);
	if (!priv_dev)
		return -ENOMEM;

	cdns->gadget_dev = priv_dev;
	priv_dev->sysdev = cdns->dev;
	priv_dev->dev = cdns->dev;
	priv_dev->regs = cdns->dev_regs;

	max_speed = usb_get_maximum_speed(cdns->dev);

	/* Check the maximum_speed parameter */
	switch (max_speed) {
	case USB_SPEED_FULL:
	case USB_SPEED_HIGH:
	case USB_SPEED_SUPER:
		break;
	default:
		dev_err(cdns->dev, "invalid maximum_speed parameter %d\n",
			max_speed);
		/* fall through */
	case USB_SPEED_UNKNOWN:
		/* default to superspeed */
		max_speed = USB_SPEED_SUPER;
		break;
	}

	/* fill gadget fields */
	priv_dev->gadget.max_speed = max_speed;
	priv_dev->gadget.speed = USB_SPEED_UNKNOWN;
	priv_dev->gadget.ops = &cdns3_gadget_ops;
	priv_dev->gadget.name = "usb-ss-gadget";
	priv_dev->gadget.sg_supported = 1;

	spin_lock_init(&priv_dev->lock);
	INIT_WORK(&priv_dev->pending_status_wq,
		  cdns3_pending_setup_status_handler);

	/* initialize endpoint container */
	INIT_LIST_HEAD(&priv_dev->gadget.ep_list);

	ret = cdns3_init_eps(priv_dev);
	if (ret) {
		dev_err(priv_dev->dev, "Failed to create endpoints\n");
		goto err1;
	}

	/* allocate memory for setup packet buffer */
	priv_dev->setup_buf = dma_alloc_coherent(priv_dev->sysdev, 8,
						 &priv_dev->setup_dma, GFP_DMA);
	if (!priv_dev->setup_buf) {
		dev_err(priv_dev->dev, "Failed to allocate memory for SETUP buffer\n");
		ret = -ENOMEM;
		goto err2;
	}

	priv_dev->dev_ver = readl(&priv_dev->regs->usb_cap6);
	dev_dbg(priv_dev->dev, "Device Controller version: %08x\n",
		readl(&priv_dev->regs->usb_cap6));
	dev_dbg(priv_dev->dev, "USB Capabilities:: %08x\n",
		readl(&priv_dev->regs->usb_cap1));
	dev_dbg(priv_dev->dev, "On-Chip memory cnfiguration: %08x\n",
		readl(&priv_dev->regs->usb_cap2));

	priv_dev->zlp_buf = kzalloc(CDNS3_EP_ZLP_BUF_SIZE, GFP_KERNEL);
	if (!priv_dev->zlp_buf) {
		ret = -ENOMEM;
		goto err3;
	}

	/* add USB gadget device */
	ret = usb_add_gadget_udc(priv_dev->dev, &priv_dev->gadget);
	if (ret < 0) {
		dev_err(priv_dev->dev,
			"Failed to register USB device controller\n");
		goto err4;
	}

	return 0;
err4:
	kfree(priv_dev->zlp_buf);
err3:
	dma_free_coherent(priv_dev->sysdev, 8, priv_dev->setup_buf,
			  priv_dev->setup_dma);
err2:
	cdns3_free_all_eps(priv_dev);
err1:
	cdns->gadget_dev = NULL;
	return ret;
}

static int __cdns3_gadget_init(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev;
	unsigned long flags;
	int ret = 0;

	ret = cdns3_gadget_start(cdns);
	if (ret)
		return ret;

	priv_dev = cdns->gadget_dev;
	ret = devm_request_threaded_irq(cdns->dev, cdns->irq,
					cdns3_device_irq_handler,
					cdns3_device_thread_irq_handler,
					IRQF_SHARED, dev_name(cdns->dev), cdns);
	if (ret)
		goto err0;

	pm_runtime_get_sync(cdns->dev);
	spin_lock_irqsave(&priv_dev->lock, flags);
	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return 0;
err0:
	cdns3_gadget_exit(cdns);
	return ret;
}

static int cdns3_gadget_suspend(struct cdns3 *cdns, bool do_wakeup)
{
	cdns3_gadget_disable(cdns);
	return 0;
}

static int cdns3_gadget_resume(struct cdns3 *cdns, bool hibernated)
{
	struct cdns3_device *priv_dev;
	unsigned long flags;

	priv_dev = cdns->gadget_dev;
	spin_lock_irqsave(&priv_dev->lock, flags);

	if (!priv_dev->gadget_driver) {
		spin_unlock_irqrestore(&priv_dev->lock, flags);
		return 0;
	}

	cdns3_gadget_config(priv_dev);
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

	rdrv->start	= __cdns3_gadget_init;
	rdrv->stop	= cdns3_gadget_exit;
	rdrv->suspend	= cdns3_gadget_suspend;
	rdrv->resume	= cdns3_gadget_resume;
	rdrv->state	= CDNS3_ROLE_STATE_INACTIVE;
	rdrv->name	= "gadget";
	cdns->roles[CDNS3_ROLE_GADGET] = rdrv;

	return 0;
}
