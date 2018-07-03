// SPDX-License-Identifier: GPL-2.0
/*
 * USBSSP device controller driver
 *
 * Copyright (C) 2018 Cadence.
 *
 * Author: Pawel Laszczak
 * Code borrowed from the Linux XHCI driver.
 */

/*
 * Ring initialization rules:
 * 1. Each segment is initialized to zero, except for link TRBs.
 * 2. Ring cycle state = 0.  This represents Producer Cycle State (PCS) or
 *    Consumer Cycle State (CCS), depending on ring function.
 * 3. Enqueue pointer = dequeue pointer = address of first TRB in the segment.
 *
 * Ring behavior rules:
 * 1. A ring is empty if enqueue == dequeue.  This means there will always be at
 *    least one free TRB in the ring.  This is useful if you want to turn that
 *    into a link TRB and expand the ring.
 * 2. When incrementing an enqueue or dequeue pointer, if the next TRB is a
 *    link TRB, then load the pointer with the address in the link TRB.  If the
 *    link TRB had its toggle bit set, you may need to update the ring cycle
 *    state (see cycle bit rules).  You may have to do this multiple times
 *    until you reach a non-link TRB.
 * 3. A ring is full if enqueue++ (for the definition of increment above)
 *    equals the dequeue pointer.
 *
 * Cycle bit rules:
 * 1. When a consumer increments a dequeue pointer and encounters a toggle bit
 *    in a link TRB, it must toggle the ring cycle state.
 * 2. When a producer increments an enqueue pointer and encounters a toggle bit
 *    in a link TRB, it must toggle the ring cycle state.
 *
 * Producer rules:
 * 1. Check if ring is full before you enqueue.
 * 2. Write the ring cycle state to the cycle bit in the TRB you're enqueuing.
 *    Update enqueue pointer between each write (which may update the ring
 *    cycle state).
 * 3. Notify consumer.  If SW is producer, it rings the doorbell for command
 *    and endpoint rings.  If DC is the producer for the event ring,
 *    and it generates an interrupt according to interrupt modulation rules.
 *
 * Consumer rules:
 * 1. Check if TRB belongs to you.  If the cycle bit == your ring cycle state,
 *    the TRB is owned by the consumer.
 * 2. Update dequeue pointer (which may update the ring cycle state) and
 *    continue processing TRBs until you reach a TRB which is not owned by you.
 * 3. Notify the producer.  SW is the consumer for the event ring, and it
 *   updates event ring dequeue pointer.  DC is the consumer for the command and
 *   endpoint rings; it generates events on the event ring for these.
 */

#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include "gadget-trace.h"
#include "gadget.h"

static void giveback_first_trb(struct usbssp_udc *usbssp_data,
			       unsigned int ep_index,
			       unsigned int stream_id,
			       int start_cycle,
			       struct usbssp_generic_trb
			       *start_trb);
/*
 * Returns zero if the TRB isn't in this segment, otherwise it returns the DMA
 * address of the TRB.
 */
dma_addr_t usbssp_trb_virt_to_dma(struct usbssp_segment *seg,
				  union usbssp_trb *trb)
{
	unsigned long segment_offset;

	if (!seg || !trb || trb < seg->trbs)
		return 0;
	/* offset in TRBs */
	segment_offset = trb - seg->trbs;
	if (segment_offset >= TRBS_PER_SEGMENT)
		return 0;
	return seg->dma + (segment_offset * sizeof(*trb));
}

static bool trb_is_noop(union usbssp_trb *trb)
{
	return TRB_TYPE_NOOP_LE32(trb->generic.field[3]);
}

static bool trb_is_link(union usbssp_trb *trb)
{
	return TRB_TYPE_LINK_LE32(trb->link.control);
}

static bool last_trb_on_seg(struct usbssp_segment *seg, union usbssp_trb *trb)
{
	return trb == &seg->trbs[TRBS_PER_SEGMENT - 1];
}

static bool last_trb_on_ring(struct usbssp_ring *ring,
			     struct usbssp_segment *seg,
			     union usbssp_trb *trb)
{
	return last_trb_on_seg(seg, trb) && (seg->next == ring->first_seg);
}

static bool link_trb_toggles_cycle(union usbssp_trb *trb)
{
	return le32_to_cpu(trb->link.control) & LINK_TOGGLE;
}

static bool last_td_in_request(struct usbssp_td *td)
{
	struct usbssp_request *req_priv = td->priv_request;

	return req_priv->num_tds_done == req_priv->num_tds;
}

static void inc_td_cnt(struct usbssp_request *priv_req)
{
	priv_req->num_tds_done++;
}

static void trb_to_noop(union usbssp_trb *trb, u32 noop_type)
{
	if (trb_is_link(trb)) {
		/* unchain chained link TRBs */
		trb->link.control &= cpu_to_le32(~TRB_CHAIN);
	} else {
		trb->generic.field[0] = 0;
		trb->generic.field[1] = 0;
		trb->generic.field[2] = 0;
		/* Preserve only the cycle bit of this TRB */
		trb->generic.field[3] &= cpu_to_le32(TRB_CYCLE);
		trb->generic.field[3] |= cpu_to_le32(TRB_TYPE(noop_type));
	}
}

/* Updates trb to point to the next TRB in the ring, and updates seg if the next
 * TRB is in a new segment.  This does not skip over link TRBs, and it does not
 * effect the ring dequeue or enqueue pointers.
 */
static void next_trb(struct usbssp_udc *usbssp_data,
		     struct usbssp_ring *ring,
		     struct usbssp_segment **seg,
		     union usbssp_trb **trb)
{
	if (trb_is_link(*trb)) {
		*seg = (*seg)->next;
		*trb = ((*seg)->trbs);
	} else {
		(*trb)++;
	}
}

/*
 * See Cycle bit rules. SW is the consumer for the event ring only.
 * Don't make a ring full of link TRBs.  That would be dumb and this would loop.
 */
void inc_deq(struct usbssp_udc *usbssp_data, struct usbssp_ring *ring)
{
	/* event ring doesn't have link trbs, check for last trb */
	if (ring->type == TYPE_EVENT) {
		if (!last_trb_on_seg(ring->deq_seg, ring->dequeue)) {
			ring->dequeue++;
			goto out;
		}
		if (last_trb_on_ring(ring, ring->deq_seg, ring->dequeue))
			ring->cycle_state ^= 1;
		ring->deq_seg = ring->deq_seg->next;
		ring->dequeue = ring->deq_seg->trbs;
		goto out;
	}

	/* All other rings have link trbs */
	if (!trb_is_link(ring->dequeue)) {
		ring->dequeue++;
		ring->num_trbs_free++;
	}
	while (trb_is_link(ring->dequeue)) {
		ring->deq_seg = ring->deq_seg->next;
		ring->dequeue = ring->deq_seg->trbs;
	}
out:
	trace_usbssp_inc_deq(ring);
}

/*
 * See Cycle bit rules. SW is the consumer for the event ring only.
 * Don't make a ring full of link TRBs.  That would be dumb and this would loop.
 *
 * If we've just enqueued a TRB that is in the middle of a TD (meaning the
 * chain bit is set), then set the chain bit in all the following link TRBs.
 * If we've enqueued the last TRB in a TD, make sure the following link TRBs
 * have their chain bit cleared (so that each Link TRB is a separate TD).
 *
 * @more_trbs_coming:	Will you enqueue more TRBs before calling
 *			prepare_transfer()?
 */
static void inc_enq(struct usbssp_udc *usbssp_data,
		    struct usbssp_ring *ring,
		    bool more_trbs_coming)
{
	u32 chain;
	union usbssp_trb *next;

	chain = le32_to_cpu(ring->enqueue->generic.field[3]) & TRB_CHAIN;
	/* If this is not event ring, there is one less usable TRB */
	if (!trb_is_link(ring->enqueue))
		ring->num_trbs_free--;
	next = ++(ring->enqueue);

	/* Update the dequeue pointer further if that was a link TRB */
	while (trb_is_link(next)) {

		/*
		 * If the caller doesn't plan on enqueueing more TDs before
		 * ringing the doorbell, then we don't want to give the link TRB
		 * to the hardware just yet. We'll give the link TRB back in
		 * prepare_ring() just before we enqueue the TD at the top of
		 * the ring.
		 */
		if (!chain && !more_trbs_coming)
			break;

		next->link.control &= cpu_to_le32(~TRB_CHAIN);
		next->link.control |= cpu_to_le32(chain);

		/* Give this link TRB to the hardware */
		wmb();
		next->link.control ^= cpu_to_le32(TRB_CYCLE);

		/* Toggle the cycle bit after the last ring segment. */
		if (link_trb_toggles_cycle(next))
			ring->cycle_state ^= 1;

		ring->enq_seg = ring->enq_seg->next;
		ring->enqueue = ring->enq_seg->trbs;
		next = ring->enqueue;
	}
	trace_usbssp_inc_enq(ring);
}

/*
 * Check to see if there's room to enqueue num_trbs on the ring and make sure
 * enqueue pointer will not advance into dequeue segment. See rules above.
 */
static inline int room_on_ring(struct usbssp_udc *usbssp_data,
			       struct usbssp_ring *ring,
			       unsigned int num_trbs)
{
	int num_trbs_in_deq_seg;

	if (ring->num_trbs_free < num_trbs)
		return 0;

	if (ring->type != TYPE_COMMAND && ring->type != TYPE_EVENT) {
		num_trbs_in_deq_seg = ring->dequeue - ring->deq_seg->trbs;

		if (ring->num_trbs_free < num_trbs + num_trbs_in_deq_seg)
			return 0;
	}

	return 1;
}

/* Ring the device controller doorbell after placing a command on the ring */
void usbssp_ring_cmd_db(struct usbssp_udc *usbssp_data)
{
	if (!(usbssp_data->cmd_ring_state & CMD_RING_STATE_RUNNING))
		return;

	usbssp_dbg(usbssp_data, "// Ding dong command ring!\n");
	writel(DB_VALUE_CMD, &usbssp_data->dba->doorbell[0]);
	/* Flush PCI posted writes */
	readl(&usbssp_data->dba->doorbell[0]);
}

static bool usbssp_mod_cmd_timer(struct usbssp_udc *usbssp_data,
				  unsigned long delay)
{
	return mod_delayed_work(system_wq, &usbssp_data->cmd_timer, delay);
	return 0;
}

static struct usbssp_command *usbssp_next_queued_cmd(
		struct usbssp_udc *usbssp_data)
{
	return list_first_entry_or_null(&usbssp_data->cmd_list,
					struct usbssp_command,
					cmd_list);
}

/*
 * Turn all commands on command ring with status set to "aborted" to no-op trbs.
 * If there are other commands waiting then restart the ring and kick the timer.
 * This must be called with command ring stopped and usbssp_data->lock held.
 */
static void usbssp_handle_stopped_cmd_ring(struct usbssp_udc *usbssp_data,
					   struct usbssp_command *cur_cmd)
{
	struct usbssp_command *i_cmd;

	/* Turn all aborted commands in list to no-ops, then restart */
	list_for_each_entry(i_cmd, &usbssp_data->cmd_list, cmd_list) {

		if (i_cmd->status != COMP_COMMAND_ABORTED)
			continue;

		i_cmd->status = COMP_COMMAND_RING_STOPPED;

		usbssp_dbg(usbssp_data, "Turn aborted command %p to no-op\n",
			 i_cmd->command_trb);

		trb_to_noop(i_cmd->command_trb, TRB_CMD_NOOP);

		/*
		 * caller waiting for completion is called when command
		 *  completion event is received for these no-op commands
		 */
	}

	usbssp_data->cmd_ring_state = CMD_RING_STATE_RUNNING;

	/* ring command ring doorbell to restart the command ring */
	if ((usbssp_data->cmd_ring->dequeue != usbssp_data->cmd_ring->enqueue) &&
	    !(usbssp_data->usbssp_state & USBSSP_STATE_DYING)) {
		usbssp_data->current_cmd = cur_cmd;
		usbssp_mod_cmd_timer(usbssp_data, USBSSP_CMD_DEFAULT_TIMEOUT);
		usbssp_ring_cmd_db(usbssp_data);
	}
}

/* Must be called with usbssp_data->lock held, releases and aquires lock back */
static int usbssp_abort_cmd_ring(struct usbssp_udc *usbssp_data,
				 unsigned long flags)
{
	u64 temp_64;
	int ret;

	usbssp_dbg(usbssp_data, "Abort command ring\n");
	reinit_completion(&usbssp_data->cmd_ring_stop_completion);

	temp_64 = usbssp_read_64(usbssp_data, &usbssp_data->op_regs->cmd_ring);
	usbssp_write_64(usbssp_data, temp_64 | CMD_RING_ABORT,
			&usbssp_data->op_regs->cmd_ring);

	/* Spec says software should also time the
	 * completion of the Command Abort operation. If CRR is not negated in 5
	 * seconds then driver handles it as if device died (-ENODEV).
	 */
	ret = usbssp_handshake(&usbssp_data->op_regs->cmd_ring,
			CMD_RING_RUNNING, 0, 5 * 1000 * 1000);

	if (ret < 0) {
		usbssp_err(usbssp_data,
				"Abort failed to stop command ring: %d\n", ret);
		usbssp_halt(usbssp_data);
		usbssp_udc_died(usbssp_data);
		return ret;
	}

	/*
	 * Writing the CMD_RING_ABORT bit should cause a cmd completion event,
	 * Wait 2 secs (arbitrary number).
	 */
	spin_unlock_irqrestore(&usbssp_data->lock, flags);
	ret = wait_for_completion_timeout(
			&usbssp_data->cmd_ring_stop_completion,
			msecs_to_jiffies(2000));
	spin_lock_irqsave(&usbssp_data->lock, flags);
	if (!ret) {
		usbssp_dbg(usbssp_data,
				"No stop event for abort, ring start fail?\n");
		usbssp_cleanup_command_queue(usbssp_data);
	} else {
		usbssp_handle_stopped_cmd_ring(usbssp_data,
				usbssp_next_queued_cmd(usbssp_data));
	}
	return 0;
}

void usbssp_ring_ep_doorbell(struct usbssp_udc *usbssp_data,
		unsigned int ep_index,
		unsigned int stream_id)
{
	__le32 __iomem *db_addr =
			&usbssp_data->dba->doorbell[usbssp_data->slot_id];
	struct usbssp_ep *ep = &usbssp_data->devs.eps[ep_index];
	unsigned int ep_state = ep->ep_state;
	unsigned int db_value;
	/* Don't ring the doorbell for this endpoint if there are pending
	 * cancellations because we don't want to interrupt processing.
	 * We don't want to restart any stream rings if there's a set dequeue
	 * pointer command pending because the device can choose to start any
	 * stream once the endpoint is on the HW schedule.
	 * Also we don't want restart any endpoint if endpoint is halted or
	 * disabled and also if endpoint disabling is pending.
	 */
	if ((ep_state & EP_STOP_CMD_PENDING) ||
	    (ep_state & SET_DEQ_PENDING) ||
	    (ep_state & EP_HALTED) ||
	    !(ep_state & USBSSP_EP_ENABLED) ||
	    (ep_state & USBSSP_EP_DISABLE_PENDING))
		return;

	if (ep_index == 0 && !usbssp_data->ep0_expect_in &&
	   usbssp_data->ep0state == USBSSP_EP0_DATA_PHASE)
		db_value = DB_VALUE_EP0_OUT(ep_index, stream_id);
	else
		db_value = DB_VALUE(ep_index, stream_id);

	usbssp_dbg(usbssp_data, "// Ding dong transfer ring for %s!"
			" - [DB addr/DB val]: [%p/%08x]\n",
			usbssp_data->devs.eps[ep_index].name, db_addr,
			db_value);

	writel(db_value, db_addr);
	/* The CPU has better things to do at this point than wait for a
	 * write-posting flush.  It'll get there soon enough.
	 */
}

/* Ring the doorbell for any rings with pending USB requests */
static void ring_doorbell_for_active_rings(struct usbssp_udc *usbssp_data,
					   unsigned int ep_index)
{
	unsigned int stream_id;
	struct usbssp_ep *ep;

	ep = &usbssp_data->devs.eps[ep_index];

	usbssp_dbg(usbssp_data, "Ring all active ring for %s\n",
		ep->name);

	/* A ring has pending Request if its TD list is not empty */
	if (!(ep->ep_state & EP_HAS_STREAMS)) {
		if (ep->ring && !(list_empty(&ep->ring->td_list)))
			usbssp_ring_ep_doorbell(usbssp_data, ep_index, 0);
		return;
	}

	for (stream_id = 1; stream_id < ep->stream_info->num_streams;
			stream_id++) {
		struct usbssp_stream_info *stream_info = ep->stream_info;

		if (!list_empty(&stream_info->stream_rings[stream_id]->td_list))
			usbssp_ring_ep_doorbell(usbssp_data,  ep_index,
					stream_id);
	}
}

/* Get the right ring for the given ep_index and stream_id.
 * If the endpoint supports streams, boundary check the USB request's stream ID.
 * If the endpoint doesn't support streams, return the singular endpoint ring.
 */
struct usbssp_ring *usbssp_triad_to_transfer_ring(
						struct usbssp_udc *usbssp_data,
						unsigned int ep_index,
						unsigned int stream_id)
{
	struct usbssp_ep *ep;

	ep = &usbssp_data->devs.eps[ep_index];

	/* Common case: no streams */
	if (!(ep->ep_state & EP_HAS_STREAMS))
		return ep->ring;

	if (stream_id == 0) {
		usbssp_warn(usbssp_data,
				"WARN: ep index %u has streams, "
				"but USB Request has no stream ID.\n",
				 ep_index);
		return NULL;
	}

	if (stream_id < ep->stream_info->num_streams)
		return ep->stream_info->stream_rings[stream_id];

	usbssp_warn(usbssp_data,
			"WARN: ep index %u has "
			"stream IDs 1 to %u allocated, "
			"but stream ID %u is requested.\n",
			ep_index,
			ep->stream_info->num_streams - 1,
			stream_id);
	return NULL;
}


/*
 * Get the hw dequeue pointer DC stopped on, either directly from the
 * endpoint context, or if streams are in use from the stream context.
 * The returned hw_dequeue contains the lowest four bits with cycle state
 * and possbile stream context type.
 */
/*static*/ u64 usbssp_get_hw_deq(struct usbssp_udc *usbssp_data,
				 struct usbssp_device *dev,
				 unsigned int ep_index,
				 unsigned int stream_id)
{
	struct usbssp_ep_ctx *ep_ctx;
	struct usbssp_stream_ctx *st_ctx;
	struct usbssp_ep *ep;

	ep = &dev->eps[ep_index];

	if (ep->ep_state & EP_HAS_STREAMS) {
		st_ctx = &ep->stream_info->stream_ctx_array[stream_id];
		return le64_to_cpu(st_ctx->stream_ring);
	}
	ep_ctx = usbssp_get_ep_ctx(usbssp_data, dev->out_ctx, ep_index);
	return le64_to_cpu(ep_ctx->deq);
}

/*
 * Move the DC endpoint ring dequeue pointer past cur_td.
 * Record the new state of the DC endpoint ring dequeue segment,
 * dequeue pointer, and new consumer cycle state in state.
 * Update our internal representation of the ring's dequeue pointer.
 *
 * We do this in three jumps:
 *  - First we update our new ring state to be the same as when the DC stopped.
 *  - Then we traverse the ring to find the segment that contains
 *    the last TRB in the TD.  We toggle the DC new cycle state when we pass
 *    any link TRBs with the toggle cycle bit set.
 *  - Finally we move the dequeue state one TRB further, toggling the cycle bit
 *    if we've moved it past a link TRB with the toggle cycle bit set.
 */
void usbssp_find_new_dequeue_state(struct usbssp_udc *usbssp_data,
				   unsigned int ep_index,
				   unsigned int stream_id,
				   struct usbssp_td *cur_td,
				   struct usbssp_dequeue_state *state)
{
	struct usbssp_device *dev_priv = &usbssp_data->devs;
	struct usbssp_ep *ep_priv = &dev_priv->eps[ep_index];
	struct usbssp_ring *ep_ring;
	struct usbssp_segment *new_seg;
	union usbssp_trb *new_deq;
	dma_addr_t addr;
	u64 hw_dequeue;
	bool cycle_found = false;
	bool td_last_trb_found = false;

	ep_ring = usbssp_triad_to_transfer_ring(usbssp_data,
			ep_index, stream_id);
	if (!ep_ring) {
		usbssp_warn(usbssp_data, "WARN can't find new dequeue state "
				"for invalid stream ID %u.\n",
				stream_id);
		return;
	}

	/* Dig out the cycle state saved by the DC during the stop ep cmd */
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_cancel_request,
			"Finding endpoint context");

	hw_dequeue = usbssp_get_hw_deq(usbssp_data, dev_priv,
			ep_index, stream_id);
	new_seg = ep_ring->deq_seg;
	new_deq = ep_ring->dequeue;
	state->new_cycle_state = hw_dequeue & 0x1;
	state->stream_id = stream_id;

	/*
	 * We want to find the pointer, segment and cycle state of the new trb
	 * (the one after current TD's last_trb). We know the cycle state at
	 * hw_dequeue, so walk the ring until both hw_dequeue and last_trb are
	 * found.
	 */
	do {
		if (!cycle_found && usbssp_trb_virt_to_dma(new_seg, new_deq)
		    == (dma_addr_t)(hw_dequeue & ~0xf)) {
			cycle_found = true;
			if (td_last_trb_found)
				break;
		}

		if (new_deq == cur_td->last_trb)
			td_last_trb_found = true;

		if (cycle_found && trb_is_link(new_deq) &&
			link_trb_toggles_cycle(new_deq))
			state->new_cycle_state ^= 0x1;

		next_trb(usbssp_data, ep_ring, &new_seg, &new_deq);

		/* Search wrapped around, bail out */
		if (new_deq == ep_priv->ring->dequeue) {
			usbssp_err(usbssp_data,
				"Error: Failed finding new dequeue state\n");
			state->new_deq_seg = NULL;
			state->new_deq_ptr = NULL;
			return;
		}

	} while (!cycle_found || !td_last_trb_found);

	state->new_deq_seg = new_seg;
	state->new_deq_ptr = new_deq;

	/* Don't update the ring cycle state for the producer (us). */
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_cancel_request,
			"Cycle state = 0x%x", state->new_cycle_state);

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_cancel_request,
			"New dequeue segment = %p (virtual)",
			state->new_deq_seg);
	addr = usbssp_trb_virt_to_dma(state->new_deq_seg, state->new_deq_ptr);
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_cancel_request,
			"New dequeue pointer = 0x%llx (DMA)",
			(unsigned long long) addr);
}

/* flip_cycle means flip the cycle bit of all but the first and last TRB.
 * (The last TRB actually points to the ring enqueue pointer, which is not part
 * of this TD.)  This is used to remove partially enqueued isoc TDs from a ring.
 */
static void td_to_noop(struct usbssp_udc *usbssp_data,
		       struct usbssp_ring *ep_ring,
		       struct usbssp_td *td, bool flip_cycle)
{
	struct usbssp_segment *seg = td->start_seg;
	union usbssp_trb *trb = td->first_trb;

	while (1) {
		trb_to_noop(trb, TRB_TR_NOOP);

		/* flip cycle if asked to */
		if (flip_cycle && trb != td->first_trb && trb != td->last_trb)
			trb->generic.field[3] ^= cpu_to_le32(TRB_CYCLE);

		if (trb == td->last_trb)
			break;

		next_trb(usbssp_data, ep_ring, &seg, &trb);
	}
}

/* Must be called with usbssp_data->lock held in interrupt context
 * or usbssp_data->irq_thread_lock from thread conext (defered interrupt)
 */
void usbssp_giveback_request_in_irq(struct usbssp_udc *usbssp_data,
				    struct usbssp_td *cur_td,
				    int status)
{
	struct usb_request	*req;
	struct usbssp_request	*req_priv;

	req_priv = cur_td->priv_request;
	req = &req_priv->request;

	usbssp_request_free_priv(req_priv);

	usbssp_gadget_giveback(req_priv->dep, req_priv, status);
}

void usbssp_unmap_td_bounce_buffer(struct usbssp_udc *usbssp_data,
				   struct usbssp_ring *ring,
				   struct usbssp_td *td)
{
	/*TODO: ??? */
}

void usbssp_remove_request(struct usbssp_udc *usbssp_data,
			   struct usbssp_request *req_priv, int ep_index)
{
	int i = 0;
	struct usbssp_ring *ep_ring;
	struct usbssp_ep *ep;
	struct usbssp_td *cur_td = NULL;
	struct usbssp_ep_ctx *ep_ctx;
	struct usbssp_device *priv_dev;
	u64 hw_deq;
	struct usbssp_dequeue_state deq_state;

	memset(&deq_state, 0, sizeof(deq_state));
	ep = &usbssp_data->devs.eps[ep_index];

	priv_dev = &usbssp_data->devs;
	ep_ctx = usbssp_get_ep_ctx(usbssp_data, priv_dev->out_ctx, ep_index);
	trace_usbssp_remove_request(ep_ctx);
	/*
	 * We have the DC lock and disabled interrupt, so nothing can modify
	 * this list until we drop it.
	 */

	i = req_priv->num_tds_done;

	for (; i < req_priv->num_tds; i++) {
		cur_td = &req_priv->td[i];
		usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_cancel_request,
			"Removing canceled TD starting at 0x%llx (dma).",
			(unsigned long long)usbssp_trb_virt_to_dma(
					cur_td->start_seg, cur_td->first_trb));

		ep_ring = usbssp_request_to_transfer_ring(usbssp_data,
				cur_td->priv_request);

		if (!ep_ring) {
			/* This shouldn't happen unless a driver is mucking
			 * with the stream ID after submission.  This will
			 * leave the TD on the hardware ring, and the hardware
			 * will try to execute it, and may access a buffer
			 * that has already been freed.  In the best case, the
			 * hardware will execute it, and the event handler will
			 * ignore the completion event for that TD, since it was
			 * removed from the td_list for that endpoint.  In
			 * short, don't muck with the stream ID after
			 * submission.
			 */
			usbssp_warn(usbssp_data, "WARN Cancelled USB Request %p"
					" has invalid stream ID %u.\n",
					cur_td->priv_request,
					cur_td->priv_request->request.stream_id);
			goto remove_finished_td;
		}

		if (!(ep->ep_state & USBSSP_EP_ENABLED) ||
		   ep->ep_state & USBSSP_EP_DISABLE_PENDING) {
			goto remove_finished_td;
		}

		/*
		 * If we stopped on the TD we need to cancel, then we have to
		 * move the DC endpoint ring dequeue pointer past this TD.
		 */
		hw_deq = usbssp_get_hw_deq(usbssp_data, priv_dev, ep_index,
				cur_td->priv_request->request.stream_id);
		hw_deq &= ~0xf;

		if (usbssp_trb_in_td(usbssp_data, cur_td->start_seg,
			cur_td->first_trb, cur_td->last_trb, hw_deq, false)) {
			usbssp_find_new_dequeue_state(usbssp_data, ep_index,
					cur_td->priv_request->request.stream_id,
					cur_td, &deq_state);
		} else {
			td_to_noop(usbssp_data, ep_ring, cur_td, false);
		}

remove_finished_td:
		/*
		 * The event handler won't see a completion for this TD anymore,
		 * so remove it from the endpoint ring's TD list.
		 */
		list_del_init(&cur_td->td_list);
	}

	ep->ep_state &= ~EP_STOP_CMD_PENDING;

	if (!(ep->ep_state & USBSSP_EP_DISABLE_PENDING) &&
		ep->ep_state & USBSSP_EP_ENABLED) {
		/* If necessary, queue a Set Transfer Ring Dequeue Pointer
		 * command
		 */
		if (deq_state.new_deq_ptr && deq_state.new_deq_seg) {
			usbssp_queue_new_dequeue_state(usbssp_data, ep_index,
					&deq_state);
			usbssp_ring_cmd_db(usbssp_data);
		} else {
			/* Otherwise ring the doorbell(s) to restart queued
			 * transfers
			 */
			ring_doorbell_for_active_rings(usbssp_data, ep_index);
		}
	}

	/*
	 * Complete the cancellation of USB request.
	 */
	i = req_priv->num_tds_done;
	for (; i < req_priv->num_tds; i++) {
		cur_td = &req_priv->td[i];

		/* Clean up the cancelled USB Request */
		/* Doesn't matter what we pass for status, since the core will
		 * just overwrite it.
		 */
		ep_ring = usbssp_request_to_transfer_ring(usbssp_data,
				cur_td->priv_request);

		usbssp_unmap_td_bounce_buffer(usbssp_data, ep_ring, cur_td);

		inc_td_cnt(cur_td->priv_request);
		if (last_td_in_request(cur_td)) {
			usbssp_giveback_request_in_irq(usbssp_data,
					cur_td, -ECONNRESET);
		}
	}
}


/*
 * When we get a command completion for a Stop Endpoint Command, we need to
 * stop timer and clear EP_STOP_CMD_PENDING flag.
 */
static void usbssp_handle_cmd_stop_ep(struct usbssp_udc *usbssp_data,
				      union usbssp_trb *trb,
				      struct usbssp_event_cmd *event)
{
	unsigned int ep_index;
	struct usbssp_ep *ep;
	struct usbssp_ep_ctx *ep_ctx;
	struct usbssp_device *priv_dev;

	ep_index = TRB_TO_EP_INDEX(le32_to_cpu(trb->generic.field[3]));
	ep = &usbssp_data->devs.eps[ep_index];

	usbssp_dbg(usbssp_data,
		"CMD stop endpoint completion for ep index: %d - %s\n",
		ep_index, ep->name);


	priv_dev = &usbssp_data->devs;
	ep_ctx = usbssp_get_ep_ctx(usbssp_data, priv_dev->out_ctx, ep_index);
	trace_usbssp_handle_cmd_stop_ep(ep_ctx);

	ep->ep_state &= ~EP_STOP_CMD_PENDING;
}


static void usbssp_kill_ring_requests(struct usbssp_udc *usbssp_data,
				      struct usbssp_ring *ring)
{
	struct usbssp_td *cur_td;
	struct usbssp_td *tmp;

	list_for_each_entry_safe(cur_td, tmp, &ring->td_list, td_list) {
		list_del_init(&cur_td->td_list);

		usbssp_unmap_td_bounce_buffer(usbssp_data, ring, cur_td);
		inc_td_cnt(cur_td->priv_request);
	}
}

void usbssp_kill_endpoint_request(struct usbssp_udc *usbssp_data,
		  int ep_index)
{
	struct usbssp_ep *ep;
	struct usbssp_ring *ring;

	ep = &usbssp_data->devs.eps[ep_index];
	if ((ep->ep_state & EP_HAS_STREAMS) ||
			(ep->ep_state & EP_GETTING_NO_STREAMS)) {
		int stream_id;

		for (stream_id = 0; stream_id < ep->stream_info->num_streams;
		     stream_id++) {

			ring = ep->stream_info->stream_rings[stream_id];
			if (!ring)
				continue;

			usbssp_dbg_trace(usbssp_data,
				trace_usbssp_dbg_cancel_request,
				"Killing Requests for slot ID %u,"
				"ep index %u, stream %u",
				usbssp_data->slot_id, ep_index, stream_id + 1);
			usbssp_kill_ring_requests(usbssp_data, ring);
		}
	} else {
		ring = ep->ring;
		if (!ring)
			return;

		usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_cancel_request,
				"Killing Requests for slot ID %u, ep index %u",
				usbssp_data->slot_id, ep_index);
		usbssp_kill_ring_requests(usbssp_data, ring);
	}
}

/*
 * USBSSP controller died, register read returns 0xffffffff
 * Complete pending commands, mark them ABORTED.
 * USB requests need to be given back as gadget core might be waiting with
 * device lock held for the Requests to finish during device disconnect,
 * blocking device remove.
 *
 */

void usbssp_udc_died(struct usbssp_udc *usbssp_data)
{
	int i;

	if (usbssp_data->usbssp_state & USBSSP_STATE_DYING)
		return;

	usbssp_err(usbssp_data,
			"USBSSP controller not responding, assume dead\n");
	usbssp_data->usbssp_state |= USBSSP_STATE_DYING;

	usbssp_cleanup_command_queue(usbssp_data);

	/* return any pending requests, remove may be waiting for them */
	for (i = 0; i < 31; i++)
		usbssp_kill_endpoint_request(usbssp_data, i);

}

static void update_ring_for_set_deq_completion(struct usbssp_udc *usbssp_data,
					       struct usbssp_device *dev,
					       struct usbssp_ring *ep_ring,
					       unsigned int ep_index)
{
	union usbssp_trb *dequeue_temp;
	int num_trbs_free_temp;
	bool revert = false;

	num_trbs_free_temp = ep_ring->num_trbs_free;
	dequeue_temp = ep_ring->dequeue;

	if (trb_is_link(ep_ring->dequeue)) {
		ep_ring->deq_seg = ep_ring->deq_seg->next;
		ep_ring->dequeue = ep_ring->deq_seg->trbs;
	}

	while (ep_ring->dequeue != dev->eps[ep_index].queued_deq_ptr) {
		/* We have more usable TRBs */
		ep_ring->num_trbs_free++;
		ep_ring->dequeue++;
		if (trb_is_link(ep_ring->dequeue)) {
			if (ep_ring->dequeue ==
					dev->eps[ep_index].queued_deq_ptr)
				break;
			ep_ring->deq_seg = ep_ring->deq_seg->next;
			ep_ring->dequeue = ep_ring->deq_seg->trbs;
		}
		if (ep_ring->dequeue == dequeue_temp) {
			revert = true;
			break;
		}
	}

	if (revert) {
		usbssp_dbg(usbssp_data, "Unable to find new dequeue pointer\n");
		ep_ring->num_trbs_free = num_trbs_free_temp;
	}
}

/*
 * When we get a completion for a Set Transfer Ring Dequeue Pointer command,
 * we need to clear the set deq pending flag in the endpoint ring state, so that
 * the TD queueing code can ring the doorbell again.  We also need to ring the
 * endpoint doorbell to restart the ring
 */
static void usbssp_handle_cmd_set_deq(struct usbssp_udc *usbssp_data,
		union usbssp_trb *trb, u32 cmd_comp_code)
{
	unsigned int ep_index;
	unsigned int stream_id;
	struct usbssp_ring *ep_ring;
	struct usbssp_device *dev;
	struct usbssp_ep *ep;
	struct usbssp_ep_ctx *ep_ctx;
	struct usbssp_slot_ctx *slot_ctx;

	ep_index = TRB_TO_EP_INDEX(le32_to_cpu(trb->generic.field[3]));
	stream_id = TRB_TO_STREAM_ID(le32_to_cpu(trb->generic.field[2]));
	dev = &usbssp_data->devs;
	ep = &dev->eps[ep_index];

	ep_ring = usbssp_stream_id_to_ring(dev, ep_index, stream_id);
	if (!ep_ring) {
		usbssp_warn(usbssp_data,
			"WARN Set TR deq ptr command for freed stream ID %u\n",
			stream_id);
		/* XXX: Harmless??? */
		goto cleanup;
	}

	ep_ctx = usbssp_get_ep_ctx(usbssp_data, dev->out_ctx, ep_index);
	slot_ctx = usbssp_get_slot_ctx(usbssp_data, dev->out_ctx);
	trace_usbssp_handle_cmd_set_deq(slot_ctx);
	trace_usbssp_handle_cmd_set_deq_ep(ep_ctx);

	if (cmd_comp_code != COMP_SUCCESS) {
		unsigned int ep_state;
		unsigned int slot_state;

		switch (cmd_comp_code) {
		case COMP_TRB_ERROR:
			usbssp_warn(usbssp_data,
				"WARN Set TR Deq Ptr cmd invalid because of "
				"stream ID configuration\n");
			break;
		case COMP_CONTEXT_STATE_ERROR:
			usbssp_warn(usbssp_data, "WARN Set TR Deq Ptr cmd "
				"failed due to incorrect slot or ep state.\n");
			ep_state = GET_EP_CTX_STATE(ep_ctx);
			slot_state = le32_to_cpu(slot_ctx->dev_state);
			slot_state = GET_SLOT_STATE(slot_state);
			usbssp_dbg_trace(usbssp_data,
					trace_usbssp_dbg_cancel_request,
					"Slot state = %u, EP state = %u",
					slot_state, ep_state);
			break;
		case COMP_SLOT_NOT_ENABLED_ERROR:
			usbssp_warn(usbssp_data,
					"WARN Set TR Deq Ptr cmd failed because"
					" slot %u was not enabled.\n",
					usbssp_data->slot_id);
			break;
		default:
			usbssp_warn(usbssp_data, "WARN Set TR Deq Ptr cmd with"
					" unknown completion code of %u.\n",
					cmd_comp_code);
			break;
		}

	} else {
		u64 deq;
		/* deq ptr is written to the stream ctx for streams */
		if (ep->ep_state & EP_HAS_STREAMS) {
			struct usbssp_stream_ctx *ctx =
				&ep->stream_info->stream_ctx_array[stream_id];
			deq = le64_to_cpu(ctx->stream_ring) & SCTX_DEQ_MASK;
		} else {
			deq = le64_to_cpu(ep_ctx->deq) & ~EP_CTX_CYCLE_MASK;
		}
		usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_cancel_request,
			"Successful Set TR Deq Ptr cmd, deq = @%08llx", deq);
		if (usbssp_trb_virt_to_dma(ep->queued_deq_seg,
		    ep->queued_deq_ptr) == deq) {
			/* Update the ring's dequeue segment and dequeue pointer
			 * to reflect the new position.
			 */
			update_ring_for_set_deq_completion(usbssp_data, dev,
				ep_ring, ep_index);
		} else {
			usbssp_warn(usbssp_data,
				"Mismatch between completed Set TR Deq "
				"Ptr command & DC internal state.\n");
			usbssp_warn(usbssp_data,
				"ep deq seg = %p, deq ptr = %p\n",
				ep->queued_deq_seg, ep->queued_deq_ptr);
		}
	}

cleanup:
	dev->eps[ep_index].ep_state &= ~SET_DEQ_PENDING;
	dev->eps[ep_index].queued_deq_seg = NULL;
	dev->eps[ep_index].queued_deq_ptr = NULL;
	/* Restart any rings with pending requests */
	ring_doorbell_for_active_rings(usbssp_data, ep_index);
}


static void usbssp_handle_cmd_reset_ep(struct usbssp_udc *usbssp_data,
				       union usbssp_trb *trb,
				       u32 cmd_comp_code)
{
	struct usbssp_ep *dep;
	struct usbssp_ep_ctx *ep_ctx;
	unsigned int ep_index;

	ep_index = TRB_TO_EP_INDEX(le32_to_cpu(trb->generic.field[3]));
	ep_ctx = usbssp_get_ep_ctx(usbssp_data, usbssp_data->devs.out_ctx,
			ep_index);
	trace_usbssp_handle_cmd_reset_ep(ep_ctx);

	/* This command will only fail if the endpoint wasn't halted,
	 * but we don't care.
	 */
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_reset_ep,
		"Ignoring reset ep completion code of %u", cmd_comp_code);

	dep = &usbssp_data->devs.eps[ep_index];

	/* Clear our internal halted state */
	dep->ep_state &= ~EP_HALTED;

	ring_doorbell_for_active_rings(usbssp_data, ep_index);
}

static void usbssp_handle_cmd_enable_slot(struct usbssp_udc *usbssp_data,
					  int slot_id,
					  struct usbssp_command *command,
					  u32 cmd_comp_code)
{
	if (cmd_comp_code == COMP_SUCCESS) {
		usbssp_dbg(usbssp_data,
			"CMD enable slot complition successfully "
			"- slto id: %d\n", slot_id);
		usbssp_data->slot_id = slot_id;
	} else {
		usbssp_dbg(usbssp_data, "CMD enable slot complition failed\n");
		usbssp_data->slot_id = 0;
	}
}

static void usbssp_handle_cmd_disable_slot(struct usbssp_udc *usbssp_data)
{
	struct usbssp_device *dev_priv;
	struct usbssp_slot_ctx *slot_ctx;

	usbssp_dbg(usbssp_data, "CMD disable slot complition\n");

	dev_priv = &usbssp_data->devs;
	if (!dev_priv)
		return;

	usbssp_data->slot_id = 0;
	slot_ctx = usbssp_get_slot_ctx(usbssp_data, dev_priv->out_ctx);
	trace_usbssp_handle_cmd_disable_slot(slot_ctx);
}

static void usbssp_handle_cmd_config_ep(struct usbssp_udc *usbssp_data,
		struct usbssp_event_cmd *event, u32 cmd_comp_code)
{
	struct usbssp_device *priv_dev;
	struct usbssp_input_control_ctx *ctrl_ctx;
	struct usbssp_ep_ctx *ep_ctx;
	unsigned int ep_index;
	u32 add_flags, drop_flags;

	/*
	 * Configure endpoint commands can come, becaouse device
	 * receive USB_SET_CONFIGURATION or SET_INTERFACE request,
	 * or because the HW needed an extra configure endpoint
	 * command after a reset or disconnect event.
	 */
	priv_dev = &usbssp_data->devs;
	ctrl_ctx = usbssp_get_input_control_ctx(priv_dev->in_ctx);
	if (!ctrl_ctx) {
		usbssp_warn(usbssp_data,
				"Could not get input context, bad type.\n");
		return;
	}

	add_flags = le32_to_cpu(ctrl_ctx->add_flags);
	drop_flags = le32_to_cpu(ctrl_ctx->drop_flags);
	/* Input ctx add_flags are the endpoint index plus one */
	ep_index = usbssp_last_valid_endpoint(add_flags) - 1;

	ep_ctx = usbssp_get_ep_ctx(usbssp_data, priv_dev->out_ctx, ep_index);
	trace_usbssp_handle_cmd_config_ep(ep_ctx);
}

static void usbssp_handle_cmd_reset_dev(struct usbssp_udc *usbssp_data,
		struct usbssp_event_cmd *event)
{
	struct usbssp_device *dev_priv;
	struct usbssp_slot_ctx *slot_ctx;

	dev_priv = &usbssp_data->devs;
	slot_ctx = usbssp_get_slot_ctx(usbssp_data, dev_priv->out_ctx);
	trace_usbssp_handle_cmd_reset_dev(slot_ctx);
	usbssp_dbg(usbssp_data, "Completed reset device command.\n");
	if (!usbssp_data->devs.gadget)
		usbssp_warn(usbssp_data, "Reset device command completion\n");
}

static void usbssp_complete_del_and_free_cmd(struct usbssp_command *cmd,
					     u32 status)
{
	list_del(&cmd->cmd_list);

	if (cmd->completion) {
		cmd->status = status;
		complete(cmd->completion);
	} else {
		kfree(cmd);
	}
}

void usbssp_cleanup_command_queue(struct usbssp_udc *usbssp_data)
{
	struct usbssp_command *cur_cmd, *tmp_cmd;

	list_for_each_entry_safe(cur_cmd, tmp_cmd, &usbssp_data->cmd_list, cmd_list)
		usbssp_complete_del_and_free_cmd(cur_cmd, COMP_COMMAND_ABORTED);
}

void usbssp_handle_command_timeout(struct work_struct *work)
{
	struct usbssp_udc *usbssp_data;
	unsigned long flags;
	u64 hw_ring_state;

	usbssp_data = container_of(to_delayed_work(work), struct usbssp_udc,
			cmd_timer);

	spin_lock_irqsave(&usbssp_data->lock, flags);

	/*
	 * If timeout work is pending, or current_cmd is NULL, it means we
	 * raced with command completion. Command is handled so just return.
	 */
	if (!usbssp_data->current_cmd ||
	     delayed_work_pending(&usbssp_data->cmd_timer)) {
		spin_unlock_irqrestore(&usbssp_data->lock, flags);
		return;
	}
	/* mark this command to be cancelled */
	usbssp_data->current_cmd->status = COMP_COMMAND_ABORTED;

	/* Make sure command ring is running before aborting it */
	hw_ring_state = usbssp_read_64(usbssp_data,
			&usbssp_data->op_regs->cmd_ring);
	if (hw_ring_state == ~(u64)0) {
		usbssp_udc_died(usbssp_data);
		goto time_out_completed;
	}

	if ((usbssp_data->cmd_ring_state & CMD_RING_STATE_RUNNING) &&
	    (hw_ring_state & CMD_RING_RUNNING))  {
		/* Prevent new doorbell, and start command abort */
		usbssp_data->cmd_ring_state = CMD_RING_STATE_ABORTED;
		usbssp_dbg(usbssp_data, "Command timeout\n");
		usbssp_abort_cmd_ring(usbssp_data, flags);
		goto time_out_completed;
	}

	/* device disconnected. Bail out */
	if (usbssp_data->usbssp_state & USBSSP_STATE_REMOVING) {
		usbssp_dbg(usbssp_data, "device removed, ring start fail?\n");
		usbssp_cleanup_command_queue(usbssp_data);
		goto time_out_completed;
	}

	/* command timeout on stopped ring, ring can't be aborted */
	usbssp_dbg(usbssp_data, "Command timeout on stopped ring\n");
	usbssp_handle_stopped_cmd_ring(usbssp_data, usbssp_data->current_cmd);

time_out_completed:
	spin_unlock_irqrestore(&usbssp_data->lock, flags);
}

static void handle_cmd_completion(struct usbssp_udc *usbssp_data,
		struct usbssp_event_cmd *event)
{
	int slot_id = TRB_TO_SLOT_ID(le32_to_cpu(event->flags));
	u64 cmd_dma;
	dma_addr_t cmd_dequeue_dma;
	u32 cmd_comp_code;
	union usbssp_trb *cmd_trb;
	struct usbssp_command *cmd;
	u32 cmd_type;

	cmd_dma = le64_to_cpu(event->cmd_trb);
	cmd_trb = usbssp_data->cmd_ring->dequeue;

	trace_usbssp_handle_command(usbssp_data->cmd_ring, &cmd_trb->generic);

	cmd_dequeue_dma = usbssp_trb_virt_to_dma(usbssp_data->cmd_ring->deq_seg,
			cmd_trb);

	/*
	 * Check whether the completion event is for our internal kept
	 * command.
	 */
	if (!cmd_dequeue_dma || cmd_dma != (u64)cmd_dequeue_dma) {
		usbssp_warn(usbssp_data,
			  "ERROR mismatched command completion event\n");
		return;
	}

	cmd = list_entry(usbssp_data->cmd_list.next, struct usbssp_command,
			cmd_list);

	cancel_delayed_work(&usbssp_data->cmd_timer);

	cmd_comp_code = GET_COMP_CODE(le32_to_cpu(event->status));

	/* If CMD ring stopped we own the trbs between enqueue and dequeue */
	if (cmd_comp_code ==  COMP_COMMAND_RING_STOPPED) {
		complete_all(&usbssp_data->cmd_ring_stop_completion);
		return;
	}

	if (cmd->command_trb != usbssp_data->cmd_ring->dequeue) {
		usbssp_err(usbssp_data,
			 "Command completion event does not match command\n");
		return;
	}

	/*
	 * device aborted the command ring, check if the current command was
	 * supposed to be aborted, otherwise continue normally.
	 * The command ring is stopped now, but the DC will issue a Command
	 * Ring Stopped event which will cause us to restart it.
	 */
	if (cmd_comp_code == COMP_COMMAND_ABORTED) {
		usbssp_data->cmd_ring_state = CMD_RING_STATE_STOPPED;

		if (cmd->status == COMP_COMMAND_ABORTED) {
			if (usbssp_data->current_cmd == cmd)
				usbssp_data->current_cmd = NULL;
			goto event_handled;
		}
	}

	cmd_type = TRB_FIELD_TO_TYPE(le32_to_cpu(cmd_trb->generic.field[3]));
	switch (cmd_type) {
	case TRB_ENABLE_SLOT:
		usbssp_handle_cmd_enable_slot(usbssp_data, slot_id,
				cmd, cmd_comp_code);
		break;
	case TRB_DISABLE_SLOT:
		usbssp_handle_cmd_disable_slot(usbssp_data);
		break;
	case TRB_CONFIG_EP:
		if (!cmd->completion)
			usbssp_handle_cmd_config_ep(usbssp_data, event,
					cmd_comp_code);
		break;
	case TRB_EVAL_CONTEXT:
		break;
	case TRB_ADDR_DEV: {
		struct usbssp_slot_ctx *slot_ctx;

		slot_ctx = usbssp_get_slot_ctx(usbssp_data,
				usbssp_data->devs.out_ctx);
		trace_usbssp_handle_cmd_addr_dev(slot_ctx);
		break;
	}
	case TRB_STOP_RING:
		WARN_ON(slot_id != TRB_TO_SLOT_ID(
				le32_to_cpu(cmd_trb->generic.field[3])));
		usbssp_handle_cmd_stop_ep(usbssp_data, cmd_trb, event);
		break;
	case TRB_SET_DEQ:
		WARN_ON(slot_id != TRB_TO_SLOT_ID(
				le32_to_cpu(cmd_trb->generic.field[3])));
		usbssp_handle_cmd_set_deq(usbssp_data, cmd_trb, cmd_comp_code);
		break;
	case TRB_CMD_NOOP:
		/* Is this an aborted command turned to NO-OP? */
		if (cmd->status == COMP_COMMAND_RING_STOPPED)
			cmd_comp_code = COMP_COMMAND_RING_STOPPED;
		break;
	case TRB_HALT_ENDPOINT:
		if (cmd->status == COMP_COMMAND_RING_STOPPED)
			cmd_comp_code = COMP_COMMAND_RING_STOPPED;
		break;
	case TRB_FLUSH_ENDPOINT:
		if (cmd->status == COMP_COMMAND_RING_STOPPED)
			cmd_comp_code = COMP_COMMAND_RING_STOPPED;
		break;
	case TRB_RESET_EP:
		WARN_ON(slot_id != TRB_TO_SLOT_ID(
				le32_to_cpu(cmd_trb->generic.field[3])));
		usbssp_handle_cmd_reset_ep(usbssp_data, cmd_trb, cmd_comp_code);
		break;
	case TRB_RESET_DEV:
		/* SLOT_ID field in reset device cmd completion event TRB is 0.
		 * Use the SLOT_ID from the command TRB instead.
		 */
		slot_id = TRB_TO_SLOT_ID(
				le32_to_cpu(cmd_trb->generic.field[3]));

		WARN_ON(slot_id != 0);
		usbssp_handle_cmd_reset_dev(usbssp_data, event);
		break;
	case TRB_FORCE_HEADER:
		break;
	default:
		/* Skip over unknown commands on the event ring */
		usbssp_info(usbssp_data, "INFO unknown command type %d\n",
				cmd_type);
		break;
	}

	/* restart timer if this wasn't the last command */
	if (!list_is_singular(&usbssp_data->cmd_list)) {
		usbssp_data->current_cmd = list_first_entry(&cmd->cmd_list,
					struct usbssp_command, cmd_list);
		usbssp_mod_cmd_timer(usbssp_data, USBSSP_CMD_DEFAULT_TIMEOUT);
	} else if (usbssp_data->current_cmd == cmd) {
		usbssp_data->current_cmd = NULL;
	}

event_handled:
	usbssp_complete_del_and_free_cmd(cmd, cmd_comp_code);
	inc_deq(usbssp_data, usbssp_data->cmd_ring);
}


static void handle_vendor_event(struct usbssp_udc *usbssp_data,
		union usbssp_trb *event)
{
	u32 trb_type;

	trb_type = TRB_FIELD_TO_TYPE(le32_to_cpu(event->generic.field[3]));
	usbssp_dbg(usbssp_data,
		"Vendor specific event or Babble TRB type = %u\n", trb_type);
}

static void handle_port_status(struct usbssp_udc *usbssp_data,
		union usbssp_trb *event)
{
	u32 port_id;
	u32 portsc, cmd_regs;
	int max_ports;
	u8 major_revision;
	__le32 __iomem *port_regs;

	/* Port status change events always have a successful completion code */
	if (GET_COMP_CODE(le32_to_cpu(event->generic.field[2])) != COMP_SUCCESS)
		usbssp_err(usbssp_data,
			"WARN: USBSSP returned failed port status event\n");


	port_id = GET_PORT_ID(le32_to_cpu(event->generic.field[0]));
	usbssp_dbg(usbssp_data,
		"Port Status Change Event for port %d\n", port_id);

	usbssp_data->devs.port_num = port_id;
	max_ports = HCS_MAX_PORTS(usbssp_data->hcs_params1);

	if ((port_id <= 0) || (port_id > max_ports)) {
		usbssp_err(usbssp_data, "Invalid port id %d\n", port_id);
		inc_deq(usbssp_data, usbssp_data->event_ring);
		return;
	}

	if (!usbssp_data->port_major_revision) {
		/* Figure out to which USB port  device is attached:
		 * is it a USB 3.0 port or a USB 2.0/1.1 port?
		 */
		major_revision = usbssp_data->port_array[port_id - 1];

		if (major_revision == 0) {
			usbssp_warn(usbssp_data, "Event for port %u not in "
					"Extended Capabilities, ignoring.\n",
					port_id);
			goto cleanup;
		}

		usbssp_data->port_major_revision = major_revision;
	}

	port_regs = usbssp_get_port_io_addr(usbssp_data);

	portsc = readl(port_regs);
	trace_usbssp_handle_port_status(usbssp_data->devs.port_num, portsc);
	usbssp_data->gadget.speed = usbssp_port_speed(portsc);
	usbssp_dbg(usbssp_data, "PORTSC info: %s\n",
			usbssp_decode_portsc(portsc));

	if ((portsc & PORT_PLC) && (portsc & PORT_PLS_MASK) == XDEV_RESUME) {
		usbssp_dbg(usbssp_data, "port resume event for port %d\n",
				port_id);
		cmd_regs = readl(&usbssp_data->op_regs->command);
		if (!(cmd_regs & CMD_RUN)) {
			usbssp_warn(usbssp_data, "DC is not running.\n");
			goto cleanup;
		}
		if (DEV_SUPERSPEED_ANY(portsc)) {
			usbssp_dbg(usbssp_data, "remote wake SS port %d\n",
					port_id);
			usbssp_test_and_clear_bit(usbssp_data, port_regs,
					PORT_PLC);
			usbssp_set_link_state(usbssp_data, port_regs, XDEV_U0);
			usbssp_resume_gadget(usbssp_data);
			goto cleanup;
		}
	}

	if ((portsc & PORT_PLC) && (portsc & PORT_PLS_MASK) == XDEV_U0 &&
			DEV_SUPERSPEED_ANY(portsc)) {
		usbssp_dbg(usbssp_data, "resume SS port %d\n", port_id);
		usbssp_test_and_clear_bit(usbssp_data, port_regs, PORT_PLC);
	}

	if ((portsc & PORT_PLC) && (portsc & PORT_PLS_MASK) == XDEV_U1 &&
			DEV_SUPERSPEED_ANY(portsc)) {
		usbssp_dbg(usbssp_data, "suspend U1 SS port %d\n", port_id);
		usbssp_test_and_clear_bit(usbssp_data, port_regs, PORT_PLC);
		usbssp_suspend_gadget(usbssp_data);
	}

	if ((portsc & PORT_PLC) && ((portsc & PORT_PLS_MASK) == XDEV_U2 ||
			(portsc & PORT_PLS_MASK) == XDEV_U3)) {
		usbssp_dbg(usbssp_data, "resume SS port %d finished\n",
				port_id);
		usbssp_test_and_clear_bit(usbssp_data, port_regs, PORT_PLC);
		usbssp_suspend_gadget(usbssp_data);
	}

	/*Attach device */
	if ((portsc & PORT_CSC) && (portsc & PORT_CONNECT)) {
		usbssp_dbg(usbssp_data, "Port status change: Device Attached\n");
		usbssp_data->defered_event |= EVENT_DEV_CONNECTED;
		queue_work(usbssp_data->bottom_irq_wq,
				&usbssp_data->bottom_irq);
		usbssp_test_and_clear_bit(usbssp_data, port_regs, PORT_CSC);
	}

	/*Detach  device*/
	if ((portsc & PORT_CSC) && !(portsc & PORT_CONNECT)) {
		usbssp_dbg(usbssp_data,
				"Port status change: Device Deattached\n");
		usbssp_data->defered_event |= EVENT_DEV_DISCONECTED;
		queue_work(usbssp_data->bottom_irq_wq,
				&usbssp_data->bottom_irq);
		usbssp_test_and_clear_bit(usbssp_data, port_regs, PORT_CSC);
	}

	/*Port Reset Change - port is in reset state */
	if ((portsc & PORT_RC) && (portsc & PORT_RESET)) {
		usbssp_dbg(usbssp_data,
			"Port status change: Port reset signaling detected\n");
		usbssp_test_and_clear_bit(usbssp_data, port_regs, PORT_RC);
	}

	/*Port Reset Change - port is not in reset state */
	if ((portsc & PORT_RC) && !(portsc & PORT_RESET)) {
		usbssp_dbg(usbssp_data,
			"Port status change: Port reset completion detected\n");
		usbssp_gadget_reset_interrupt(usbssp_data);
		usbssp_data->defered_event |= EVENT_USB_RESET;
		queue_work(usbssp_data->bottom_irq_wq,
				&usbssp_data->bottom_irq);
		usbssp_test_and_clear_bit(usbssp_data, port_regs, PORT_RC);
	}

	/*Port Warm Reset Change*/
	if (portsc & PORT_WRC) {
		usbssp_dbg(usbssp_data,
			"Port status change: Port Warm Reset detected\n");
		usbssp_test_and_clear_bit(usbssp_data, port_regs, PORT_WRC);
	}

	/*Port Over-Curretn Change*/
	if (portsc & PORT_OCC) {
		usbssp_dbg(usbssp_data,
			"Port status change: Port Over Current detected\n");
		usbssp_test_and_clear_bit(usbssp_data, port_regs, PORT_OCC);
	}

	/*Port Configure Error Change*/
	if (portsc & PORT_CEC) {
		usbssp_dbg(usbssp_data,
			"Port status change: Port Configure Error detected\n");
		usbssp_test_and_clear_bit(usbssp_data, port_regs, PORT_CEC);
	}

	if (usbssp_data->port_major_revision == 0x02) {
		usbssp_test_and_clear_bit(usbssp_data, port_regs,
					PORT_PLC);
	}

cleanup:
	/* Update event ring dequeue pointer before dropping the lock */
	inc_deq(usbssp_data, usbssp_data->event_ring);

}

/*
 * This TD is defined by the TRBs starting at start_trb in start_seg and ending
 * at end_trb, which may be in another segment.  If the suspect DMA address is a
 * TRB in this TD, this function returns that TRB's segment.  Otherwise it
 * returns 0.
 */
struct usbssp_segment *usbssp_trb_in_td(struct usbssp_udc *usbssp_data,
					struct usbssp_segment *start_seg,
					union usbssp_trb *start_trb,
					union usbssp_trb *end_trb,
					dma_addr_t suspect_dma,
		bool		debug)
{
	dma_addr_t start_dma;
	dma_addr_t end_seg_dma;
	dma_addr_t end_trb_dma;
	struct usbssp_segment *cur_seg;

	start_dma = usbssp_trb_virt_to_dma(start_seg, start_trb);
	cur_seg = start_seg;

	do {
		if (start_dma == 0)
			return NULL;
		/* We may get an event for a Link TRB in the middle of a TD */
		end_seg_dma = usbssp_trb_virt_to_dma(cur_seg,
				&cur_seg->trbs[TRBS_PER_SEGMENT - 1]);
		/* If the end TRB isn't in this segment, this is set to 0 */
		end_trb_dma = usbssp_trb_virt_to_dma(cur_seg, end_trb);

		if (debug)
			usbssp_warn(usbssp_data,
				"Looking for event-dma %016llx trb-start"
				"%016llx trb-end %016llx seg-start %016llx"
				" seg-end %016llx\n",
				(unsigned long long)suspect_dma,
				(unsigned long long)start_dma,
				(unsigned long long)end_trb_dma,
				(unsigned long long)cur_seg->dma,
				(unsigned long long)end_seg_dma);

		if (end_trb_dma > 0) {
			/* The end TRB is in this segment, so suspect should
			 *  be here
			 */
			if (start_dma <= end_trb_dma) {
				if (suspect_dma >= start_dma &&
				    suspect_dma <= end_trb_dma)
					return cur_seg;
			} else {
				/* Case for one segment with
				 * a TD wrapped around to the top
				 */
				if ((suspect_dma >= start_dma &&
						suspect_dma <= end_seg_dma) ||
						(suspect_dma >= cur_seg->dma &&
						 suspect_dma <= end_trb_dma))
					return cur_seg;
			}
			return NULL;
		} else {
			/* Might still be somewhere in this segment */
			if (suspect_dma >= start_dma &&
			    suspect_dma <= end_seg_dma)
				return cur_seg;
		}
		cur_seg = cur_seg->next;
		start_dma = usbssp_trb_virt_to_dma(cur_seg, &cur_seg->trbs[0]);
	} while (cur_seg != start_seg);

	return NULL;
}

void usbssp_cleanup_halted_endpoint(struct usbssp_udc *usbssp_data,
				    unsigned int ep_index,
				    unsigned int stream_id,
				    struct usbssp_td *td,
				    enum usbssp_ep_reset_type reset_type)
{
	struct usbssp_command *command;
	struct usbssp_ep_ctx *ep_ctx;
	int interrupt_disabled_locally;

	ep_ctx = usbssp_get_ep_ctx(usbssp_data, usbssp_data->devs.out_ctx,
			ep_index);

	if (GET_EP_CTX_STATE(ep_ctx) != EP_STATE_HALTED) {
		usbssp_dbg(usbssp_data,
			"Endpint index %d is not in  halted state.\n",
			ep_index);
		usbssp_status_stage(usbssp_data);
		return;
	}

	command = usbssp_alloc_command(usbssp_data, true, GFP_ATOMIC);
	if (!command)
		return;

	usbssp_queue_reset_ep(usbssp_data, command, ep_index,
			reset_type);

	usbssp_ring_cmd_db(usbssp_data);

	if (irqs_disabled()) {
		spin_unlock_irqrestore(&usbssp_data->irq_thread_lock,
				usbssp_data->irq_thread_flag);
		interrupt_disabled_locally = 1;
	} else {
		spin_unlock(&usbssp_data->irq_thread_lock);
	}

	wait_for_completion(command->completion);

	if (interrupt_disabled_locally)
		spin_lock_irqsave(&usbssp_data->irq_thread_lock,
				usbssp_data->irq_thread_flag);
	else
		spin_lock(&usbssp_data->irq_thread_lock);

	usbssp_free_command(usbssp_data, command);
	if (ep_index != 0)
		usbssp_status_stage(usbssp_data);
}

int usbssp_is_vendor_info_code(struct usbssp_udc *usbssp_data, unsigned int trb_comp_code)
{
	if (trb_comp_code >= 224 && trb_comp_code <= 255) {
		/* Vendor defined "informational" completion code,
		 * treat as not-an-error.
		 */
		usbssp_dbg(usbssp_data,
				"Vendor defined info completion code %u\n",
				trb_comp_code);
		usbssp_dbg(usbssp_data, "Treating code as success.\n");
		return 1;
	}
	return 0;
}

static int usbssp_td_cleanup(struct usbssp_udc *usbssp_data, struct usbssp_td *td,
		struct usbssp_ring *ep_ring, int *status)
{
	struct usbssp_request *req_priv = NULL;

	/* Clean up the endpoint's TD list */
	req_priv = td->priv_request;

	/* if a bounce buffer was used to align this td then unmap it */
	usbssp_unmap_td_bounce_buffer(usbssp_data, ep_ring, td);

	/* Do one last check of the actual transfer length.
	 * If the DC controller said we transferred more data than the buffer
	 * length, req_priv->request.actual will be a very big number (since it's
	 * unsigned).  Play it safe and say we didn't transfer anything.
	 */
	if (req_priv->request.actual > req_priv->request.length) {
		usbssp_warn(usbssp_data,
			"USB req %u and actual %u transfer length mismatch\n",
			req_priv->request.length, req_priv->request.actual);
		req_priv->request.actual = 0;
		*status = 0;
	}
	list_del_init(&td->td_list);

	inc_td_cnt(req_priv);
	/* Giveback the USB request when all the tds are completed */
	if (last_td_in_request(td)) {
		if ((req_priv->request.actual != req_priv->request.length &&
		     td->priv_request->request.short_not_ok) ||
		    (*status != 0 &&
		     !usb_endpoint_xfer_isoc(req_priv->dep->endpoint.desc)))
			usbssp_dbg(usbssp_data,
				"Giveback Request %p, len = %d, expected = %d"
				" status = %d\n",
				req_priv, req_priv->request.actual,
				req_priv->request.length, *status);

		if (usb_endpoint_xfer_isoc(req_priv->dep->endpoint.desc))
			*status = 0;

		usbssp_giveback_request_in_irq(usbssp_data, td, *status);
	}

	return 0;
}


static int finish_td(struct usbssp_udc *usbssp_data, struct usbssp_td *td,
	struct usbssp_transfer_event *event, struct usbssp_ep *ep, int *status)
{
	struct usbssp_device *dev_priv;
	struct usbssp_ring *ep_ring;
	unsigned int slot_id;
	int ep_index;
	struct usbssp_ep_ctx *ep_ctx;
	u32 trb_comp_code;

	slot_id = TRB_TO_SLOT_ID(le32_to_cpu(event->flags));
	dev_priv = &usbssp_data->devs;
	ep_index = TRB_TO_EP_ID(le32_to_cpu(event->flags)) - 1;
	ep_ring = usbssp_dma_to_transfer_ring(ep, le64_to_cpu(event->buffer));
	ep_ctx = usbssp_get_ep_ctx(usbssp_data, dev_priv->out_ctx, ep_index);
	trb_comp_code = GET_COMP_CODE(le32_to_cpu(event->transfer_len));

	if (trb_comp_code == COMP_STOPPED_LENGTH_INVALID ||
			trb_comp_code == COMP_STOPPED ||
			trb_comp_code == COMP_STOPPED_SHORT_PACKET) {
		/* The Endpoint Stop Command completion will take care of any
		 * stopped TDs.  A stopped TD may be restarted, so don't update
		 * the ring dequeue pointer or take this TD off any lists yet.
		 */
		return 0;
	}

	/* Update ring dequeue pointer */
	while (ep_ring->dequeue != td->last_trb)
		inc_deq(usbssp_data, ep_ring);

	inc_deq(usbssp_data, ep_ring);

	return usbssp_td_cleanup(usbssp_data, td, ep_ring, status);
}

/* sum trb lengths from ring dequeue up to stop_trb, _excluding_ stop_trb */
static int sum_trb_lengths(struct usbssp_udc *usbssp_data,
			   struct usbssp_ring *ring,
			   union usbssp_trb *stop_trb)
{
	u32 sum;
	union usbssp_trb *trb = ring->dequeue;
	struct usbssp_segment *seg = ring->deq_seg;

	for (sum = 0; trb != stop_trb; next_trb(usbssp_data, ring, &seg, &trb)) {
		if (!trb_is_noop(trb) && !trb_is_link(trb))
			sum += TRB_LEN(le32_to_cpu(trb->generic.field[2]));
	}
	return sum;
}

/*
 * Process control tds, update USB request status and actual_length.
 */
static int process_ctrl_td(struct usbssp_udc *usbssp_data, struct usbssp_td *td,
	union usbssp_trb *event_trb, struct usbssp_transfer_event *event,
	struct usbssp_ep *ep_priv, int *status)
{
	struct usbssp_device *dev_priv;
	struct usbssp_ring *ep_ring;
	unsigned int slot_id;
	int ep_index;
	struct usbssp_ep_ctx *ep_ctx;
	u32 trb_comp_code;
	u32 remaining, requested;
	u32 trb_type;

	trb_type = TRB_FIELD_TO_TYPE(le32_to_cpu(event_trb->generic.field[3]));
	slot_id = TRB_TO_SLOT_ID(le32_to_cpu(event->flags));
	dev_priv = &usbssp_data->devs;
	ep_index = TRB_TO_EP_ID(le32_to_cpu(event->flags)) - 1;
	ep_ring = usbssp_dma_to_transfer_ring(ep_priv, le64_to_cpu(event->buffer));
	ep_ctx = usbssp_get_ep_ctx(usbssp_data, dev_priv->out_ctx, ep_index);
	trb_comp_code = GET_COMP_CODE(le32_to_cpu(event->transfer_len));
	requested = td->priv_request->request.length;
	remaining = EVENT_TRB_LEN(le32_to_cpu(event->transfer_len));

	switch (trb_comp_code) {
	case COMP_SUCCESS:
		*status = 0;
		break;
	case COMP_SHORT_PACKET:
		*status = 0;
		break;
	case COMP_STOPPED_SHORT_PACKET:
		if (trb_type == TRB_DATA || trb_type == TRB_NORMAL)
			td->priv_request->request.actual  = remaining;
	goto finish_td;
	case COMP_STOPPED:
		switch (trb_type) {
		case TRB_DATA:
		case TRB_NORMAL:
			td->priv_request->request.actual =
				requested - remaining;
			goto finish_td;
		case TRB_STATUS:
			td->priv_request->request.actual = requested;
			goto finish_td;
		default:
			usbssp_warn(usbssp_data,
				"WARN: unexpected TRB Type %d\n",
				trb_type);
			goto finish_td;
		}
	case COMP_STOPPED_LENGTH_INVALID:
		goto finish_td;
	default:
		usbssp_dbg(usbssp_data, "TRB error code %u, "
			"halted endpoint index = %u\n",
			trb_comp_code, ep_index);
	}

	/*
	 * if on data stage then update the actual_length of the USB
	 * request and flag it as set, so it won't be overwritten in the event
	 * for the last TRB.
	 */
	if (trb_type == TRB_DATA ||
		trb_type == TRB_NORMAL) {
		td->request_length_set = true;
		td->priv_request->request.actual = requested - remaining;
	}

	/* at status stage */
	if (!td->request_length_set)
		td->priv_request->request.actual = requested;

	if (usbssp_data->ep0state == USBSSP_EP0_DATA_PHASE
	   && ep_priv->number == 0
	   && usbssp_data->three_stage_setup) {

		td = list_entry(ep_ring->td_list.next,
			struct usbssp_td, td_list);
		usbssp_data->ep0state = USBSSP_EP0_STATUS_PHASE;
		usbssp_dbg(usbssp_data, "Arm Status stage\n");
		giveback_first_trb(usbssp_data, ep_index, 0,
			ep_ring->cycle_state, &td->last_trb->generic);
		return 0;
	}
finish_td:
	return finish_td(usbssp_data, td, event, ep_priv, status);
}

/*
 * Process isochronous tds, update usb request status and actual_length.
 */
static int process_isoc_td(struct usbssp_udc *usbssp_data, struct usbssp_td *td,
	union usbssp_trb *ep_trb, struct usbssp_transfer_event *event,
	struct usbssp_ep *ep_priv, int *status)
{
	struct usbssp_ring *ep_ring;
	struct usbssp_request *req_priv;
	int idx;
	u32 trb_comp_code;
	u32 remaining, requested, ep_trb_len;
	bool sum_trbs_for_length = false;
	int short_framestatus;

	ep_ring = usbssp_dma_to_transfer_ring(ep_priv,
			le64_to_cpu(event->buffer));
	trb_comp_code = GET_COMP_CODE(le32_to_cpu(event->transfer_len));
	req_priv = td->priv_request;
	idx = req_priv->num_tds;
	requested = req_priv->request.length;
	remaining = EVENT_TRB_LEN(le32_to_cpu(event->transfer_len));
	ep_trb_len = TRB_LEN(le32_to_cpu(ep_trb->generic.field[2]));
	short_framestatus = req_priv->request.short_not_ok ?
			-EREMOTEIO : 0;

	/* handle completion code */
	switch (trb_comp_code) {
	case COMP_SUCCESS:
		if (remaining) {
			req_priv->request.status = short_framestatus;
			break;
		}
		req_priv->request.status = 0;
		break;
	case COMP_SHORT_PACKET:
		req_priv->request.status = short_framestatus;
		sum_trbs_for_length = true;
		break;
	case COMP_ISOCH_BUFFER_OVERRUN:
	case COMP_BABBLE_DETECTED_ERROR:
		req_priv->request.status = -EOVERFLOW;
		break;
	case COMP_USB_TRANSACTION_ERROR:
		req_priv->request.status = -EPROTO;
		if (ep_trb != td->last_trb)
			return 0;
		break;
	case COMP_STOPPED:
		sum_trbs_for_length = true;
		break;
	case COMP_STOPPED_SHORT_PACKET:
		/* field normally containing residue now contains tranferred */
		req_priv->request.status  = short_framestatus;
		requested = remaining;
		break;
	case COMP_STOPPED_LENGTH_INVALID:
		requested = 0;
		remaining = 0;
		break;
	default:
		sum_trbs_for_length = true;
		req_priv->request.status = -1;
		break;
	}

	/*Fixme*/
#if 0
	if (sum_trbs_for_length)
		req_priv->request.actual = sum_trb_lengths(usbssp_data,
				ep_ring, ep_trb) +
				ep_trb_len - remaining;
	else
		req_priv->request.actual = requested;

	td->req_priv->request.actual += frame->actual_length;
#endif
	return finish_td(usbssp_data, td, event, ep_priv, status);
}

static int skip_isoc_td(struct usbssp_udc *usbssp_data,
			struct usbssp_td *td,
			struct usbssp_transfer_event *event,
			struct usbssp_ep *ep_priv,
			int *status)
{
	struct usbssp_ring *ep_ring;
	struct usbssp_request *req_priv;
	//struct usb_iso_packet_descriptor *frame;
	int idx;

	ep_ring = usbssp_dma_to_transfer_ring(ep_priv,
			le64_to_cpu(event->buffer));
	req_priv = td->priv_request/*->hcpriv*/;
	idx = req_priv->num_tds;
	//TODO
//	frame = &td->priv_request->iso_frame_desc[idx];

	/* The transfer is partly done. */
//	frame->status = -EXDEV;

	/* calc actual length */
//	frame->actual_length = 0;

	/* Update ring dequeue pointer */
	while (ep_ring->dequeue != td->last_trb)
		inc_deq(usbssp_data, ep_ring);
	inc_deq(usbssp_data, ep_ring);

	return finish_td(usbssp_data, td, event, ep_priv, status);
}

/*
 * Process bulk and interrupt tds, update usb request status and actual_length.
 */
static int process_bulk_intr_td(struct usbssp_udc *usbssp_data,
				struct usbssp_td *td,
				union usbssp_trb *ep_trb,
				struct usbssp_transfer_event *event,
				struct usbssp_ep *ep, int *status)
{
	struct usbssp_ring *ep_ring;
	u32 trb_comp_code;
	u32 remaining, requested, ep_trb_len;

	ep_ring = usbssp_dma_to_transfer_ring(ep, le64_to_cpu(event->buffer));
	trb_comp_code = GET_COMP_CODE(le32_to_cpu(event->transfer_len));
	remaining = EVENT_TRB_LEN(le32_to_cpu(event->transfer_len));
	ep_trb_len = TRB_LEN(le32_to_cpu(ep_trb->generic.field[2]));
	requested = td->priv_request->request.length;

	switch (trb_comp_code) {
	case COMP_SUCCESS:
		/* handle success with untransferred data as short packet */
		if (ep_trb != td->last_trb || remaining) {
			usbssp_warn(usbssp_data, "WARN Successful completion "
					"on short TX\n");
			usbssp_dbg(usbssp_data,
				"ep %#x - asked for %d bytes, %d bytes untransferred\n",
				td->priv_request->dep->endpoint.desc->bEndpointAddress,
				requested, remaining);
		}
		*status = 0;
		break;
	case COMP_SHORT_PACKET:
		usbssp_dbg(usbssp_data,
			"ep %#x - asked for %d bytes, %d bytes untransferred\n",
			 td->priv_request->dep->endpoint.desc->bEndpointAddress,
			 requested, remaining);

		*status = 0;
		break;
	case COMP_STOPPED_SHORT_PACKET:
		td->priv_request->request.length = remaining;
		goto finish_td;
	case COMP_STOPPED_LENGTH_INVALID:
		/* stopped on ep trb with invalid length, exclude it */
		ep_trb_len	= 0;
		remaining	= 0;
		break;
	default:
		/* Others already handled above */
		break;
	}

	if (ep_trb == td->last_trb)
		td->priv_request->request.actual = requested - remaining;
	else
		td->priv_request->request.actual =
			sum_trb_lengths(usbssp_data, ep_ring, ep_trb) +
			ep_trb_len - remaining;
finish_td:
	if (remaining > requested) {
		usbssp_warn(usbssp_data,
			"bad transfer trb length %d in event trb\n",
			remaining);
		td->priv_request->request.actual = 0;
	}

	return finish_td(usbssp_data, td, event, ep, status);
}

/*
 * If this function returns an error condition, it means it got a Transfer
 * event with a corrupted Slot ID, Endpoint ID, or TRB DMA address.
 * At this point, the USBSSP controller is probably hosed and should be reset.
 */
static int handle_tx_event(struct usbssp_udc *usbssp_data,
			   struct usbssp_transfer_event *event)
{
	struct usbssp_device *dev_priv;
	struct usbssp_ep *ep_priv;
	struct usbssp_ring *ep_ring;
	unsigned int slot_id;
	int ep_index;
	struct usbssp_td *td = NULL;
	dma_addr_t ep_trb_dma;
	struct usbssp_segment *ep_seg;
	union usbssp_trb *ep_trb;
	int status = -EINPROGRESS;
	struct usbssp_ep_ctx *ep_ctx;
	struct list_head *tmp;
	u32 trb_comp_code;
	int ret = 0;
	int td_num = 0;
	bool handling_skipped_tds = false;
	const struct usb_endpoint_descriptor *desc;

	slot_id = TRB_TO_SLOT_ID(le32_to_cpu(event->flags));
	ep_index = TRB_TO_EP_ID(le32_to_cpu(event->flags)) - 1;
	trb_comp_code = GET_COMP_CODE(le32_to_cpu(event->transfer_len));
	ep_trb_dma = le64_to_cpu(event->buffer);

	dev_priv = &usbssp_data->devs;

	ep_priv = &dev_priv->eps[ep_index];
	ep_ring = usbssp_dma_to_transfer_ring(ep_priv,
			le64_to_cpu(event->buffer));
	ep_ctx = usbssp_get_ep_ctx(usbssp_data, dev_priv->out_ctx, ep_index);

	if (GET_EP_CTX_STATE(ep_ctx) == EP_STATE_DISABLED) {
		usbssp_err(usbssp_data,
			 "ERROR Transfer event for disabled endpoint slot %u ep %u\n",
			  slot_id, ep_index);
		goto err_out;
	}

	/* Some transfer events don't always point to a trb*/
	if (!ep_ring) {
		switch (trb_comp_code) {
		case COMP_USB_TRANSACTION_ERROR:
		case COMP_INVALID_STREAM_TYPE_ERROR:
		case COMP_INVALID_STREAM_ID_ERROR:
			goto cleanup;
		case COMP_RING_UNDERRUN:
		case COMP_RING_OVERRUN:
			goto cleanup;
		default:
			usbssp_err(usbssp_data, "ERROR Transfer event for "
				"unknown stream ring slot %u ep %u\n",
				 slot_id, ep_index);
			goto err_out;
		}
	}

	/* Count current td numbers if ep->skip is set */
	if (ep_priv->skip) {
		list_for_each(tmp, &ep_ring->td_list)
			td_num++;
	}

	/* Look for common error cases */
	switch (trb_comp_code) {
	/* Skip codes that require special handling depending on
	 * transfer type
	 */
	case COMP_SUCCESS:
		if (EVENT_TRB_LEN(le32_to_cpu(event->transfer_len)) == 0)
			break;

		usbssp_warn_ratelimited(usbssp_data,
			"WARN Successful completion on short TX\n");
	case COMP_SHORT_PACKET:
		break;
	case COMP_STOPPED:
		usbssp_dbg(usbssp_data, "Stopped on Transfer TRB for ep %u\n",
			ep_index);
		break;
	case COMP_STOPPED_LENGTH_INVALID:
		usbssp_dbg(usbssp_data,
			"Stopped on No-op or Link TRB for ep %u\n",
			ep_index);
		break;
	case COMP_STOPPED_SHORT_PACKET:
		usbssp_dbg(usbssp_data,
			"Stopped with short packet transfer detected for ep %u\n",
			ep_index);
		usbssp_dbg_ctx(usbssp_data, usbssp_data->devs.out_ctx, 2);
		break;
	case COMP_BABBLE_DETECTED_ERROR:
		usbssp_dbg(usbssp_data, "Babble error for ep %u on endpoint\n",
			ep_index);
		status = -EOVERFLOW;
		break;
	case COMP_TRB_ERROR:
		usbssp_warn(usbssp_data, "WARN: TRB error on endpoint %u\n",
			ep_index);
		status = -EILSEQ;
		break;
	/* completion codes not indicating endpoint state change */
	case COMP_DATA_BUFFER_ERROR:
		usbssp_warn(usbssp_data,
			"WARN: USBSSP couldn't access mem fast enough for ep %u\n",
			ep_index);
		status = -ENOSR;
		break;
	case COMP_ISOCH_BUFFER_OVERRUN:
		usbssp_warn(usbssp_data,
			"WARN: buffer overrun event for ep %u on endpoint",
			ep_index);
		break;
	case COMP_RING_UNDERRUN:
		/*
		 * When the Isoch ring is empty, the DC will generate
		 * a Ring Overrun Event for IN Isoch endpoint or Ring
		 * Underrun Event for OUT Isoch endpoint.
		 */
		usbssp_dbg(usbssp_data, "underrun event on endpoint\n");
		if (!list_empty(&ep_ring->td_list))
			usbssp_dbg(usbssp_data, "Underrun Event for ep %d "
				"still with TDs queued?\n", ep_index);
		goto cleanup;
	case COMP_RING_OVERRUN:
		usbssp_dbg(usbssp_data, "overrun event on endpoint\n");
		if (!list_empty(&ep_ring->td_list))
			usbssp_dbg(usbssp_data, "Overrun Event for ep %d "
				"still with TDs queued?\n",
				ep_index);
		goto cleanup;
	case COMP_MISSED_SERVICE_ERROR:
		/*
		 * When encounter missed service error, one or more isoc tds
		 * may be missed by DC.
		 * Set skip flag of the ep_ring; Complete the missed tds as
		 * short transfer when process the ep_ring next time.
		 */
		ep_priv->skip = true;
		usbssp_dbg(usbssp_data,
			"Miss service interval error for ep %u, set skip flag\n",
			ep_index);
		goto cleanup;
	case COMP_INCOMPATIBLE_DEVICE_ERROR:
		/* needs disable slot command to recover */
		usbssp_warn(usbssp_data,
			"WARN: detect an incompatible device for ep %u",
			ep_index);
		status = -EPROTO;
		break;
	default:
		if (usbssp_is_vendor_info_code(usbssp_data, trb_comp_code)) {
			status = 0;
			break;
		}
		usbssp_warn(usbssp_data,
			"ERROR Unknown event condition %u, for ep %u - USBSSP probably busted\n",
			trb_comp_code, ep_index);
		goto cleanup;
	}

	do {
		/* This TRB should be in the TD at the head of this ring's
		 * TD list.
		 */
		if (list_empty(&ep_ring->td_list)) {
			/*
			 * Don't print wanings if it's due to a stopped endpoint
			 * generating an extra completion event if the device
			 * was suspended. Or, a event for the last TRB of a
			 * short TD we already got a short event for.
			 * The short TD is already removed from the TD list.
			 */
			if (!(trb_comp_code == COMP_STOPPED ||
			    trb_comp_code == COMP_STOPPED_LENGTH_INVALID  ||
			    ep_ring->last_td_was_short)) {
				usbssp_warn(usbssp_data,
					"WARN Event TRB for ep %d with no TDs queued?\n",
					ep_index);
			}

			if (ep_priv->skip) {
				ep_priv->skip = false;
				usbssp_dbg(usbssp_data,
					"td_list is empty while skip "
					"flag set. Clear skip flag for ep %u.\n",
					ep_index);
			}
			goto cleanup;
		}

		/* We've skipped all the TDs on the ep ring when ep->skip set */
		if (ep_priv->skip && td_num == 0) {
			ep_priv->skip = false;
			usbssp_dbg(usbssp_data,
				"All tds on the ep_ring skipped. "
				"Clear skip flag for ep %u.\n", ep_index);
			goto cleanup;
		}

		td = list_entry(ep_ring->td_list.next, struct usbssp_td,
				td_list);

		if (ep_priv->skip)
			td_num--;

		/* Is this a TRB in the currently executing TD? */
		ep_seg = usbssp_trb_in_td(usbssp_data, ep_ring->deq_seg,
				ep_ring->dequeue, td->last_trb,
				ep_trb_dma, false);

		/*
		 * Skip the Force Stopped Event. The event_trb(ep_trb_dma)
		 * of FSE is not in the current TD pointed by ep_ring->dequeue
		 * because that the hardware dequeue pointer still at the
		 * previous TRB of the current TD. The previous TRB maybe a
		 * Link TD or the last TRB of the previous TD. The command
		 * completion handle will take care the rest.
		 */
		if (!ep_seg && (trb_comp_code == COMP_STOPPED ||
		     trb_comp_code == COMP_STOPPED_LENGTH_INVALID)) {
			goto cleanup;
		}

		desc = td->priv_request->dep->endpoint.desc;
		if (!ep_seg) {
			if (!ep_priv->skip || !usb_endpoint_xfer_isoc(desc)) {

				/* USBSSP is busted, give up! */
				usbssp_err(usbssp_data,
					"ERROR Transfer event TRB DMA ptr not "
					"part of current TD ep_index %d "
					"comp_code %u\n", ep_index,
					trb_comp_code);
				usbssp_trb_in_td(usbssp_data, ep_ring->deq_seg,
					ep_ring->dequeue, td->last_trb,
					ep_trb_dma, true);
				return -ESHUTDOWN;
			}

			ret = skip_isoc_td(usbssp_data, td, event, ep_priv,
					&status);
			goto cleanup;
		}

		if (trb_comp_code == COMP_SHORT_PACKET)
			ep_ring->last_td_was_short = true;
		else
			ep_ring->last_td_was_short = false;

		if (ep_priv->skip) {
			usbssp_dbg(usbssp_data,
				"Found td. Clear skip flag for ep %u.\n",
				ep_index);
			ep_priv->skip = false;
		}

		ep_trb = &ep_seg->trbs[(ep_trb_dma - ep_seg->dma) / sizeof(*ep_trb)];

		trace_usbssp_handle_transfer(ep_ring,
				(struct usbssp_generic_trb *) ep_trb);

		if (trb_is_noop(ep_trb)) {
			usbssp_dbg(usbssp_data,
				"event_trb is a no-op TRB. Skip it\n");
			goto cleanup;
		}

		if (usb_endpoint_xfer_control(desc)) {
			ret = process_ctrl_td(usbssp_data, td, ep_trb, event,
				ep_priv, &status);
		} else if (usb_endpoint_xfer_isoc(desc)) {
			ret = process_isoc_td(usbssp_data, td, ep_trb,
				event, ep_priv, &status);
		} else {
			ret = process_bulk_intr_td(usbssp_data, td, ep_trb,
				event, ep_priv, &status);
		}
cleanup:
		handling_skipped_tds = ep_priv->skip &&
			trb_comp_code != COMP_MISSED_SERVICE_ERROR;

		/*
		 * Do not update event ring dequeue pointer if we're in a loop
		 * processing missed tds.
		 */
		if (!handling_skipped_tds)
			inc_deq(usbssp_data, usbssp_data->event_ring);
	/*
	 * If ep->skip is set, it means there are missed tds on the
	 * endpoint ring need to take care of.
	 * Process them as short transfer until reach the td pointed by
	 * the event.
	 */
	} while (handling_skipped_tds);

	return 0;

err_out:
	usbssp_err(usbssp_data, "@%016llx %08x %08x %08x %08x\n",
		 (unsigned long long) usbssp_trb_virt_to_dma(
			 usbssp_data->event_ring->deq_seg,
			 usbssp_data->event_ring->dequeue),
		 lower_32_bits(le64_to_cpu(event->buffer)),
		 upper_32_bits(le64_to_cpu(event->buffer)),
		 le32_to_cpu(event->transfer_len),
		 le32_to_cpu(event->flags));
	return -ENODEV;
}

/*
 * This function handles all events on the event ring.
 * Function can defers handling of some events to kernel thread.
 * Returns >0 for "possibly more events to process" (caller should call again),
 * otherwise 0 if done.  In future, <0 returns should indicate error code.
 */
int usbssp_handle_event(struct usbssp_udc *usbssp_data)
{
	union usbssp_trb *event;
	int update_ptrs = 1;
	int ret = 0;
	__le32 cycle_bit;

	unsigned int trb_comp_code;

	if (!usbssp_data->event_ring || !usbssp_data->event_ring->dequeue) {
		usbssp_err(usbssp_data, "ERROR event ring not ready\n");
		return -ENOMEM;
	}

	event = usbssp_data->event_ring->dequeue;

	cycle_bit = (le32_to_cpu(event->event_cmd.flags) & TRB_CYCLE);
	/* Does the USBSSP or Driver own the TRB? */
	if (cycle_bit != usbssp_data->event_ring->cycle_state)
		return 0;

	trace_usbssp_handle_event(usbssp_data->event_ring, &event->generic);

	/*
	 * Barrier between reading the TRB_CYCLE (valid) flag above and any
	 * speculative reads of the event's flags/data below.
	 */
	rmb();

	switch ((le32_to_cpu(event->event_cmd.flags) & TRB_TYPE_BITMASK)) {
	case TRB_TYPE(TRB_COMPLETION):
		handle_cmd_completion(usbssp_data, &event->event_cmd);
		break;
	case TRB_TYPE(TRB_PORT_STATUS):
		handle_port_status(usbssp_data, event);
		update_ptrs = 0;
		break;
	case TRB_TYPE(TRB_TRANSFER):
		ret = handle_tx_event(usbssp_data, &event->trans_event);

		if (ret >= 0)
			update_ptrs = 0;
		break;
	case TRB_TYPE(TRB_SETUP): {
		/*handling of SETUP packet are deferred to thread. */

		usbssp_data->ep0state = USBSSP_EP0_SETUP_PHASE;
		usbssp_data->setupId = TRB_SETUPID_TO_TYPE(event->trans_event.flags);
		usbssp_data->setup_speed = TRB_SETUP_SPEEDID(event->trans_event.flags);

		/*save current setup packet. It some case it will be used
		 * latter
		 */
		usbssp_data->setup = *((struct usb_ctrlrequest  *)&event->trans_event.buffer);

		usbssp_dbg(usbssp_data,
			"Setup packet (id: %d) defered to thread\n",
			usbssp_data->setupId);

		usbssp_data->defered_event |= EVENT_SETUP_PACKET;
		queue_work(usbssp_data->bottom_irq_wq,
				&usbssp_data->bottom_irq);
		break;
	}

	case TRB_TYPE(TRB_HC_EVENT):
		trb_comp_code = GET_COMP_CODE(le32_to_cpu(event->generic.field[2]));
		usbssp_warn(usbssp_data,
			"Host Controller Error detected with error code 0x%02x\n",
			trb_comp_code);
		/* Look for common error cases */
		switch (trb_comp_code) {
		case COMP_EVENT_RING_FULL_ERROR:
			usbssp_dbg(usbssp_data,
				"Error: Event Ring Full\n");
			break;
		default:
			usbssp_dbg(usbssp_data,
				"Not supported completion code\n");
		}
		break;
	default:
		if ((le32_to_cpu(event->event_cmd.flags) & TRB_TYPE_BITMASK) >=
		    TRB_TYPE(48))
			handle_vendor_event(usbssp_data, event);
		else
			usbssp_warn(usbssp_data, "ERROR unknown event type %d\n",
				TRB_FIELD_TO_TYPE(
				le32_to_cpu(event->event_cmd.flags)));
	}


	/* Any of the above functions may drop and re-acquire the lock, so check
	 * to make sure a watchdog timer didn't mark the device as
	 * non-responsive.
	 */
	if (usbssp_data->usbssp_state & USBSSP_STATE_DYING) {
		usbssp_dbg(usbssp_data, "USBSSP device dying, returning from "
			"event handle.\n");
		return 0;
	}

	if (update_ptrs) {
		/* Update SW event ring dequeue pointer */
		inc_deq(usbssp_data, usbssp_data->event_ring);
	}

	/* Are there more items on the event ring?  Caller will call us again to
	 * check.
	 */
	return 1;
}


irqreturn_t usbssp_irq(int irq, void *priv)
{
	struct usbssp_udc *usbssp_data = (struct usbssp_udc *)priv;
	union usbssp_trb *event_ring_deq;
	irqreturn_t ret = IRQ_NONE;
	unsigned long flags;
	dma_addr_t deq;
	u64 temp_64;
	u32 status;

	spin_lock_irqsave(&usbssp_data->lock, flags);

	/* Check if the USBSSP controller generated the interrupt,
	 * or the irq is shared
	 */
	status = readl(&usbssp_data->op_regs->status);
	if (status == ~(u32)0) {
		usbssp_udc_died(usbssp_data);
		ret = IRQ_HANDLED;
		goto out;
	}

	if (!(status & STS_EINT))
		goto out;

	if (status & STS_FATAL) {
		usbssp_warn(usbssp_data, "WARNING: Device Controller Error\n");
		usbssp_halt(usbssp_data);
		ret = IRQ_HANDLED;
		goto out;
	}

	/*
	 * Clear the op reg interrupt status first,
	 * so we can receive interrupts from other MSI-X interrupters.
	 * Write 1 to clear the interrupt status.
	 */
	status |= STS_EINT;
	writel(status, &usbssp_data->op_regs->status);

	if (usbssp_data->msi_enabled) {
		u32 irq_pending;

		irq_pending = readl(&usbssp_data->ir_set->irq_pending);
		irq_pending |= IMAN_IP;
		writel(irq_pending, &usbssp_data->ir_set->irq_pending);
	}

	if (usbssp_data->usbssp_state & USBSSP_STATE_DYING ||
			usbssp_data->usbssp_state & USBSSP_STATE_HALTED) {
		usbssp_dbg(usbssp_data,
				"USBSSP controller dying, ignoring interrupt. "
				"Shouldn't IRQs be disabled?\n");
		/* Clear the event handler busy flag (RW1C);
		 * the event ring should be empty.
		 */
		temp_64 = usbssp_read_64(usbssp_data,
				&usbssp_data->ir_set->erst_dequeue);
		usbssp_write_64(usbssp_data, temp_64 | ERST_EHB,
				&usbssp_data->ir_set->erst_dequeue);
		ret = IRQ_HANDLED;
		goto out;
	}

	event_ring_deq = usbssp_data->event_ring->dequeue;

	while ((ret = usbssp_handle_event(usbssp_data)) == 1) {
	}

	temp_64 = usbssp_read_64(usbssp_data,
			&usbssp_data->ir_set->erst_dequeue);
	/* If necessary, update the HW's version of the event ring deq ptr. */
	if (event_ring_deq != usbssp_data->event_ring->dequeue) {

		deq = usbssp_trb_virt_to_dma(usbssp_data->event_ring->deq_seg,
				usbssp_data->event_ring->dequeue);

		if (deq == 0)
			usbssp_warn(usbssp_data,
					"WARN something wrong with SW event "
					"ring dequeue ptr.\n");
		/* Update USBSSP event ring dequeue pointer */
		temp_64 &= ERST_PTR_MASK;
		temp_64 |= ((u64) deq & (u64) ~ERST_PTR_MASK);
	}

	/* Clear the event handler busy flag (RW1C); event ring is empty. */
	temp_64 |= ERST_EHB;
	usbssp_write_64(usbssp_data, temp_64,
			&usbssp_data->ir_set->erst_dequeue);
	ret = IRQ_HANDLED;

out:
	spin_unlock_irqrestore(&usbssp_data->lock, flags);
	return ret;
}

irqreturn_t usbssp_msi_irq(int irq, void *usbssp_data)
{
	return usbssp_irq(irq, usbssp_data);
}

/****		Endpoint Ring Operations	****/

/*
 * Generic function for queueing a TRB on a ring.
 * The caller must have checked to make sure there's room on the ring.
 *
 * @more_trbs_coming:	Will you enqueue more TRBs before calling
 *			prepare_transfer()?
 */
static void queue_trb(struct usbssp_udc *usbssp_data, struct usbssp_ring *ring,
		bool more_trbs_coming,
		u32 field1, u32 field2, u32 field3, u32 field4)
{
	struct usbssp_generic_trb *trb;

	trb = &ring->enqueue->generic;

	usbssp_dbg(usbssp_data, "Queue TRB at virt: %p, dma: %llx\n", trb,
			usbssp_trb_virt_to_dma(ring->enq_seg, ring->enqueue));

	trb->field[0] = cpu_to_le32(field1);
	trb->field[1] = cpu_to_le32(field2);
	trb->field[2] = cpu_to_le32(field3);
	trb->field[3] = cpu_to_le32(field4);

	trace_usbssp_queue_trb(ring, trb);
	inc_enq(usbssp_data, ring, more_trbs_coming);
}

/*
 * Does various checks on the endpoint ring, and makes it ready to
 * queue num_trbs.
 */
static int prepare_ring(struct usbssp_udc *usbssp_data,
			struct usbssp_ring *ep_ring,
			u32 ep_state, unsigned
			int num_trbs,
			gfp_t mem_flags)
{
	unsigned int num_trbs_needed;

	/* Make sure the endpoint has been added to USBSSP schedule */
	switch (ep_state) {
	case EP_STATE_DISABLED:
		usbssp_warn(usbssp_data,
			"WARN request submitted to disabled ep\n");
		return -ENOENT;
	case EP_STATE_ERROR:
		usbssp_warn(usbssp_data,
			"WARN waiting for error on ep to be cleared\n");
		return -EINVAL;
	case EP_STATE_HALTED:
		usbssp_dbg(usbssp_data,
			"WARN halted endpoint, queueing request anyway.\n");
	case EP_STATE_STOPPED:
	case EP_STATE_RUNNING:
		break;
	default:
		usbssp_err(usbssp_data,
			"ERROR unknown endpoint state for ep\n");
		return -EINVAL;
	}

	while (1) {
		if (room_on_ring(usbssp_data, ep_ring, num_trbs))
			break;

		if (ep_ring == usbssp_data->cmd_ring) {
			usbssp_err(usbssp_data,
				"Do not support expand command ring\n");
			return -ENOMEM;
		}

		usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_ring_expansion,
				"ERROR no room on ep ring, try ring expansion");

		num_trbs_needed = num_trbs - ep_ring->num_trbs_free;
		if (usbssp_ring_expansion(usbssp_data, ep_ring, num_trbs_needed,
					mem_flags)) {
			usbssp_err(usbssp_data, "Ring expansion failed\n");
			return -ENOMEM;
		}
	}

	while (trb_is_link(ep_ring->enqueue)) {

		ep_ring->enqueue->link.control |= cpu_to_le32(TRB_CHAIN);
		wmb();
		ep_ring->enqueue->link.control ^= cpu_to_le32(TRB_CYCLE);

		/* Toggle the cycle bit after the last ring segment. */
		if (link_trb_toggles_cycle(ep_ring->enqueue))
			ep_ring->cycle_state ^= 1;
		ep_ring->enq_seg = ep_ring->enq_seg->next;
		ep_ring->enqueue = ep_ring->enq_seg->trbs;
	}
	return 0;
}

static int prepare_transfer(struct usbssp_udc *usbssp_data,
		struct usbssp_device *dev_priv,
		unsigned int ep_index,
		unsigned int stream_id,
		unsigned int num_trbs,
		struct usbssp_request *req_priv,
		unsigned int td_index,
		gfp_t mem_flags)
{
	int ret;
	struct usbssp_td	*td;
	struct usbssp_ring *ep_ring;
	struct usbssp_ep_ctx *ep_ctx = usbssp_get_ep_ctx(usbssp_data,
					dev_priv->out_ctx, ep_index);

	ep_ring = usbssp_stream_id_to_ring(dev_priv, ep_index, stream_id);

	if (!ep_ring) {
		usbssp_dbg(usbssp_data,
				"Can't prepare ring for bad stream ID %u\n",
				stream_id);
		return -EINVAL;
	}

	ret = prepare_ring(usbssp_data, ep_ring, GET_EP_CTX_STATE(ep_ctx),
			num_trbs, mem_flags);

	if (ret)
		return ret;

	td = &req_priv->td[td_index];
	INIT_LIST_HEAD(&td->td_list);

	td->priv_request = req_priv;
	/* Add this TD to the tail of the endpoint ring's TD list */
	list_add_tail(&td->td_list, &ep_ring->td_list);
	td->start_seg = ep_ring->enq_seg;
	td->first_trb = ep_ring->enqueue;

	return 0;
}

unsigned int count_trbs(u64 addr, u64 len)
{
	unsigned int num_trbs;

	num_trbs = DIV_ROUND_UP(len + (addr & (TRB_MAX_BUFF_SIZE - 1)),
			TRB_MAX_BUFF_SIZE);
	if (num_trbs == 0)
		num_trbs++;

	return num_trbs;
}

static inline unsigned int count_trbs_needed(struct usbssp_request *req_priv)
{
	return count_trbs(req_priv->request.dma, req_priv->request.length);
}

static unsigned int count_sg_trbs_needed(struct usbssp_request *req_priv)
{
	struct scatterlist *sg;
	unsigned int i, len, full_len, num_trbs = 0;

	full_len = req_priv->request.length;

	for_each_sg(req_priv->sg, sg, req_priv->num_pending_sgs, i) {
		len = sg_dma_len(sg);
		num_trbs += count_trbs(sg_dma_address(sg), len);
		len = min_t(unsigned int, len, full_len);
		full_len -= len;
		if (full_len == 0)
			break;
	}

	return num_trbs;
}

static unsigned int count_isoc_trbs_needed(struct usbssp_request *req_priv)
{
	u64 addr, len;

	addr = (u64) req_priv->request.dma;
	len = req_priv->request.length;

	return count_trbs(addr, len);
}

static void check_trb_math(struct usbssp_request *req_priv, int running_total)
{
	if (unlikely(running_total != req_priv->request.length))
		dev_err(req_priv->dep->usbssp_data->dev,
				"%s - ep %#x - Miscalculated tx length, "
				"queued %#x (%d), asked for %#x (%d)\n",
				__func__,
				req_priv->dep->endpoint.desc->bEndpointAddress,
				running_total, running_total,
				req_priv->request.length,
				req_priv->request.length);
}

static void giveback_first_trb(struct usbssp_udc *usbssp_data,
			       unsigned int ep_index,
			       unsigned int stream_id,
			       int start_cycle,
			       struct usbssp_generic_trb *start_trb)
{
	/*
	 * Pass all the TRBs to the hardware at once and make sure this write
	 * isn't reordered.
	 */
	wmb();
	if (start_cycle)
		start_trb->field[3] |= cpu_to_le32(start_cycle);
	else
		start_trb->field[3] &= cpu_to_le32(~TRB_CYCLE);

	usbssp_dbg_ep_rings(usbssp_data, ep_index,
			&usbssp_data->devs.eps[ep_index]);
	usbssp_ring_ep_doorbell(usbssp_data,  ep_index, stream_id);
}

/*
 * USBSSP uses normal TRBs for both bulk and interrupt.  When the interrupt
 * endpoint is to be serviced, the DC will consume (at most) one TD.  A TD
 * (comprised of sg list entries) can take several service intervals to
 * transmit.
 */
int usbssp_queue_intr_tx(struct usbssp_udc *usbssp_data, gfp_t mem_flags,
		struct usbssp_request *req_priv, unsigned int ep_index)
{
	struct usbssp_ep_ctx *ep_ctx;

	ep_ctx = usbssp_get_ep_ctx(usbssp_data, usbssp_data->devs.out_ctx,
			ep_index);

	return usbssp_queue_bulk_tx(usbssp_data, mem_flags, req_priv, ep_index);
}

/*
 * For USBSSP controllers, TD size is the number of max packet sized
 * packets remaining in the TD (*not* including this TRB).
 *
 * Total TD packet count = total_packet_count =
 *     DIV_ROUND_UP(TD size in bytes / wMaxPacketSize)
 *
 * Packets transferred up to and including this TRB = packets_transferred =
 *     rounddown(total bytes transferred including this TRB / wMaxPacketSize)
 *
 * TD size = total_packet_count - packets_transferred
 *
 * For USBSSP it must fit in bits 21:17, so it can't be bigger than 31.
 * This is taken care of in the TRB_TD_SIZE() macro
 *
 * The last TRB in a TD must have the TD size set to zero.
 */
static u32 usbssp_td_remainder(struct usbssp_udc *usbssp_data,
			       int transferred,
			       int trb_buff_len,
			       unsigned int td_total_len,
			       struct usbssp_request *req_priv,
			       bool more_trbs_coming)
{
	u32 maxp, total_packet_count;

	/* One TRB with a zero-length data packet. */
	if (!more_trbs_coming || (transferred == 0 && trb_buff_len == 0) ||
	    trb_buff_len == td_total_len)
		return 0;

	maxp = usb_endpoint_maxp(req_priv->dep->endpoint.desc);
	total_packet_count = DIV_ROUND_UP(td_total_len, maxp);

	/* Queuing functions don't count the current TRB into transferred */
	return (total_packet_count - ((transferred + trb_buff_len) / maxp));
}

static int usbssp_align_td(struct usbssp_udc *usbssp_data,
		     struct usbssp_request *req_priv, u32 enqd_len,
			 u32 *trb_buff_len, struct usbssp_segment *seg)
{
	struct device *dev = usbssp_data->dev;
	unsigned int unalign;
	unsigned int max_pkt;
	u32 new_buff_len;

	max_pkt = GET_MAX_PACKET(
			usb_endpoint_maxp(req_priv->dep->endpoint.desc));
	unalign = (enqd_len + *trb_buff_len) % max_pkt;

	/* we got lucky, last normal TRB data on segment is packet aligned */
	if (unalign == 0)
		return 0;

	usbssp_dbg(usbssp_data, "Unaligned %d bytes, buff len %d\n",
		 unalign, *trb_buff_len);

	/* is the last nornal TRB alignable by splitting it */
	if (*trb_buff_len > unalign) {
		*trb_buff_len -= unalign;
		usbssp_dbg(usbssp_data, "split align, new buff len %d\n",
				*trb_buff_len);
		return 0;
	}

	/*
	 * We want enqd_len + trb_buff_len to sum up to a number aligned to
	 * number which is divisible by the endpoint's wMaxPacketSize. IOW:
	 * (size of currently enqueued TRBs + remainder) % wMaxPacketSize == 0.
	 */
	new_buff_len = max_pkt - (enqd_len % max_pkt);

	if (new_buff_len > (req_priv->request.length - enqd_len))
		new_buff_len = (req_priv->request.length - enqd_len);

	/* create a max max_pkt sized bounce buffer pointed to by last trb */
	if (req_priv->direction) {
		sg_pcopy_to_buffer(req_priv->request.sg,
				req_priv->request.num_mapped_sgs,
				seg->bounce_buf, new_buff_len, enqd_len);
		seg->bounce_dma = dma_map_single(dev, seg->bounce_buf,
						max_pkt, DMA_TO_DEVICE);
	} else {
		seg->bounce_dma = dma_map_single(dev, seg->bounce_buf,
						max_pkt, DMA_FROM_DEVICE);
	}

	if (dma_mapping_error(dev, seg->bounce_dma)) {
		/* try without aligning.*/
		usbssp_warn(usbssp_data,
			"Failed mapping bounce buffer, not aligning\n");
		return 0;
	}
	*trb_buff_len = new_buff_len;
	seg->bounce_len = new_buff_len;
	seg->bounce_offs = enqd_len;

	usbssp_dbg(usbssp_data, "Bounce align, new buff len %d\n",
			*trb_buff_len);

	return 1;
}

int usbssp_queue_bulk_tx(struct usbssp_udc *usbssp_data,
			 gfp_t mem_flags,
			 struct usbssp_request *req_priv,
			 unsigned int ep_index)
{
	struct usbssp_ring *ring;
	struct usbssp_td *td;
	struct usbssp_generic_trb *start_trb;
	struct scatterlist *sg = NULL;
	bool more_trbs_coming = true;
	bool need_zero_pkt = false;
	bool first_trb = true;
	unsigned int num_trbs;
	unsigned int start_cycle, num_sgs = 0;
	unsigned int enqd_len, block_len, trb_buff_len, full_len;
	int sent_len, ret;
	u32 field, length_field, remainder;
	u64 addr, send_addr;

	ring = usbssp_request_to_transfer_ring(usbssp_data, req_priv);
	if (!ring)
		return -EINVAL;

	full_len = req_priv->request.length;
	/* If we have scatter/gather list, we use it. */
	if (req_priv->request.num_sgs) {
		num_sgs = req_priv->num_pending_sgs;
		sg = req_priv->sg;
		addr = (u64) sg_dma_address(sg);
		block_len = sg_dma_len(sg);
		num_trbs = count_sg_trbs_needed(req_priv);
	} else {
		num_trbs = count_trbs_needed(req_priv);
		addr = (u64) req_priv->request.dma;
		block_len = full_len;
	}

	ret = prepare_transfer(usbssp_data, &usbssp_data->devs,
			ep_index, req_priv->request.stream_id,
			num_trbs, req_priv, 0, mem_flags);
	if (unlikely(ret < 0))
		return ret;

	/* Deal with request.zero - need one more td/trb */
	if (req_priv->request.zero && req_priv->num_tds_done > 1)
		need_zero_pkt = true;

	td = &req_priv->td[0];

	usbssp_dbg(usbssp_data, "Queue Bulk transfer to %s - ep_index: %d,"
				" num trb: %d, block len %d, nzp: %d\n",
				req_priv->dep->name, ep_index,
				num_trbs, block_len, need_zero_pkt);

	/*
	 * Don't give the first TRB to the hardware (by toggling the cycle bit)
	 * until we've finished creating all the other TRBs.  The ring's cycle
	 * state may change as we enqueue the other TRBs, so save it too.
	 */
	start_trb = &ring->enqueue->generic;
	start_cycle = ring->cycle_state;
	send_addr = addr;

	/* Queue the TRBs, even if they are zero-length */
	for (enqd_len = 0; first_trb || enqd_len < full_len;
			enqd_len += trb_buff_len) {
		field = TRB_TYPE(TRB_NORMAL);

		/* TRB buffer should not cross 64KB boundaries */
		trb_buff_len = TRB_BUFF_LEN_UP_TO_BOUNDARY(addr);
		trb_buff_len = min_t(unsigned int, trb_buff_len, block_len);

		if (enqd_len + trb_buff_len > full_len)
			trb_buff_len = full_len - enqd_len;

		/* Don't change the cycle bit of the first TRB until later */
		if (first_trb) {
			first_trb = false;
			if (start_cycle == 0)
				field |= TRB_CYCLE;
		} else
			field |= ring->cycle_state;

		/* Chain all the TRBs together; clear the chain bit in the last
		 * TRB to indicate it's the last TRB in the chain.
		 */
		if (enqd_len + trb_buff_len < full_len) {
			field |= TRB_CHAIN;
			if (trb_is_link(ring->enqueue + 1)) {
				if (usbssp_align_td(usbssp_data, req_priv,
					enqd_len, &trb_buff_len,
					ring->enq_seg)) {
					send_addr = ring->enq_seg->bounce_dma;
					/* assuming TD won't span 2 segs */
					td->bounce_seg = ring->enq_seg;
				}
			}
		}
		if (enqd_len + trb_buff_len >= full_len) {
			field &= ~TRB_CHAIN;
			field |= TRB_IOC;
			more_trbs_coming = false;
			td->last_trb = ring->enqueue;
		}

		/* Only set interrupt on short packet for OUT endpoints */
		if (!req_priv->direction)
			field |= TRB_ISP;

		/* Set the TRB length, TD size, and interrupter fields. */
		remainder = usbssp_td_remainder(usbssp_data, enqd_len,
				trb_buff_len, full_len, req_priv,
				more_trbs_coming);

		length_field = TRB_LEN(trb_buff_len) |
			TRB_TD_SIZE(remainder) |
			TRB_INTR_TARGET(0);

		queue_trb(usbssp_data, ring, more_trbs_coming | need_zero_pkt,
				lower_32_bits(send_addr),
				upper_32_bits(send_addr),
				length_field,
				field);

		addr += trb_buff_len;
		sent_len = trb_buff_len;

		while (sg && sent_len >= block_len) {
			/* New sg entry */
			--num_sgs;
			sent_len -= block_len;
			if (num_sgs != 0) {
				sg = sg_next(sg);
				block_len = sg_dma_len(sg);
				addr = (u64) sg_dma_address(sg);
				addr += sent_len;
			}
		}
		block_len -= sent_len;
		send_addr = addr;
	}

	if (need_zero_pkt) {
		ret = prepare_transfer(usbssp_data, &usbssp_data->devs,
				ep_index, req_priv->request.stream_id,
				1, req_priv, 1, mem_flags);
		req_priv->td[1].last_trb = ring->enqueue;
		field = TRB_TYPE(TRB_NORMAL) | ring->cycle_state | TRB_IOC;
		queue_trb(usbssp_data, ring, 0, 0, 0,
				TRB_INTR_TARGET(0), field);
	}

	check_trb_math(req_priv, enqd_len);
	giveback_first_trb(usbssp_data,  ep_index, req_priv->request.stream_id,
			start_cycle, start_trb);
	return 0;
}

int usbssp_queue_ctrl_tx(struct usbssp_udc *usbssp_data,
			 gfp_t mem_flags,
			 struct usbssp_request *req_priv,
			 unsigned int ep_index)
{
	struct usbssp_ring *ep_ring;
	int num_trbs;
	int ret;
	struct usbssp_generic_trb *start_trb;
	int start_cycle;
	u32 field, length_field, remainder;
	struct usbssp_td *td;
	struct usbssp_ep *dep = req_priv->dep;

	ep_ring = usbssp_request_to_transfer_ring(usbssp_data, req_priv);
	if (!ep_ring)
		return -EINVAL;

	if (usbssp_data->delayed_status) {
		usbssp_dbg(usbssp_data, "Queue CTRL: delayed finished\n");
		usbssp_data->delayed_status = false;
		usb_gadget_set_state(&usbssp_data->gadget,
				USB_STATE_CONFIGURED);
	}

	if (usbssp_data->bos_event_detected) {
		usbssp_data->bos_event_detected = 0;
		usb_gadget_unmap_request_by_dev(usbssp_data->dev,
				&req_priv->request,
			dep->direction);
		usbssp_set_usb2_hardware_lpm(usbssp_data,
				&req_priv->request, 1);
		ret = usb_gadget_map_request_by_dev(usbssp_data->dev,
				&req_priv->request, dep->direction);
	}

	/* 1 TRB for data, 1 for status */
	if (usbssp_data->three_stage_setup)
		num_trbs = 2;
	else
		num_trbs = 1;

	ret = prepare_transfer(usbssp_data, &usbssp_data->devs,
			req_priv->epnum, req_priv->request.stream_id,
			num_trbs, req_priv, 0, mem_flags);

	if (ret < 0)
		return ret;

	td = &req_priv->td[0];
	/*
	 * Don't give the first TRB to the hardware (by toggling the cycle bit)
	 * until we've finished creating all the other TRBs.  The ring's cycle
	 * state may change as we enqueue the other TRBs, so save it too.
	 */
	start_trb = &ep_ring->enqueue->generic;
	start_cycle = ep_ring->cycle_state;

	/* If there's data, queue data TRBs */
	/* Only set interrupt on short packet for OUT endpoints */

	if (usbssp_data->ep0_expect_in)
		field = TRB_TYPE(TRB_DATA) | TRB_IOC;
	else
		field = TRB_ISP | TRB_TYPE(TRB_DATA) | TRB_IOC;

	if (req_priv->request.length > 0) {
		remainder = usbssp_td_remainder(usbssp_data, 0,
				req_priv->request.length,
				req_priv->request.length, req_priv, 1);

		length_field = TRB_LEN(req_priv->request.length) |
			TRB_TD_SIZE(remainder) |
			TRB_INTR_TARGET(0);

		if (usbssp_data->ep0_expect_in)
			field |= TRB_DIR_IN;

		queue_trb(usbssp_data, ep_ring, true,
				lower_32_bits(req_priv->request.dma),
				upper_32_bits(req_priv->request.dma),
				length_field,
				field | ep_ring->cycle_state |
				TRB_SETUPID(usbssp_data->setupId) |
				usbssp_data->setup_speed);
		usbssp_data->ep0state = USBSSP_EP0_DATA_PHASE;
	}

	/* Save the DMA address of the last TRB in the TD */
	td->last_trb = ep_ring->enqueue;

	/* Queue status TRB*/
	/* If the device sent data, the status stage is an OUT transfer */

	if (req_priv->request.length > 0 && usbssp_data->ep0_expect_in)
		field = TRB_DIR_IN;
	else
		field = 0;

	if (req_priv->request.length == 0)
		field  |= ep_ring->cycle_state;
	else
		field |= (ep_ring->cycle_state ^ 1);

	if (dep->ep_state & EP0_HALTED_STATUS) {
		/* If endpoint should be halted in Status Stage then
		 * driver shall set TRB_SETUPSTAT_STALL bit
		 */
		usbssp_dbg(usbssp_data,
				"Status Stage phase prepared with STALL bit\n");
		dep->ep_state &= ~EP0_HALTED_STATUS;
		field |= TRB_SETUPSTAT(TRB_SETUPSTAT_STALL);
	} else {
		field |= TRB_SETUPSTAT(TRB_SETUPSTAT_ACK);
	}

	queue_trb(usbssp_data, ep_ring, false,
			0,
			0,
			TRB_INTR_TARGET(0),
			/* Event on completion */
			field | TRB_IOC |  TRB_SETUPID(usbssp_data->setupId) |
			TRB_TYPE(TRB_STATUS) | usbssp_data->setup_speed);

	usbssp_dbg_ep_rings(usbssp_data, 0, dep);
	usbssp_ring_ep_doorbell(usbssp_data,  ep_index,
			req_priv->request.stream_id);
	return 0;
}

/* Stop endpoint after disconnecting device.*/
int usbssp_cmd_stop_ep(struct usbssp_udc *usbssp_data, struct usb_gadget *g,
		       struct usbssp_ep *ep_priv)
{
	int ret = 0;
	struct usbssp_command *command;
	unsigned int ep_index;
	struct usbssp_container_ctx *out_ctx;
	struct usbssp_ep_ctx *ep_ctx;
	int interrupt_disabled_locally = 0;

	ep_index = usbssp_get_endpoint_index(ep_priv->endpoint.desc);

	if ((ep_priv->ep_state & EP_STOP_CMD_PENDING)) {
		usbssp_dbg(usbssp_data,
			"Stop endpoint command on %s (index: %d) is pending\n",
			ep_priv->name, ep_index);
		return 0;
	}

	command = usbssp_alloc_command(usbssp_data, true, GFP_ATOMIC);
	if (!command)
		return -ENOMEM;

	ep_priv->ep_state |= EP_STOP_CMD_PENDING;

	usbssp_queue_stop_endpoint(usbssp_data, command,
			ep_index, 0);
	usbssp_ring_cmd_db(usbssp_data);

	out_ctx = usbssp_data->devs.out_ctx;
	ep_ctx = usbssp_get_ep_ctx(usbssp_data, out_ctx, ep_index);

	if (irqs_disabled()) {
		spin_unlock_irqrestore(&usbssp_data->irq_thread_lock,
				usbssp_data->irq_thread_flag);
		interrupt_disabled_locally = 1;
	} else {
		spin_unlock(&usbssp_data->irq_thread_lock);
	}

	/* Wait for last stop endpoint command to finish */
	wait_for_completion(command->completion);

	if (interrupt_disabled_locally)
		spin_lock_irqsave(&usbssp_data->irq_thread_lock,
				usbssp_data->irq_thread_flag);
	else
		spin_lock(&usbssp_data->irq_thread_lock);

	if (command->status == COMP_COMMAND_ABORTED ||
			command->status == COMP_COMMAND_RING_STOPPED) {
		usbssp_warn(usbssp_data,
			"Timeout while waiting for stop endpoint command\n");
		ret = -ETIME;
	}

	usbssp_free_command(usbssp_data, command);
	return ret;
}

/*
 * The transfer burst count field of the isochronous TRB defines the number of
 * bursts that are required to move all packets in this TD.  Only SuperSpeed
 * devices can burst up to bMaxBurst number of packets per service interval.
 * This field is zero based, meaning a value of zero in the field means one
 * burst.  Basically, for everything but SuperSpeed devices, this field will be
 * zero.
 */
static unsigned int usbssp_get_burst_count(struct usbssp_udc *usbssp_data,
					   struct usbssp_request *req_priv,
					   unsigned int total_packet_count)
{
	unsigned int max_burst;

	if (usbssp_data->gadget.speed < USB_SPEED_SUPER)
		return 0;

	max_burst = req_priv->dep->endpoint.comp_desc->bMaxBurst;
	return DIV_ROUND_UP(total_packet_count, max_burst + 1) - 1;
}

/*
 * Returns the number of packets in the last "burst" of packets.  This field is
 * valid for all speeds of devices.  USB 2.0 devices can only do one "burst", so
 * the last burst packet count is equal to the total number of packets in the
 * TD.  SuperSpeed endpoints can have up to 3 bursts.  All but the last burst
 * must contain (bMaxBurst + 1) number of packets, but the last burst can
 * contain 1 to (bMaxBurst + 1) packets.
 */
static unsigned int usbssp_get_last_burst_packet_count(
					struct usbssp_udc *usbssp_data,
					struct usbssp_request *req_priv,
					unsigned int total_packet_count)
{
	unsigned int max_burst;
	unsigned int residue;

	if (usbssp_data->gadget.speed >= USB_SPEED_SUPER) {
		/* bMaxBurst is zero based: 0 means 1 packet per burst */
		max_burst = req_priv->dep->endpoint.comp_desc->bMaxBurst;
		residue = total_packet_count % (max_burst + 1);
		/* If residue is zero, the last burst contains (max_burst + 1)
		 * number of packets, but the TLBPC field is zero-based.
		 */
		if (residue == 0)
			return max_burst;
		return residue - 1;
	}
	if (total_packet_count == 0)
		return 0;
	return total_packet_count - 1;
}

/*
 * Calculates Frame ID field of the isochronous TRB identifies the
 * target frame that the Interval associated with this Isochronous
 * Transfer Descriptor will start on.
 *
 * Returns actual frame id on success, negative value on error.
 */
static int usbssp_get_isoc_frame_id(struct usbssp_udc *usbssp_data,
		struct usbssp_request *req_priv, int index)
{
	int start_frame = 0, ist, ret = 0;
	int start_frame_id, end_frame_id, current_frame_id;

	/* Isochronous Scheduling Threshold (IST, bits 0~3 in HCSPARAMS2):
	 *
	 * If bit [3] of IST is cleared to '0', software can add a TRB no
	 * later than IST[2:0] Microframes before that TRB is scheduled to
	 * be executed.
	 * If bit [3] of IST is set to '1', software can add a TRB no later
	 * than IST[2:0] Frames before that TRB is scheduled to be executed.
	 */
	ist = HCS_IST(usbssp_data->hcs_params2) & 0x7;
	if (HCS_IST(usbssp_data->hcs_params2) & (1 << 3))
		ist <<= 3;

	/* Software shall not schedule an Isoch TD with a Frame ID value that
	 * is less than the Start Frame ID or greater than the End Frame ID,
	 * where:
	 *
	 * End Frame ID = (Current MFINDEX register value + 895 ms.) MOD 2048
	 * Start Frame ID = (Current MFINDEX register value + IST + 1) MOD 2048
	 *
	 * Both the End Frame ID and Start Frame ID values are calculated
	 * in microframes. When software determines the valid Frame ID value;
	 * The End Frame ID value should be rounded down to the nearest Frame
	 * boundary, and the Start Frame ID value should be rounded up to the
	 * nearest Frame boundary.
	 */
	current_frame_id = readl(&usbssp_data->run_regs->microframe_index);
	start_frame_id = roundup(current_frame_id + ist + 1, 8);
	end_frame_id = rounddown(current_frame_id + 895 * 8, 8);

	start_frame &= 0x7ff;
	start_frame_id = (start_frame_id >> 3) & 0x7ff;
	end_frame_id = (end_frame_id >> 3) & 0x7ff;

	usbssp_dbg(usbssp_data, "%s: index %d, reg 0x%x start_frame_id 0x%x,"
		"end_frame_id 0x%x, start_frame 0x%x\n",
		 __func__, index,
		 readl(&usbssp_data->run_regs->microframe_index),
		 start_frame_id, end_frame_id, start_frame);

	if (start_frame_id < end_frame_id) {
		if (start_frame > end_frame_id || start_frame < start_frame_id)
			ret = -EINVAL;
	} else if (start_frame_id > end_frame_id) {
		if (start_frame > end_frame_id && start_frame < start_frame_id)
			ret = -EINVAL;
	} else {
		ret = -EINVAL;
	}

	if (index == 0) {
		if (ret == -EINVAL || start_frame == start_frame_id) {
			start_frame = start_frame_id + 1;
			if (usbssp_data->gadget.speed == USB_SPEED_LOW ||
			    usbssp_data->gadget.speed == USB_SPEED_FULL)
				req_priv->start_frame = start_frame;
			else
				req_priv->start_frame = start_frame << 3;
			ret = 0;
		}
	}

	if (ret) {
		usbssp_warn(usbssp_data,
			"Frame ID %d (reg %d, index %d) beyond range (%d, %d)\n",
			start_frame, current_frame_id, index,
			start_frame_id, end_frame_id);
		usbssp_warn(usbssp_data,
			"Ignore frame ID field, use SIA bit instead\n");
		return ret;
	}

	return start_frame;
}

/* This is for isoc transfer */
static int usbssp_queue_isoc_tx(struct usbssp_udc *usbssp_data,
				gfp_t mem_flags,
				struct usbssp_request *req_priv,
				unsigned int ep_index)
{
	struct usbssp_ring *ep_ring;
	struct usbssp_td *td;
	int num_tds, trbs_per_td;
	struct usbssp_generic_trb *start_trb;
	bool first_trb;
	int start_cycle;
	u32 field, length_field;
	int running_total, trb_buff_len, td_len, td_remain_len, ret;
	u64 start_addr, addr;
	int i, j;
	bool more_trbs_coming;
	struct usbssp_ep *ep_priv;
	int frame_id;

	ep_priv = &usbssp_data->devs.eps[ep_index];
	ep_ring = usbssp_data->devs.eps[ep_index].ring;

	num_tds = 1;

	if (num_tds < 1) {
		usbssp_dbg(usbssp_data, "Isoc request with zero packets?\n");
		return -EINVAL;
	}
	start_addr = (u64) req_priv->request.dma;
	start_trb = &ep_ring->enqueue->generic;
	start_cycle = ep_ring->cycle_state;


	/* Queue the TRBs for each TD, even if they are zero-length */
	for (i = 0; i < num_tds; i++) {
		unsigned int total_pkt_count, max_pkt;
		unsigned int burst_count, last_burst_pkt_count;
		u32 sia_frame_id;

		first_trb = true;
		running_total = 0;
		addr = start_addr;
		td_len = req_priv->request.length;
		td_remain_len = td_len;
		max_pkt = GET_MAX_PACKET(usb_endpoint_maxp(req_priv->dep->endpoint.desc));
		total_pkt_count = DIV_ROUND_UP(td_len, max_pkt);

		/* A zero-length transfer still involves at least one packet. */
		if (total_pkt_count == 0)
			total_pkt_count++;
		burst_count = usbssp_get_burst_count(usbssp_data, req_priv,
				total_pkt_count);
		last_burst_pkt_count = usbssp_get_last_burst_packet_count(
				usbssp_data, req_priv, total_pkt_count);

		trbs_per_td = count_isoc_trbs_needed(req_priv);

		ret = prepare_transfer(usbssp_data, &usbssp_data->devs,
				ep_index, req_priv->request.stream_id,
				trbs_per_td, req_priv, i, mem_flags);
		if (ret < 0) {
			if (i == 0)
				return ret;
			goto cleanup;
		}
		td = &req_priv->td[i];

		/* use SIA as default, if frame id is used overwrite it */
		sia_frame_id = TRB_SIA;
		if (HCC_CFC(usbssp_data->hcc_params)) {
			frame_id = usbssp_get_isoc_frame_id(usbssp_data,
					req_priv, i);
			if (frame_id >= 0)
				sia_frame_id = TRB_FRAME_ID(frame_id);
		}
		/*
		 * Set isoc specific data for the first TRB in a TD.
		 * Prevent HW from getting the TRBs by keeping the cycle state
		 * inverted in the first TDs isoc TRB.
		 */
		field = TRB_TYPE(TRB_ISOC) |
			TRB_TLBPC(last_burst_pkt_count) |
			sia_frame_id |
			(i ? ep_ring->cycle_state : !start_cycle);

		if (!ep_priv->use_extended_tbc)
			field |= TRB_TBC(burst_count);

		/* fill the rest of the TRB fields, and remaining normal TRBs */
		for (j = 0; j < trbs_per_td; j++) {
			u32 remainder = 0;

			/* only first TRB is isoc, overwrite otherwise */
			if (!first_trb)
				field = TRB_TYPE(TRB_NORMAL) |
					ep_ring->cycle_state;

			/* Only set interrupt on short packet for IN EPs */
			if (usb_endpoint_dir_out(req_priv->dep->endpoint.desc))
				field |= TRB_ISP;

			/* Set the chain bit for all except the last TRB  */
			if (j < trbs_per_td - 1) {
				more_trbs_coming = true;
				field |= TRB_CHAIN;
			} else {
				more_trbs_coming = false;
				td->last_trb = ep_ring->enqueue;
				field |= TRB_IOC;
				/* set BEI, except for the last TD */
				if (i < num_tds - 1)
					field |= TRB_BEI;
			}
			/* Calculate TRB length */
			trb_buff_len = TRB_BUFF_LEN_UP_TO_BOUNDARY(addr);
			if (trb_buff_len > td_remain_len)
				trb_buff_len = td_remain_len;

			/* Set the TRB length, TD size, & interrupter fields. */
			remainder = usbssp_td_remainder(usbssp_data,
					running_total, trb_buff_len, td_len,
					req_priv, more_trbs_coming);

			length_field = TRB_LEN(trb_buff_len) |
				TRB_INTR_TARGET(0);

			if (first_trb && ep_priv->use_extended_tbc)
				length_field |= TRB_TD_SIZE_TBC(burst_count);
			else
				length_field |= TRB_TD_SIZE(remainder);
			first_trb = false;

			queue_trb(usbssp_data, ep_ring, more_trbs_coming,
				lower_32_bits(addr),
				upper_32_bits(addr),
				length_field,
				field);
			running_total += trb_buff_len;

			addr += trb_buff_len;
			td_remain_len -= trb_buff_len;
		}

		/* Check TD length */
		if (running_total != td_len) {
			usbssp_err(usbssp_data, "ISOC TD length unmatch\n");
			ret = -EINVAL;
			goto cleanup;
		}
	}

	/* store the next frame id */
//	if (HCC_CFC(usbssp_data->hcc_params))
//		ep_priv->next_frame_id = req_priv->start_frame + num_tds *
//			req_priv->request.interval;

	giveback_first_trb(usbssp_data,  ep_index, req_priv->request.stream_id,
			start_cycle, start_trb);
	return 0;
cleanup:
	/* Clean up a partially enqueued isoc transfer. */

	for (i--; i >= 0; i--)
		list_del_init(&req_priv->td[i].td_list);

	/* Use the first TD as a temporary variable to turn the TDs we've queued
	 * into No-ops with a software-owned cycle bit. That way the hardware
	 * won't accidentally start executing bogus TDs when we partially
	 * overwrite them.  td->first_trb and td->start_seg are already set.
	 */
	req_priv->td[0].last_trb = ep_ring->enqueue;
	/* Every TRB except the first & last will have its cycle bit flipped. */
	td_to_noop(usbssp_data, ep_ring, &req_priv->td[0], true);

	/* Reset the ring enqueue back to the first TRB and its cycle bit. */
	ep_ring->enqueue = req_priv->td[0].first_trb;
	ep_ring->enq_seg = req_priv->td[0].start_seg;
	ep_ring->cycle_state = start_cycle;
	ep_ring->num_trbs_free = ep_ring->num_trbs_free_temp;
	return ret;
}

int usbssp_queue_isoc_tx_prepare(struct usbssp_udc *usbssp_data,
				 gfp_t mem_flags,
				 struct usbssp_request *req_priv,
				 unsigned int ep_index)
{
	struct usbssp_device *dev_priv;
	struct usbssp_ring *ep_ring;
	struct usbssp_ep_ctx *ep_ctx;
	int start_frame;
	int num_trbs;
	int ret;
	struct usbssp_ep *ep_priv;
	int ist;

	dev_priv = &usbssp_data->devs;
	ep_priv = &usbssp_data->devs.eps[ep_index];
	ep_ring = usbssp_data->devs.eps[ep_index].ring;
	ep_ctx = usbssp_get_ep_ctx(usbssp_data, dev_priv->out_ctx, ep_index);

	/*Single usb_request can use only one TD, Linux gadget drivers doesn't
	 * use sg for isoc so sg will be omitted
	 */
	num_trbs = count_isoc_trbs_needed(req_priv);


	/* Check the ring to guarantee there is enough room for the whole
	 * request. Do not insert any td of the USB Request to the ring if the
	 * check failed.
	 */
	ret = prepare_ring(usbssp_data, ep_ring, GET_EP_CTX_STATE(ep_ctx),
			   num_trbs, mem_flags);
	if (ret)
		return ret;

	if (HCC_CFC(usbssp_data->hcc_params) && !list_empty(&ep_ring->td_list)) {
		if ((le32_to_cpu(ep_ctx->ep_info) & EP_STATE_MASK) ==
		     EP_STATE_RUNNING) {
			req_priv->start_frame = ep_priv->next_frame_id;
			goto skip_start_over;
		}
	}

	start_frame = readl(&usbssp_data->run_regs->microframe_index);
	start_frame &= 0x3fff;
	/*
	 * Round up to the next frame and consider the time before trb really
	 * gets scheduled by hardare.
	 */
	ist = HCS_IST(usbssp_data->hcs_params2) & 0x7;
	if (HCS_IST(usbssp_data->hcs_params2) & (1 << 3))
		ist <<= 3;
	start_frame += ist + USBSSP_CFC_DELAY;
	start_frame = roundup(start_frame, 8);
skip_start_over:
	ep_ring->num_trbs_free_temp = ep_ring->num_trbs_free;

	return usbssp_queue_isoc_tx(usbssp_data, mem_flags, req_priv, ep_index);
}

/****		Command Ring Operations		****/
/* Generic function for queueing a command TRB on the command ring.
 * Check to make sure there's room on the command ring for one command TRB.
 * Also check that there's room reserved for commands that must not fail.
 * If this is a command that must not fail, meaning command_must_succeed = TRUE,
 * then only check for the number of reserved spots.
 * Don't decrement usbssp_data->cmd_ring_reserved_trbs after we've queued the
 * TRB because the command event handler may want to resubmit a failed command.
 */
static int queue_command(struct usbssp_udc *usbssp_data,
			 struct usbssp_command *cmd,
			 u32 field1, u32 field2,
			 u32 field3, u32 field4,
			 bool command_must_succeed)
{
	int reserved_trbs = usbssp_data->cmd_ring_reserved_trbs;
	int ret;

	if ((usbssp_data->usbssp_state & USBSSP_STATE_DYING) ||
		(usbssp_data->usbssp_state & USBSSP_STATE_HALTED)) {
		usbssp_dbg(usbssp_data,
			"USBSSP dying or halted, can't queue command\n");
		return -ESHUTDOWN;
	}

	if (!command_must_succeed)
		reserved_trbs++;

	ret = prepare_ring(usbssp_data, usbssp_data->cmd_ring, EP_STATE_RUNNING,
			reserved_trbs, GFP_ATOMIC);
	if (ret < 0) {
		usbssp_err(usbssp_data,
			"ERR: No room for command on command ring\n");
		if (command_must_succeed)
			usbssp_err(usbssp_data,
				"ERR: Reserved TRB counting for "
				"unfailable commands failed.\n");
		return ret;
	}

	cmd->command_trb = usbssp_data->cmd_ring->enqueue;

	/* if there are no other commands queued we start the timeout timer */
	if (list_empty(&usbssp_data->cmd_list)) {
		usbssp_data->current_cmd = cmd;
		usbssp_mod_cmd_timer(usbssp_data, USBSSP_CMD_DEFAULT_TIMEOUT);
	}

	list_add_tail(&cmd->cmd_list, &usbssp_data->cmd_list);

	queue_trb(usbssp_data, usbssp_data->cmd_ring, false, field1, field2,
		field3, field4 | usbssp_data->cmd_ring->cycle_state);
	return 0;
}

/* Queue a slot enable or disable request on the command ring */
int usbssp_queue_slot_control(struct usbssp_udc *usbssp_data,
			      struct usbssp_command *cmd,
			      u32 trb_type)
{
	return queue_command(usbssp_data, cmd, 0, 0, 0,
			TRB_TYPE(trb_type) |
			SLOT_ID_FOR_TRB(usbssp_data->slot_id), false);
}

/* Queue an address device command TRB */
int usbssp_queue_address_device(struct usbssp_udc *usbssp_data,
				struct usbssp_command *cmd,
				dma_addr_t in_ctx_ptr,
				enum usbssp_setup_dev setup)
{
	return queue_command(usbssp_data, cmd, lower_32_bits(in_ctx_ptr),
			upper_32_bits(in_ctx_ptr), 0,
			TRB_TYPE(TRB_ADDR_DEV) |
			SLOT_ID_FOR_TRB(usbssp_data->slot_id)
			| (setup == SETUP_CONTEXT_ONLY ? TRB_BSR : 0), false);
}

int usbssp_queue_vendor_command(struct usbssp_udc *usbssp_data,
				struct usbssp_command *cmd,
				u32 field1, u32 field2, u32 field3, u32 field4)
{
	return queue_command(usbssp_data, cmd, field1, field2, field3,
			field4, false);
}

/* Queue a reset device command TRB */
int usbssp_queue_reset_device(struct usbssp_udc *usbssp_data,
			      struct usbssp_command *cmd)
{
	return queue_command(usbssp_data, cmd, 0, 0, 0,
			TRB_TYPE(TRB_RESET_DEV) |
			SLOT_ID_FOR_TRB(usbssp_data->slot_id),
			false);
}

/* Queue a configure endpoint command TRB */
int usbssp_queue_configure_endpoint(struct usbssp_udc *usbssp_data,
				    struct usbssp_command *cmd,
				    dma_addr_t in_ctx_ptr,
				    bool command_must_succeed)
{
	return queue_command(usbssp_data, cmd, lower_32_bits(in_ctx_ptr),
			upper_32_bits(in_ctx_ptr), 0,
			TRB_TYPE(TRB_CONFIG_EP) |
			SLOT_ID_FOR_TRB(usbssp_data->slot_id),
			command_must_succeed);
}

/* Queue an evaluate context command TRB */
int usbssp_queue_evaluate_context(struct usbssp_udc *usbssp_data,
				  struct usbssp_command *cmd,
				  dma_addr_t in_ctx_ptr,
				  bool command_must_succeed)
{
	return queue_command(usbssp_data, cmd, lower_32_bits(in_ctx_ptr),
			upper_32_bits(in_ctx_ptr), 0,
			TRB_TYPE(TRB_EVAL_CONTEXT) |
			SLOT_ID_FOR_TRB(usbssp_data->slot_id),
			command_must_succeed);
}

/*
 * Suspend is set to indicate "Stop Endpoint Command" is being issued to stop
 * activity on an endpoint that is about to be suspended.
 */
int usbssp_queue_stop_endpoint(struct usbssp_udc *usbssp_data,
			       struct usbssp_command *cmd,
			       unsigned int ep_index, int suspend)
{
	u32 trb_slot_id = SLOT_ID_FOR_TRB(usbssp_data->slot_id);
	u32 trb_ep_index = EP_ID_FOR_TRB(ep_index);
	u32 type = TRB_TYPE(TRB_STOP_RING);
	u32 trb_suspend = SUSPEND_PORT_FOR_TRB(suspend);

	return queue_command(usbssp_data, cmd, 0, 0, 0,
			trb_slot_id | trb_ep_index | type | trb_suspend, false);
}

/* Set Transfer Ring Dequeue Pointer command */
void usbssp_queue_new_dequeue_state(struct  usbssp_udc *usbssp_data,
				    unsigned int ep_index,
				    struct usbssp_dequeue_state *deq_state)
{
	dma_addr_t addr;
	u32 trb_slot_id = SLOT_ID_FOR_TRB(usbssp_data->slot_id);
	u32 trb_ep_index = EP_ID_FOR_TRB(ep_index);
	u32 trb_stream_id = STREAM_ID_FOR_TRB(deq_state->stream_id);
	u32 trb_sct = 0;
	u32 type = TRB_TYPE(TRB_SET_DEQ);
	struct usbssp_ep *ep_priv;
	struct usbssp_command *cmd;
	int ret;

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_cancel_request,
		"Set TR Deq Ptr cmd, new deq seg = %p (0x%llx dma), "
		"new deq ptr = %p (0x%llx dma), new cycle = %u",
		deq_state->new_deq_seg,
		(unsigned long long)deq_state->new_deq_seg->dma,
		deq_state->new_deq_ptr,
		(unsigned long long)usbssp_trb_virt_to_dma(
			deq_state->new_deq_seg, deq_state->new_deq_ptr),
		deq_state->new_cycle_state);

	addr = usbssp_trb_virt_to_dma(deq_state->new_deq_seg,
				    deq_state->new_deq_ptr);
	if (addr == 0) {
		usbssp_warn(usbssp_data, "WARN Cannot submit Set TR Deq Ptr\n");
		usbssp_warn(usbssp_data, "WARN deq seg = %p, deq pt = %p\n",
			  deq_state->new_deq_seg, deq_state->new_deq_ptr);
		return;
	}
	ep_priv = &usbssp_data->devs.eps[ep_index];
	if ((ep_priv->ep_state & SET_DEQ_PENDING)) {
		usbssp_warn(usbssp_data, "WARN Cannot submit Set TR Deq Ptr\n");
		usbssp_warn(usbssp_data,
			"A Set TR Deq Ptr command is pending.\n");
		return;
	}

	/* This function gets called from contexts where it cannot sleep */
	cmd = usbssp_alloc_command(usbssp_data, false, GFP_ATOMIC);
	if (!cmd) {
		usbssp_warn(usbssp_data,
			"WARN Cannot submit Set TR Deq Ptr: ENOMEM\n");
		return;
	}

	ep_priv->queued_deq_seg = deq_state->new_deq_seg;
	ep_priv->queued_deq_ptr = deq_state->new_deq_ptr;
	if (deq_state->stream_id)
		trb_sct = SCT_FOR_TRB(SCT_PRI_TR);
	ret = queue_command(usbssp_data, cmd,
		lower_32_bits(addr) | trb_sct | deq_state->new_cycle_state,
		upper_32_bits(addr), trb_stream_id,
		trb_slot_id | trb_ep_index | type, false);
	if (ret < 0) {
		usbssp_free_command(usbssp_data, cmd);
		return;
	}

	/* Stop the TD queueing code from ringing the doorbell until
	 * this command completes.  The DC won't set the dequeue pointer
	 * if the ring is running, and ringing the doorbell starts the
	 * ring running.
	 */
	ep_priv->ep_state |= SET_DEQ_PENDING;
}

int usbssp_queue_reset_ep(struct usbssp_udc *usbssp_data,
			  struct usbssp_command *cmd,
			  unsigned int ep_index,
			  enum usbssp_ep_reset_type reset_type)
{
	u32 trb_slot_id = SLOT_ID_FOR_TRB(usbssp_data->slot_id);
	u32 trb_ep_index = EP_ID_FOR_TRB(ep_index);
	u32 type = TRB_TYPE(TRB_RESET_EP);

	if (reset_type == EP_SOFT_RESET)
		type |= TRB_TSP;

	return queue_command(usbssp_data, cmd, 0, 0, 0,
			trb_slot_id | trb_ep_index | type, false);
}

/*
 * Queue an NOP command TRB
 */
int usbssp_queue_nop(struct usbssp_udc *usbssp_data,
		     struct usbssp_command *cmd)
{
	return queue_command(usbssp_data, cmd, 0, 0, 0,
			TRB_TYPE(TRB_CMD_NOOP), false);
}

/*
 * Queue a halt endpoint request on the command ring
 */
int usbssp_queue_halt_endpoint(struct usbssp_udc *usbssp_data,
			       struct usbssp_command *cmd,
			       unsigned int ep_index)
{
	u32 trb_slot_id = SLOT_ID_FOR_TRB(usbssp_data->slot_id);
	u32 trb_ep_index = EP_ID_FOR_TRB(ep_index);

	return queue_command(usbssp_data, cmd, 0, 0, 0,
			TRB_TYPE(TRB_HALT_ENDPOINT) | trb_slot_id |
			trb_ep_index, false);
}
