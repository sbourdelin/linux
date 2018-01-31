/*
 *  XDP user-space ring structure
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

#ifndef _LINUX_XDP_RING_H
#define _LINUX_XDP_RING_H

#include <linux/types.h>
#include <linux/if_xdp.h>

#include "xsk.h"
#include "xsk_buff.h"
#include "xsk_packet_array.h"

struct xsk_queue {
	/* struct xsk_user_queue has to be first */
	struct xsk_user_queue queue_ops;
	struct xdp_desc *ring;

	u32 used_idx;
	u32 last_avail_idx;
	u32 ring_mask;
	u32 num_free;

	u32 nentries;
	struct xsk_buff_info *buff_info;
	enum xsk_validation validation;
};

static inline unsigned int xsk_get_data_headroom(struct xsk_umem *umem)
{
	return umem->data_headroom + XDP_KERNEL_HEADROOM;
}

/**
 * xskq_is_valid_entry - Is the entry valid?
 *
 * @q: Pointer to the tp4 queue the descriptor resides in
 * @desc: Pointer to the descriptor to examine
 * @validation: The type of validation to perform
 *
 * Returns true if the entry is a valid, otherwise false
 **/
static inline bool xskq_is_valid_entry(struct xsk_queue *q,
				       struct xdp_desc *d)
{
	unsigned int buff_len;

	if (q->validation == XSK_VALIDATION_NONE)
		return true;

	if (unlikely(d->idx >= q->buff_info->nbuffs)) {
		d->error = EBADF;
		return false;
	}

	if (q->validation == XSK_VALIDATION_RX) {
		d->offset = xsk_buff_info_get_rx_headroom(q->buff_info);
		return true;
	}

	buff_len = xsk_buff_info_get_buff_len(q->buff_info);
	/* XSK_VALIDATION_TX */
	if (unlikely(d->len > buff_len || d->len == 0 || d->offset > buff_len ||
		     d->offset + d->len > buff_len)) {
		d->error = EBADF;
		return false;
	}

	return true;
}

/**
 * xskq_nb_avail - Returns the number of available entries
 *
 * @q: Pointer to the queue to examine
 * @dcnt: Max number of entries to check
 *
 * Returns the the number of entries available in the queue up to dcnt
 **/
static inline int xskq_nb_avail(struct xsk_queue *q, int dcnt)
{
	unsigned int idx, last_avail_idx = q->last_avail_idx;
	int i, entries = 0;

	for (i = 0; i < dcnt; i++) {
		idx = (last_avail_idx++) & q->ring_mask;
		if (!(q->ring[idx].flags & XDP_DESC_KERNEL))
			break;
		entries++;
	}

	return entries;
}

/**
 * xskq_enqueue - Enqueue entries to a the queue
 *
 * @q: Pointer to the queue the descriptor resides in
 * @d: Pointer to the descriptor to examine
 * @dcnt: Max number of entries to dequeue
 *
 * Returns 0 for success or an errno at failure
 **/
static inline int xskq_enqueue(struct xsk_queue *q,
			       const struct xdp_desc *d, int dcnt)
{
	unsigned int used_idx = q->used_idx;
	int i;

	if (q->num_free < dcnt)
		return -ENOSPC;

	q->num_free -= dcnt;

	for (i = 0; i < dcnt; i++) {
		unsigned int idx = (used_idx++) & q->ring_mask;

		q->ring[idx].idx = d[i].idx;
		q->ring[idx].len = d[i].len;
		q->ring[idx].offset = d[i].offset;
		q->ring[idx].error = d[i].error;
	}

	/* Order flags and data */
	smp_wmb();

	for (i = dcnt - 1; i >= 0; i--) {
		unsigned int idx = (q->used_idx + i) & q->ring_mask;

		q->ring[idx].flags = d[i].flags & ~XDP_DESC_KERNEL;
	}
	q->used_idx += dcnt;

	return 0;
}

/**
 * xskq_enqueue_from_array - Enqueue entries from packet array to the queue
 *
 * @a: Pointer to the packet array to enqueue from
 * @dcnt: Max number of entries to enqueue
 *
 * Returns 0 for success or an errno at failure
 **/
static inline int xskq_enqueue_from_array(struct xsk_packet_array *a,
					  u32 dcnt)
{
	struct xsk_queue *q = (struct xsk_queue *)a->q_ops;
	unsigned int used_idx = q->used_idx;
	struct xdp_desc *d = a->items;
	int i;

	if (q->num_free < dcnt)
		return -ENOSPC;

	q->num_free -= dcnt;

	for (i = 0; i < dcnt; i++) {
		unsigned int idx = (used_idx++) & q->ring_mask;
		unsigned int didx = (a->start + i) & a->mask;

		q->ring[idx].idx = d[didx].idx;
		q->ring[idx].len = d[didx].len;
		q->ring[idx].offset = d[didx].offset;
		q->ring[idx].error = d[didx].error;
	}

	/* Order flags and data */
	smp_wmb();

	for (i = dcnt - 1; i >= 0; i--) {
		unsigned int idx = (q->used_idx + i) & q->ring_mask;
		unsigned int didx = (a->start + i) & a->mask;

		q->ring[idx].flags = d[didx].flags & ~XDP_DESC_KERNEL;
	}
	q->used_idx += dcnt;

	return 0;
}

/**
 * xskq_enqueue_completed_from_array - Enqueue only completed entries
 *				       from packet array
 *
 * @a: Pointer to the packet array to enqueue from
 * @dcnt: Max number of entries to enqueue
 *
 * Returns the number of entries successfully enqueued or a negative errno
 * at failure.
 **/
static inline int xskq_enqueue_completed_from_array(struct xsk_packet_array *a,
						    u32 dcnt)
{
	struct xsk_queue *q = (struct xsk_queue *)a->q_ops;
	unsigned int used_idx = q->used_idx;
	struct xdp_desc *d = a->items;
	int i, j;

	if (q->num_free < dcnt)
		return -ENOSPC;

	for (i = 0; i < dcnt; i++) {
		unsigned int didx = (a->start + i) & a->mask;

		if (d[didx].flags & XSK_FRAME_COMPLETED) {
			unsigned int idx = (used_idx++) & q->ring_mask;

			q->ring[idx].idx = d[didx].idx;
			q->ring[idx].len = d[didx].len;
			q->ring[idx].offset = d[didx].offset;
			q->ring[idx].error = d[didx].error;
		} else {
			break;
		}
	}

	if (i == 0)
		return 0;

	/* Order flags and data */
	smp_wmb();

	for (j = i - 1; j >= 0; j--) {
		unsigned int idx = (q->used_idx + j) & q->ring_mask;
		unsigned int didx = (a->start + j) & a->mask;

		q->ring[idx].flags = d[didx].flags & ~XDP_DESC_KERNEL;
	}
	q->num_free -= i;
	q->used_idx += i;

	return i;
}

/**
 * xskq_dequeue_to_array - Dequeue entries from the queue to a packet array
 *
 * @a: Pointer to the packet array to dequeue from
 * @dcnt: Max number of entries to dequeue
 *
 * Returns the number of entries dequeued. Non valid entries will be
 * discarded.
 **/
static inline int xskq_dequeue_to_array(struct xsk_packet_array *a, u32 dcnt)
{
	struct xdp_desc *d = a->items;
	int i, entries, valid_entries = 0;
	struct xsk_queue *q = (struct xsk_queue *)a->q_ops;
	u32 start = a->end;

	entries = xskq_nb_avail(q, dcnt);
	q->num_free += entries;

	/* Order flags and data */
	smp_rmb();

	for (i = 0; i < entries; i++) {
		unsigned int d_idx = start & a->mask;
		unsigned int idx;

		idx = (q->last_avail_idx++) & q->ring_mask;
		d[d_idx] = q->ring[idx];
		if (!xskq_is_valid_entry(q, &d[d_idx])) {
			WARN_ON_ONCE(xskq_enqueue(q, &d[d_idx], 1));
			continue;
		}

		start++;
		valid_entries++;
	}
	return valid_entries;
}

static inline u32 xskq_get_ring_size(struct xsk_queue *q)
{
	return q->nentries * sizeof(*q->ring);
}

static inline char *xskq_get_ring_address(struct xsk_queue *q)
{
	return (char *)q->ring;
}

static inline void xskq_set_buff_info(struct xsk_queue *q,
				      struct xsk_buff_info *buff_info,
				      enum xsk_validation validation)
{
	q->buff_info = buff_info;
	q->validation = validation;
}

struct xsk_queue *xskq_create(u32 nentries);
void xskq_destroy(struct xsk_queue *q_ops);

#endif /* _LINUX_XDP_RING_H */
