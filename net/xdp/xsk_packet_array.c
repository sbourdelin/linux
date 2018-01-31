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

#include <linux/slab.h>

#include "xsk_packet_array.h"

/**
 * xskpa_create - Create new packet array
 * @q_ops: opaque reference to queue associated with this packet array
 * @buff_info: buffer info
 * @elems: number of elements
 *
 * Returns a reference to the new packet array or NULL for failure
 **/
struct xsk_packet_array *xskpa_create(struct xsk_user_queue *q_ops,
				      struct xsk_buff_info *buff_info,
				      size_t elems)
{
	struct xsk_packet_array *arr;

	if (!is_power_of_2(elems))
		return NULL;

	arr = kzalloc(sizeof(*arr) + elems * sizeof(struct xdp_desc),
		      GFP_KERNEL);
	if (!arr)
		return NULL;

	arr->q_ops = q_ops;
	arr->buff_info = buff_info;
	arr->mask = elems - 1;
	return arr;
}

void xskpa_destroy(struct xsk_packet_array *a)
{
	struct xsk_frame_set f;

	if (a) {
		/* Flush all outstanding requests. */
		if (xskpa_get_flushable_frame_set(a, &f)) {
			do {
				xskf_set_frame(&f, 0, 0, true);
			} while (xskf_next_frame(&f));
		}

		WARN_ON_ONCE(xskpa_flush(a));
		kfree(a);
	}
}
