/*
 * Driver for KeyStream wireless LAN cards.
 *
 * Copyright (C) 2005-2008 KeyStream Corp.
 * Copyright (C) 2009 Renesas Technology Corp.
 * Copyright (C) 2017 Tobin C. Harding.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/circ_buf.h>

#include "ks7010.h"
#include "hif.h"
#include "sdio.h"

/**
 * ks7010_rx() - Copy rx data from device.
 * @ks: The ks7010 device.
 * @size: Number of octets to copy.
 *
 * Reads rx data from the ks7010 device to the driver. Called in
 * interrupt context by @ks7010_sdio_interrupt().
 */
void ks7010_rx(struct ks7010 *ks, u16 size)
{
	struct rx_queue *q = &ks->rxq;
	struct rx_data *rxd;
	int head, tail;
	unsigned long flags;
	int ret = 0;

	if (size == 0 || size > RX_DATA_MAX_SIZE) {
		ks_debug("rx data size invalid %d\n", size);
		ret = -EINVAL;
		goto idle;
	}

	spin_lock_irqsave(&q->producer_lock, flags);

	head = q->head;
	tail = READ_ONCE(q->tail);

	if (CIRC_SPACE(head, tail, KS7010_RX_QUEUE_SIZE) < 1)
		goto unlock;

	rxd = &q->buf[head];

	ret = ks7010_sdio_rx_read(ks, rxd->data, size);

	/* Finish reading descriptor before incrementing tail. */
	smp_store_release(&q->head, (head + 1) & (KS7010_RX_QUEUE_SIZE - 1));

unlock:
	spin_unlock_irqrestore(&q->producer_lock, flags);

idle:
	ks7010_sdio_set_read_status_idle(ks);
	if (ret == 0)
		tasklet_schedule(&ks->rx_bh_task);
}

static void ks7010_rx_bh_task(unsigned long dev)
{
	struct ks7010 *ks = (struct ks7010 *)dev;
	struct rx_queue *q = &ks->rxq;
	struct rx_data *rxd;
	int head, tail;
	int pending;

	/* FIXME do we need a consumer lock when only one tasklet can
	 * run at a time?
	 */

	spin_lock_bh(&q->consumer_lock);

	/* Read index before reading contents at that index. */
	head = smp_load_acquire(&q->head);
	tail = q->tail;

	pending = CIRC_CNT(head, tail, KS7010_RX_QUEUE_SIZE);
	if (pending == 0)
		goto unlock;

	rxd = &q->buf[tail];
	ks7010_hif_rx(ks, rxd->data, rxd->data_size);

	/* Finish reading descriptor before incrementing tail. */
	smp_store_release(&q->tail, (tail + 1) & (KS7010_RX_QUEUE_SIZE - 1));

	if (pending > 1)
		tasklet_schedule(&ks->rx_bh_task);

unlock:
	spin_unlock_bh(&q->consumer_lock);
}

/**
 * ks7010_rx_init() - Rx initialization function.
 * @ks: The ks7010 device.
 */
int ks7010_rx_init(struct ks7010 *ks)
{
	struct rx_queue *q = &ks->rxq;

	tasklet_init(&ks->rx_bh_task, ks7010_rx_bh_task, (unsigned long)ks);

	spin_lock_init(&q->producer_lock);
	spin_lock_init(&q->consumer_lock);

	return 0;
}

/**
 * ks7010_rx_cleanup() - Rx cleanup function.
 * @ks: The ks7010 device.
 */
void ks7010_rx_cleanup(struct ks7010 *ks)
{
	tasklet_kill(&ks->rx_bh_task);
}
