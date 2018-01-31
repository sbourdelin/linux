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

#include <linux/slab.h>

#include "xsk_ring.h"

/**
 * xskq_init - Initializas an XDP queue
 *
 * @nentries: Number of descriptor entries in the queue
 *
 * Returns the created queue in *q_ops and the function returns zero
 * for success.
 **/
struct xsk_queue *xskq_create(u32 nentries)
{
	struct xsk_queue *q;

	q = kzalloc(sizeof(*q), GFP_KERNEL);
	if (!q)
		return NULL;

	q->ring = kcalloc(nentries, sizeof(*q->ring), GFP_KERNEL);
	if (!q->ring) {
		kfree(q);
		return NULL;
	}

	q->queue_ops.enqueue = xskq_enqueue_from_array;
	q->queue_ops.enqueue_completed = xskq_enqueue_completed_from_array;
	q->queue_ops.dequeue = xskq_dequeue_to_array;
	q->used_idx = 0;
	q->last_avail_idx = 0;
	q->ring_mask = nentries - 1;
	q->num_free = 0;
	q->nentries = nentries;

	return q;
}

void xskq_destroy(struct xsk_queue *q)
{
	if (!q)
		return;

	kfree(q->ring);
	kfree(q);
}
