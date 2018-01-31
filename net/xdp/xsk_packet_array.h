/*
 *  XDP packet arrays
 *  Copyright(c) 2017 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifndef _LINUX_XDP_PACKET_ARRAY_H
#define _LINUX_XDP_PACKET_ARRAY_H

#include <linux/dma-direction.h>
#include <linux/if_xdp.h>
#include <linux/types.h>
#include <linux/mm.h>

#include "xsk.h"
#include "xsk_buff.h"
#include "xsk_user_queue.h"

/**
 * struct xsk_packet_array - An array of packets/frames
 *
 * @q_ops:
 * @buff_info:
 * @start: the first packet that has not been processed
 * @curr: the packet that is currently being processed
 * @end: the last packet in the array
 * @mask: convenience variable for internal operations on the array
 * @items: the actual descriptors to frames/packets that are in the array
 **/
struct xsk_packet_array {
	struct xsk_user_queue *q_ops;
	struct xsk_buff_info *buff_info;
	u32 start;
	u32 curr;
	u32 end;
	u32 mask;
	struct xdp_desc items[0];
};

/**
 * struct xsk_frame_set - A view of a packet array consisting of
 *			  one or more frames
 *
 * @pkt_arr: the packet array this frame set is located in
 * @start: the first frame that has not been processed
 * @curr: the frame that is currently being processed
 * @end: the last frame in the frame set
 *
 * This frame set can either be one or more frames or a single packet
 * consisting of one or more frames. xskf_ functions with packet in the
 * name return a frame set representing a packet, while the other
 * xskf_ functions return one or more frames not taking into account if
 * they consitute a packet or not.
 **/
struct xsk_frame_set {
	struct xsk_packet_array *pkt_arr;
	u32 start;
	u32 curr;
	u32 end;
};

static inline struct xsk_user_queue *xsk_user_queue(struct xsk_packet_array *a)
{
	return a->q_ops;
}

static inline struct xdp_desc *xskf_get_desc(struct xsk_frame_set *p)
{
	return &p->pkt_arr->items[p->curr & p->pkt_arr->mask];
}

/**
 * xskf_reset - Start to traverse the frames in the set from the beginning
 * @p: pointer to frame set
 **/
static inline void xskf_reset(struct xsk_frame_set *p)
{
	p->curr = p->start;
}

static inline u32 xskf_get_frame_id(struct xsk_frame_set *p)
{
	return p->pkt_arr->items[p->curr & p->pkt_arr->mask].idx;
}

static inline void xskf_set_error(struct xsk_frame_set *p, int errno)
{
	p->pkt_arr->items[p->curr & p->pkt_arr->mask].error = errno;
}

static inline u32 xskf_get_frame_len(struct xsk_frame_set *p)
{
	return p->pkt_arr->items[p->curr & p->pkt_arr->mask].len;
}

/**
 * xskf_set_frame - Sets the properties of a frame
 * @p: pointer to frame
 * @len: the length in bytes of the data in the frame
 * @offset: offset to start of data in frame
 * @is_eop: Set if this is the last frame of the packet
 **/
static inline void xskf_set_frame(struct xsk_frame_set *p, u32 len, u16 offset,
				  bool is_eop)
{
	struct xdp_desc *d =
		&p->pkt_arr->items[p->curr & p->pkt_arr->mask];

	d->len = len;
	d->offset = offset;
	if (!is_eop)
		d->flags |= XDP_PKT_CONT;
}

static inline void xskf_set_frame_no_offset(struct xsk_frame_set *p,
					    u32 len, bool is_eop)
{
	struct xdp_desc *d =
		&p->pkt_arr->items[p->curr & p->pkt_arr->mask];

	d->len = len;
	if (!is_eop)
		d->flags |= XDP_PKT_CONT;
}

/**
 * xskf_get_data - Gets a pointer to the start of the packet
 *
 * @q: Pointer to the frame
 *
 * Returns a pointer to the start of the packet the descriptor is pointing
 * to
 **/
static inline void *xskf_get_data(struct xsk_frame_set *p)
{
	struct xdp_desc *desc = xskf_get_desc(p);
	struct xsk_buff *buff;

	buff = xsk_buff_info_get_buff(p->pkt_arr->buff_info, desc->idx);

	return buff->data + desc->offset;
}

static inline u32 xskf_get_data_offset(struct xsk_frame_set *p)
{
	return p->pkt_arr->items[p->curr & p->pkt_arr->mask].offset;
}

/**
 * xskf_next_frame - Go to next frame in frame set
 * @p: pointer to frame set
 *
 * Returns true if there is another frame in the frame set.
 * Advances curr pointer.
 **/
static inline bool xskf_next_frame(struct xsk_frame_set *p)
{
	if (p->curr + 1 == p->end)
		return false;

	p->curr++;
	return true;
}

/**
 * xskf_get_packet_len - Length of packet
 * @p: pointer to packet
 *
 * Returns the length of the packet in bytes.
 * Resets curr pointer of packet.
 **/
static inline u32 xskf_get_packet_len(struct xsk_frame_set *p)
{
	u32 len = 0;

	xskf_reset(p);

	do {
		len += xskf_get_frame_len(p);
	} while (xskf_next_frame(p));

	return len;
}

/**
 * xskf_packet_completed - Mark packet as completed
 * @p: pointer to packet
 *
 * Resets curr pointer of packet.
 **/
static inline void xskf_packet_completed(struct xsk_frame_set *p)
{
	xskf_reset(p);

	do {
		p->pkt_arr->items[p->curr & p->pkt_arr->mask].flags |=
			XSK_FRAME_COMPLETED;
	} while (xskf_next_frame(p));
}

/**
 * xskpa_flush_completed - Flushes only frames marked as completed
 * @a: pointer to packet array
 *
 * Returns 0 for success and -1 for failure
 **/
static inline int xskpa_flush_completed(struct xsk_packet_array *a)
{
	u32 avail = a->curr - a->start;
	int ret;

	if (avail == 0)
		return 0; /* nothing to flush */

	ret = xsk_user_queue(a)->enqueue_completed(a, avail);
	if (ret < 0)
		return -1;

	a->start += ret;
	return 0;
}

/**
 * xskpa_next_packet - Get next packet in array and advance curr pointer
 * @a: pointer to packet array
 * @p: supplied pointer to packet structure that is filled in by function
 *
 * Returns true if there is a packet, false otherwise. Packet returned in *p.
 **/
static inline bool xskpa_next_packet(struct xsk_packet_array *a,
				     struct xsk_frame_set *p)
{
	u32 avail = a->end - a->curr;

	if (avail == 0)
		return false; /* empty */

	p->pkt_arr = a;
	p->start = a->curr;
	p->curr = a->curr;
	p->end = a->curr;

	/* XXX Sanity check for too-many-frames packets? */
	while (a->items[p->end++ & a->mask].flags & XDP_PKT_CONT) {
		avail--;
		if (avail == 0)
			return false;
	}

	a->curr += (p->end - p->start);
	return true;
}

/**
 * xskpa_populate - Populate an array with packets from associated queue
 * @a: pointer to packet array
 **/
static inline void xskpa_populate(struct xsk_packet_array *a)
{
	u32 cnt, free = a->mask + 1 - (a->end - a->start);

	if (free == 0)
		return; /* no space! */

	cnt = xsk_user_queue(a)->dequeue(a, free);
	a->end += cnt;
}

/**
 * xskpa_next_frame - Get next frame in array and advance curr pointer
 * @a: pointer to packet array
 * @p: supplied pointer to packet structure that is filled in by function
 *
 * Returns true if there is a frame, false otherwise. Frame returned in *p.
 **/
static inline bool xskpa_next_frame(struct xsk_packet_array *a,
				    struct xsk_frame_set *p)
{
	u32 avail = a->end - a->curr;

	if (avail == 0)
		return false; /* empty */

	p->pkt_arr = a;
	p->start = a->curr;
	p->curr = a->curr;
	p->end = ++a->curr;

	return true;
}

/**
 * xskpa_next_frame_populate - Get next frame and populate array if empty
 * @a: pointer to packet array
 * @p: supplied pointer to packet structure that is filled in by function
 *
 * Returns true if there is a frame, false otherwise. Frame returned in *p.
 **/
static inline bool xskpa_next_frame_populate(struct xsk_packet_array *a,
					     struct xsk_frame_set *p)
{
	bool more_frames;

	more_frames = xskpa_next_frame(a, p);
	if (!more_frames) {
		xskpa_populate(a);
		more_frames = xskpa_next_frame(a, p);
	}

	return more_frames;
}

/**
 * xskpa_get_flushable_frame_set - Create a frame set of the flushable region
 * @a: pointer to packet array
 * @p: frame set
 *
 * Returns true for success and false for failure
 **/
static inline bool xskpa_get_flushable_frame_set(struct xsk_packet_array *a,
						 struct xsk_frame_set *p)
{
	u32 curr = READ_ONCE(a->curr);
	u32 avail = curr - a->start;

	if (avail == 0)
		return false; /* empty */

	p->pkt_arr = a;
	p->start = a->start;
	p->curr = a->start;
	p->end = curr;

	return true;
}

static inline int __xskpa_flush(struct xsk_packet_array *a, u32 npackets)
{
	int ret;

	if (npackets == 0)
		return 0; /* nothing to flush */

	ret = xsk_user_queue(a)->enqueue(a, npackets);
	if (ret < 0)
		return ret;

	a->start += npackets;
	return 0;
}

/**
 * xskpa_flush - Flush processed packets to associated queue
 * @a: pointer to packet array
 *
 * Returns 0 for success and -errno for failure
 **/
static inline int xskpa_flush(struct xsk_packet_array *a)
{
	u32 curr = READ_ONCE(a->curr);
	u32 avail = curr - a->start;

	return __xskpa_flush(a, avail);
}

/**
 * xskpa_flush_n - Flush N processed packets to associated queue
 * @a: pointer to packet array
 * @npackets: number of packets to flush
 *
 * Returns 0 for success and -errno for failure
 **/
static inline int xskpa_flush_n(struct xsk_packet_array *a, u32 npackets)
{
	if (npackets > a->curr - a->start)
		return -ENOSPC;

	return __xskpa_flush(a, npackets);
}

struct xsk_packet_array *xskpa_create(struct xsk_user_queue *q_ops,
				      struct xsk_buff_info *buff_info,
				      size_t elems);
void xskpa_destroy(struct xsk_packet_array *a);

#endif /* _LINUX_XDP_PACKET_ARRAY_H */
