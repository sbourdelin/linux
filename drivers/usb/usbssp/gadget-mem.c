// SPDX-License-Identifier: GPL-2.0
/*
 * USBSSP device controller driver
 *
 * Copyright (C) 2018 Cadence.
 *
 * Author: Pawel Laszczak
 * Some code borrowed from the Linux XHCI driver.
 */

#include <linux/usb.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/dmapool.h>
#include <linux/dma-mapping.h>
#include "gadget.h"
#include "gadget-trace.h"
#include "gadget-debugfs.h"

/*
 * Allocates a generic ring segment from the ring pool, sets the dma address,
 * initializes the segment to zero, and sets the private next pointer to NULL.
 *
 * "All components of all Command and Transfer TRBs shall be initialized to '0'"
 */
static struct usbssp_segment *usbssp_segment_alloc(
		struct usbssp_udc *usbssp_data, unsigned int cycle_state,
		unsigned int max_packet, gfp_t flags)
{
	struct usbssp_segment *seg;
	dma_addr_t dma;
	int i;

	seg = kzalloc(sizeof(*seg), flags);
	if (!seg)
		return NULL;

	seg->trbs = dma_pool_zalloc(usbssp_data->segment_pool, flags, &dma);
	if (!seg->trbs) {
		kfree(seg);
		return NULL;
	}

	if (max_packet) {
		seg->bounce_buf = kzalloc(max_packet, flags | GFP_DMA);
		if (!seg->bounce_buf) {
			dma_pool_free(usbssp_data->segment_pool,
				seg->trbs, dma);
			kfree(seg);
			return NULL;
		}
	}

	/* If the cycle state is 0, set the cycle bit to 1 for all the TRBs */
	if (cycle_state == 0) {
		for (i = 0; i < TRBS_PER_SEGMENT; i++)
			seg->trbs[i].link.control |= cpu_to_le32(TRB_CYCLE);
	}
	seg->dma = dma;
	seg->next = NULL;

	return seg;
}

static void usbssp_segment_free(struct usbssp_udc *usbssp_data,
				struct usbssp_segment *seg)
{
	if (seg->trbs) {
		dma_pool_free(usbssp_data->segment_pool, seg->trbs, seg->dma);
		seg->trbs = NULL;
	}
	kfree(seg->bounce_buf);
	kfree(seg);
}

static void usbssp_free_segments_for_ring(struct usbssp_udc *usbssp_data,
					  struct usbssp_segment *first)
{
	struct usbssp_segment *seg;

	seg = first->next;
	while (seg != first) {
		struct usbssp_segment *next = seg->next;

		usbssp_segment_free(usbssp_data, seg);
		seg = next;
	}
	usbssp_segment_free(usbssp_data, first);
}

/*
 * Make the prev segment point to the next segment.
 *
 * Change the last TRB in the prev segment to be a Link TRB which points to the
 * DMA address of the next segment.  The caller needs to set any Link TRB
 * related flags, such as End TRB, Toggle Cycle, and no snoop.
 */
static void usbssp_link_segments(struct usbssp_udc *usbssp_data,
				 struct usbssp_segment *prev,
				 struct usbssp_segment *next,
				 enum usbssp_ring_type type)
{
	u32 val;

	if (!prev || !next)
		return;
	prev->next = next;
	if (type != TYPE_EVENT) {
		prev->trbs[TRBS_PER_SEGMENT-1].link.segment_ptr =
			cpu_to_le64(next->dma);

		/* Set the last TRB in the segment to have a TRB type ID
		 * of Link TRB
		 */
		val = le32_to_cpu(prev->trbs[TRBS_PER_SEGMENT-1].link.control);
		val &= ~TRB_TYPE_BITMASK;
		val |= TRB_TYPE(TRB_LINK);
		prev->trbs[TRBS_PER_SEGMENT-1].link.control = cpu_to_le32(val);
	}
}

/*
 * Link the ring to the new segments.
 * Set Toggle Cycle for the new ring if needed.
 */
static void usbssp_link_rings(struct usbssp_udc *usbssp_data,
			      struct usbssp_ring *ring,
			      struct usbssp_segment *first,
			      struct usbssp_segment *last,
			      unsigned int num_segs)
{
	struct usbssp_segment *next;

	if (!ring || !first || !last)
		return;

	next = ring->enq_seg->next;
	usbssp_link_segments(usbssp_data, ring->enq_seg, first, ring->type);
	usbssp_link_segments(usbssp_data, last, next, ring->type);
	ring->num_segs += num_segs;
	ring->num_trbs_free += (TRBS_PER_SEGMENT - 1) * num_segs;

	if (ring->type != TYPE_EVENT && ring->enq_seg == ring->last_seg) {
		ring->last_seg->trbs[TRBS_PER_SEGMENT-1].link.control
			&= ~cpu_to_le32(LINK_TOGGLE);
		last->trbs[TRBS_PER_SEGMENT-1].link.control
			|= cpu_to_le32(LINK_TOGGLE);
		ring->last_seg = last;
	}
}

/*
 * We need a radix tree for mapping physical addresses of TRBs to which stream
 * ID they belong to.  We need to do this because the device controller won't
 * tell us which stream ring the TRB came from.  We could store the stream ID
 * in an event data TRB, but that doesn't help us for the cancellation case,
 * since the endpoint may stop before it reaches that event data TRB.
 *
 * The radix tree maps the upper portion of the TRB DMA address to a ring
 * segment that has the same upper portion of DMA addresses.  For example,
 * say I have segments of size 1KB, that are always 1KB aligned.  A segment may
 * start at 0x10c91000 and end at 0x10c913f0.  If I use the upper 10 bits, the
 * key to the stream ID is 0x43244.  I can use the DMA address of the TRB to
 * pass the radix tree a key to get the right stream ID:
 *
 *	0x10c90fff >> 10 = 0x43243
 *	0x10c912c0 >> 10 = 0x43244
 *	0x10c91400 >> 10 = 0x43245
 *
 * Obviously, only those TRBs with DMA addresses that are within the segment
 * will make the radix tree return the stream ID for that ring.
 *
 * Caveats for the radix tree:
 *
 * The radix tree uses an unsigned long as a key pair.  On 32-bit systems, an
 * unsigned long will be 32-bits; on a 64-bit system an unsigned long will be
 * 64-bits.  Since we only request 32-bit DMA addresses, we can use that as the
 * key on 32-bit or 64-bit systems (it would also be fine if we asked for 64-bit
 * PCI DMA addresses on a 64-bit system).  There might be a problem on 32-bit
 * extended systems (where the DMA address can be bigger than 32-bits),
 * if we allow the PCI dma mask to be bigger than 32-bits.  So don't do that.
 */
static int usbssp_insert_segment_mapping(
		struct radix_tree_root *trb_address_map,
		struct usbssp_ring *ring,
		struct usbssp_segment *seg,
		gfp_t mem_flags)
{
	unsigned long key;
	int ret;

	key = (unsigned long)(seg->dma >> TRB_SEGMENT_SHIFT);
	/* Skip any segments that were already added. */
	if (radix_tree_lookup(trb_address_map, key))
		return 0;

	ret = radix_tree_maybe_preload(mem_flags);
	if (ret)
		return ret;
	ret = radix_tree_insert(trb_address_map, key, ring);
	radix_tree_preload_end();
	return ret;
}

static void usbssp_remove_segment_mapping(
				struct radix_tree_root *trb_address_map,
				struct usbssp_segment *seg)
{
	unsigned long key;

	key = (unsigned long)(seg->dma >> TRB_SEGMENT_SHIFT);
	if (radix_tree_lookup(trb_address_map, key))
		radix_tree_delete(trb_address_map, key);
}

static int usbssp_update_stream_segment_mapping(
		struct radix_tree_root *trb_address_map,
		struct usbssp_ring *ring,
		struct usbssp_segment *first_seg,
		struct usbssp_segment *last_seg,
		gfp_t mem_flags)
{
	struct usbssp_segment *seg;
	struct usbssp_segment *failed_seg;
	int ret;

	if (WARN_ON_ONCE(trb_address_map == NULL))
		return 0;

	seg = first_seg;
	do {
		ret = usbssp_insert_segment_mapping(trb_address_map,
				ring, seg, mem_flags);
		if (ret)
			goto remove_streams;
		if (seg == last_seg)
			return 0;
		seg = seg->next;
	} while (seg != first_seg);

	return 0;

remove_streams:
	failed_seg = seg;
	seg = first_seg;
	do {
		usbssp_remove_segment_mapping(trb_address_map, seg);
		if (seg == failed_seg)
			return ret;
		seg = seg->next;
	} while (seg != first_seg);

	return ret;
}

static void usbssp_remove_stream_mapping(struct usbssp_ring *ring)
{
	struct usbssp_segment *seg;

	if (WARN_ON_ONCE(ring->trb_address_map == NULL))
		return;

	seg = ring->first_seg;
	do {
		usbssp_remove_segment_mapping(ring->trb_address_map, seg);
		seg = seg->next;
	} while (seg != ring->first_seg);
}

static int usbssp_update_stream_mapping(struct usbssp_ring *ring,
					gfp_t mem_flags)
{
	return usbssp_update_stream_segment_mapping(ring->trb_address_map, ring,
			ring->first_seg, ring->last_seg, mem_flags);
}

void usbssp_ring_free(struct usbssp_udc *usbssp_data, struct usbssp_ring *ring)
{
	if (!ring)
		return;

	trace_usbssp_ring_free(ring);

	if (ring->first_seg) {
		if (ring->type == TYPE_STREAM)
			usbssp_remove_stream_mapping(ring);
		usbssp_free_segments_for_ring(usbssp_data, ring->first_seg);
	}

	kfree(ring);
}

static void usbssp_initialize_ring_info(struct usbssp_ring *ring,
					unsigned int cycle_state)
{
	/* The ring is empty, so the enqueue pointer == dequeue pointer */
	ring->enqueue = ring->first_seg->trbs;
	ring->enq_seg = ring->first_seg;
	ring->dequeue = ring->enqueue;
	ring->deq_seg = ring->first_seg;
	/* The ring is initialized to 0. The producer must write 1 to the cycle
	 * bit to handover ownership of the TRB, so PCS = 1.  The consumer must
	 * compare CCS to the cycle bit to check ownership, so CCS = 1.
	 *
	 * New rings are initialized with cycle state equal to 1; if we are
	 * handling ring expansion, set the cycle state equal to the old ring.
	 */
	ring->cycle_state = cycle_state;

	/*
	 * Each segment has a link TRB, and leave an extra TRB for SW
	 * accounting purpose
	 */
	ring->num_trbs_free = ring->num_segs * (TRBS_PER_SEGMENT - 1) - 1;
}

/* Allocate segments and link them for a ring */
static int usbssp_alloc_segments_for_ring(struct usbssp_udc *usbssp_data,
					  struct usbssp_segment **first,
					  struct usbssp_segment **last,
					  unsigned int num_segs,
					  unsigned int cycle_state,
					  enum usbssp_ring_type type,
					  unsigned int max_packet,
					  gfp_t flags)
{
	struct usbssp_segment *prev;

	/*allocation first segment */
	prev = usbssp_segment_alloc(usbssp_data, cycle_state,
			max_packet, flags);
	if (!prev)
		return -ENOMEM;
	num_segs--;

	*first = prev;
	/*allocation all other segments*/
	while (num_segs > 0) {
		struct usbssp_segment	*next;

		next = usbssp_segment_alloc(usbssp_data, cycle_state,
				max_packet, flags);
		if (!next) {
			prev = *first;
			/*Free all reserved segment*/
			while (prev) {
				next = prev->next;
				usbssp_segment_free(usbssp_data, prev);
				prev = next;
			}
			return -ENOMEM;
		}
		usbssp_link_segments(usbssp_data, prev, next, type);

		prev = next;
		num_segs--;
	}
	usbssp_link_segments(usbssp_data, prev, *first, type);
	*last = prev;

	return 0;
}

/**
 * Create a new ring with zero or more segments.
 *
 * Link each segment together into a ring.
 * Set the end flag and the cycle toggle bit on the last segment.
 * See section 4.9.1 and figures 15 and 16.
 */
static struct usbssp_ring *usbssp_ring_alloc(struct usbssp_udc *usbssp_data,
					     unsigned int num_segs,
					     unsigned int cycle_state,
					     enum usbssp_ring_type type,
					     unsigned int max_packet,
					     gfp_t flags)
{
	struct usbssp_ring *ring;
	int ret;

	ring = kzalloc(sizeof *(ring), flags);
	if (!ring)
		return NULL;

	ring->num_segs = num_segs;
	ring->bounce_buf_len = max_packet;
	INIT_LIST_HEAD(&ring->td_list);
	ring->type = type;
	if (num_segs == 0)
		return ring;

	ret = usbssp_alloc_segments_for_ring(usbssp_data, &ring->first_seg,
			&ring->last_seg, num_segs, cycle_state, type,
			max_packet, flags);
	if (ret)
		goto fail;

	/* Only event ring does not use link TRB */
	if (type != TYPE_EVENT) {
		/* See section 4.9.2.1 and 6.4.4.1 */
		ring->last_seg->trbs[TRBS_PER_SEGMENT - 1].link.control |=
			cpu_to_le32(LINK_TOGGLE);
	}
	usbssp_initialize_ring_info(ring, cycle_state);
	trace_usbssp_ring_alloc(ring);
	return ring;
fail:
	kfree(ring);
	return NULL;
}

void usbssp_free_endpoint_ring(struct usbssp_udc *usbssp_data,
			       struct usbssp_device *dev_priv,
			       unsigned int ep_index)
{
	usbssp_ring_free(usbssp_data, dev_priv->eps[ep_index].ring);
	dev_priv->eps[ep_index].ring = NULL;
}

/*
 * Expand an existing ring.
 * Allocate a new ring which has same segment numbers and link the two rings.
 */
int usbssp_ring_expansion(struct usbssp_udc *usbssp_data,
			  struct usbssp_ring *ring,
			  unsigned int num_trbs, gfp_t flags)
{
	struct usbssp_segment *first;
	struct usbssp_segment *last;
	unsigned int num_segs;
	unsigned int num_segs_needed;
	int ret;

	num_segs_needed = (num_trbs + (TRBS_PER_SEGMENT - 1) - 1) /
			(TRBS_PER_SEGMENT - 1);

	/* Allocate number of segments we needed, or double the ring size */
	num_segs = ring->num_segs > num_segs_needed ?
			ring->num_segs : num_segs_needed;

	ret = usbssp_alloc_segments_for_ring(usbssp_data, &first, &last,
			num_segs, ring->cycle_state, ring->type,
			ring->bounce_buf_len, flags);
	if (ret)
		return -ENOMEM;

	if (ring->type == TYPE_STREAM)
		ret = usbssp_update_stream_segment_mapping(
				ring->trb_address_map, ring, first,
				last, flags);
	if (ret) {
		struct usbssp_segment *next;

		do {
			next = first->next;
			usbssp_segment_free(usbssp_data, first);
			if (first == last)
				break;
			first = next;
		} while (true);
		return ret;
	}

	usbssp_link_rings(usbssp_data, ring, first, last, num_segs);
	trace_usbssp_ring_expansion(ring);
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_ring_expansion,
			"ring expansion succeed, now has %d segments",
			ring->num_segs);

	return 0;
}

struct usbssp_container_ctx *usbssp_alloc_container_ctx(
					struct usbssp_udc *usbssp_data,
					int type, gfp_t flags)
{
	struct usbssp_container_ctx *ctx;

	if ((type != USBSSP_CTX_TYPE_DEVICE) && (type != USBSSP_CTX_TYPE_INPUT))
		return NULL;

	ctx = kzalloc(sizeof(*ctx), flags);
	if (!ctx)
		return NULL;

	ctx->type = type;
	ctx->size = HCC_64BYTE_CONTEXT(usbssp_data->hcc_params) ? 2048 : 1024;
	if (type == USBSSP_CTX_TYPE_INPUT)
		ctx->size += CTX_SIZE(usbssp_data->hcc_params);

	ctx->bytes = dma_pool_zalloc(usbssp_data->device_pool,
				flags, &ctx->dma);

	if (!ctx->bytes) {
		kfree(ctx);
		return NULL;
	}
	return ctx;
}

void usbssp_free_container_ctx(struct usbssp_udc *usbssp_data,
			       struct usbssp_container_ctx *ctx)
{
	if (!ctx)
		return;
	dma_pool_free(usbssp_data->device_pool, ctx->bytes, ctx->dma);
	kfree(ctx);
}

struct usbssp_input_control_ctx *usbssp_get_input_control_ctx(
					struct usbssp_container_ctx *ctx)
{
	if (ctx->type != USBSSP_CTX_TYPE_INPUT)
		return NULL;

	return (struct usbssp_input_control_ctx *)ctx->bytes;
}

struct usbssp_slot_ctx *usbssp_get_slot_ctx(struct usbssp_udc *usbssp_data,
					    struct usbssp_container_ctx *ctx)
{
	if (ctx->type == USBSSP_CTX_TYPE_DEVICE)
		return (struct usbssp_slot_ctx *)ctx->bytes;

	return (struct usbssp_slot_ctx *) (ctx->bytes +
		CTX_SIZE(usbssp_data->hcc_params));
}

struct usbssp_ep_ctx *usbssp_get_ep_ctx(struct usbssp_udc *usbssp_data,
					struct usbssp_container_ctx *ctx,
					unsigned int ep_index)
{
	/* increment ep index by offset of start of ep ctx array */
	ep_index++;
	if (ctx->type == USBSSP_CTX_TYPE_INPUT)
		ep_index++;

	return (struct usbssp_ep_ctx *) (ctx->bytes +
		(ep_index * CTX_SIZE(usbssp_data->hcc_params)));
}


/***************** Streams structures manipulation *************************/
static void usbssp_free_stream_ctx(struct usbssp_udc *usbssp_data,
				   unsigned int num_stream_ctxs,
				   struct usbssp_stream_ctx *stream_ctx,
				   dma_addr_t dma)
{
	struct device *dev = usbssp_data->dev;
	size_t size = sizeof(struct usbssp_stream_ctx) * num_stream_ctxs;

	if (size > MEDIUM_STREAM_ARRAY_SIZE)
		dma_free_coherent(dev, size, stream_ctx, dma);
	else if (size <= SMALL_STREAM_ARRAY_SIZE)
		return dma_pool_free(usbssp_data->small_streams_pool,
				stream_ctx, dma);
	else
		return dma_pool_free(usbssp_data->medium_streams_pool,
				stream_ctx, dma);
}


/*
 * The stream context array for each endpoint with bulk streams enabled can
 * vary in size, based on:
 *  - how many streams the endpoint supports,
 *  - the maximum primary stream array size the host controller supports,
 *  - and how many streams the device driver asks for.
 *
 * The stream context array must be a power of 2, and can be as small as
 * 64 bytes or as large as 1MB.
 */
static struct usbssp_stream_ctx *usbssp_alloc_stream_ctx(
					struct usbssp_udc *usbssp_data,
					unsigned int num_stream_ctxs,
					dma_addr_t *dma,
					gfp_t mem_flags)
{
	struct device *dev = usbssp_data->dev;
	size_t size = sizeof(struct usbssp_stream_ctx) * num_stream_ctxs;

	if (size > MEDIUM_STREAM_ARRAY_SIZE)
		return dma_alloc_coherent(dev, size,
				dma, mem_flags);
	else if (size <= SMALL_STREAM_ARRAY_SIZE)
		return dma_pool_alloc(usbssp_data->small_streams_pool,
				mem_flags, dma);
	else
		return dma_pool_alloc(usbssp_data->medium_streams_pool,
				mem_flags, dma);
}


struct usbssp_ring *usbssp_dma_to_transfer_ring(struct usbssp_ep *ep,
						u64 address)
{
	if (ep->ep_state & EP_HAS_STREAMS)
		return radix_tree_lookup(&ep->stream_info->trb_address_map,
					 address >> TRB_SEGMENT_SHIFT);
	return ep->ring;
}

struct usbssp_ring *usbssp_stream_id_to_ring(struct usbssp_device *dev,
					     unsigned int ep_index,
					     unsigned int stream_id)
{
	struct usbssp_ep *ep = &dev->eps[ep_index];

	if (stream_id == 0)
		return ep->ring;

	if (!ep->stream_info)
		return NULL;

	if (stream_id > ep->stream_info->num_streams)
		return NULL;
	return ep->stream_info->stream_rings[stream_id];
}

/*
 * Change an endpoint's internal structure so it supports stream IDs.  The
 * number of requested streams includes stream 0, which cannot be used by device
 * drivers.
 *
 * The number of stream contexts in the stream context array may be bigger than
 * the number of streams the driver wants to use.  This is because the number of
 * stream context array entries must be a power of two.
 */
struct usbssp_stream_info *usbssp_alloc_stream_info(
				struct usbssp_udc *usbssp_data,
				unsigned int num_stream_ctxs,
				unsigned int num_streams,
				unsigned int max_packet,
				gfp_t mem_flags)
{
	struct usbssp_stream_info *stream_info;
	u32 cur_stream;
	struct usbssp_ring *cur_ring;
	u64 addr;
	int ret;

	usbssp_dbg(usbssp_data,
		"Allocating %u streams and %u stream context array entries.\n",
		num_streams, num_stream_ctxs);

	if (usbssp_data->cmd_ring_reserved_trbs == MAX_RSVD_CMD_TRBS) {
		usbssp_dbg(usbssp_data,
			"Command ring has no reserved TRBs available\n");
		return NULL;
	}
	usbssp_data->cmd_ring_reserved_trbs++;

	stream_info = kzalloc(sizeof(struct usbssp_stream_info), mem_flags);
	if (!stream_info)
		goto cleanup_trbs;

	stream_info->num_streams = num_streams;
	stream_info->num_stream_ctxs = num_stream_ctxs;

	/* Initialize the array of virtual pointers to stream rings. */
	stream_info->stream_rings =
			kzalloc(sizeof(struct usbssp_ring *)*num_streams,
				mem_flags);
	if (!stream_info->stream_rings)
		goto cleanup_info;

	/* Initialize the array of DMA addresses for stream rings for the HW. */
	stream_info->stream_ctx_array = usbssp_alloc_stream_ctx(
			usbssp_data, num_stream_ctxs,
			&stream_info->ctx_array_dma, mem_flags);
	if (!stream_info->stream_ctx_array)
		goto cleanup_ctx;
	memset(stream_info->stream_ctx_array, 0,
			sizeof(struct usbssp_stream_ctx)*num_stream_ctxs);

	/* Allocate everything needed to free the stream rings later */
	stream_info->free_streams_command =
		usbssp_alloc_command(usbssp_data, true, mem_flags);
	if (!stream_info->free_streams_command)
		goto cleanup_ctx;

	INIT_RADIX_TREE(&stream_info->trb_address_map, GFP_ATOMIC);

	/* Allocate rings for all the streams that the driver will use,
	 * and add their segment DMA addresses to the radix tree.
	 * Stream 0 is reserved.
	 */

	for (cur_stream = 1; cur_stream < num_streams; cur_stream++) {
		stream_info->stream_rings[cur_stream] =
			usbssp_ring_alloc(usbssp_data, 2, 1, TYPE_STREAM,
					max_packet, mem_flags);
		cur_ring = stream_info->stream_rings[cur_stream];
		if (!cur_ring)
			goto cleanup_rings;
		cur_ring->stream_id = cur_stream;
		cur_ring->trb_address_map = &stream_info->trb_address_map;
		/* Set deq ptr, cycle bit, and stream context type */
		addr = cur_ring->first_seg->dma | SCT_FOR_CTX(SCT_PRI_TR) |
				cur_ring->cycle_state;
		stream_info->stream_ctx_array[cur_stream].stream_ring =
				cpu_to_le64(addr);
		usbssp_dbg(usbssp_data,
			"Setting stream %d ring ptr to 0x%08llx\n",
			cur_stream, (unsigned long long) addr);

		ret = usbssp_update_stream_mapping(cur_ring, mem_flags);
		if (ret) {
			usbssp_ring_free(usbssp_data, cur_ring);
			stream_info->stream_rings[cur_stream] = NULL;
			goto cleanup_rings;
		}
	}
	/* Leave the other unused stream ring pointers in the stream context
	 * array initialized to zero.  This will cause the DC to give us an
	 * error if the host asks for a stream ID we don't have setup (if it
	 * was any other way, the device controller would assume the ring is
	 * "empty" and wait forever for data to be queued to that stream ID).
	 */

	return stream_info;

cleanup_rings:
	for (cur_stream = 1; cur_stream < num_streams; cur_stream++) {
		cur_ring = stream_info->stream_rings[cur_stream];
		if (cur_ring) {
			usbssp_ring_free(usbssp_data, cur_ring);
			stream_info->stream_rings[cur_stream] = NULL;
		}
	}
	usbssp_free_command(usbssp_data, stream_info->free_streams_command);
cleanup_ctx:
	kfree(stream_info->stream_rings);
cleanup_info:
	kfree(stream_info);
cleanup_trbs:
	usbssp_data->cmd_ring_reserved_trbs--;
	return NULL;
}

/*
 * Sets the MaxPStreams field and the Linear Stream Array field.
 * Sets the dequeue pointer to the stream context array.
 */
void usbssp_setup_streams_ep_input_ctx(struct usbssp_udc *usbssp_data,
				       struct usbssp_ep_ctx *ep_ctx,
				       struct usbssp_stream_info *stream_info)
{
	u32 max_primary_streams;
	/* MaxPStreams is the number of stream context array entries, not the
	 * number we're actually using.  Must be in 2^(MaxPstreams + 1) format.
	 * fls(0) = 0, fls(0x1) = 1, fls(0x10) = 2, fls(0x100) = 3, etc.
	 */
	max_primary_streams = fls(stream_info->num_stream_ctxs) - 2;
	usbssp_dbg_trace(usbssp_data,  trace_usbssp_dbg_context_change,
			"Setting number of stream ctx array entries to %u",
			1 << (max_primary_streams + 1));
	ep_ctx->ep_info &= cpu_to_le32(~EP_MAXPSTREAMS_MASK);
	ep_ctx->ep_info |= cpu_to_le32(EP_MAXPSTREAMS(max_primary_streams)
				| EP_HAS_LSA);
	ep_ctx->deq  = cpu_to_le64(stream_info->ctx_array_dma);
}

/*
 * Sets the MaxPStreams field and the Linear Stream Array field to 0.
 * Reinstalls the "normal" endpoint ring (at its previous dequeue mark,
 * not at the beginning of the ring).
 */
void usbssp_setup_no_streams_ep_input_ctx(struct usbssp_ep_ctx *ep_ctx,
					  struct usbssp_ep *ep)
{
	dma_addr_t addr;

	ep_ctx->ep_info &= cpu_to_le32(~(EP_MAXPSTREAMS_MASK | EP_HAS_LSA));
	addr = usbssp_trb_virt_to_dma(ep->ring->deq_seg, ep->ring->dequeue);
	ep_ctx->deq  = cpu_to_le64(addr | ep->ring->cycle_state);
}

/* Frees all stream contexts associated with the endpoint,
 *
 * Caller should fix the endpoint context streams fields.
 */
void usbssp_free_stream_info(struct usbssp_udc *usbssp_data,
			     struct usbssp_stream_info *stream_info)
{
	int cur_stream;
	struct usbssp_ring *cur_ring;

	if (!stream_info)
		return;

	for (cur_stream = 1; cur_stream < stream_info->num_streams;
			cur_stream++) {
		cur_ring = stream_info->stream_rings[cur_stream];
		if (cur_ring) {
			usbssp_ring_free(usbssp_data, cur_ring);
			stream_info->stream_rings[cur_stream] = NULL;
		}
	}
	usbssp_free_command(usbssp_data, stream_info->free_streams_command);
	usbssp_data->cmd_ring_reserved_trbs--;
	if (stream_info->stream_ctx_array)
		usbssp_free_stream_ctx(usbssp_data,
				stream_info->num_stream_ctxs,
				stream_info->stream_ctx_array,
				stream_info->ctx_array_dma);

	kfree(stream_info->stream_rings);
	kfree(stream_info);
}


/***************** Device context manipulation *************************/

/* All the usbssp_tds in the ring's TD list should be freed at this point.
 */
void usbssp_free_priv_device(struct usbssp_udc *usbssp_data)
{
	struct usbssp_device *dev;
	int i;

	/* if slot_id = 0 then no device slot is used */
	if (usbssp_data->slot_id == 0)
		return;

	dev = &usbssp_data->devs;
	trace_usbssp_free_priv_device(dev);

	usbssp_data->dcbaa->dev_context_ptrs[usbssp_data->slot_id] = 0;
	if (!dev)
		return;

	for (i = 0; i < 31; ++i) {
		if (dev->eps[i].ring)
			usbssp_ring_free(usbssp_data, dev->eps[i].ring);

		if (dev->eps[i].stream_info) {
			usbssp_free_stream_info(usbssp_data,
					dev->eps[i].stream_info);
		}
	}

	if (dev->in_ctx)
		usbssp_free_container_ctx(usbssp_data, dev->in_ctx);
	if (dev->out_ctx)
		usbssp_free_container_ctx(usbssp_data, dev->out_ctx);

	usbssp_data->slot_id = 0;
}

int usbssp_alloc_priv_device(struct usbssp_udc *usbssp_data, gfp_t flags)
{
	struct usbssp_device *priv_dev;

	/* Slot ID 0 is reserved */
	if (usbssp_data->slot_id == 0) {
		usbssp_warn(usbssp_data, "Bad Slot ID %d\n",
				usbssp_data->slot_id);
		return 0;
	}

	priv_dev = &usbssp_data->devs;

	/* Allocate the (output) device context that will be
	 * used in the USBSSP.
	 */
	priv_dev->out_ctx = usbssp_alloc_container_ctx(usbssp_data,
			USBSSP_CTX_TYPE_DEVICE, flags);

	if (!priv_dev->out_ctx)
		goto fail;

	usbssp_dbg(usbssp_data, "Slot %d output ctx = 0x%llx (dma)\n",
			usbssp_data->slot_id,
			(unsigned long long)priv_dev->out_ctx->dma);

	/* Allocate the (input) device context for address device command */
	priv_dev->in_ctx = usbssp_alloc_container_ctx(usbssp_data,
			USBSSP_CTX_TYPE_INPUT, flags);

	if (!priv_dev->in_ctx)
		goto fail;

	usbssp_dbg(usbssp_data, "Slot %d input ctx = 0x%llx (dma)\n",
			usbssp_data->slot_id,
			(unsigned long long)priv_dev->in_ctx->dma);

	/* Allocate endpoint 0 ring */
	priv_dev->eps[0].ring = usbssp_ring_alloc(usbssp_data, 2, 1,
			TYPE_CTRL, 0, flags);
	if (!priv_dev->eps[0].ring)
		goto fail;

	priv_dev->gadget = &usbssp_data->gadget;

	/* Point to output device context in dcbaa. */
	usbssp_data->dcbaa->dev_context_ptrs[usbssp_data->slot_id] =
		cpu_to_le64(priv_dev->out_ctx->dma);
	usbssp_dbg(usbssp_data, "Set slot id %d dcbaa entry %p to 0x%llx\n",
		usbssp_data->slot_id,
		&usbssp_data->dcbaa->dev_context_ptrs[usbssp_data->slot_id],
		le64_to_cpu(usbssp_data->dcbaa->dev_context_ptrs[usbssp_data->slot_id]));

	trace_usbssp_alloc_priv_device(priv_dev);
	return 1;
fail:
	if (priv_dev->in_ctx)
		usbssp_free_container_ctx(usbssp_data, priv_dev->in_ctx);
	if (priv_dev->out_ctx)
		usbssp_free_container_ctx(usbssp_data, priv_dev->out_ctx);

	return 0;
}

void usbssp_copy_ep0_dequeue_into_input_ctx(struct usbssp_udc *usbssp_data)
{
	struct usbssp_device *priv_dev;
	struct usbssp_ep_ctx *ep0_ctx;
	struct usbssp_ring *ep_ring;

	priv_dev = &usbssp_data->devs;
	ep0_ctx = usbssp_get_ep_ctx(usbssp_data, priv_dev->in_ctx, 0);
	ep_ring = priv_dev->eps[0].ring;
	/*
	 * We don't keep track of the dequeue pointer very well after a
	 * Set TR dequeue pointer, so we're setting the dequeue pointer of the
	 * device to our enqueue pointer.  This should only be called after a
	 * configured device has reset, so all control transfers should have
	 * been completed or cancelled before the reset.
	 */
	ep0_ctx->deq = cpu_to_le64(usbssp_trb_virt_to_dma(ep_ring->enq_seg,
			ep_ring->enqueue) | ep_ring->cycle_state);
}

/* Setup an DC private device for a Set Address command */
int usbssp_setup_addressable_priv_dev(struct usbssp_udc *usbssp_data)
{
	struct usbssp_device *dev_priv;
	struct usbssp_ep_ctx *ep0_ctx;
	struct usbssp_slot_ctx *slot_ctx;
	u32 max_packets;

	dev_priv = &usbssp_data->devs;
	/* Slot ID 0 is reserved */
	if (usbssp_data->slot_id == 0 || !dev_priv->gadget) {
		usbssp_warn(usbssp_data,
			"Slot ID %d is not assigned to this device\n",
			usbssp_data->slot_id);
		return -EINVAL;
	}

	ep0_ctx = usbssp_get_ep_ctx(usbssp_data, dev_priv->in_ctx, 0);
	slot_ctx = usbssp_get_slot_ctx(usbssp_data, dev_priv->in_ctx);

	/* 3) Only the control endpoint is valid - one endpoint context */
	slot_ctx->dev_info |= cpu_to_le32(LAST_CTX(1) /*| udev->route*/);

	switch (dev_priv->gadget->speed) {
	case USB_SPEED_SUPER_PLUS:
		slot_ctx->dev_info |= cpu_to_le32(SLOT_SPEED_SSP);
		max_packets = MAX_PACKET(512);
		break;
	case USB_SPEED_SUPER:
		slot_ctx->dev_info |= cpu_to_le32(SLOT_SPEED_SS);
		max_packets = MAX_PACKET(512);
		break;
	case USB_SPEED_HIGH:
		slot_ctx->dev_info |= cpu_to_le32(SLOT_SPEED_HS);
		max_packets = MAX_PACKET(64);
		break;
	case USB_SPEED_FULL:
		slot_ctx->dev_info |= cpu_to_le32(SLOT_SPEED_FS);
		max_packets = MAX_PACKET(64);
		break;
	case USB_SPEED_LOW:
		slot_ctx->dev_info |= cpu_to_le32(SLOT_SPEED_LS);
		max_packets = MAX_PACKET(8);
		break;
	case USB_SPEED_WIRELESS:
		usbssp_dbg(usbssp_data,
			"USBSSP doesn't support wireless speeds\n");
		return -EINVAL;
	default:
		/* Speed was not set , this shouldn't happen. */
		return -EINVAL;
	}

	if (!usbssp_data->devs.port_num)
		return -EINVAL;

	slot_ctx->dev_info2 |=
			cpu_to_le32(ROOT_DEV_PORT(usbssp_data->devs.port_num));
	slot_ctx->dev_state |= (usbssp_data->device_address & DEV_ADDR_MASK);

	ep0_ctx->tx_info = EP_AVG_TRB_LENGTH(0x8);
		/*cpu_to_le32(EP_MAX_ESIT_PAYLOAD_LO(max_esit_payload) |*/

	/* Step 4 - ring already allocated */
	/* Step 5 */
	ep0_ctx->ep_info2 = cpu_to_le32(EP_TYPE(CTRL_EP));

	/* EP 0 can handle "burst" sizes of 1, so Max Burst Size field is 0 */
	ep0_ctx->ep_info2 |= cpu_to_le32(MAX_BURST(0) | ERROR_COUNT(3) |
			max_packets);

	ep0_ctx->deq = cpu_to_le64(dev_priv->eps[0].ring->first_seg->dma |
			dev_priv->eps[0].ring->cycle_state);

	trace_usbssp_setup_addressable_priv_device(dev_priv);
	/* Steps 7 and 8 were done in usbssp_alloc_priv_device() */

	return 0;
}

/*
 * Convert interval expressed as 2^(bInterval - 1) == interval into
 * straight exponent value 2^n == interval.
 *
 */
static unsigned int usbssp_parse_exponent_interval(struct usb_gadget *g,
		struct usbssp_ep *dep)
{
	unsigned int interval;

	interval = clamp_val(dep->endpoint.desc->bInterval, 1, 16) - 1;
	if (interval != dep->endpoint.desc->bInterval - 1)
		dev_warn(&g->dev,
				"ep %#x - rounding interval to %d %sframes\n",
				dep->endpoint.desc->bEndpointAddress,
				1 << interval,
				g->speed == USB_SPEED_FULL ? "" : "micro");

	if (g->speed == USB_SPEED_FULL) {
		/*
		 * Full speed isoc endpoints specify interval in frames,
		 * not microframes. We are using microframes everywhere,
		 * so adjust accordingly.
		 */
		interval += 3;	/* 1 frame = 2^3 uframes */
	}

	return interval;
}

/*
 * Convert bInterval expressed in microframes (in 1-255 range) to exponent of
 * microframes, rounded down to nearest power of 2.
 */
static unsigned int usbssp_microframes_to_exponent(struct usb_gadget *g,
						   struct usbssp_ep *dep,
						   unsigned int desc_interval,
						   unsigned int min_exponent,
						   unsigned int max_exponent)
{
	unsigned int interval;

	interval = fls(desc_interval) - 1;
	interval = clamp_val(interval, min_exponent, max_exponent);
	if ((1 << interval) != desc_interval)
		dev_dbg(&g->dev,
			"ep %#x - rounding interval to %d microframes,"
			"ep desc says %d microframes\n",
			dep->endpoint.desc->bEndpointAddress,
			1 << interval,
			desc_interval);

	return interval;
}

static unsigned int usbssp_parse_microframe_interval(struct usb_gadget *g,
		struct usbssp_ep *dep)
{
	if (dep->endpoint.desc->bInterval == 0)
		return 0;
	return usbssp_microframes_to_exponent(g, dep,
			dep->endpoint.desc->bInterval, 0, 15);
}


static unsigned int usbssp_parse_frame_interval(struct usb_gadget *g,
						struct usbssp_ep *dep)
{

	return usbssp_microframes_to_exponent(g, dep,
			dep->endpoint.desc->bInterval * 8, 3, 10);
}

/* Return the polling or NAK interval.
 *
 * The polling interval is expressed in "microframes".  If DC's Interval field
 * is set to N, it will service the endpoint every 2^(Interval)*125us.
 *
 * The NAK interval is one NAK per 1 to 255 microframes, or no NAKs if interval
 * is set to 0.
 */
static unsigned int usbssp_get_endpoint_interval(struct usb_gadget *g,
						 struct usbssp_ep *dep)
{
	unsigned int interval = 0;

	switch (g->speed) {
	case USB_SPEED_HIGH:
		/* Max NAK rate */
		if (usb_endpoint_xfer_control(dep->endpoint.desc) ||
		    usb_endpoint_xfer_bulk(dep->endpoint.desc)) {
			interval = usbssp_parse_microframe_interval(g, dep);
			break;
		}
		/* Fall through - SS and HS isoc/int have same decoding */

	case USB_SPEED_SUPER_PLUS:
	case USB_SPEED_SUPER:
		if (usb_endpoint_xfer_int(dep->endpoint.desc) ||
		    usb_endpoint_xfer_isoc(dep->endpoint.desc)) {
			interval = usbssp_parse_exponent_interval(g, dep);
		}
		break;

	case USB_SPEED_FULL:
		if (usb_endpoint_xfer_isoc(dep->endpoint.desc)) {
			interval = usbssp_parse_exponent_interval(g, dep);
			break;
		}
		/*
		 * Fall through for interrupt endpoint interval decoding
		 * since it uses the same rules as low speed interrupt
		 * endpoints.
		 */

	case USB_SPEED_LOW:
		if (usb_endpoint_xfer_int(dep->endpoint.desc) ||
		    usb_endpoint_xfer_isoc(dep->endpoint.desc)) {

			interval = usbssp_parse_frame_interval(g, dep);
		}
		break;

	default:
		BUG();
	}
	return interval;
}

/* The "Mult" field in the endpoint context is only set for SuperSpeed isoc eps.
 * High speed endpoint descriptors can define "the number of additional
 * transaction opportunities per microframe", but that goes in the Max Burst
 * endpoint context field.
 */
static u32 usbssp_get_endpoint_mult(struct usb_gadget *g,
				    struct usbssp_ep *dep)
{
	if (g->speed < USB_SPEED_SUPER ||
	    !usb_endpoint_xfer_isoc(dep->endpoint.desc))
		return 0;

	return dep->endpoint.comp_desc->bmAttributes;
}

static u32 usbssp_get_endpoint_max_burst(struct usb_gadget *g,
					 struct usbssp_ep *dep)
{
	/* Super speed and Plus have max burst in ep companion desc */
	if (g->speed >= USB_SPEED_SUPER)
		return dep->endpoint.comp_desc->bMaxBurst;

	if (g->speed == USB_SPEED_HIGH &&
	   (usb_endpoint_xfer_isoc(dep->endpoint.desc) ||
	    usb_endpoint_xfer_int(dep->endpoint.desc)))
		return (usb_endpoint_maxp(dep->endpoint.desc) & 0x1800) >> 11;

	return 0;
}

static u32 usbssp_get_endpoint_type(const struct usb_endpoint_descriptor *desc)
{
	int in;

	in = usb_endpoint_dir_in(desc);

	switch (usb_endpoint_type(desc)) {
	case USB_ENDPOINT_XFER_CONTROL:
		return CTRL_EP;
	case USB_ENDPOINT_XFER_BULK:
		return in ? BULK_IN_EP : BULK_OUT_EP;
	case USB_ENDPOINT_XFER_ISOC:
		return in ? ISOC_IN_EP : ISOC_OUT_EP;
	case USB_ENDPOINT_XFER_INT:
		return in ? INT_IN_EP : INT_OUT_EP;
	}
	return 0;
}

/* Return the maximum endpoint service interval time (ESIT) payload.
 * Basically, this is the maxpacket size, multiplied by the burst size
 * and mult size.
 */
static u32 usbssp_get_max_esit_payload(struct usb_gadget *g,
		struct usbssp_ep *dep)
{
	int max_burst;
	int max_packet;

	/* Only applies for interrupt or isochronous endpoints*/
	if (usb_endpoint_xfer_control(dep->endpoint.desc) ||
	    usb_endpoint_xfer_bulk(dep->endpoint.desc))
		return 0;

	/* SuperSpeedPlus Isoc ep sending over 48k per esit*/

	if ((g->speed >= USB_SPEED_SUPER_PLUS) &&
	    USB_SS_SSP_ISOC_COMP(dep->endpoint.desc->bmAttributes))
		return le32_to_cpu(dep->endpoint.comp_desc->wBytesPerInterval);
	/* SuperSpeed or SuperSpeedPlus Isoc ep with less than 48k per esit */
	else if (g->speed >= USB_SPEED_SUPER)
		return le16_to_cpu(dep->endpoint.comp_desc->wBytesPerInterval);

	max_packet = usb_endpoint_maxp(dep->endpoint.desc);
	max_burst = usb_endpoint_maxp_mult(dep->endpoint.desc);
	/* A 0 in max burst means 1 transfer per ESIT */
	return max_packet * max_burst;
}

/* Set up an endpoint with one ring segment.  Do not allocate stream rings.
 * Drivers will have to call usb_alloc_streams() to do that.
 */
int usbssp_endpoint_init(struct usbssp_udc *usbssp_data,
			 struct usbssp_device *dev_priv,
			 struct usbssp_ep *dep,
			 gfp_t mem_flags)
{
	unsigned int ep_index;
	struct usbssp_ep_ctx *ep_ctx;
	struct usbssp_ring *ep_ring;
	unsigned int max_packet;
	enum usbssp_ring_type ring_type;
	u32 max_esit_payload;
	u32 endpoint_type;
	unsigned int max_burst;
	unsigned int interval;
	unsigned int mult;
	unsigned int avg_trb_len;
	unsigned int err_count = 0;

	ep_index = usbssp_get_endpoint_index(dep->endpoint.desc);
	ep_ctx = usbssp_get_ep_ctx(usbssp_data, dev_priv->in_ctx, ep_index);

	endpoint_type = usbssp_get_endpoint_type(dep->endpoint.desc);
	if (!endpoint_type)
		return -EINVAL;

	ring_type = usb_endpoint_type(dep->endpoint.desc);

	/*
	 * Get values to fill the endpoint context, mostly from ep descriptor.
	 * The average TRB buffer lengt for bulk endpoints is unclear as we
	 * have no clue on scatter gather list entry size. For Isoc and Int,
	 * set it to max available.
	 */
	max_esit_payload =
			usbssp_get_max_esit_payload(&usbssp_data->gadget, dep);
	interval = usbssp_get_endpoint_interval(&usbssp_data->gadget, dep);
	mult = usbssp_get_endpoint_mult(&usbssp_data->gadget, dep);
	max_packet = GET_MAX_PACKET(usb_endpoint_maxp(dep->endpoint.desc));
	max_burst = usbssp_get_endpoint_max_burst(&usbssp_data->gadget, dep);
	avg_trb_len = max_esit_payload;

	/* Allow 3 retries for everything but isoc, set CErr = 3 */
	if (!usb_endpoint_xfer_isoc(dep->endpoint.desc))
		err_count = 3;
	if (usb_endpoint_xfer_bulk(dep->endpoint.desc) &&
	    usbssp_data->gadget.speed == USB_SPEED_HIGH)
		max_packet = 512;
	/* DC spec indicates that ctrl ep avg TRB Length should be 8 */
	if (usb_endpoint_xfer_control(dep->endpoint.desc))
		avg_trb_len = 8;

	/* Set up the endpoint ring */
	dev_priv->eps[ep_index].new_ring = usbssp_ring_alloc(usbssp_data, 2, 1,
					ring_type, max_packet, mem_flags);

	dev_priv->eps[ep_index].skip = false;
	ep_ring = dev_priv->eps[ep_index].new_ring;

	/* Fill the endpoint context */
	ep_ctx->ep_info = cpu_to_le32(EP_MAX_ESIT_PAYLOAD_HI(max_esit_payload) |
			EP_INTERVAL(interval) | EP_MULT(mult));
	ep_ctx->ep_info2 = cpu_to_le32(EP_TYPE(endpoint_type) |
			MAX_PACKET(max_packet) | MAX_BURST(max_burst) |
			ERROR_COUNT(err_count));
	ep_ctx->deq = cpu_to_le64(ep_ring->first_seg->dma |
			ep_ring->cycle_state);

	ep_ctx->tx_info = cpu_to_le32(EP_MAX_ESIT_PAYLOAD_LO(max_esit_payload) |
			EP_AVG_TRB_LENGTH(avg_trb_len));

	return 0;
}

void usbssp_endpoint_zero(struct usbssp_udc *usbssp_data,
			  struct usbssp_device *dev_priv,
			  struct usbssp_ep *ep)
{
	unsigned int ep_index;
	struct usbssp_ep_ctx *ep_ctx;

	ep_index = usbssp_get_endpoint_index(ep->endpoint.desc);
	ep_ctx = usbssp_get_ep_ctx(usbssp_data, dev_priv->in_ctx, ep_index);

	ep_ctx->ep_info = 0;
	ep_ctx->ep_info2 = 0;
	ep_ctx->deq = 0;
	ep_ctx->tx_info = 0;
	/* Don't free the endpoint ring until the set interface or configuration
	 * request succeeds.
	 */
}

/* Copy output usbssp_ep_ctx to the input usbssp_ep_ctx copy.
 * Useful when you want to change one particular aspect of the endpoint and then
 * issue a configure endpoint command.
 */
void usbssp_endpoint_copy(struct usbssp_udc *usbssp_data,
			  struct usbssp_container_ctx *in_ctx,
			  struct usbssp_container_ctx *out_ctx,
			  unsigned int ep_index)
{
	struct usbssp_ep_ctx *out_ep_ctx;
	struct usbssp_ep_ctx *in_ep_ctx;

	out_ep_ctx = usbssp_get_ep_ctx(usbssp_data, out_ctx, ep_index);
	in_ep_ctx = usbssp_get_ep_ctx(usbssp_data, in_ctx, ep_index);

	in_ep_ctx->ep_info = out_ep_ctx->ep_info;
	in_ep_ctx->ep_info2 = out_ep_ctx->ep_info2;
	in_ep_ctx->deq = out_ep_ctx->deq;
	in_ep_ctx->tx_info = out_ep_ctx->tx_info;
}

/* Copy output usbssp_slot_ctx to the input usbssp_slot_ctx.
 * Useful when you want to change one particular aspect of the endpoint and then
 * issue a configure endpoint command.  Only the context entries field matters,
 * but we'll copy the whole thing anyway.
 */
void usbssp_slot_copy(struct usbssp_udc *usbssp_data,
		      struct usbssp_container_ctx *in_ctx,
		      struct usbssp_container_ctx *out_ctx)
{
	struct usbssp_slot_ctx *in_slot_ctx;
	struct usbssp_slot_ctx *out_slot_ctx;

	in_slot_ctx = usbssp_get_slot_ctx(usbssp_data, in_ctx);
	out_slot_ctx = usbssp_get_slot_ctx(usbssp_data, out_ctx);

	in_slot_ctx->dev_info = out_slot_ctx->dev_info;
	in_slot_ctx->dev_info2 = out_slot_ctx->dev_info2;
	in_slot_ctx->int_target = out_slot_ctx->int_target;
	in_slot_ctx->dev_state = out_slot_ctx->dev_state;
}

/* Set up the scratchpad buffer array and scratchpad buffers, if needed. */
static int scratchpad_alloc(struct usbssp_udc *usbssp_data, gfp_t flags)
{
	int i;
	struct device *dev = usbssp_data->dev;
	int num_sp = HCS_MAX_SCRATCHPAD(usbssp_data->hcs_params2);

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"Allocating %d scratchpad buffers", num_sp);

	if (!num_sp)
		return 0;

	usbssp_data->scratchpad = kzalloc(sizeof(*usbssp_data->scratchpad),
			 flags);
	if (!usbssp_data->scratchpad)
		goto fail_sp;

	usbssp_data->scratchpad->sp_array =
			dma_alloc_coherent(dev, num_sp * sizeof(u64),
					&usbssp_data->scratchpad->sp_dma,
					flags);

	if (!usbssp_data->scratchpad->sp_array)
		goto fail_sp2;

	usbssp_data->scratchpad->sp_buffers = kzalloc(sizeof(void *) * num_sp,
			flags);
	if (!usbssp_data->scratchpad->sp_buffers)
		goto fail_sp3;

	usbssp_data->dcbaa->dev_context_ptrs[0] =
			cpu_to_le64(usbssp_data->scratchpad->sp_dma);
	for (i = 0; i < num_sp; i++) {
		dma_addr_t dma;

		void *buf = dma_zalloc_coherent(dev, usbssp_data->page_size,
				&dma, flags);

		if (!buf)
			goto fail_sp4;

		usbssp_data->scratchpad->sp_array[i] = dma;
		usbssp_data->scratchpad->sp_buffers[i] = buf;
	}

	return 0;

fail_sp4:
	for (i = i - 1; i >= 0; i--) {
		dma_free_coherent(dev, usbssp_data->page_size,
				usbssp_data->scratchpad->sp_buffers[i],
				usbssp_data->scratchpad->sp_array[i]);
	}

	kfree(usbssp_data->scratchpad->sp_buffers);

fail_sp3:
	dma_free_coherent(dev, num_sp * sizeof(u64),
			usbssp_data->scratchpad->sp_array,
			usbssp_data->scratchpad->sp_dma);

fail_sp2:
	kfree(usbssp_data->scratchpad);
	usbssp_data->scratchpad = NULL;

fail_sp:
	return -ENOMEM;
}

static void scratchpad_free(struct usbssp_udc *usbssp_data)
{
	int num_sp;
	int i;
	struct device *dev = usbssp_data->dev;

	if (!usbssp_data->scratchpad)
		return;

	num_sp = HCS_MAX_SCRATCHPAD(usbssp_data->hcs_params2);

	for (i = 0; i < num_sp; i++) {
		dma_free_coherent(dev, usbssp_data->page_size,
				usbssp_data->scratchpad->sp_buffers[i],
				usbssp_data->scratchpad->sp_array[i]);
	}

	kfree(usbssp_data->scratchpad->sp_buffers);
	dma_free_coherent(dev, num_sp * sizeof(u64),
			usbssp_data->scratchpad->sp_array,
			usbssp_data->scratchpad->sp_dma);
	kfree(usbssp_data->scratchpad);
	usbssp_data->scratchpad = NULL;
}

struct usbssp_command *usbssp_alloc_command(struct usbssp_udc *usbssp_data,
					    bool allocate_completion,
					    gfp_t mem_flags)
{
	struct usbssp_command *command;

	command = kzalloc(sizeof(*command), mem_flags);
	if (!command)
		return NULL;

	if (allocate_completion) {
		command->completion =
			kzalloc(sizeof(struct completion), mem_flags);
		if (!command->completion) {
			kfree(command);
			return NULL;
		}
		init_completion(command->completion);
	}

	command->status = 0;
	INIT_LIST_HEAD(&command->cmd_list);

	return command;
}

struct usbssp_command *usbssp_alloc_command_with_ctx(
					struct usbssp_udc *usbssp_data,
					bool allocate_completion,
					gfp_t mem_flags)
{
	struct usbssp_command *command;

	command = usbssp_alloc_command(usbssp_data,
			allocate_completion, mem_flags);
	if (!command)
		return NULL;

	command->in_ctx = usbssp_alloc_container_ctx(usbssp_data,
				USBSSP_CTX_TYPE_INPUT, mem_flags);
	if (!command->in_ctx) {
		kfree(command->completion);
		kfree(command);
		return NULL;
	}
	return command;
}

void usbssp_request_free_priv(struct usbssp_request *priv_req)
{
	if (priv_req)
		kfree(priv_req->td);
}

void usbssp_free_command(struct usbssp_udc *usbssp_data,
			 struct usbssp_command *command)
{
	usbssp_free_container_ctx(usbssp_data, command->in_ctx);
	kfree(command->completion);
	kfree(command);
}

int usbssp_alloc_erst(struct usbssp_udc *usbssp_data,
		      struct usbssp_ring *evt_ring,
		      struct usbssp_erst *erst,
		      gfp_t flags)
{
	size_t size;
	unsigned int val;
	struct usbssp_segment *seg;
	struct usbssp_erst_entry *entry;

	size = sizeof(struct usbssp_erst_entry) * evt_ring->num_segs;
	erst->entries = dma_zalloc_coherent(usbssp_data->dev,
			size, &erst->erst_dma_addr, flags);
	if (!erst->entries)
		return -ENOMEM;

	erst->num_entries = evt_ring->num_segs;

	seg = evt_ring->first_seg;
	for (val = 0; val < evt_ring->num_segs; val++) {
		entry = &erst->entries[val];
		entry->seg_addr = cpu_to_le64(seg->dma);
		entry->seg_size = cpu_to_le32(TRBS_PER_SEGMENT);
		entry->rsvd = 0;
		seg = seg->next;
	}

	return 0;
}

void usbssp_free_erst(struct usbssp_udc *usbssp_data, struct usbssp_erst *erst)
{
	size_t size;
	struct device *dev = usbssp_data->dev;

	size = sizeof(struct usbssp_erst_entry) * (erst->num_entries);
	if (erst->entries)
		dma_free_coherent(dev, size, erst->entries,
				erst->erst_dma_addr);
	erst->entries = NULL;
}

void usbssp_mem_cleanup(struct usbssp_udc *usbssp_data)
{
	struct device	*dev = usbssp_data->dev;
	int num_ports;

	cancel_delayed_work_sync(&usbssp_data->cmd_timer);
	cancel_work_sync(&usbssp_data->bottom_irq);
	destroy_workqueue(usbssp_data->bottom_irq_wq);

	/* Free the Event Ring Segment Table and the actual Event Ring */
	usbssp_free_erst(usbssp_data, &usbssp_data->erst);

	if (usbssp_data->event_ring)
		usbssp_ring_free(usbssp_data, usbssp_data->event_ring);
	usbssp_data->event_ring = NULL;
	usbssp_dbg_trace(usbssp_data,
			trace_usbssp_dbg_init, "Freed event ring");

	if (usbssp_data->cmd_ring)
		usbssp_ring_free(usbssp_data, usbssp_data->cmd_ring);
	usbssp_data->cmd_ring = NULL;
	usbssp_dbg_trace(usbssp_data,
			trace_usbssp_dbg_init, "Freed command ring");
	usbssp_cleanup_command_queue(usbssp_data);

	num_ports = HCS_MAX_PORTS(usbssp_data->hcs_params1);

	usbssp_free_priv_device(usbssp_data);

	dma_pool_destroy(usbssp_data->segment_pool);
	usbssp_data->segment_pool = NULL;
	usbssp_dbg_trace(usbssp_data,
			trace_usbssp_dbg_init, "Freed segment pool");
	dma_pool_destroy(usbssp_data->device_pool);
	usbssp_data->device_pool = NULL;
	usbssp_dbg_trace(usbssp_data,
			trace_usbssp_dbg_init, "Freed device context pool");
	dma_pool_destroy(usbssp_data->small_streams_pool);
	usbssp_data->small_streams_pool = NULL;
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"Freed small stream array pool");

	dma_pool_destroy(usbssp_data->medium_streams_pool);
	usbssp_data->medium_streams_pool = NULL;
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"Freed medium stream array pool");

	if (usbssp_data->dcbaa)
		dma_free_coherent(dev, sizeof(*usbssp_data->dcbaa),
				usbssp_data->dcbaa, usbssp_data->dcbaa->dma);

	usbssp_data->dcbaa = NULL;

	scratchpad_free(usbssp_data);

	usbssp_data->cmd_ring_reserved_trbs = 0;
	usbssp_data->num_usb2_ports = 0;
	usbssp_data->num_usb3_ports = 0;
	usbssp_data->num_active_eps = 0;
	kfree(usbssp_data->port_array);
	kfree(usbssp_data->ext_caps);
	usbssp_data->usb2_ports = NULL;
	usbssp_data->usb3_ports = NULL;
	usbssp_data->port_array = NULL;
	usbssp_data->ext_caps = NULL;

	usbssp_data->page_size = 0;
	usbssp_data->page_shift = 0;
}

static int usbssp_test_trb_in_td(struct usbssp_udc *usbssp_data,
				 struct usbssp_segment *input_seg,
				 union usbssp_trb *start_trb,
				 union usbssp_trb *end_trb,
				 dma_addr_t input_dma,
				 struct usbssp_segment *result_seg,
				 char *test_name, int test_number)
{
	unsigned long long start_dma;
	unsigned long long end_dma;
	struct usbssp_segment *seg;

	start_dma = usbssp_trb_virt_to_dma(input_seg, start_trb);
	end_dma = usbssp_trb_virt_to_dma(input_seg, end_trb);

	seg = usbssp_trb_in_td(usbssp_data, input_seg, start_trb,
			end_trb, input_dma, false);
	if (seg != result_seg) {
		usbssp_warn(usbssp_data, "WARN: %s TRB math test %d failed!\n",
				test_name, test_number);
		usbssp_warn(usbssp_data, "Tested TRB math w/ seg %p and "
				"input DMA 0x%llx\n",
				input_seg,
				(unsigned long long) input_dma);
		usbssp_warn(usbssp_data, "starting TRB %p (0x%llx DMA), "
				"ending TRB %p (0x%llx DMA)\n",
				start_trb, start_dma,
				end_trb, end_dma);
		usbssp_warn(usbssp_data, "Expected seg %p, got seg %p\n",
				result_seg, seg);
		usbssp_trb_in_td(usbssp_data, input_seg, start_trb,
			end_trb, input_dma, true);
		return -1;
	}
	return 0;
}

/* TRB math checks for usbssp_trb_in_td(), using the command and event rings. */
static int usbssp_check_trb_in_td_math(struct usbssp_udc *usbssp_data)
{
	struct {
		dma_addr_t input_dma;
		struct usbssp_segment *result_seg;
	} simple_test_vector[] = {
		/* A zeroed DMA field should fail */
		{ 0, NULL },
		/* One TRB before the ring start should fail */
		{ usbssp_data->event_ring->first_seg->dma - 16, NULL },
		/* One byte before the ring start should fail */
		{ usbssp_data->event_ring->first_seg->dma - 1, NULL },
		/* Starting TRB should succeed */
		{ usbssp_data->event_ring->first_seg->dma,
				usbssp_data->event_ring->first_seg },
		/* Ending TRB should succeed */
		{ usbssp_data->event_ring->first_seg->dma +
				(TRBS_PER_SEGMENT - 1)*16,
			usbssp_data->event_ring->first_seg },
		/* One byte after the ring end should fail */
		{ usbssp_data->event_ring->first_seg->dma +
				(TRBS_PER_SEGMENT - 1)*16 + 1, NULL },
		/* One TRB after the ring end should fail */
		{ usbssp_data->event_ring->first_seg->dma +
				(TRBS_PER_SEGMENT)*16, NULL },
		/* An address of all ones should fail */
		{ (dma_addr_t) (~0), NULL },
	};
	struct {
		struct usbssp_segment *input_seg;
		union usbssp_trb *start_trb;
		union usbssp_trb *end_trb;
		dma_addr_t input_dma;
		struct usbssp_segment *result_seg;
	} complex_test_vector[] = {
		/* Test feeding a valid DMA address from a different ring */
		{	.input_seg = usbssp_data->event_ring->first_seg,
			.start_trb = usbssp_data->event_ring->first_seg->trbs,
			.end_trb = &usbssp_data->event_ring->first_seg->trbs[TRBS_PER_SEGMENT - 1],
			.input_dma = usbssp_data->cmd_ring->first_seg->dma,
			.result_seg = NULL,
		},
		/* Test feeding a valid end TRB from a different ring */
		{	.input_seg = usbssp_data->event_ring->first_seg,
			.start_trb = usbssp_data->event_ring->first_seg->trbs,
			.end_trb = &usbssp_data->cmd_ring->first_seg->trbs[TRBS_PER_SEGMENT - 1],
			.input_dma = usbssp_data->cmd_ring->first_seg->dma,
			.result_seg = NULL,
		},
		/* Test feeding a valid start and end TRB from a different ring */
		{	.input_seg = usbssp_data->event_ring->first_seg,
			.start_trb = usbssp_data->cmd_ring->first_seg->trbs,
			.end_trb = &usbssp_data->cmd_ring->first_seg->trbs[TRBS_PER_SEGMENT - 1],
			.input_dma = usbssp_data->cmd_ring->first_seg->dma,
			.result_seg = NULL,
		},
		/* TRB in this ring, but after this TD */
		{	.input_seg = usbssp_data->event_ring->first_seg,
			.start_trb = &usbssp_data->event_ring->first_seg->trbs[0],
			.end_trb = &usbssp_data->event_ring->first_seg->trbs[3],
			.input_dma = usbssp_data->event_ring->first_seg->dma + 4*16,
			.result_seg = NULL,
		},
		/* TRB in this ring, but before this TD */
		{	.input_seg = usbssp_data->event_ring->first_seg,
			.start_trb = &usbssp_data->event_ring->first_seg->trbs[3],
			.end_trb = &usbssp_data->event_ring->first_seg->trbs[6],
			.input_dma = usbssp_data->event_ring->first_seg->dma + 2*16,
			.result_seg = NULL,
		},
		/* TRB in this ring, but after this wrapped TD */
		{	.input_seg = usbssp_data->event_ring->first_seg,
			.start_trb = &usbssp_data->event_ring->first_seg->trbs[TRBS_PER_SEGMENT - 3],
			.end_trb = &usbssp_data->event_ring->first_seg->trbs[1],
			.input_dma = usbssp_data->event_ring->first_seg->dma + 2*16,
			.result_seg = NULL,
		},
		/* TRB in this ring, but before this wrapped TD */
		{	.input_seg = usbssp_data->event_ring->first_seg,
			.start_trb = &usbssp_data->event_ring->first_seg->trbs[TRBS_PER_SEGMENT - 3],
			.end_trb = &usbssp_data->event_ring->first_seg->trbs[1],
			.input_dma = usbssp_data->event_ring->first_seg->dma + (TRBS_PER_SEGMENT - 4)*16,
			.result_seg = NULL,
		},
		/* TRB not in this ring, and we have a wrapped TD */
		{	.input_seg = usbssp_data->event_ring->first_seg,
			.start_trb = &usbssp_data->event_ring->first_seg->trbs[TRBS_PER_SEGMENT - 3],
			.end_trb = &usbssp_data->event_ring->first_seg->trbs[1],
			.input_dma = usbssp_data->cmd_ring->first_seg->dma + 2*16,
			.result_seg = NULL,
		},
	};

	unsigned int num_tests;
	int i, ret;

	num_tests = ARRAY_SIZE(simple_test_vector);
	for (i = 0; i < num_tests; i++) {
		ret = usbssp_test_trb_in_td(usbssp_data,
				usbssp_data->event_ring->first_seg,
				usbssp_data->event_ring->first_seg->trbs,
				&usbssp_data->event_ring->first_seg->trbs[TRBS_PER_SEGMENT - 1],
				simple_test_vector[i].input_dma,
				simple_test_vector[i].result_seg,
				"Simple", i);
		if (ret < 0)
			return ret;
	}

	num_tests = ARRAY_SIZE(complex_test_vector);
	for (i = 0; i < num_tests; i++) {
		ret = usbssp_test_trb_in_td(usbssp_data,
				complex_test_vector[i].input_seg,
				complex_test_vector[i].start_trb,
				complex_test_vector[i].end_trb,
				complex_test_vector[i].input_dma,
				complex_test_vector[i].result_seg,
				"Complex", i);
		if (ret < 0)
			return ret;
	}
	usbssp_dbg(usbssp_data, "TRB math tests passed.\n");
	return 0;
}

static void usbssp_set_event_deq(struct usbssp_udc *usbssp_data)
{
	u64 temp;
	dma_addr_t deq;

	deq = usbssp_trb_virt_to_dma(usbssp_data->event_ring->deq_seg,
			usbssp_data->event_ring->dequeue);
	if (deq == 0 && !in_interrupt())
		usbssp_warn(usbssp_data,
			"WARN something wrong with SW event ring dequeue ptr.\n");
	/* Update USBSSP event ring dequeue pointer */
	temp = usbssp_read_64(usbssp_data, &usbssp_data->ir_set->erst_dequeue);
	temp &= ERST_PTR_MASK;
	/* Don't clear the EHB bit (which is RW1C) because
	 * there might be more events to service.
	 */
	temp &= ~ERST_EHB;
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
		"// Write event ring dequeue pointer, preserving EHB bit");
	usbssp_write_64(usbssp_data, ((u64) deq & (u64) ~ERST_PTR_MASK) | temp,
			&usbssp_data->ir_set->erst_dequeue);
}

static void usbssp_add_in_port(struct usbssp_udc *usbssp_data,
			       unsigned int num_ports,
			       __le32 __iomem *addr,
			       int max_caps)
{
	u32 temp, port_offset, port_count;
	int i;
	u8 major_revision;
	struct usbssp_ports *rport;

	temp = readl(addr);
	major_revision = USBSSP_EXT_PORT_MAJOR(temp);

	if (major_revision == 0x03) {
		rport = &usbssp_data->usb3_rhub;
	} else if (major_revision <= 0x02) {
		rport = &usbssp_data->usb2_rhub;
	} else {
		usbssp_warn(usbssp_data, "Ignoring unknown port speed, "
				"Ext Cap %p, revision = 0x%x\n",
				addr, major_revision);
		/* Ignoring port protocol we can't understand. */
		return;
	}

	rport->maj_rev = USBSSP_EXT_PORT_MAJOR(temp);
	rport->min_rev = USBSSP_EXT_PORT_MINOR(temp);

	/* Port offset and count in the third dword, see section 7.2 */
	temp = readl(addr + 2);
	port_offset = USBSSP_EXT_PORT_OFF(temp);
	port_count = USBSSP_EXT_PORT_COUNT(temp);
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
		"Ext Cap %p, port offset = %u, count = %u, revision = 0x%x",
		addr, port_offset, port_count, major_revision);

	if (port_count > 1) {
		usbssp_warn(usbssp_data,
			"DC support only single port but it detect %d ports",
			port_count);
		port_count = 1;
	}
	/* Port count includes the current port offset */
	if (port_offset == 0 || (port_offset + port_count - 1) > num_ports)
		return;

	rport->psi_count = USBSSP_EXT_PORT_PSIC(temp);
	if (rport->psi_count) {
		rport->psi = kcalloc(rport->psi_count, sizeof(*rport->psi),
				GFP_KERNEL);
		if (!rport->psi)
			rport->psi_count = 0;

		rport->psi_uid_count++;
		for (i = 0; i < rport->psi_count; i++) {
			rport->psi[i] = readl(addr + 4 + i);

			/* count unique ID values, two consecutive entries can
			 * have the same ID if link is assymetric
			 */
			if (i && (USBSSP_EXT_PORT_PSIV(rport->psi[i]) !=
				  USBSSP_EXT_PORT_PSIV(rport->psi[i - 1])))
				rport->psi_uid_count++;

			usbssp_dbg(usbssp_data,
				"PSIV:%d PSIE:%d PLT:%d PFD:%d LP:%d PSIM:%d\n",
				USBSSP_EXT_PORT_PSIV(rport->psi[i]),
				USBSSP_EXT_PORT_PSIE(rport->psi[i]),
				USBSSP_EXT_PORT_PLT(rport->psi[i]),
				USBSSP_EXT_PORT_PFD(rport->psi[i]),
				USBSSP_EXT_PORT_LP(rport->psi[i]),
				USBSSP_EXT_PORT_PSIM(rport->psi[i]));
		}
	}

	/* cache usb2 port capabilities */
	if (major_revision < 0x03 && usbssp_data->num_ext_caps < max_caps)
		usbssp_data->ext_caps[usbssp_data->num_ext_caps++] = temp;

	if (major_revision != 0x03) {
		usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
				"USBSSP: support USB2 software lpm");
		usbssp_data->sw_lpm_support = 1;
		if (temp & USBSSP_HLC) {
			usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
					"USBSSP: support USB2 hardware lpm");
			usbssp_data->hw_lpm_support = 1;
		}
	}

	usbssp_data->port_array[port_offset-1] = major_revision;
	if (major_revision == 0x03)
		usbssp_data->num_usb3_ports++;
	else
		usbssp_data->num_usb2_ports++;
}

/*
 * Scan the Extended Capabilities for the "Supported Protocol Capabilities" that
 * specify what speeds each port is supposed to be.
 */
static int usbssp_setup_port_arrays(struct usbssp_udc *usbssp_data, gfp_t flags)
{
	void __iomem *base;
	u32 offset;
	u32 port3offset = 0;
	u32 port2offset = 0;
	unsigned int num_ports;
	int i;
	int cap_count = 0;
	u32 cap_start;

	num_ports = HCS_MAX_PORTS(usbssp_data->hcs_params1);

	/*USBSSP can support only two ports - one for USB2.0 and
	 * second for USB3.0
	 */
	if (num_ports > MAX_USBSSP_PORTS) {
		usbssp_err(usbssp_data,
			"USBSSP-Dev can't support more then %d ports\n",
			MAX_USBSSP_PORTS);
		return -EINVAL;
	}

	usbssp_data->port_array =
		kzalloc(sizeof(*usbssp_data->port_array)*num_ports, flags);
	if (!usbssp_data->port_array)
		return -ENOMEM;

	base = &usbssp_data->cap_regs->hc_capbase;

	cap_start = usbssp_find_next_ext_cap(base, 0, USBSSP_EXT_CAPS_PROTOCOL);
	if (!cap_start) {
		usbssp_err(usbssp_data,
			"No Ext. Cap. registers, unable to set up ports\n");
		return -ENODEV;
	}

	offset = cap_start;

	/* count extended protocol capability entries for later caching */
	while (offset) {
		u32 temp;
		u8 major_revision;

		temp = readl(base + offset);
		major_revision = USBSSP_EXT_PORT_MAJOR(temp);

		if (major_revision == 0x03 && port3offset == 0)
			port3offset = offset;
		else if (major_revision <= 0x02 && port2offset == 0)
			port2offset = offset;

		cap_count++;

		offset = usbssp_find_next_ext_cap(base, offset,
				USBSSP_EXT_CAPS_PROTOCOL);
	}

	if (cap_count > MAX_USBSSP_PORTS) {
		usbssp_err(usbssp_data, "Too many  Ext. Cap. registers\n");
		return -EINVAL;
	}

	if (!port3offset &&  !port2offset) {
		usbssp_warn(usbssp_data, "No ports on the USBSSP?\n");
		return -ENODEV;
	}

	usbssp_data->ext_caps =
		kzalloc(sizeof(*usbssp_data->ext_caps) * cap_count, flags);
	if (!usbssp_data->ext_caps)
		return -ENOMEM;

	/** if exist add USB3 port*/
	if (port3offset)
		usbssp_add_in_port(usbssp_data, num_ports,
				base + port3offset, cap_count);

	/** add USB2 port*/
	if (port2offset)
		usbssp_add_in_port(usbssp_data, num_ports,
				base + port2offset, cap_count);

	if (usbssp_data->num_usb2_ports == 0 &&
	    usbssp_data->num_usb3_ports == 0) {
		usbssp_warn(usbssp_data, "No ports on the USBSSP?\n");
		return -ENODEV;
	}

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"Found %u USB 2.0 ports and %u USB 3.0 ports.",
			usbssp_data->num_usb2_ports,
			usbssp_data->num_usb3_ports);

	//Only one port USB3.0 and USB2.0 can be supported by USBSSP_DEV
	if (usbssp_data->num_usb3_ports > 1) {
		usbssp_err(usbssp_data, "Limiting USB 3.0 ports to 1\n");
		return -EINVAL;
	}

	if (usbssp_data->num_usb2_ports > 1) {
		usbssp_err(usbssp_data, "Limiting USB 2.0 ports to 1\n");
		return -EINVAL;
	}

	/*
	 * Note we could have only USB 3.0 ports, or  USB 2.0 ports.
	 */
	if (usbssp_data->num_usb2_ports) {
		for (i = 0; i < num_ports; i++) {

			if (usbssp_data->port_array[i] == 0x03)
				continue;
			usbssp_data->usb2_ports =
				&usbssp_data->op_regs->port_status_base +
				NUM_PORT_REGS*i;

			usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
					"USB 2.0 port at index %u, addr = %p",
					i, usbssp_data->usb2_ports);
		}
	}

	if (usbssp_data->num_usb3_ports) {

		for (i = 0; i < num_ports; i++)
			if (usbssp_data->port_array[i] == 0x03) {
				usbssp_data->usb3_ports =
					&usbssp_data->op_regs->port_status_base +
					NUM_PORT_REGS*i;

				usbssp_dbg_trace(usbssp_data,
					trace_usbssp_dbg_init,
					"USB 3.0 port at index %u, addr = %p",
					i, usbssp_data->usb3_ports);
			}
	}

	return 0;
}

void usbssp_force_fs_mode(struct usbssp_udc *usbssp_data)
{
	#define D_XEC_CFG_DEV_20PORT_REG6 0x2130
	#define D_XEC_CFG_DEV_20PORT_REG6_FORCE_FS 1

	writel(D_XEC_CFG_DEV_20PORT_REG6_FORCE_FS,
			usbssp_data->regs + D_XEC_CFG_DEV_20PORT_REG6);
}

int usbssp_mem_init(struct usbssp_udc *usbssp_data, gfp_t flags)
{
	dma_addr_t	dma;
	struct device	*dev = usbssp_data->dev;
	unsigned int	val, val2;
	u64		val_64;
	u32 page_size;
	int i, ret;

	INIT_LIST_HEAD(&usbssp_data->cmd_list);

	/* init command timeout work */
	INIT_DELAYED_WORK(&usbssp_data->cmd_timer,
			usbssp_handle_command_timeout);
	init_completion(&usbssp_data->cmd_ring_stop_completion);

	usbssp_data->bottom_irq_wq =
		create_singlethread_workqueue(dev_name(usbssp_data->dev));

	if (!usbssp_data->bottom_irq_wq)
		goto fail;

	INIT_WORK(&usbssp_data->bottom_irq, usbssp_bottom_irq);

	page_size = readl(&usbssp_data->op_regs->page_size);
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"Supported page size register = 0x%x", page_size);
	for (i = 0; i < 16; i++) {
		if ((0x1 & page_size) != 0)
			break;
		page_size = page_size >> 1;
	}
	if (i < 16)
		usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"Supported page size of %iK", (1 << (i+12)) / 1024);
	else
		usbssp_warn(usbssp_data, "WARN: no supported page size\n");

	/* Use 4K pages, since that's common and the minimum the
	 * USBSSP supports
	 */
	usbssp_data->page_shift = 12;
	usbssp_data->page_size = 1 << usbssp_data->page_shift;
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"USBSSP page size set to %iK",
			usbssp_data->page_size / 1024);

	/* In device mode this value should be equal 1*/
	val = DEV_HCS_MAX_SLOTS(readl(&usbssp_data->cap_regs->hcs_params1));
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"// USBSSP can handle at most %d device slots.", val);

	/*device should have only 1 slot*/
	if (val > DEV_MAX_SLOTS)
		pr_err("Invalid number of supported slots");

	val2 = readl(&usbssp_data->op_regs->config_reg);
	val |= (val2 & ~DEV_HCS_SLOTS_MASK);

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"// Setting Max device slots reg = 0x%x.", val);
	writel(val, &usbssp_data->op_regs->config_reg);

	/*
	 * Doorbell array must be physically contiguous
	 * and 64-byte (cache line) aligned.
	 */
	usbssp_data->dcbaa = dma_alloc_coherent(dev,
			sizeof(*usbssp_data->dcbaa), &dma, GFP_KERNEL);
	if (!usbssp_data->dcbaa)
		goto fail;
	memset(usbssp_data->dcbaa, 0, sizeof *(usbssp_data->dcbaa));
	usbssp_data->dcbaa->dma = dma;

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"// DCBA array address = 0x%llx (DMA), %p (virt)",
			(unsigned long long)usbssp_data->dcbaa->dma,
			usbssp_data->dcbaa);
	usbssp_write_64(usbssp_data, dma, &usbssp_data->op_regs->dcbaa_ptr);

	/*
	 * Initialize the ring segment pool.  The ring must be a contiguous
	 * structure comprised of TRBs.  The TRBs must be 16 byte aligned,
	 * however, the command ring segment needs 64-byte aligned segments
	 * and our use of dma addresses in the trb_address_map radix tree needs
	 * TRB_SEGMENT_SIZE alignment, so we pick the greater alignment need.
	 */
	usbssp_data->segment_pool = dma_pool_create("USBSSP ring segments", dev,
			TRB_SEGMENT_SIZE, TRB_SEGMENT_SIZE,
			usbssp_data->page_size);

	/* See Table 46 and Note on Figure 55 */
	usbssp_data->device_pool =
			dma_pool_create("USBSSP input/output contexts", dev,
					2112, 64, usbssp_data->page_size);
	if (!usbssp_data->segment_pool || !usbssp_data->device_pool)
		goto fail;

	/* Linear stream context arrays don't have any boundary restrictions,
	 * and only need to be 16-byte aligned.
	 */
	usbssp_data->small_streams_pool =
			dma_pool_create("USBSSP 256 byte stream ctx arrays",
					dev, SMALL_STREAM_ARRAY_SIZE, 16, 0);
	usbssp_data->medium_streams_pool =
			dma_pool_create("USBSSP 1KB stream ctx arrays",
					dev, MEDIUM_STREAM_ARRAY_SIZE, 16, 0);

	/* Any stream context array bigger than MEDIUM_STREAM_ARRAY_SIZE
	 * will be allocated with dma_alloc_coherent()
	 */
	if (!usbssp_data->small_streams_pool ||
	    !usbssp_data->medium_streams_pool)
		goto fail;

	/* Set up the command ring to have one segments for now. */
	usbssp_data->cmd_ring = usbssp_ring_alloc(usbssp_data, 1, 1,
			TYPE_COMMAND, 0, flags);
	if (!usbssp_data->cmd_ring)
		goto fail;

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"Allocated command ring at %p", usbssp_data->cmd_ring);
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"First segment DMA is 0x%llx",
			(unsigned long long)usbssp_data->cmd_ring->first_seg->dma);

	/* Set the address in the Command Ring Control register */
	val_64 = usbssp_read_64(usbssp_data, &usbssp_data->op_regs->cmd_ring);
	val_64 = (val_64 & (u64) CMD_RING_RSVD_BITS) |
		(usbssp_data->cmd_ring->first_seg->dma & (u64) ~CMD_RING_RSVD_BITS) |
		usbssp_data->cmd_ring->cycle_state;
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"// Setting command ring address to 0x%x", val_64);
	usbssp_write_64(usbssp_data, val_64, &usbssp_data->op_regs->cmd_ring);
	usbssp_dbg_cmd_ptrs(usbssp_data);

	val = readl(&usbssp_data->cap_regs->db_off);
	val &= DBOFF_MASK;
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"// Doorbell array is located at offset 0x%x"
			" from cap regs base addr", val);
	usbssp_data->dba = (void __iomem *) usbssp_data->cap_regs + val;
	usbssp_dbg_regs(usbssp_data);
	usbssp_print_run_regs(usbssp_data);
	/* Set ir_set to interrupt register set 0 */
	usbssp_data->ir_set = &usbssp_data->run_regs->ir_set[0];

	/*
	 * Event ring setup: Allocate a normal ring, but also setup
	 * the event ring segment table (ERST).
	 */
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"// Allocating event ring");
	usbssp_data->event_ring = usbssp_ring_alloc(usbssp_data,
			ERST_NUM_SEGS, 1, TYPE_EVENT, 0, flags);
	if (!usbssp_data->event_ring)
		goto fail;

	/*invoke  check procedure for usbssp_trb_in_td function*/
	if (usbssp_check_trb_in_td_math(usbssp_data) < 0)
		goto fail;

	ret = usbssp_alloc_erst(usbssp_data, usbssp_data->event_ring,
			&usbssp_data->erst, flags);
	if (ret)
		goto fail;

	/* set ERST count with the number of entries in the segment table */
	val = readl(&usbssp_data->ir_set->erst_size);
	val &= ERST_SIZE_MASK;
	val |= ERST_NUM_SEGS;
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"// Write ERST size = %i to ir_set 0 (some bits preserved)",
			val);
	writel(val, &usbssp_data->ir_set->erst_size);

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"// Set ERST entries to point to event ring.");

	/* set the segment table base address */
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"// Set ERST base address for ir_set 0 = 0x%llx",
			(unsigned long long)usbssp_data->erst.erst_dma_addr);
	val_64 = usbssp_read_64(usbssp_data, &usbssp_data->ir_set->erst_base);
	val_64 &= ERST_PTR_MASK;
	val_64 |= (usbssp_data->erst.erst_dma_addr & (u64) ~ERST_PTR_MASK);
	usbssp_write_64(usbssp_data, val_64, &usbssp_data->ir_set->erst_base);

	/* Set the event ring dequeue address */
	usbssp_set_event_deq(usbssp_data);
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"Wrote ERST address to ir_set 0.");

	if (scratchpad_alloc(usbssp_data, flags))
		goto fail;

	if (usbssp_setup_port_arrays(usbssp_data, flags))
		goto fail;

	return 0;

fail:
	usbssp_warn(usbssp_data, "Couldn't initialize memory\n");
	usbssp_halt(usbssp_data);
	usbssp_reset(usbssp_data);
	usbssp_mem_cleanup(usbssp_data);
	return -ENOMEM;
}

