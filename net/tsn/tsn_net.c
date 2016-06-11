/*
 *   Network part of TSN
 *
 *   Copyright (C) 2015- Henrik Austad <haustad@cisco.com>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/socket.h>
#include <linux/skbuff.h>
#include <linux/if_vlan.h>
#include <linux/skbuff.h>
#include <net/sock.h>

#include <linux/tsn.h>
#include <trace/events/tsn.h>
#include "tsn_internal.h"

/**
 * tsn_rx_handler - consume all TSN-tagged frames and forward to tsn_link.
 *
 * This handler, if it regsters properly, will consume all TSN-tagged
 * frames belonging to registered Stream IDs
 *
 * Unknown StreamIDs will be passed through without being touched.
 *
 * @param pskb sk_buff with incomign data
 * @returns RX_HANDLER_CONSUMED for TSN frames to known StreamIDs,
 *	    RX_HANDLER_PASS for everything else.
 */
static rx_handler_result_t tsn_rx_handler(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	const struct ethhdr *ethhdr = eth_hdr(skb);
	struct avtp_ch *ch;
	struct tsn_link *link;
	rx_handler_result_t ret = RX_HANDLER_PASS;

	ch = tsnh_ch_from_skb(skb);
	if (!ch)
		return RX_HANDLER_PASS;
	/* We do not (currently) touch control_data frames. */
	if (ch->cd)
		return RX_HANDLER_PASS;

	link = tsn_find_by_stream_id(be64_to_cpu(ch->stream_id));
	if (!link)
		return RX_HANDLER_PASS;

	tsn_lock(link);

	if (!tsn_link_is_on(link))
		goto out_unlock;

	/* If link->ops is not set yet, there's nothing we can do, just
	 * ignore this frame
	 */
	if (!link->ops)
		goto out_unlock;

	if (_tsnh_validate_du_header(link, ch, skb))
		goto out_unlock;

	trace_tsn_rx_handler(link, ethhdr, be64_to_cpu(ch->stream_id));

	/* Handle dataunit, if it failes, pass on the frame and let
	 * userspace pick it up.
	 */
	if (_tsnh_handle_du(link, ch) < 0)
		goto out_unlock;

	/* Done, data has been copied, free skb and return consumed */
	consume_skb(skb);
	ret = RX_HANDLER_CONSUMED;

out_unlock:
	tsn_unlock(link);
	return ret;
}

int tsn_net_add_rx(struct tsn_list *tlist)
{
	struct tsn_nic *nic;

	if (!tlist)
		return -EINVAL;

	/* Setup receive handler for TSN traffic.
	 *
	 * Receive will happen all the time, once a link is active as a
	 * Listener, we will add a hook into the receive-handler to
	 * steer the frames to the correct link.
	 *
	 * We try to add Rx-handlers to all the card listed in tlist (we
	 * assume core has filtered the NICs appropriatetly sothat only
	 * TSN-capable cards are present).
	 */
	mutex_lock(&tlist->lock);
	list_for_each_entry(nic, &tlist->head, list) {
		rtnl_lock();
		if (netdev_rx_handler_register(nic->dev, tsn_rx_handler, nic) < 0) {
			pr_err("%s: could not attach an Rx-handler to %s, this link will not be able to accept TSN traffic\n",
			       __func__, nic->name);
			rtnl_unlock();
			continue;
		}
		rtnl_unlock();
		pr_info("%s: attached rx-handler to %s\n",
			__func__, nic->name);
		nic->rx_registered = 1;
	}
	mutex_unlock(&tlist->lock);
	return 0;
}

void tsn_net_remove_rx(struct tsn_list *tlist)
{
	struct tsn_nic *nic;

	if (!tlist)
		return;
	mutex_lock(&tlist->lock);
	list_for_each_entry(nic, &tlist->head, list) {
		rtnl_lock();
		if (nic->rx_registered)
			netdev_rx_handler_unregister(nic->dev);
		rtnl_unlock();
		nic->rx_registered = 0;
		pr_info("%s: RX-handler for %s removed\n",
			__func__, nic->name);
	}
	mutex_unlock(&tlist->lock);
}

int tsn_net_prepare_tx(struct tsn_list *tlist)
{
	struct tsn_nic *nic;
	struct device *dev;
	int ret = 0;

	if (!tlist)
		return -EINVAL;

	mutex_lock(&tlist->lock);
	list_for_each_entry(nic, &tlist->head, list) {
		if (!nic)
			continue;
		if (!nic->capable)
			continue;

		if (!nic->dev->netdev_ops)
			continue;

		dev = nic->dev->dev.parent;
		nic->dma_mem = dma_alloc_coherent(dev, nic->dma_size,
						  &nic->dma_handle, GFP_KERNEL);
		if (!nic->dma_mem) {
			nic->capable = 0;
			nic->dma_size = 0;
			continue;
		}
		ret++;
	}
	mutex_unlock(&tlist->lock);
	pr_info("%s: configured %d cards to use DMA\n", __func__, ret);
	return ret;
}

void tsn_net_disable_tx(struct tsn_list *tlist)
{
	struct tsn_nic *nic;
	struct device *dev;
	int res = 0;

	if (!tlist)
		return;
	mutex_lock(&tlist->lock);
	list_for_each_entry(nic, &tlist->head, list) {
		if (nic->capable && nic->dma_mem) {
			dev = nic->dev->dev.parent;
			dma_free_coherent(dev, nic->dma_size, nic->dma_mem,
					  nic->dma_handle);
			res++;
		}
	}
	mutex_unlock(&tlist->lock);
	pr_info("%s: freed DMA regions from %d cards\n", __func__, res);
}

void tsn_net_close(struct tsn_link *link)
{
	/* struct tsn_rx_handler_data *rx_data; */

	/* Careful! we need to make sure that we actually succeeded in
	 * registering the handler in open unless we want to unregister
	 * some random rx_handler..
	 */
	if (!link->estype_talker) {
		;
		/* Make sure we notify rx-handler so it doesn't write
		 * into NULL
		 */
	}
}

int tsn_net_set_vlan(struct tsn_link *link)
{
	int err;
	struct tsn_nic *nic = link->nic;
	const struct net_device_ops *ops = nic->dev->netdev_ops;

	int vf   = 2;
	u16 vlan = link->vlan_id;
	u8 qos   = link->class_a ? link->pcp_a : link->pcp_b;

	pr_info("%s:%s Setting vlan=%u,vf=%d,qos=%u\n",
		__func__, nic->name, vlan, vf, qos);
	if (ops->ndo_set_vf_vlan) {
		err = ops->ndo_set_vf_vlan(nic->dev, vf, vlan, qos);
		if (err != 0) {
			pr_err("%s:%s could not set VLAN to %u, got %d\n",
			       __func__,  nic->name, vlan, err);
			return -EINVAL;
		}
		return 0;
	}
	return -1;
}

static inline u16 _get_8021q_vid(struct tsn_link *link)
{
	u16 pcp = link->class_a ? link->pcp_a : link->pcp_b;
	/* If not explicitly provided, use SR_PVID 0x2*/
	return (link->vlan_id & VLAN_VID_MASK) | ((pcp & 0x7) << 13);
}

/* create and initialize a sk_buff with appropriate TSN Header values
 *
 * layout of frame:
 * - Ethernet header
 *   dst (6) | src (6) | 802.1Q (4) | EtherType (2)
 * - 1722 (sizeof struct avtpdu)
 * - payload data
 *	- type header (e.g. iec61883-6 hdr)
 *	- payload data
 *
 * Required size:
 *  Ethernet: 18 -> VLAN_ETH_HLEN
 *  1722: tsnh_len()
 *  payload: shim_hdr_size + data_bytes
 *
 * Note:
 *	- seqnr is not set
 *	- payload is not set
 */
static struct sk_buff *_skbuf_create_init(struct tsn_link *link,
					  size_t data_bytes,
					  size_t shim_hdr_size,
					  u64 ts_pres_ns, u8 more)
{
	struct sk_buff *skb = NULL;
	struct avtpdu_header *avtpdu;
	struct net_device *netdev = link->nic->dev;
	int queue_idx;
	int res = 0;
	int hard_hdr_len;

	/* length is size of AVTPDU + data
	 * +-----+ <-- head
	 * | - link layer header
	 * | - 1722 header (avtpdu_header)
	 * +-----+ <-- data
	 * | - shim_header
	 * | - data
	 * +-----+ <-- tail
	 * |
	 * +-----+ <--end
	 * We stuff all of TSN-related
	 * headers in the data-segment to make it easy
	 */
	size_t hdr_len = VLAN_ETH_HLEN;
	size_t avtpdu_len = tsnh_len() + shim_hdr_size + data_bytes;

	skb = alloc_skb(hdr_len + avtpdu_len + netdev->needed_tailroom,
			GFP_ATOMIC | GFP_DMA);
	if (!skb)
		return NULL;
	skb_reserve(skb, hdr_len);

	skb->protocol = htons(ETH_P_TSN);
	skb->pkt_type = PACKET_OUTGOING;
	skb->priority = (link->class_a ? link->pcp_a : link->pcp_b);
	skb->dev = link->nic->dev;
	skb_shinfo(skb)->tx_flags |= SKBTX_HW_TSTAMP;
	skb->xmit_more = (more > 0 ? 1 : 0);
	skb_set_mac_header(skb, 0);

	/* We are using a ethernet-type frame (even though we could send
	 * TSN over other medium.
	 *
	 * - skb_push(skb, ETH_HLEN)
	 * - set header htons(header)
	 * - set source addr (netdev mac addr)
	 * - set dest addr
	 * - return ETH_HLEN
	 */
	hard_hdr_len = dev_hard_header(skb, skb->dev, ETH_P_TSN,
				       link->remote_mac, NULL, 6);

	skb = vlan_insert_tag(skb, htons(ETH_P_8021Q), _get_8021q_vid(link));
	if (!skb) {
		pr_err("%s: could not insert tag in buffer, aborting\n",
		       __func__);
		return NULL;
	}

	/* tsnh_assemble_du() will deref avtpdu to find start of data
	 * segment and use that, this is to update the skb
	 * appropriately.
	 *
	 * tsnh_assemble_du() will grab tsn-lock before updating link
	 */
	avtpdu = (struct avtpdu_header *)skb_put(skb, avtpdu_len);
	res = tsnh_assemble_du(link, avtpdu, data_bytes, ts_pres_ns);
	if (res < 0) {
		pr_err("%s: Error initializing header (-> %d) , we are in an inconsistent state!\n",
		       __func__, res);
		kfree_skb(skb);
		return NULL;
	}

	/* FIXME: Find a suitable Tx-queue
	 *
	 * For igb, this returns -1
	 */
	queue_idx = sk_tx_queue_get(skb->sk);
	if (queue_idx < 0 || queue_idx >= netdev->real_num_tx_queues)
		queue_idx = 0;
	skb_set_queue_mapping(skb, queue_idx);
	skb->queue_mapping = 0;

	skb->csum = skb_checksum(skb, 0, hdr_len + data_bytes, 0);
	return skb;
}

/**
 * Send a set of frames as efficiently as possible
 */
int tsn_net_send_set(struct tsn_link *link, size_t num, u64 ts_base_ns,
		     u64 ts_delta_ns)
{
	struct sk_buff *skb;
	struct net_device *dev;
	size_t data_size;
	int res;
	struct netdev_queue *txq;
	u64 ts_pres_ns = ts_base_ns;

	if (!link)
		return -EINVAL;
	dev = link->nic->dev;

	/* create and init sk_buff_head */
	while (num-- > 0) {
		data_size = tsn_shim_get_framesize(link);

		skb = _skbuf_create_init(link, data_size,
					 tsn_shim_get_hdr_size(link),
					 ts_pres_ns, (num > 0));
		if (!skb) {
			pr_err("%s: could not allocate memory for skb\n",
			       __func__);
			return -ENOMEM;
		}

		trace_tsn_pre_tx(link, skb, data_size);
		txq = skb_get_tx_queue(dev, skb);
		if (!txq) {
			pr_err("%s: Could not get tx_queue, dropping sending\n",
			       __func__);
			kfree_skb(skb);
			return -EINVAL;
		}
		res = netdev_start_xmit(skb, dev, txq, (num > 0));
		if (res != NETDEV_TX_OK) {
			pr_err("%s: Tx FAILED\n", __func__);
			return res;
		}
		ts_pres_ns += ts_delta_ns;
	}
	return 0;
}
