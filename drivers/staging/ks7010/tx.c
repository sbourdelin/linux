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
#include <linux/workqueue.h>
#include <linux/circ_buf.h>

#include "ks7010.h"
#include "sdio.h"
#include "hif.h"

/**
 * ks7010_tx_start() - Start transmission.
 * @ndev: The net_device associated with this socket buffer.
 * @skb: socket buffer passed down from the networking stack.
 *
 * Called by the networking stack (tx queue producer).
 */
int ks7010_tx_start(struct sk_buff *skb, struct net_device *ndev)
{
	struct ks7010 *ks = ks7010_ndev_to_ks(ndev);
	struct tx_data txd;
	int ret;

	memset(&txd, 0, sizeof(txd));

	ks_debug("%s: skb=0x%p, data=0x%p, len=0x%x\n", __func__,
		 skb, skb->data, skb->len);

	if (eth_skb_pad(skb))
		return NETDEV_TX_OK;

	ret = ks7010_hif_tx_start(ks, skb, &txd);
	if (ret)
		goto out;

	ret = ks7010_tx_enqueue(ks, txd.datap, txd.size);
	if (ret) {
		kfree(txd.datap);
		goto out;
	}

	netdev_sent_queue(ndev, skb->len);

out:
	dev_kfree_skb(skb);

	return NETDEV_TX_OK;
}

/**
 * ks7010_tx_enqueue() - Enqueue tx data in the tx buffer.
 * @ks: The ks7010 device.
 * @data: The tx data.
 * @data_size: Size of data.
 */
int ks7010_tx_enqueue(struct ks7010 *ks, u8 *data, size_t data_size)
{
	struct tx_queue *q = &ks->txq;
	struct tx_data *txd;
	int head, tail;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&q->producer_lock, flags);

	head = q->head;
	tail = READ_ONCE(q->tail);

	if (CIRC_SPACE(head, tail, KS7010_TX_QUEUE_SIZE) < 1) {
		ret = -EOVERFLOW;
		goto unlock;
	}

	txd = &q->buf[head];

	txd->datap = data;
	txd->size = data_size;

	/* Finish reading descriptor before incrementing tail. */
	smp_store_release(&q->head, (head + 1) & (KS7010_RX_QUEUE_SIZE - 1));

	ret = 0;
unlock:
	spin_unlock_irqrestore(&q->producer_lock, flags);

	return ret;
}

/**
 * ks7010_tx_hw() - Send tx packet to the device.
 * @ks: The ks7010 device.
 *
 * Called in interrupt context. Txq consumer, empty queue and send to device.
 */
void ks7010_tx_hw(struct ks7010 *ks)
{
	struct tx_queue *q = &ks->txq;
	struct tx_data *txd;
	int head, tail;
	unsigned long flags;
	bool tx_succeeded;
	int ret;

	spin_lock_irqsave(&q->consumer_lock, flags);

	/* Read index before reading contents at that index. */
	head = smp_load_acquire(&q->head);
	tail = q->tail;

	if (CIRC_CNT(head, tail, KS7010_TX_QUEUE_SIZE) < 1)
		goto unlock;

	txd = &q->buf[tail];

	ret = ks7010_sdio_tx(ks, txd->datap, txd->size);
	if (ret) {
		WARN_ONCE(ret, "tx write failed, leaving data in queue");
		tx_succeeded = false;
		goto unlock;
	}

	tx_succeeded = true;
	kfree(txd->datap);

	/* Finish reading descriptor before incrementing tail. */
	smp_store_release(&q->tail, (tail + 1) & (KS7010_TX_QUEUE_SIZE - 1));

unlock:
	spin_unlock_irqrestore(&q->consumer_lock, flags);

	/* TODO update stats (based on tx_succeeded) */
}

/**
 * ks7010_tx_init() - Initialize transmit path.
 * @ks: The ks7010 device.
 */
int ks7010_tx_init(struct ks7010 *ks)
{
	struct tx_queue *q = &ks->txq;

	spin_lock_init(&q->producer_lock);
	spin_lock_init(&q->consumer_lock);

	return 0;
}

/**
 * ks7010_tx_cleanup() - Cleanup the transmit path.
 * @ks: The ks7010 device.
 */
void ks7010_tx_cleanup(struct ks7010 *ks)
{
}
