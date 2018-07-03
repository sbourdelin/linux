// SPDX-License-Identifier: GPL-2.0
/*
 * USBSSP device controller driver
 *
 * Copyright (C) 2018 Cadence.
 *
 * Author: Pawel Laszczak
 * Code borrowed from the Linux XHCI driver.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM usbssp-dev

/*
 * The TRACE_SYSTEM_VAR defaults to TRACE_SYSTEM, but must be a
 * legitimate C variable. It is not exported to user space.
 */
#undef TRACE_SYSTEM_VAR
#define TRACE_SYSTEM_VAR usbssp_dev

#if !defined(__USBSSP_DEV_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define __USBSSP_DEV_TRACE_H

#include <linux/tracepoint.h>
#include "gadget.h"

#define USBSSP_DEV_MSG_MAX	500

DECLARE_EVENT_CLASS(usbssp_log_msg,
	TP_PROTO(struct va_format *vaf),
	TP_ARGS(vaf),
	TP_STRUCT__entry(__dynamic_array(char, msg, USBSSP_DEV_MSG_MAX)),
	TP_fast_assign(
		vsnprintf(__get_str(msg), USBSSP_DEV_MSG_MAX, vaf->fmt, *vaf->va);
	),
	TP_printk("%s", __get_str(msg))
);

DEFINE_EVENT(usbssp_log_msg, usbssp_dbg_address,
	TP_PROTO(struct va_format *vaf),
	TP_ARGS(vaf)
);

DEFINE_EVENT(usbssp_log_msg, usbssp_dbg_context_change,
	TP_PROTO(struct va_format *vaf),
	TP_ARGS(vaf)
);

DEFINE_EVENT(usbssp_log_msg, usbssp_dbg_quirks,
	TP_PROTO(struct va_format *vaf),
	TP_ARGS(vaf)
);

DEFINE_EVENT(usbssp_log_msg, usbssp_dbg_reset_ep,
	TP_PROTO(struct va_format *vaf),
	TP_ARGS(vaf)
);

DEFINE_EVENT(usbssp_log_msg, usbssp_dbg_cancel_request,
	TP_PROTO(struct va_format *vaf),
	TP_ARGS(vaf)
);

DEFINE_EVENT(usbssp_log_msg, usbssp_dbg_init,
	TP_PROTO(struct va_format *vaf),
	TP_ARGS(vaf)
);

DEFINE_EVENT(usbssp_log_msg, usbssp_dbg_ring_expansion,
	TP_PROTO(struct va_format *vaf),
	TP_ARGS(vaf)
);

DECLARE_EVENT_CLASS(usbssp_log_ctx,
	TP_PROTO(struct usbssp_udc *usbssp_data, struct usbssp_container_ctx *ctx,
		 unsigned int ep_num),
	TP_ARGS(usbssp_data, ctx, ep_num),
	TP_STRUCT__entry(
		__field(int, ctx_64)
		__field(unsigned int, ctx_type)
		__field(dma_addr_t, ctx_dma)
		__field(u8 *, ctx_va)
		__field(unsigned int, ctx_ep_num)
		__field(int, slot_id)
		__dynamic_array(u32, ctx_data,
			((HCC_64BYTE_CONTEXT(usbssp_data->hcc_params) + 1) * 8) *
			((ctx->type == USBSSP_CTX_TYPE_INPUT) + ep_num + 1))
	),
	TP_fast_assign(
		__entry->ctx_64 = HCC_64BYTE_CONTEXT(usbssp_data->hcc_params);
		__entry->ctx_type = ctx->type;
		__entry->ctx_dma = ctx->dma;
		__entry->ctx_va = ctx->bytes;
		__entry->slot_id = usbssp_data->slot_id;
		__entry->ctx_ep_num = ep_num;
		memcpy(__get_dynamic_array(ctx_data), ctx->bytes,
			((HCC_64BYTE_CONTEXT(usbssp_data->hcc_params) + 1) * 32) *
			((ctx->type == USBSSP_CTX_TYPE_INPUT) + ep_num + 1));
	),
	TP_printk("\nctx_64=%d, ctx_type=%u, ctx_dma=@%llx, ctx_va=@%p",
			__entry->ctx_64, __entry->ctx_type,
			(unsigned long long) __entry->ctx_dma, __entry->ctx_va
	)
);

DEFINE_EVENT(usbssp_log_ctx, usbssp_address_ctx,
	TP_PROTO(struct usbssp_udc *usbssp_data, struct usbssp_container_ctx *ctx,
		 unsigned int ep_num),
	TP_ARGS(usbssp_data, ctx, ep_num)
);


DECLARE_EVENT_CLASS(usbssp_log_trb,
	TP_PROTO(struct usbssp_ring *ring, struct usbssp_generic_trb *trb),
	TP_ARGS(ring, trb),
	TP_STRUCT__entry(
		__field(u32, type)
		__field(u32, field0)
		__field(u32, field1)
		__field(u32, field2)
		__field(u32, field3)
	),
	TP_fast_assign(
		__entry->type = ring->type;
		__entry->field0 = le32_to_cpu(trb->field[0]);
		__entry->field1 = le32_to_cpu(trb->field[1]);
		__entry->field2 = le32_to_cpu(trb->field[2]);
		__entry->field3 = le32_to_cpu(trb->field[3]);
	),
	TP_printk("%s: %s", usbssp_ring_type_string(__entry->type),
			usbssp_decode_trb(__entry->field0, __entry->field1,
					__entry->field2, __entry->field3)
	)
);

DEFINE_EVENT(usbssp_log_trb, usbssp_handle_event,
	TP_PROTO(struct usbssp_ring *ring, struct usbssp_generic_trb *trb),
	TP_ARGS(ring, trb)
);

DEFINE_EVENT(usbssp_log_trb, usbssp_handle_command,
	TP_PROTO(struct usbssp_ring *ring, struct usbssp_generic_trb *trb),
	TP_ARGS(ring, trb)
);

DEFINE_EVENT(usbssp_log_trb, usbssp_handle_transfer,
	TP_PROTO(struct usbssp_ring *ring, struct usbssp_generic_trb *trb),
	TP_ARGS(ring, trb)
);

DEFINE_EVENT(usbssp_log_trb, usbssp_queue_trb,
	TP_PROTO(struct usbssp_ring *ring, struct usbssp_generic_trb *trb),
	TP_ARGS(ring, trb)
);

DEFINE_EVENT(usbssp_log_trb, usbssp_dbc_handle_event,
	TP_PROTO(struct usbssp_ring *ring, struct usbssp_generic_trb *trb),
	TP_ARGS(ring, trb)
);

DEFINE_EVENT(usbssp_log_trb, usbssp_dbc_handle_transfer,
	TP_PROTO(struct usbssp_ring *ring, struct usbssp_generic_trb *trb),
	TP_ARGS(ring, trb)
);

DEFINE_EVENT(usbssp_log_trb, usbssp_dbc_gadget_ep_queue,
	TP_PROTO(struct usbssp_ring *ring, struct usbssp_generic_trb *trb),
	TP_ARGS(ring, trb)
);

DECLARE_EVENT_CLASS(usbssp_log_priv_dev,
	TP_PROTO(struct usbssp_device *priv_dev),
	TP_ARGS(priv_dev),
	TP_STRUCT__entry(
		__field(struct usbssp_device *, priv_dev)
		__field(struct usb_gadget *, gadget)
		__field(unsigned long long, out_ctx)
		__field(unsigned long long, in_ctx)
		__field(u8, port_num)
	),
	TP_fast_assign(
		__entry->priv_dev = priv_dev;
		__entry->gadget = priv_dev->gadget;
		__entry->in_ctx = (unsigned long long) priv_dev->in_ctx->dma;
		__entry->out_ctx = (unsigned long long) priv_dev->out_ctx->dma;
		__entry->port_num = priv_dev->port_num;
	),
	TP_printk("priv_dev %p gadget %p ctx %llx | %llx, port %d ",
		__entry->priv_dev, __entry->gadget, __entry->in_ctx, __entry->out_ctx,
		__entry->port_num
	)
);

DEFINE_EVENT(usbssp_log_priv_dev, usbssp_alloc_priv_device,
	TP_PROTO(struct usbssp_device *vdev),
	TP_ARGS(vdev)
);

DEFINE_EVENT(usbssp_log_priv_dev, usbssp_free_priv_device,
	TP_PROTO(struct usbssp_device *vdev),
	TP_ARGS(vdev)
);

DEFINE_EVENT(usbssp_log_priv_dev, usbssp_setup_device,
	TP_PROTO(struct usbssp_device *vdev),
	TP_ARGS(vdev)
);

DEFINE_EVENT(usbssp_log_priv_dev, usbssp_setup_addressable_priv_device,
	TP_PROTO(struct usbssp_device *vdev),
	TP_ARGS(vdev)
);

DEFINE_EVENT(usbssp_log_priv_dev, usbssp_stop_device,
	TP_PROTO(struct usbssp_device *vdev),
	TP_ARGS(vdev)
);

DECLARE_EVENT_CLASS(usbssp_log_request,
	TP_PROTO(struct usb_request *request),
	TP_ARGS(request),
	TP_STRUCT__entry(
		__field(struct usb_request *, request)
		__field(void *, buf)
		__field(unsigned int, length)
		__field(dma_addr_t, dma)
		__field(struct scatterlist*, sg)
		__field(unsigned int, num_sgs)
		__field(unsigned int, num_mapped_sgs)
		__field(unsigned int, stream_id)
		__field(unsigned int, no_interrupt)
		__field(unsigned int, zero)
		__field(unsigned int, short_not_ok)
		__field(unsigned int, dma_mapped)
		__field(int, status)
		__field(unsigned int, actual)
	),
	TP_fast_assign(
		__entry->request = request;
		__entry->buf = request->buf;
		__entry->length = request->length;
		__entry->dma = request->dma;
		__entry->sg = request->sg;
		__entry->num_sgs = request->num_sgs;
		__entry->num_mapped_sgs = request->num_mapped_sgs;
		__entry->stream_id = request->stream_id;
		__entry->no_interrupt = request->no_interrupt;
		__entry->zero = request->zero;
		__entry->short_not_ok = request->short_not_ok;
		__entry->dma_mapped = 0 /*request->dma_mapped*/;
		__entry->status = request->status;
		__entry->actual = request->actual;
	),

	TP_printk("req %p;  buf %p, len %d, dma %llx, sg %p, num_sg %d, num_m_sg %d,"
			"stream_id %d, no_int %x, zero %x, short_not_ok %x, dma_mapped %x, "
			"status %d, actual %d",
			__entry->request,
			__entry->buf, __entry->length, __entry->dma, __entry->sg,
			__entry->num_sgs, __entry->num_mapped_sgs, __entry->stream_id,
			__entry->no_interrupt, __entry->zero, __entry->short_not_ok,
			__entry->dma_mapped, __entry->status, __entry->actual
		)

);

DEFINE_EVENT(usbssp_log_request, usbssp_request_enqueue,
	TP_PROTO(struct usb_request *request),
	TP_ARGS(request)
);

DEFINE_EVENT(usbssp_log_request, usbssp_request_giveback,
	TP_PROTO(struct usb_request *request),
	TP_ARGS(request)
);

DEFINE_EVENT(usbssp_log_request, usbssp_request_dequeue,
	TP_PROTO(struct usb_request *request),
	TP_ARGS(request)
);

DEFINE_EVENT(usbssp_log_request, usbssp_alloc_request,
	TP_PROTO(struct usb_request *request),
	TP_ARGS(request)
);

DEFINE_EVENT(usbssp_log_request, usbssp_free_request,
	TP_PROTO(struct usb_request *request),
	TP_ARGS(request)
);

DECLARE_EVENT_CLASS(usbssp_log_ep_ctx,
	TP_PROTO(struct usbssp_ep_ctx *ctx),
	TP_ARGS(ctx),
	TP_STRUCT__entry(
		__field(u32, info)
		__field(u32, info2)
		__field(u64, deq)
		__field(u32, tx_info)
	),
	TP_fast_assign(
		__entry->info = le32_to_cpu(ctx->ep_info);
		__entry->info2 = le32_to_cpu(ctx->ep_info2);
		__entry->deq = le64_to_cpu(ctx->deq);
		__entry->tx_info = le32_to_cpu(ctx->tx_info);
	),
	TP_printk("%s",  usbssp_decode_ep_context(__entry->info,
		__entry->info2, __entry->deq, __entry->tx_info)
	)
);

DEFINE_EVENT(usbssp_log_ep_ctx, usbssp_remove_request,
	TP_PROTO(struct usbssp_ep_ctx *ctx),
	TP_ARGS(ctx)
);

DEFINE_EVENT(usbssp_log_ep_ctx, usbssp_handle_cmd_stop_ep,
	TP_PROTO(struct usbssp_ep_ctx *ctx),
	TP_ARGS(ctx)
);

DEFINE_EVENT(usbssp_log_ep_ctx, usbssp_handle_cmd_set_deq_ep,
	TP_PROTO(struct usbssp_ep_ctx *ctx),
	TP_ARGS(ctx)
);

DEFINE_EVENT(usbssp_log_ep_ctx, usbssp_handle_cmd_reset_ep,
	TP_PROTO(struct usbssp_ep_ctx *ctx),
	TP_ARGS(ctx)
);

DEFINE_EVENT(usbssp_log_ep_ctx, usbssp_handle_cmd_config_ep,
	TP_PROTO(struct usbssp_ep_ctx *ctx),
	TP_ARGS(ctx)
);

DECLARE_EVENT_CLASS(usbssp_log_slot_ctx,
	TP_PROTO(struct usbssp_slot_ctx *ctx),
	TP_ARGS(ctx),
	TP_STRUCT__entry(
		__field(u32, info)
		__field(u32, info2)
		__field(u32, int_target)
		__field(u32, state)
	),
	TP_fast_assign(
		__entry->info = le32_to_cpu(ctx->dev_info);
		__entry->info2 = le32_to_cpu(ctx->dev_info2);
		__entry->int_target = le64_to_cpu(ctx->int_target);
		__entry->state = le32_to_cpu(ctx->dev_state);
	),
	TP_printk("%s", usbssp_decode_slot_context(__entry->info,
			__entry->info2, __entry->int_target,
			__entry->state)
	)
);

DEFINE_EVENT(usbssp_log_slot_ctx, usbssp_alloc_dev,
	TP_PROTO(struct usbssp_slot_ctx *ctx),
	TP_ARGS(ctx)
);

DEFINE_EVENT(usbssp_log_slot_ctx, usbssp_free_dev,
	TP_PROTO(struct usbssp_slot_ctx *ctx),
	TP_ARGS(ctx)
);

DEFINE_EVENT(usbssp_log_slot_ctx, usbssp_handle_cmd_disable_slot,
	TP_PROTO(struct usbssp_slot_ctx *ctx),
	TP_ARGS(ctx)
);

DEFINE_EVENT(usbssp_log_slot_ctx, usbssp_reset_device,
	TP_PROTO(struct usbssp_slot_ctx *ctx),
	TP_ARGS(ctx)
);

DEFINE_EVENT(usbssp_log_slot_ctx, usbssp_setup_device_slot,
	TP_PROTO(struct usbssp_slot_ctx *ctx),
	TP_ARGS(ctx)
);

DEFINE_EVENT(usbssp_log_slot_ctx, usbssp_handle_cmd_addr_dev,
	TP_PROTO(struct usbssp_slot_ctx *ctx),
	TP_ARGS(ctx)
);

DEFINE_EVENT(usbssp_log_slot_ctx, usbssp_handle_cmd_reset_dev,
	TP_PROTO(struct usbssp_slot_ctx *ctx),
	TP_ARGS(ctx)
);

DEFINE_EVENT(usbssp_log_slot_ctx, usbssp_handle_cmd_set_deq,
	TP_PROTO(struct usbssp_slot_ctx *ctx),
	TP_ARGS(ctx)
);

DEFINE_EVENT(usbssp_log_slot_ctx, usbssp_configure_endpoint,
	TP_PROTO(struct usbssp_slot_ctx *ctx),
	TP_ARGS(ctx)
);

DECLARE_EVENT_CLASS(usbssp_log_ring,
	TP_PROTO(struct usbssp_ring *ring),
	TP_ARGS(ring),
	TP_STRUCT__entry(
		__field(u32, type)
		__field(void *, ring)
		__field(dma_addr_t, enq)
		__field(dma_addr_t, deq)
		__field(dma_addr_t, enq_seg)
		__field(dma_addr_t, deq_seg)
		__field(unsigned int, num_segs)
		__field(unsigned int, stream_id)
		__field(unsigned int, cycle_state)
		__field(unsigned int, num_trbs_free)
		__field(unsigned int, bounce_buf_len)
	),
	TP_fast_assign(
		__entry->ring = ring;
		__entry->type = ring->type;
		__entry->num_segs = ring->num_segs;
		__entry->stream_id = ring->stream_id;
		__entry->enq_seg = ring->enq_seg->dma;
		__entry->deq_seg = ring->deq_seg->dma;
		__entry->cycle_state = ring->cycle_state;
		__entry->num_trbs_free = ring->num_trbs_free;
		__entry->bounce_buf_len = ring->bounce_buf_len;
		__entry->enq = usbssp_trb_virt_to_dma(ring->enq_seg, ring->enqueue);
		__entry->deq = usbssp_trb_virt_to_dma(ring->deq_seg, ring->dequeue);
	),
	TP_printk("%s %p: enq %pad(%pad) deq %pad(%pad) segs %d stream %d free_trbs %d bounce %d cycle %d",
			usbssp_ring_type_string(__entry->type), __entry->ring,
			&__entry->enq, &__entry->enq_seg,
			&__entry->deq, &__entry->deq_seg,
			__entry->num_segs,
			__entry->stream_id,
			__entry->num_trbs_free,
			__entry->bounce_buf_len,
			__entry->cycle_state
		)
);

DEFINE_EVENT(usbssp_log_ring, usbssp_ring_alloc,
	TP_PROTO(struct usbssp_ring *ring),
	TP_ARGS(ring)
);

DEFINE_EVENT(usbssp_log_ring, usbssp_ring_free,
	TP_PROTO(struct usbssp_ring *ring),
	TP_ARGS(ring)
);

DEFINE_EVENT(usbssp_log_ring, usbssp_ring_expansion,
	TP_PROTO(struct usbssp_ring *ring),
	TP_ARGS(ring)
);

DEFINE_EVENT(usbssp_log_ring, usbssp_inc_enq,
	TP_PROTO(struct usbssp_ring *ring),
	TP_ARGS(ring)
);

DEFINE_EVENT(usbssp_log_ring, usbssp_inc_deq,
	TP_PROTO(struct usbssp_ring *ring),
	TP_ARGS(ring)
);

DECLARE_EVENT_CLASS(usbssp_log_portsc,
		TP_PROTO(u32 portnum, u32 portsc),
		TP_ARGS(portnum, portsc),
		TP_STRUCT__entry(
				__field(u32, portnum)
				__field(u32, portsc)
				),
		TP_fast_assign(
				__entry->portnum = portnum;
				__entry->portsc = portsc;
				),
		TP_printk("port-%d: %s",
			__entry->portnum,
			usbssp_decode_portsc(__entry->portsc)
			)
);

DEFINE_EVENT(usbssp_log_portsc, usbssp_handle_port_status,
		TP_PROTO(u32 portnum, u32 portsc),
		TP_ARGS(portnum, portsc)
);

DEFINE_EVENT(usbssp_log_portsc, usbssp_get_port_status,
	TP_PROTO(u32 portnum, u32 portsc),
	TP_ARGS(portnum, portsc)
);

#endif /* __USBSSP_TRACE_H */
/* this part must be outside header guard */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE gadget-trace

#include <trace/define_trace.h>
