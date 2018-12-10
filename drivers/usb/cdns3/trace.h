/* SPDX-License-Identifier: GPL-2.0 */
/*
 * USBSS device controller driver.
 * Trace support header file.
 *
 * Copyright (C) 2018 Cadence.
 *
 * Author: Pawel Laszczak <pawell@cadence.com>
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM cdns3

#if !defined(__LINUX_CDNS3_TRACE) || defined(TRACE_HEADER_MULTI_READ)
#define __LINUX_CDNS3_TRACE

#include <linux/types.h>
#include <linux/tracepoint.h>
#include <asm/byteorder.h>
#include "core.h"
#include "gadget.h"
#include "debug.h"

#define CDNS3_MSG_MAX	500

DECLARE_EVENT_CLASS(cdns3_log_doorbell,
	TP_PROTO(const char *ep_name),
	TP_ARGS(ep_name),
	TP_STRUCT__entry(
		__field(const char *, ep_name)
	),
	TP_fast_assign(
		__entry->ep_name = ep_name;
	),
	TP_printk("//Ding Dong %s", __entry->ep_name)
);

DEFINE_EVENT(cdns3_log_doorbell, cdns3_doorbell_ep0,
	TP_PROTO(const char *ep_name),
	TP_ARGS(ep_name)
);

DEFINE_EVENT(cdns3_log_doorbell, cdns3_doorbell_epx,
	TP_PROTO(const char *ep_name),
	TP_ARGS(ep_name)
);

DECLARE_EVENT_CLASS(cdns3_log_usb_irq,
	TP_PROTO(struct cdns3_device *priv_dev, u32 usb_ists),
	TP_ARGS(priv_dev, usb_ists),
	TP_STRUCT__entry(
		__field(struct cdns3_device *, priv_dev)
		__field(u32, usb_ists)
	),
	TP_fast_assign(
		__entry->priv_dev = priv_dev;
		__entry->usb_ists = usb_ists;
	),
	TP_printk("%s", cdns3_decode_usb_irq(__entry->priv_dev,
					     __entry->usb_ists))
);

DEFINE_EVENT(cdns3_log_usb_irq, cdns3_usb_irq,
	TP_PROTO(struct cdns3_device *priv_dev, u32 usb_ists),
	TP_ARGS(priv_dev, usb_ists)
);

DECLARE_EVENT_CLASS(cdns3_log_epx_irq,
	TP_PROTO(struct cdns3_endpoint *priv_ep),
	TP_ARGS(priv_ep),
	TP_STRUCT__entry(
		__field(struct cdns3_endpoint *, priv_ep)
	),
	TP_fast_assign(
		__entry->priv_ep = priv_ep;
	),
	TP_printk("%s", cdns3_decode_epx_irq(__entry->priv_ep))
);

DEFINE_EVENT(cdns3_log_epx_irq, cdns3_epx_irq,
	TP_PROTO(struct cdns3_endpoint *priv_ep),
	TP_ARGS(priv_ep)
);

DECLARE_EVENT_CLASS(cdns3_log_ep0_irq,
	TP_PROTO(struct cdns3_device *priv_dev),
	TP_ARGS(priv_dev),
	TP_STRUCT__entry(
		__field(struct cdns3_device *, priv_dev)
	),
	TP_fast_assign(
		__entry->priv_dev = priv_dev;
	),
	TP_printk("%s", cdns3_decode_ep0_irq(__entry->priv_dev))
);

DEFINE_EVENT(cdns3_log_ep0_irq, cdns3_ep0_irq,
	TP_PROTO(struct cdns3_device *priv_dev),
	TP_ARGS(priv_dev)
);

DECLARE_EVENT_CLASS(cdns3_log_ctrl,
	TP_PROTO(struct usb_ctrlrequest *ctrl),
	TP_ARGS(ctrl),
	TP_STRUCT__entry(
		__field(u8, bRequestType)
		__field(u8, bRequest)
		__field(u16, wValue)
		__field(u16, wIndex)
		__field(u16, wLength)
		__dynamic_array(char, str, CDNS3_MSG_MAX)
	),
	TP_fast_assign(
		__entry->bRequestType = ctrl->bRequestType;
		__entry->bRequest = ctrl->bRequest;
		__entry->wValue = le16_to_cpu(ctrl->wValue);
		__entry->wIndex = le16_to_cpu(ctrl->wIndex);
		__entry->wLength = le16_to_cpu(ctrl->wLength);
	),
	TP_printk("%s", cdns3_decode_ctrl(__get_str(str), __entry->bRequestType,
					__entry->bRequest, __entry->wValue,
					__entry->wIndex, __entry->wLength)
	)
);

DEFINE_EVENT(cdns3_log_ctrl, cdns3_ctrl_req,
	TP_PROTO(struct usb_ctrlrequest *ctrl),
	TP_ARGS(ctrl)
);

DECLARE_EVENT_CLASS(cdns3_log_request,
	TP_PROTO(struct cdns3_request *req),
	TP_ARGS(req),
	TP_STRUCT__entry(
		__string(name, req->priv_ep->name)
		__field(struct cdns3_request *, req)
		__field(unsigned int, actual)
		__field(unsigned int, length)
		__field(int, status)
		__field(int, zero)
		__field(int, short_not_ok)
		__field(int, no_interrupt)
		__field(int, start_trb)
		__field(int, end_trb)
		__field(struct cdns3_trb *, start_trb_addr)
		__field(int, flags)
	),
	TP_fast_assign(
		__assign_str(name, req->priv_ep->name);
		__entry->req = req;
		__entry->actual = req->request.actual;
		__entry->length = req->request.length;
		__entry->status = req->request.status;
		__entry->zero = req->request.zero;
		__entry->short_not_ok = req->request.short_not_ok;
		__entry->no_interrupt = req->request.no_interrupt;
		__entry->start_trb = req->start_trb;
		__entry->end_trb = req->end_trb;
		__entry->start_trb_addr = req->trb;
		__entry->flags = req->flags;
	),
	TP_printk("%s: req: %p, length: %u/%u %s%s%s, status: %d,"
		  " trb: [start:%d, end:%d: virt addr %pa], flags:%x ",
		__get_str(name), __entry->req, __entry->actual, __entry->length,
		__entry->zero ? "zero | " : "",
		__entry->short_not_ok ? "short | " : "",
		__entry->no_interrupt ? "no int" : "",
		__entry->status,
		__entry->start_trb,
		__entry->end_trb,
		__entry->start_trb_addr,
		__entry->flags
	)
);

DEFINE_EVENT(cdns3_log_request, cdns3_alloc_request,
	TP_PROTO(struct cdns3_request *req),
	TP_ARGS(req)
);

DEFINE_EVENT(cdns3_log_request, cdns3_free_request,
	TP_PROTO(struct cdns3_request *req),
	TP_ARGS(req)
);

DEFINE_EVENT(cdns3_log_request, cdns3_ep_queue,
	TP_PROTO(struct cdns3_request *req),
	TP_ARGS(req)
);

DEFINE_EVENT(cdns3_log_request, cdns3_ep_dequeue,
	TP_PROTO(struct cdns3_request *req),
	TP_ARGS(req)
);

DEFINE_EVENT(cdns3_log_request, cdns3_gadget_giveback,
	TP_PROTO(struct cdns3_request *req),
	TP_ARGS(req)
);

DECLARE_EVENT_CLASS(cdns3_log_trb,
	TP_PROTO(struct cdns3_endpoint *priv_ep, struct cdns3_trb *trb),
	TP_ARGS(priv_ep, trb),
	TP_STRUCT__entry(
		__string(name, priv_ep->name)
		__field(struct cdns3_trb *, trb)
		__field(u32, buffer)
		__field(u32, length)
		__field(u32, control)
		__field(u32, type)
	),
	TP_fast_assign(
		__assign_str(name, priv_ep->name);
		__entry->trb = trb;
		__entry->buffer = trb->buffer;
		__entry->length = trb->length;
		__entry->control = trb->control;
		__entry->type = usb_endpoint_type(priv_ep->endpoint.desc);
	),
	TP_printk("%s: trb 0x%pa, dma buf: 0x%08x, size: %ld, ctrl: 0x%08x (%s%s%s%s%s%s%s)",
		__get_str(name), __entry->trb, __entry->buffer,
		TRB_LEN(__entry->length), __entry->control,
		__entry->control & TRB_CYCLE ? "C=1, " : "C=0, ",
		__entry->control & TRB_TOGGLE ? "T=1, " : "T=0, ",
		__entry->control & TRB_ISP ? "ISP, " : "",
		__entry->control & TRB_FIFO_MODE ? "FIFO, " : "",
		__entry->control & TRB_CHAIN ? "CHAIN, " : "",
		__entry->control & TRB_IOC ? "IOC, " : "",
		  TRB_FIELD_TO_TYPE(__entry->control) == TRB_NORMAL ? "Normal" : "LINK"
	)
);

DEFINE_EVENT(cdns3_log_trb, cdns3_prepare_trb,
	TP_PROTO(struct cdns3_endpoint *priv_ep, struct cdns3_trb *trb),
	TP_ARGS(priv_ep, trb)
);

DEFINE_EVENT(cdns3_log_trb, cdns3_complete_trb,
	TP_PROTO(struct cdns3_endpoint *priv_ep, struct cdns3_trb *trb),
	TP_ARGS(priv_ep, trb)
);

DECLARE_EVENT_CLASS(cdns3_log_ring,
	TP_PROTO(struct cdns3_endpoint *priv_ep),
	TP_ARGS(priv_ep),
	TP_STRUCT__entry(
		__field(struct cdns3_endpoint *, priv_ep)
		__field(int, free_trbs)
		__field(u8, pcs)
		__field(u8, ccs)
		__field(int, enqueue)
		__field(int, dequeue)
		__dynamic_array(u32, ring, TRB_RING_SIZE)
		__dynamic_array(char, buffer,
				(TRBS_PER_SEGMENT * 65) + CDNS3_MSG_MAX)
	),
	TP_fast_assign(
		__entry->priv_ep = priv_ep;
		__entry->free_trbs = priv_ep->free_trbs;
		__entry->pcs = priv_ep->pcs;
		__entry->ccs = priv_ep->ccs;
		__entry->enqueue = priv_ep->enqueue;
		__entry->dequeue = priv_ep->dequeue;
		memcpy(__get_dynamic_array(ring), priv_ep->trb_pool,
		       TRB_RING_SIZE);
	),

	TP_printk("%s",
		  cdns3_dbg_ring(__entry->priv_ep, __entry->free_trbs,
				 __entry->pcs, __entry->ccs,
				 __entry->enqueue, __entry->dequeue,
				 (struct cdns3_trb *)__get_str(ring),
				 __get_str(buffer)))
);

DEFINE_EVENT(cdns3_log_ring, cdns3_ring,
	TP_PROTO(struct cdns3_endpoint *priv_ep),
	TP_ARGS(priv_ep)
);

DECLARE_EVENT_CLASS(cdns3_log_ep,
	TP_PROTO(struct cdns3_endpoint *priv_ep),
	TP_ARGS(priv_ep),
	TP_STRUCT__entry(
		__string(name, priv_ep->name)
		__field(unsigned int, maxpacket)
		__field(unsigned int, maxpacket_limit)
		__field(unsigned int, max_streams)
		__field(unsigned int, maxburst)
		__field(unsigned int, flags)
		__field(unsigned int, dir)
		__field(u8, enqueue)
		__field(u8, dequeue)
	),
	TP_fast_assign(
		__assign_str(name, priv_ep->name);
		__entry->maxpacket = priv_ep->endpoint.maxpacket;
		__entry->maxpacket_limit = priv_ep->endpoint.maxpacket_limit;
		__entry->max_streams = priv_ep->endpoint.max_streams;
		__entry->maxburst = priv_ep->endpoint.maxburst;
		__entry->flags = priv_ep->flags;
		__entry->dir = priv_ep->dir;
		__entry->enqueue = priv_ep->enqueue;
		__entry->dequeue = priv_ep->dequeue;
	),
	TP_printk("%s: mps: %d/%d. streams: %d, burst: %d, enq idx: %d, "
		  "deq idx: %d, flags %s%s%s%s%s%s%s%s, dir: %s",
		__get_str(name), __entry->maxpacket,
		__entry->maxpacket_limit, __entry->max_streams,
		__entry->maxburst, __entry->enqueue,
		__entry->dequeue,
		__entry->flags & EP_ENABLED ? "EN | " : "",
		__entry->flags & EP_STALL ? "STALL | " : "",
		__entry->flags & EP_WEDGE ? "WEDGE | " : "",
		__entry->flags & EP_TRANSFER_STARTED ? "STARTED | " : "",
		__entry->flags & EP_UPDATE_EP_TRBADDR ? "UPD TRB | " : "",
		__entry->flags & EP_PENDING_REQUEST ? "REQ PEN | " : "",
		__entry->flags & EP_RING_FULL ? "RING FULL |" : "",
		__entry->flags & EP_CLAIMED ?  "CLAIMED " : "",
		__entry->dir ? "IN" : "OUT"
	)
);

DEFINE_EVENT(cdns3_log_ep, cdns3_gadget_ep_enable,
	TP_PROTO(struct cdns3_endpoint *priv_ep),
	TP_ARGS(priv_ep)
);

DEFINE_EVENT(cdns3_log_ep, cdns3_gadget_ep_disable,
	TP_PROTO(struct cdns3_endpoint *priv_ep),
	TP_ARGS(priv_ep)
);
#endif /* __LINUX_CDNS3_TRACE */

/* this part must be outside header guard */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace

#include <trace/define_trace.h>
