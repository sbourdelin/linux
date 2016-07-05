/*
 * Copyright (c) 2016, NVIDIA CORPORATION.  All rights reserved.
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

#ifndef __TEGRA_IVC_H

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/types.h>

struct ivc_channel_header;

struct ivc {
	struct ivc_channel_header *rx_channel, *tx_channel;
	uint32_t w_pos, r_pos;

	void (*notify)(struct ivc *);
	uint32_t nframes, frame_size;

	struct device *peer_device;
	dma_addr_t rx_handle, tx_handle;
};

/**
 * tegra_ivc_read_get_next_frame - Peek at the next frame to receive
 * @ivc		pointer of the IVC channel
 *
 * Peek at the next frame to be received, without removing it from
 * the queue.
 *
 * Returns a pointer to the frame, or an error encoded pointer.
 */
void *tegra_ivc_read_get_next_frame(struct ivc *ivc);

/**
 * tegra_ivc_read_advance - Advance the read queue
 * @ivc		pointer of the IVC channel
 *
 * Advance the read queue
 *
 * Returns 0, or a negative error value if failed.
 */
int tegra_ivc_read_advance(struct ivc *ivc);

/**
 * tegra_ivc_write_get_next_frame - Poke at the next frame to transmit
 * @ivc		pointer of the IVC channel
 *
 * Get access to the next frame.
 *
 * Returns a pointer to the frame, or an error encoded pointer.
 */
void *tegra_ivc_write_get_next_frame(struct ivc *ivc);

/**
 * tegra_ivc_write_advance - Advance the write queue
 * @ivc		pointer of the IVC channel
 *
 * Advance the write queue
 *
 * Returns 0, or a negative error value if failed.
 */
int tegra_ivc_write_advance(struct ivc *ivc);

/**
 * tegra_ivc_channel_notified - handle internal messages
 * @ivc		pointer of the IVC channel
 *
 * This function must be called following every notification.
 *
 * Returns 0 if the channel is ready for communication, or -EAGAIN if a channel
 * reset is in progress.
 */
int tegra_ivc_channel_notified(struct ivc *ivc);

/**
 * tegra_ivc_channel_reset - initiates a reset of the shared memory state
 * @ivc		pointer of the IVC channel
 *
 * This function must be called after a channel is reserved before it is used
 * for communication. The channel will be ready for use when a subsequent call
 * to notify the remote of the channel reset.
 */
void tegra_ivc_channel_reset(struct ivc *ivc);

size_t tegra_ivc_align(size_t size);
unsigned tegra_ivc_total_queue_size(unsigned queue_size);
int tegra_ivc_init(struct ivc *ivc, uintptr_t rx_base, dma_addr_t rx_handle,
		   uintptr_t tx_base, dma_addr_t tx_handle, unsigned nframes,
		   unsigned frame_size, struct device *peer_device,
		   void (*notify)(struct ivc *));

#endif /* __TEGRA_IVC_H */
