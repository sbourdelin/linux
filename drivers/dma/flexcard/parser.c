/*
 * Eberspächer Flexcard PMC II Carrier Board PCI Driver - packet parser/mux
 *
 * Copyright (c) 2014 - 2016, Linutronix GmbH
 * Author: Benedikt Spranger <b.spranger@linutronix.de>
 *         Holger Dengler <dengler@linutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/slab.h>

#include <linux/mfd/flexcard.h>
#include "flexcard-dma.h"

static LIST_HEAD(rx_cb_list);
static DEFINE_SPINLOCK(rx_cb_lock);

struct fc_rx_cb {
	struct list_head list;
	int (*rx_cb)(void *priv, void *data, size_t len);
	int cc;
	void *priv;
};

/**
 * flexcard_register_rx_cb() - Registers a callback for received packages
 * @cc:		communication controller id
 * @priv:	pointer to private data of the cc
 * @rx_cp:	pionter to the receive callback
 *
 * Registers a callback for a communication controller specific handling for
 * received packages. The callback is called by the generic parser, if the
 * communication controller id inside of the received package matches the cc
 * of the callback owner.
 *
 * Return: 0 is returned on success and a negative errno code for failure.
 */
int flexcard_register_rx_cb(int cc, void *priv,
			    int (*rx_cb)(void *priv, void *data, size_t len))
{
	unsigned long flags;
	struct fc_rx_cb *cb, *next;

	if (!rx_cb)
		return -EINVAL;

	cb = kmalloc(sizeof(*cb), GFP_KERNEL);
	if (!cb)
		return -ENOMEM;

	cb->cc = cc;
	cb->priv = priv;
	cb->rx_cb = rx_cb;

	spin_lock_irqsave(&rx_cb_lock, flags);
	list_for_each_entry(next, &rx_cb_list, list)
		if (next->cc == cc)
			goto out;

	list_add_tail(&cb->list, &rx_cb_list);
	spin_unlock_irqrestore(&rx_cb_lock, flags);

	return 0;
out:
	spin_unlock_irqrestore(&rx_cb_lock, flags);
	kfree(cb);

	return -EBUSY;
}
EXPORT_SYMBOL_GPL(flexcard_register_rx_cb);

/**
 * flexcard_unregister_rx_cb() - Unregisters a callback for received packages
 * @cc:		communication controller id
 *
 * Unregisters a callback for a communication controller specific handling for
 * received packages.
 *
 * Return: 0 is returned on success and a negative errno code for failure.
 */
void flexcard_unregister_rx_cb(int cc)
{
	unsigned long flags;
	struct fc_rx_cb *cur, *next;
	int found = 0;

	spin_lock_irqsave(&rx_cb_lock, flags);
	list_for_each_entry_safe(cur, next, &rx_cb_list, list) {
		if (cur->cc == cc) {
			list_del(&cur->list);
			kfree(cur);
			found = 1;
			break;
		}
	}

	if (!found)
		pr_err("no callback registered for cc %d\n", cc);

	spin_unlock_irqrestore(&rx_cb_lock, flags);
}
EXPORT_SYMBOL_GPL(flexcard_unregister_rx_cb);

static int flexcard_queue_rx(int cc, void *buf, size_t len)
{
	struct fc_rx_cb *next;
	unsigned long flags;
	int ret = -ENODEV;

	spin_lock_irqsave(&rx_cb_lock, flags);
	list_for_each_entry(next, &rx_cb_list, list)
		if (next->cc == cc)
			ret = next->rx_cb(next->priv, buf, len);
	spin_unlock_irqrestore(&rx_cb_lock, flags);

	return ret;
}

static u32 flexcard_get_packet_len(u32 header)
{
	u32 len;

	/*
	 * header contains the number of transmitted 16bit words in bits 30-16.
	 * if the number is odd the DMA engine padded with zero to 32bit.
	 * calculate the number of transmitted bytes.
	 */

	len = le32_to_cpu(header);

	len >>= FLEXCARD_BUF_HEADER_LEN_SHIFT;
	len &= FLEXCARD_BUF_HEADER_LEN_MASK;

	len = roundup(len, 4);

	return len;
}

/**
 * selfsync_cc - adjust the cc number for self-sync packages
 * @dma:	pointer to dma structure
 * @cc:		package cc
 *
 * Some Flexcards has support for self-synci bus configurations. With this
 * feature it is possible to get a synchronized bus configuration with a
 * single card.
 * Indication for a self-sync package is eray_nr == 1 and cc == 1. The
 * packages are always handled by communication controller 0.
 */
static inline u32 selfsync_cc(struct flexcard_dma *dma, u32 cc)
{
		if ((dma->nr_eray == 1) && (cc == 1))
			return 0;
		return cc;
}

u32 flexcard_parse_packet(struct fc_packet_buf *pb, u32 avail,
			  struct flexcard_dma *dma)
{
	u32 l, cc, len = sizeof(struct fc_packet);
	union fc_packet_types *pt = &pb->packet;

	switch (le32_to_cpu(pb->header.type)) {
	case FC_PACKET_TYPE_INFO:
		len += sizeof(struct fc_info_packet);
		cc = pt->info_packet.cc;
		break;
	case FC_PACKET_TYPE_ERROR:
		len += sizeof(struct fc_error_packet);
		cc = pt->error_packet.cc;
		break;
	case FC_PACKET_TYPE_STATUS:
		len += sizeof(struct fc_status_packet);
		cc = selfsync_cc(dma, pt->status_packet.cc);
		break;
	case FC_PACKET_TYPE_NMV_VECTOR:
		len += sizeof(struct fc_nm_vector_packet);
		cc = pt->nm_vector_packet.cc;
		break;
	case FC_PACKET_TYPE_NOTIFICATION:
		len += sizeof(struct fc_notification_packet);
		cc = 0;
		break;
	case FC_PACKET_TYPE_TRIGGER_EX:
		len += sizeof(struct fc_trigger_ex_info_packet);
		cc = 0;
		break;
	case FC_PACKET_TYPE_CAN:
		len += sizeof(struct fc_can_packet);
		cc = FLEXCARD_CANIF_OFFSET + pt->can_packet.cc;
		break;
	case FC_PACKET_TYPE_CAN_ERROR:
		len += sizeof(struct fc_can_error_packet);
		cc = FLEXCARD_CANIF_OFFSET + pt->can_error_packet.cc;
		break;
	case FC_PACKET_TYPE_FLEXRAY_FRAME:
		len += sizeof(struct fc_flexray_frame);
		pt->flexray_frame.pdata = len;
		l = flexcard_get_packet_len(pt->flexray_frame.header);
		len += l;
		cc = pt->flexray_frame.cc;
		break;
	case FC_PACKET_TYPE_TX_ACK:
		len += sizeof(struct fc_tx_ack_packet);
		pt->tx_ack_packet.pdata = len;
		l = flexcard_get_packet_len(pt->tx_ack_packet.header);
		len += l;
		cc = selfsync_cc(dma, pt->tx_ack_packet.cc);
		break;
	case FC_PACKET_TYPE_TRIGGER:
	default:
		pr_debug("pkt->type = %08x\n", pb->header.type);
		return 0;
	}

	if (len > avail)
		return 0;

	flexcard_queue_rx(cc, pb, len);

	return len;
}
