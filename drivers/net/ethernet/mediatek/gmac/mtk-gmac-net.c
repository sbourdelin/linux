// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2018 MediaTek Inc.
#include <linux/netdevice.h>
#include <linux/tcp.h>
#include <linux/interrupt.h>

#include "mtk-gmac.h"

static int gmac_one_poll(struct napi_struct *, int);
static int gmac_all_poll(struct napi_struct *, int);

static inline unsigned int gmac_tx_avail_desc(struct gmac_ring *ring)
{
	return (ring->dma_desc_count - (ring->cur - ring->dirty));
}

static inline unsigned int gmac_rx_dirty_desc(struct gmac_ring *ring)
{
	return (ring->cur - ring->dirty);
}

static int gmac_maybe_stop_tx_queue(struct gmac_channel *channel,
				    struct gmac_ring *ring,
				    unsigned int count)
{
	struct gmac_pdata *pdata = channel->pdata;

	if (count > gmac_tx_avail_desc(ring)) {
		netif_info(pdata, drv, pdata->netdev,
			   "Tx queue stopped, not enough descriptors available\n");
		netif_stop_subqueue(pdata->netdev, channel->queue_index);
		ring->tx.queue_stopped = 1;

		/* If we haven't notified the hardware because of xmit_more
		 * support, tell it now
		 */
		if (ring->tx.xmit_more)
			pdata->hw_ops.tx_start_xmit(channel, ring);

		return NETDEV_TX_BUSY;
	}

	return 0;
}

static void gmac_prep_vlan(struct sk_buff *skb,
			   struct gmac_pkt_info *pkt_info)
{
	if (skb_vlan_tag_present(skb))
		pkt_info->vlan_ctag = skb_vlan_tag_get(skb);
}

static int gmac_prep_tso(struct gmac_pdata *pdata,
			 struct sk_buff *skb,
			 struct gmac_pkt_info *pkt_info)
{
	int ret;

	if (!GMAC_GET_REG_BITS(pkt_info->attributes,
			       TX_PACKET_ATTRIBUTES_TSO_ENABLE_POS,
			       TX_PACKET_ATTRIBUTES_TSO_ENABLE_LEN))
		return 0;

	ret = skb_cow_head(skb, 0);
	if (ret)
		return ret;

	pkt_info->header_len = skb_transport_offset(skb) + tcp_hdrlen(skb);
	pkt_info->tcp_header_len = tcp_hdrlen(skb);
	pkt_info->tcp_payload_len = skb->len - pkt_info->header_len;
	pkt_info->mss = skb_shinfo(skb)->gso_size;

	netif_dbg(pdata, tx_queued, pdata->netdev,
		  "header_len=%u\n", pkt_info->header_len);
	netif_dbg(pdata, tx_queued, pdata->netdev,
		  "tcp_header_len=%u, tcp_payload_len=%u\n",
		  pkt_info->tcp_header_len, pkt_info->tcp_payload_len);
	netif_dbg(pdata, tx_queued, pdata->netdev, "mss=%u\n", pkt_info->mss);

	/* Update the number of packets that will ultimately be transmitted
	 * along with the extra bytes for each extra packet
	 */
	pkt_info->tx_packets = skb_shinfo(skb)->gso_segs;
	pkt_info->tx_bytes +=
		(pkt_info->tx_packets - 1) * pkt_info->header_len;

	return 0;
}

static int gmac_is_tso(struct sk_buff *skb)
{
	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return 0;

	if (!skb_is_gso(skb))
		return 0;

	return 1;
}

static void gmac_prep_tx_pkt(struct gmac_pdata *pdata,
			     struct gmac_ring *ring,
			     struct sk_buff *skb,
			     struct gmac_pkt_info *pkt_info)
{
	struct skb_frag_struct *frag;
	unsigned int context_desc;
	unsigned int len;
	unsigned int i;

	pkt_info->skb = skb;

	context_desc = 0;
	pkt_info->desc_count = 0;

	pkt_info->tx_packets = 1;
	pkt_info->tx_bytes = skb->len;

	if (gmac_is_tso(skb)) {
		/* TSO requires an extra descriptor if mss is different */
		if (skb_shinfo(skb)->gso_size != ring->tx.cur_mss) {
			context_desc = 1;
			pkt_info->desc_count++;
		}

		/* TSO requires an extra descriptor for TSO header */
		pkt_info->desc_count++;

		pkt_info->attributes = GMAC_SET_REG_BITS(pkt_info->attributes,
							 TX_PACKET_ATTRIBUTES_TSO_ENABLE_POS,
							 TX_PACKET_ATTRIBUTES_TSO_ENABLE_LEN,
							 1);
		pkt_info->attributes = GMAC_SET_REG_BITS(pkt_info->attributes,
							 TX_PACKET_ATTRIBUTES_CSUM_ENABLE_POS,
							 TX_PACKET_ATTRIBUTES_CSUM_ENABLE_LEN,
							 1);
	} else if (skb->ip_summed == CHECKSUM_PARTIAL) {
		pkt_info->attributes = GMAC_SET_REG_BITS(pkt_info->attributes,
							 TX_PACKET_ATTRIBUTES_CSUM_ENABLE_POS,
							 TX_PACKET_ATTRIBUTES_CSUM_ENABLE_LEN,
							 1);
	}

	if (skb_vlan_tag_present(skb)) {
		/* VLAN requires an extra descriptor if tag is different */
		if (skb_vlan_tag_get(skb) != ring->tx.cur_vlan_ctag)
			/* We can share with the TSO context descriptor */
			if (!context_desc) {
				context_desc = 1;
				pkt_info->desc_count++;
			}

		pkt_info->attributes = GMAC_SET_REG_BITS(pkt_info->attributes,
							 TX_PACKET_ATTRIBUTES_VLAN_CTAG_POS,
							 TX_PACKET_ATTRIBUTES_VLAN_CTAG_LEN,
							 1);
	}

	if (unlikely((skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP) &&
		     pdata->hw_feat.ts_src &&
		     pdata->hwts_tx_en)) {
		/* declare that device is doing timestamping */
		skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
		pkt_info->attributes = GMAC_SET_REG_BITS(pkt_info->attributes,
							 TX_PACKET_ATTRIBUTES_PTP_POS,
							 TX_PACKET_ATTRIBUTES_PTP_LEN,
							 1);
	}

	for (len = skb_headlen(skb); len;) {
		pkt_info->desc_count++;
		len -= min_t(unsigned int, len, GMAC_TX_MAX_BUF_SIZE);
	}

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		frag = &skb_shinfo(skb)->frags[i];
		for (len = skb_frag_size(frag); len; ) {
			pkt_info->desc_count++;
			len -= min_t(unsigned int, len, GMAC_TX_MAX_BUF_SIZE);
		}
	}
}

static int gmac_calc_rx_buf_size(struct net_device *netdev, unsigned int mtu)
{
	unsigned int rx_buf_size;

	if (mtu > ETH_DATA_LEN) {
		netdev_alert(netdev, "MTU exceeds maximum supported value\n");
		return -EINVAL;
	}

	rx_buf_size = mtu + ETH_HLEN + ETH_FCS_LEN + VLAN_HLEN;

	return rx_buf_size;
}

static void gmac_enable_rx_tx_ints(struct gmac_pdata *pdata)
{
	struct gmac_hw_ops *hw_ops = &pdata->hw_ops;
	struct gmac_channel *channel;
	enum gmac_int int_id;
	unsigned int i;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (channel->tx_ring && channel->rx_ring)
			int_id = GMAC_INT_DMA_CH_SR_TI_RI;
		else if (channel->tx_ring)
			int_id = GMAC_INT_DMA_CH_SR_TI;
		else if (channel->rx_ring)
			int_id = GMAC_INT_DMA_CH_SR_RI;
		else
			continue;

		hw_ops->enable_int(channel, int_id);
	}
}

static void gmac_disable_rx_tx_ints(struct gmac_pdata *pdata)
{
	struct gmac_hw_ops *hw_ops = &pdata->hw_ops;
	struct gmac_channel *channel;
	enum gmac_int int_id;
	unsigned int i;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (channel->tx_ring && channel->rx_ring)
			int_id = GMAC_INT_DMA_CH_SR_TI_RI;
		else if (channel->tx_ring)
			int_id = GMAC_INT_DMA_CH_SR_TI;
		else if (channel->rx_ring)
			int_id = GMAC_INT_DMA_CH_SR_RI;
		else
			continue;

		hw_ops->disable_int(channel, int_id);
	}
}

static void gmac_rgsmii(struct gmac_pdata *pdata)
{
	struct gmac_hw_ops *hw_ops = &pdata->hw_ops;
	struct net_device *ndev = pdata->netdev;
	u32 status, duplex;

	status = GMAC_IOREAD(pdata, MAC_PCSR);
	if (GMAC_GET_REG_BITS(status, MAC_RGMII_LNKSTS_POS,
			      MAC_RGMII_LNKSTS_LEN)) {
		int speed_value;

		speed_value = GMAC_GET_REG_BITS(status,
						MAC_RGMII_SPEED_POS,
						MAC_RGMII_SPEED_LEN);
		if (speed_value == GMAC_RGSMIIIS_SPEED_125) {
			hw_ops->set_gmii_1000_speed(pdata);
			pdata->phy_speed = SPEED_1000;
		} else if (speed_value == GMAC_RGSMIIIS_SPEED_25) {
			hw_ops->set_gmii_100_speed(pdata);
			pdata->phy_speed = SPEED_100;
		} else {
			hw_ops->set_gmii_10_speed(pdata);
			pdata->phy_speed = SPEED_10;
		}

		duplex = GMAC_GET_REG_BITS(status,
					   MAC_RGMII_LNKMODE_POS,
					   MAC_RGMII_LNKMODE_LEN);
		if (duplex) {
			hw_ops->set_full_duplex(pdata);
			pdata->phy_speed = DUPLEX_FULL;
		} else {
			hw_ops->set_half_duplex(pdata);
			pdata->phy_speed = DUPLEX_HALF;
		}

		netif_carrier_on(ndev);
	} else {
		netif_carrier_off(ndev);
	}
}

static int gmac_hw_dma_interrupt(struct gmac_pdata *pdata)
{
	struct gmac_channel *channel;
	unsigned int dma_isr, dma_ch_isr;
	int ret = 0, i;

	dma_isr = GMAC_IOREAD(pdata, DMA_ISR);

	/* Handle DMA interrupts */
	for (i = 0; i < pdata->channel_count; i++) {
		if (!(dma_isr & (1 << i)))
			continue;

		channel = pdata->channel_head + i;

		dma_ch_isr =
			GMAC_IOREAD(pdata, DMA_CH_SR(channel->queue_index));
		netif_dbg(pdata, intr, pdata->netdev, "DMA_CH%u_ISR=%#010x\n",
			  i, dma_ch_isr);

		if (GMAC_GET_REG_BITS(dma_ch_isr,
				      DMA_CH_ISR_AIS_POS,
				      DMA_CH_ISR_AIS_LEN)) {
			if (GMAC_GET_REG_BITS(dma_ch_isr,
					      DMA_CH_ISR_TPS_POS,
					      DMA_CH_ISR_TPS_LEN))
				pdata->stats.tx_process_stopped++;

			if (GMAC_GET_REG_BITS(dma_ch_isr,
					      DMA_CH_ISR_RPS_POS,
					      DMA_CH_ISR_RPS_LEN))
				pdata->stats.rx_process_stopped++;

			if (GMAC_GET_REG_BITS(dma_ch_isr,
					      DMA_CH_ISR_TBU_POS,
					      DMA_CH_ISR_TBU_LEN))
				pdata->stats.tx_buffer_unavailable++;

			if (GMAC_GET_REG_BITS(dma_ch_isr,
					      DMA_CH_ISR_RBU_POS,
					      DMA_CH_ISR_RBU_LEN))
				pdata->stats.rx_buffer_unavailable++;

			/* Restart the device on a Fatal Bus Error */
			if (GMAC_GET_REG_BITS(dma_ch_isr,
					      DMA_CH_ISR_FBE_POS,
					      DMA_CH_ISR_FBE_LEN)) {
				pdata->stats.fatal_bus_error++;
				schedule_work(&pdata->restart_work);
				ret = tx_hard_error;
			}
		}

		/* TX/RX NORMAL interrupts */
		if (GMAC_GET_REG_BITS(dma_ch_isr,
				      DMA_CH_ISR_NIS_POS,
				      DMA_CH_ISR_NIS_LEN)) {
			if (GMAC_GET_REG_BITS(dma_ch_isr,
					      DMA_CH_ISR_RI_POS,
					      DMA_CH_ISR_RI_LEN)) {
				ret |= handle_rx;
			}
			if (GMAC_GET_REG_BITS(dma_ch_isr,
					      DMA_CH_ISR_TI_POS,
					      DMA_CH_ISR_TI_LEN))
				ret |= handle_tx;
		}

		/* Clear the interrupt by writing a logic 1 to the CSR5[15-0] */
		GMAC_IOWRITE(pdata,
			     DMA_CH_SR(channel->queue_index),
			     dma_ch_isr & 0x1ffff);
	}

	return ret;
}

static void gmac_dma_interrupt(struct gmac_pdata *pdata)
{
	int status;

	status = gmac_hw_dma_interrupt(pdata);

	if (!pdata->per_channel_irq &&
	    likely((status & handle_rx) || (status & handle_tx)))
		if (likely(napi_schedule_prep(&pdata->napi))) {
			gmac_disable_rx_tx_ints(pdata);
			pdata->stats.napi_poll_isr++;
			/* Turn on polling */
			__napi_schedule_irqoff(&pdata->napi);
		}
}

static irqreturn_t gmac_isr(int irq, void *data)
{
	unsigned int dma_isr, mac_isr;
	struct gmac_pdata *pdata = data;
	struct gmac_hw_ops *hw_ops;

	hw_ops = &pdata->hw_ops;

	/* The DMA interrupt status register also reports MAC and MTL
	 * interrupts. So for polling mode, we just need to check for
	 * this register to be non-zero
	 */
	dma_isr = GMAC_IOREAD(pdata, DMA_ISR);
	if (!dma_isr)
		return IRQ_HANDLED;

	netif_dbg(pdata, intr, pdata->netdev, "DMA_ISR=%#010x\n", dma_isr);

	if (GMAC_GET_REG_BITS(dma_isr, DMA_ISR_MACIS_POS,
			      DMA_ISR_MACIS_LEN)) {
		mac_isr = GMAC_IOREAD(pdata, MAC_ISR);

		if (GMAC_GET_REG_BITS(mac_isr,
				      MAC_ISR_MMCTXIS_POS,
				      MAC_ISR_MMCTXIS_LEN))
			hw_ops->tx_mmc_int(pdata);

		if (GMAC_GET_REG_BITS(mac_isr,
				      MAC_ISR_MMCRXIS_POS,
				      MAC_ISR_MMCRXIS_LEN))
			hw_ops->rx_mmc_int(pdata);

		if (GMAC_GET_REG_BITS(mac_isr,
				      MAC_ISR_MMCRXIPIS_POS,
				      MAC_ISR_MMCRXIPIS_LEN))
			hw_ops->rxipc_mmc_int(pdata);

		if (GMAC_GET_REG_BITS(mac_isr,
				      MAC_ISR_RGSMIIS_POS,
				      MAC_ISR_RGSMIIS_LEN))
			gmac_rgsmii(pdata);
	}

	gmac_dma_interrupt(pdata);

	return IRQ_HANDLED;
}

static irqreturn_t gmac_dma_isr(int irq, void *data)
{
	struct gmac_channel *channel = data;

	/* Per channel DMA interrupts are enabled, so we use the per
	 * channel napi structure and not the private data napi structure
	 */
	if (napi_schedule_prep(&channel->napi)) {
		/* Disable Tx and Rx interrupts */
		disable_irq_nosync(channel->dma_irq);

		/* Turn on polling */
		__napi_schedule_irqoff(&channel->napi);
	}

	return IRQ_HANDLED;
}

static void gmac_tx_timer(struct timer_list *t)
{
	struct gmac_channel *channel = from_timer(channel, t, tx_timer);
	struct gmac_pdata *pdata = channel->pdata;
	struct napi_struct *napi;

	napi = (pdata->per_channel_irq) ? &channel->napi : &pdata->napi;

	if (napi_schedule_prep(napi)) {
		/* Disable Tx and Rx interrupts */
		if (pdata->per_channel_irq)
			disable_irq_nosync(channel->dma_irq);
		else
			gmac_disable_rx_tx_ints(pdata);

		pdata->stats.napi_poll_txtimer++;
		/* Turn on polling */
		__napi_schedule(napi);
	}

	channel->tx_timer_active = 0;
}

static void gmac_init_timers(struct gmac_pdata *pdata)
{
	struct gmac_channel *channel;
	unsigned int i;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			break;

		timer_setup(&channel->tx_timer, gmac_tx_timer, 0);
	}
}

static void gmac_stop_timers(struct gmac_pdata *pdata)
{
	struct gmac_channel *channel;
	unsigned int i;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			break;

		del_timer_sync(&channel->tx_timer);
	}
}

static void gmac_napi_enable(struct gmac_pdata *pdata)
{
	struct gmac_channel *channel;
	unsigned int i;

	if (pdata->per_channel_irq) {
		channel = pdata->channel_head;
		for (i = 0; i < pdata->channel_count; i++, channel++) {
			netif_napi_add(pdata->netdev,
				       &channel->napi,
				       gmac_one_poll,
				       NAPI_POLL_WEIGHT);

			napi_enable(&channel->napi);
		}
	} else {
		netif_napi_add(pdata->netdev,
			       &pdata->napi,
			       gmac_all_poll,
			       NAPI_POLL_WEIGHT);

		napi_enable(&pdata->napi);
	}
}

static void gmac_napi_disable(struct gmac_pdata *pdata)
{
	struct gmac_channel *channel;
	unsigned int i;

	if (pdata->per_channel_irq) {
		channel = pdata->channel_head;
		for (i = 0; i < pdata->channel_count; i++, channel++) {
			napi_disable(&channel->napi);

			netif_napi_del(&channel->napi);
		}
	} else {
		napi_disable(&pdata->napi);

		netif_napi_del(&pdata->napi);
	}
}

static int gmac_request_irqs(struct gmac_pdata *pdata)
{
	struct net_device *netdev = pdata->netdev;
	struct gmac_channel *channel;
	unsigned int i;
	int ret;

	ret = devm_request_irq(pdata->dev, pdata->dev_irq, gmac_isr,
			       IRQF_SHARED, netdev->name, pdata);
	if (ret) {
		netdev_alert(netdev, "error requesting irq %d\n",
			     pdata->dev_irq);
		return ret;
	}

	if (!pdata->per_channel_irq)
		return 0;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		snprintf(channel->dma_irq_name,
			 sizeof(channel->dma_irq_name) - 1,
			 "%s-TxRx-%u", netdev_name(netdev),
			 channel->queue_index);

		ret = devm_request_irq(pdata->dev, channel->dma_irq,
				       gmac_dma_isr, 0,
				       channel->dma_irq_name, channel);
		if (ret) {
			netdev_alert(netdev, "error requesting irq %d\n",
				     channel->dma_irq);
			goto err_irq;
		}
	}

	return 0;

err_irq:
	/* Using an unsigned int, 'i' will go to UINT_MAX and exit */
	for (i--, channel--; i < pdata->channel_count; i--, channel--)
		devm_free_irq(pdata->dev, channel->dma_irq, channel);

	devm_free_irq(pdata->dev, pdata->dev_irq, pdata);

	return ret;
}

static void gmac_free_irqs(struct gmac_pdata *pdata)
{
	struct gmac_channel *channel;
	unsigned int i;

	devm_free_irq(pdata->dev, pdata->dev_irq, pdata);

	if (!pdata->per_channel_irq)
		return;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++)
		devm_free_irq(pdata->dev, channel->dma_irq, channel);
}

static void gmac_free_tx_data(struct gmac_pdata *pdata)
{
	struct gmac_desc_ops *desc_ops = &pdata->desc_ops;
	struct gmac_desc_data *desc_data;
	struct gmac_channel *channel;
	struct gmac_ring *ring;
	unsigned int i, j;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		ring = channel->tx_ring;
		if (!ring)
			break;

		for (j = 0; j < ring->dma_desc_count; j++) {
			desc_data = GMAC_GET_DESC_DATA(ring, j);
			desc_ops->unmap_desc_data(pdata, desc_data, 1);
		}
	}
}

static void gmac_free_rx_data(struct gmac_pdata *pdata)
{
	struct gmac_desc_ops *desc_ops = &pdata->desc_ops;
	struct gmac_desc_data *desc_data;
	struct gmac_channel *channel;
	struct gmac_ring *ring;
	unsigned int i, j;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		ring = channel->rx_ring;
		if (!ring)
			break;

		for (j = 0; j < ring->dma_desc_count; j++) {
			desc_data = GMAC_GET_DESC_DATA(ring, j);
			desc_ops->unmap_desc_data(pdata, desc_data, 0);
		}
	}
}

static int gmac_start(struct gmac_pdata *pdata)
{
	struct gmac_hw_ops *hw_ops = &pdata->hw_ops;
	struct net_device *netdev = pdata->netdev;
	int ret;

	hw_ops->init(pdata);
	gmac_napi_enable(pdata);

	ret = gmac_request_irqs(pdata);
	if (ret)
		goto err_napi;

	hw_ops->enable_tx(pdata);
	hw_ops->enable_rx(pdata);
	netif_tx_start_all_queues(netdev);

	return 0;

err_napi:
	gmac_napi_disable(pdata);
	hw_ops->exit(pdata);

	return ret;
}

static void gmac_stop(struct gmac_pdata *pdata)
{
	struct gmac_hw_ops *hw_ops = &pdata->hw_ops;
	struct net_device *netdev = pdata->netdev;
	struct gmac_channel *channel;
	struct netdev_queue *txq;
	unsigned int i;

	netif_tx_stop_all_queues(netdev);
	gmac_stop_timers(pdata);
	hw_ops->disable_tx(pdata);
	hw_ops->disable_rx(pdata);
	gmac_free_irqs(pdata);
	gmac_napi_disable(pdata);
	hw_ops->exit(pdata);

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			continue;

		txq = netdev_get_tx_queue(netdev, channel->queue_index);
		netdev_tx_reset_queue(txq);
	}
}

static void gmac_restart_dev(struct gmac_pdata *pdata)
{
	/* If not running, "restart" will happen on open */
	if (!netif_running(pdata->netdev))
		return;

	gmac_stop(pdata);

	gmac_free_tx_data(pdata);
	gmac_free_rx_data(pdata);

	gmac_start(pdata);
}

static void gmac_restart(struct work_struct *work)
{
	struct gmac_pdata *pdata = container_of(work,
						struct gmac_pdata,
						restart_work);

	rtnl_lock();

	gmac_restart_dev(pdata);

	rtnl_unlock();
}

static int gmac_open(struct net_device *netdev)
{
	struct gmac_pdata *pdata = netdev_priv(netdev);
	struct gmac_desc_ops *desc_ops;
	int ret;

	desc_ops = &pdata->desc_ops;

	/* Calculate the Rx buffer size before allocating rings */
	ret = gmac_calc_rx_buf_size(netdev, netdev->mtu);
	if (ret < 0)
		return ret;
	pdata->rx_buf_size = ret;

	/* Allocate the channels and rings */
	ret = desc_ops->alloc_channles_and_rings(pdata);
	if (ret)
		return ret;

	INIT_WORK(&pdata->restart_work, gmac_restart);
	gmac_init_timers(pdata);

	ret = gmac_start(pdata);
	if (ret)
		goto err_channels_and_rings;

	return 0;

err_channels_and_rings:
	desc_ops->free_channels_and_rings(pdata);

	return ret;
}

static int gmac_close(struct net_device *netdev)
{
	struct gmac_pdata *pdata = netdev_priv(netdev);
	struct gmac_desc_ops *desc_ops;

	desc_ops = &pdata->desc_ops;

	/* Stop the device */
	gmac_stop(pdata);

	gmac_free_tx_data(pdata);
	gmac_free_rx_data(pdata);

	/* Free the channels and rings */
	desc_ops->free_channels_and_rings(pdata);

	return 0;
}

static void gmac_tx_timeout(struct net_device *netdev)
{
	struct gmac_pdata *pdata = netdev_priv(netdev);

	netdev_warn(netdev, "tx timeout, device restarting\n");
	schedule_work(&pdata->restart_work);
}

static int gmac_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct gmac_pdata *pdata = netdev_priv(netdev);
	struct gmac_pkt_info *tx_pkt_info;
	struct gmac_desc_ops *desc_ops;
	struct gmac_channel *channel;
	struct gmac_hw_ops *hw_ops;
	struct netdev_queue *txq;
	struct gmac_ring *ring;
	int ret;

	desc_ops = &pdata->desc_ops;
	hw_ops = &pdata->hw_ops;

	netif_dbg(pdata, tx_queued, pdata->netdev,
		  "skb->len = %d\n", skb->len);

	channel = pdata->channel_head + skb->queue_mapping;
	txq = netdev_get_tx_queue(netdev, channel->queue_index);
	ring = channel->tx_ring;
	tx_pkt_info = &ring->pkt_info;

	if (skb->len == 0) {
		netif_err(pdata, tx_err, netdev,
			  "empty skb received from stack\n");
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	/* Prepare preliminary packet info for TX */
	memset(tx_pkt_info, 0, sizeof(*tx_pkt_info));
	gmac_prep_tx_pkt(pdata, ring, skb, tx_pkt_info);

	/* Check that there are enough descriptors available */
	ret = gmac_maybe_stop_tx_queue(channel,
				       ring,
				       tx_pkt_info->desc_count);
	if (ret)
		return ret;

	ret = gmac_prep_tso(pdata, skb, tx_pkt_info);
	if (ret) {
		netif_err(pdata, tx_err, netdev,
			  "error processing TSO packet\n");
		dev_kfree_skb_any(skb);
		return ret;
	}
	gmac_prep_vlan(skb, tx_pkt_info);

	if (!desc_ops->map_tx_skb(channel, skb)) {
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	/* Report on the actual number of bytes (to be) sent */
	netdev_tx_sent_queue(txq, tx_pkt_info->tx_bytes);

	/* Fallback to software timestamping if
	 * core doesn't support hardware timestamping
	 */
	if (pdata->hw_feat.ts_src == 0 ||
	    pdata->hwts_tx_en == 0)
		skb_tx_timestamp(skb);

	/* Configure required descriptor fields for transmission */
	hw_ops->dev_xmit(channel);

	if (netif_msg_pktdata(pdata))
		gmac_print_pkt(netdev, skb, true);

	/* Stop the queue in advance if there may not be enough descriptors */
	gmac_maybe_stop_tx_queue(channel, ring, GMAC_TX_MAX_DESC_NR);

	return NETDEV_TX_OK;
}

static void gmac_get_stats64(struct net_device *netdev,
			     struct rtnl_link_stats64 *s)
{
	struct gmac_pdata *pdata = netdev_priv(netdev);
	struct gmac_stats *pstats = &pdata->stats;

	pdata->hw_ops.read_mmc_stats(pdata);

	s->rx_packets = pstats->rxframecount_gb;
	s->rx_bytes = pstats->rxoctetcount_gb;
	s->rx_errors = pstats->rxframecount_gb -
		       pstats->rxbroadcastframes_g -
		       pstats->rxmulticastframes_g -
		       pstats->rxunicastframes_g;
	s->multicast = pstats->rxmulticastframes_g;
	s->rx_length_errors = pstats->rxlengtherror;
	s->rx_crc_errors = pstats->rxcrcerror;
	s->rx_fifo_errors = pstats->rxfifooverflow;

	s->tx_packets = pstats->txframecount_gb;
	s->tx_bytes = pstats->txoctetcount_gb;
	s->tx_errors = pstats->txframecount_gb - pstats->txframecount_g;
	s->tx_dropped = netdev->stats.tx_dropped;
}

static int gmac_set_mac_address(struct net_device *netdev, void *addr)
{
	struct gmac_pdata *pdata = netdev_priv(netdev);
	struct gmac_hw_ops *hw_ops = &pdata->hw_ops;
	struct sockaddr *saddr = addr;

	if (!is_valid_ether_addr(saddr->sa_data))
		return -EADDRNOTAVAIL;

	memcpy(netdev->dev_addr, saddr->sa_data, netdev->addr_len);

	hw_ops->set_mac_address(pdata, netdev->dev_addr, 0);

	return 0;
}

static int gmac_hwtstamp_ioctl(struct net_device *netdev, struct ifreq *ifr)
{
	struct gmac_pdata *pdata = netdev_priv(netdev);
	struct gmac_hw_ops *hw_ops = &pdata->hw_ops;
	struct hwtstamp_config config;
	struct timespec64 now;
	u64 temp = 0;
	u32 value = 0;
	u32 sec_inc;

	if (!pdata->hw_feat.ts_src) {
		netdev_alert(pdata->netdev, "No support for HW timestamping\n");
		pdata->hwts_tx_en = 0;
		pdata->hwts_rx_en = 0;

		return -EOPNOTSUPP;
	}

	if (copy_from_user(&config, ifr->ifr_data,
			   sizeof(struct hwtstamp_config)))
		return -EFAULT;

	netdev_dbg(pdata->netdev, "%s config flags:0x%x, tx_type:0x%x, rx_filter:0x%x\n",
		   __func__, config.flags, config.tx_type, config.rx_filter);

	/* reserved for future extensions */
	if (config.flags)
		return -EINVAL;

	if (config.tx_type != HWTSTAMP_TX_OFF &&
	    config.tx_type != HWTSTAMP_TX_ON)
		return -ERANGE;

	switch (config.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		/* time stamp no incoming packet at all */
		config.rx_filter = HWTSTAMP_FILTER_NONE;
		break;

	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
		/* PTP v1, UDP, any kind of event packet */
		config.rx_filter = HWTSTAMP_FILTER_PTP_V1_L4_EVENT;
		/* take time stamp for all event messages */
		value = GMAC_SET_REG_BITS(value, PTP_TCR_SNAPTYPSEL_POS,
					  PTP_TCR_SNAPTYPSEL_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPV4ENA_POS,
					  PTP_TCR_TSIPV4ENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPV6ENA_POS,
					  PTP_TCR_TSIPV6ENA_LEN, 1);

		break;

	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
		/* PTP v1, UDP, Sync packet */
		config.rx_filter = HWTSTAMP_FILTER_PTP_V1_L4_SYNC;
		/* take time stamp for SYNC messages only */
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSEVNTENA_POS,
					  PTP_TCR_TSEVNTENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPV4ENA_POS,
					  PTP_TCR_TSIPV4ENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPV6ENA_POS,
					  PTP_TCR_TSIPV6ENA_LEN, 1);
		break;

	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
		/* PTP v1, UDP, Delay_req packet */
		config.rx_filter = HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ;
		/* take time stamp for Delay_Req messages only */
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSMSTRENA_POS,
					  PTP_TCR_TSMSTRENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSEVNTENA_POS,
					  PTP_TCR_TSEVNTENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPV4ENA_POS,
					  PTP_TCR_TSIPV4ENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPV6ENA_POS,
					  PTP_TCR_TSIPV6ENA_LEN, 1);
		break;

	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
		/* PTP v2, UDP, any kind of event packet */
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_EVENT;

		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSVER2ENA_POS,
					  PTP_TCR_TSVER2ENA_LEN, 1);
		/* take time stamp for all event messages */
		value = GMAC_SET_REG_BITS(value, PTP_TCR_SNAPTYPSEL_POS,
					  PTP_TCR_SNAPTYPSEL_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPV4ENA_POS,
					  PTP_TCR_TSIPV4ENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPV6ENA_POS,
					  PTP_TCR_TSIPV6ENA_LEN, 1);
		break;

	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
		/* PTP v2, UDP, Sync packet */
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_SYNC;

		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSVER2ENA_POS,
					  PTP_TCR_TSVER2ENA_LEN, 1);
		/* take time stamp for SYNC messages only */
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSEVNTENA_POS,
					  PTP_TCR_TSEVNTENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPV4ENA_POS,
					  PTP_TCR_TSIPV4ENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPV6ENA_POS,
					  PTP_TCR_TSIPV6ENA_LEN, 1);
		break;

	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
		/* PTP v2, UDP, Delay_req packet */
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ;

		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSVER2ENA_POS,
					  PTP_TCR_TSVER2ENA_LEN, 1);
		/* take time stamp for Delay_Req messages only */
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSMSTRENA_POS,
					  PTP_TCR_TSMSTRENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSEVNTENA_POS,
					  PTP_TCR_TSEVNTENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPV4ENA_POS,
					  PTP_TCR_TSIPV4ENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPV6ENA_POS,
					  PTP_TCR_TSIPV6ENA_LEN, 1);
		break;

	case HWTSTAMP_FILTER_PTP_V2_EVENT:
		/* PTP v2/802.AS1 any layer, any kind of event packet */
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_EVENT;

		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSVER2ENA_POS,
					  PTP_TCR_TSVER2ENA_LEN, 1);
		/* take time stamp for all event messages */
		value = GMAC_SET_REG_BITS(value, PTP_TCR_SNAPTYPSEL_POS,
					  PTP_TCR_SNAPTYPSEL_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPV4ENA_POS,
					  PTP_TCR_TSIPV4ENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPV6ENA_POS,
					  PTP_TCR_TSIPV6ENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPENA_POS,
					  PTP_TCR_TSIPENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_AV8021ASMEN_POS,
					  PTP_TCR_AV8021ASMEN_LEN, 1);
		break;

	case HWTSTAMP_FILTER_PTP_V2_SYNC:
		/* PTP v2/802.AS1, any layer, Sync packet */
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_SYNC;

		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSVER2ENA_POS,
					  PTP_TCR_TSVER2ENA_LEN, 1);
		/* take time stamp for SYNC messages only */
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSEVNTENA_POS,
					  PTP_TCR_TSEVNTENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPV4ENA_POS,
					  PTP_TCR_TSIPV4ENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPV6ENA_POS,
					  PTP_TCR_TSIPV6ENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPENA_POS,
					  PTP_TCR_TSIPENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_AV8021ASMEN_POS,
					  PTP_TCR_AV8021ASMEN_LEN, 1);
		break;

	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
		/* PTP v2/802.AS1, any layer, Delay_req packet */
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_DELAY_REQ;

		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSVER2ENA_POS,
					  PTP_TCR_TSVER2ENA_LEN, 1);
		/* take time stamp for Delay_Req messages only */
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSMSTRENA_POS,
					  PTP_TCR_TSMSTRENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSEVNTENA_POS,
					  PTP_TCR_TSEVNTENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPV4ENA_POS,
					  PTP_TCR_TSIPV4ENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPV6ENA_POS,
					  PTP_TCR_TSIPV6ENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSIPENA_POS,
					  PTP_TCR_TSIPENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_AV8021ASMEN_POS,
					  PTP_TCR_AV8021ASMEN_LEN, 1);
		break;

	case HWTSTAMP_FILTER_ALL:
		/* time stamp any incoming packet */
		config.rx_filter = HWTSTAMP_FILTER_ALL;

		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSENALL_POS,
					  PTP_TCR_TSENALL_LEN, 1);
		break;

	default:
		return -ERANGE;
	}
	pdata->hwts_rx_en =
		((config.rx_filter == HWTSTAMP_FILTER_NONE) ? 0 : 1);
	pdata->hwts_tx_en = config.tx_type == HWTSTAMP_TX_ON;

	if (!pdata->hwts_tx_en && !pdata->hwts_rx_en) {
		hw_ops->config_hw_timestamping(pdata, 0);
	} else {
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSENA_POS,
					  PTP_TCR_TSENA_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSCFUPDT_POS,
					  PTP_TCR_TSCFUPDT_LEN, 1);
		value = GMAC_SET_REG_BITS(value, PTP_TCR_TSCTRLSSR_POS,
					  PTP_TCR_TSCTRLSSR_LEN, 1);
		hw_ops->config_hw_timestamping(pdata, value);

		/* program Sub Second Increment reg */
		hw_ops->config_sub_second_increment(pdata,
						    pdata->ptpclk_rate,
						    &sec_inc);
		temp = div_u64(1000000000, sec_inc);

		/* calculate default added value:
		 * formula is :
		 * addend = (2^32)/freq_div_ratio;
		 * where, freq_div_ratio = 1e9ns/sec_inc
		 */
		temp = (u64)(temp << 32);
		pdata->default_addend = div_u64(temp, pdata->ptpclk_rate);
		hw_ops->config_addend(pdata, pdata->default_addend);

		/* initialize system time */
		ktime_get_real_ts64(&now);

		hw_ops->init_systime(pdata, (u32)now.tv_sec, now.tv_nsec);
	}

	return copy_to_user(ifr->ifr_data, &config,
			    sizeof(struct hwtstamp_config)) ? -EFAULT : 0;
}

static int gmac_ioctl(struct net_device *netdev,
		      struct ifreq *ifreq, int cmd)
{
	int ret = -EOPNOTSUPP;

	if (!netif_running(netdev))
		return -ENODEV;

	switch (cmd) {
	case SIOCSHWTSTAMP:
		ret = gmac_hwtstamp_ioctl(netdev, ifreq);
			break;
	default:
			break;
	}

	return ret;
}

static int gmac_change_mtu(struct net_device *netdev, int mtu)
{
	struct gmac_pdata *pdata = netdev_priv(netdev);
	int ret;

	if (netif_running(netdev)) {
		netdev_err(netdev, "must be stopped to change its MTU\n");
		return -EBUSY;
	}

	ret = gmac_calc_rx_buf_size(netdev, mtu);
	if (ret < 0)
		return ret;

	pdata->rx_buf_size = ret;
	netdev->mtu = mtu;

	gmac_restart_dev(pdata);

	return 0;
}

static int gmac_vlan_rx_add_vid(struct net_device *netdev,
				__be16 proto,
				u16 vid)
{
	struct gmac_pdata *pdata = netdev_priv(netdev);
	struct gmac_hw_ops *hw_ops = &pdata->hw_ops;

	if (pdata->hw_feat.vlhash) {
		set_bit(vid, pdata->active_vlans);
		hw_ops->update_vlan_hash_table(pdata);
	} else if (pdata->vlan_weight < 4) {
		set_bit(vid, pdata->active_vlans);
		pdata->vlan_weight =
			__bitmap_weight(pdata->active_vlans, VLAN_N_VID);
		hw_ops->update_vlan(pdata);
	} else {
		return -EINVAL;
	}

	return 0;
}

static int gmac_vlan_rx_kill_vid(struct net_device *netdev,
				 __be16 proto,
				 u16 vid)
{
	struct gmac_pdata *pdata = netdev_priv(netdev);
	struct gmac_hw_ops *hw_ops = &pdata->hw_ops;

	clear_bit(vid, pdata->active_vlans);

	if (pdata->hw_feat.vlhash)
		hw_ops->update_vlan_hash_table(pdata);
	else
		hw_ops->update_vlan(pdata);

	return 0;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void gmac_poll_controller(struct net_device *netdev)
{
	struct gmac_pdata *pdata = netdev_priv(netdev);
	struct gmac_channel *channel;
	unsigned int i;

	if (pdata->per_channel_irq) {
		channel = pdata->channel_head;
		for (i = 0; i < pdata->channel_count; i++, channel++)
			gmac_dma_isr(channel->dma_irq, channel);
	} else {
		disable_irq(pdata->dev_irq);
		gmac_isr(pdata->dev_irq, pdata);
		enable_irq(pdata->dev_irq);
	}
}
#endif /* CONFIG_NET_POLL_CONTROLLER */

static int gmac_set_features(struct net_device *netdev,
			     netdev_features_t features)
{
	netdev_features_t rxcsum, rxvlan, rxvlan_filter;
	struct gmac_pdata *pdata = netdev_priv(netdev);
	struct gmac_hw_ops *hw_ops = &pdata->hw_ops;

	rxcsum = pdata->netdev_features & NETIF_F_RXCSUM;
	rxvlan = pdata->netdev_features & NETIF_F_HW_VLAN_CTAG_RX;
	rxvlan_filter = pdata->netdev_features & NETIF_F_HW_VLAN_CTAG_FILTER;

	if ((features & NETIF_F_RXCSUM) && !rxcsum)
		hw_ops->enable_rx_csum(pdata);
	else if (!(features & NETIF_F_RXCSUM) && rxcsum)
		hw_ops->disable_rx_csum(pdata);

	if ((features & NETIF_F_HW_VLAN_CTAG_RX) && !rxvlan)
		hw_ops->enable_rx_vlan_stripping(pdata);
	else if (!(features & NETIF_F_HW_VLAN_CTAG_RX) && rxvlan)
		hw_ops->disable_rx_vlan_stripping(pdata);

	if ((features & NETIF_F_HW_VLAN_CTAG_FILTER) && !rxvlan_filter)
		hw_ops->enable_rx_vlan_filtering(pdata);
	else if (!(features & NETIF_F_HW_VLAN_CTAG_FILTER) && rxvlan_filter)
		hw_ops->disable_rx_vlan_filtering(pdata);

	pdata->netdev_features = features;

	return 0;
}

static void gmac_set_rx_mode(struct net_device *netdev)
{
	struct gmac_pdata *pdata = netdev_priv(netdev);
	struct gmac_hw_ops *hw_ops = &pdata->hw_ops;

	hw_ops->config_rx_mode(pdata);
}

static const struct net_device_ops gmac_netdev_ops = {
	.ndo_open		= gmac_open,
	.ndo_stop		= gmac_close,
	.ndo_start_xmit		= gmac_xmit,
	.ndo_tx_timeout		= gmac_tx_timeout,
	.ndo_get_stats64	= gmac_get_stats64,
	.ndo_change_mtu		= gmac_change_mtu,
	.ndo_set_mac_address	= gmac_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_do_ioctl		= gmac_ioctl,
	.ndo_vlan_rx_add_vid	= gmac_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid	= gmac_vlan_rx_kill_vid,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= gmac_poll_controller,
#endif
	.ndo_set_features	= gmac_set_features,
	.ndo_set_rx_mode	= gmac_set_rx_mode,
};

const struct net_device_ops *gmac_get_netdev_ops(void)
{
	return &gmac_netdev_ops;
}

static void gmac_rx_refresh(struct gmac_channel *channel)
{
	struct gmac_pdata *pdata = channel->pdata;
	struct gmac_ring *ring = channel->rx_ring;
	struct gmac_desc_data *desc_data;
	struct gmac_desc_ops *desc_ops;
	struct gmac_hw_ops *hw_ops;

	desc_ops = &pdata->desc_ops;
	hw_ops = &pdata->hw_ops;

	while (ring->dirty != ring->cur) {
		desc_data = GMAC_GET_DESC_DATA(ring, ring->dirty);

		/* Reset desc_data values */
		desc_ops->unmap_desc_data(pdata, desc_data, 0);

		if (desc_ops->map_rx_buffer(pdata, ring, desc_data))
			break;

		hw_ops->rx_desc_reset(pdata, desc_data, ring->dirty);

		ring->dirty++;
	}

	/* Make sure everything is written before the register write */
	wmb();

	/* Update the Rx Tail Pointer Register with address of
	 * the last cleaned entry
	 */
	desc_data = GMAC_GET_DESC_DATA(ring, ring->dirty - 1);
	GMAC_IOWRITE(pdata, DMA_CH_RDTR(channel->queue_index),
		     lower_32_bits(desc_data->dma_desc_addr));
}

static int gmac_tx_poll(struct gmac_channel *channel)
{
	struct gmac_pdata *pdata = channel->pdata;
	struct gmac_ring *ring = channel->tx_ring;
	struct net_device *netdev = pdata->netdev;
	unsigned int tx_packets = 0, tx_bytes = 0;
	struct gmac_desc_data *desc_data;
	struct gmac_dma_desc *dma_desc;
	struct gmac_desc_ops *desc_ops;
	struct gmac_hw_ops *hw_ops;
	struct netdev_queue *txq;
	int processed = 0;
	unsigned int cur;

	desc_ops = &pdata->desc_ops;
	hw_ops = &pdata->hw_ops;

	/* Nothing to do if there isn't a Tx ring for this channel */
	if (!ring)
		return 0;

	cur = ring->cur;

	/* Be sure we get ring->cur before accessing descriptor data */
	smp_rmb();

	txq = netdev_get_tx_queue(netdev, channel->queue_index);

	while ((processed < GMAC_TX_DESC_MAX_PROC) &&
	       (ring->dirty != cur)) {
		desc_data = GMAC_GET_DESC_DATA(ring, ring->dirty);
		dma_desc = desc_data->dma_desc;

		if (!hw_ops->tx_complete(dma_desc))
			break;

		/* Make sure descriptor fields are read after reading
		 * the OWN bit
		 */
		dma_rmb();

		if (netif_msg_tx_done(pdata))
			gmac_dump_tx_desc(pdata, ring, ring->dirty, 1, 0);

		if (hw_ops->is_last_desc(dma_desc) &&
		    !hw_ops->is_context_desc(dma_desc)) {
			tx_packets += desc_data->trx.packets;
			tx_bytes += desc_data->trx.bytes;
			hw_ops->get_tx_hwtstamp(pdata, dma_desc,
						desc_data->skb);
		}

		/* Free the SKB and reset the descriptor for re-use */
		desc_ops->unmap_desc_data(pdata, desc_data, 1);
		hw_ops->tx_desc_reset(desc_data);

		processed++;
		ring->dirty++;
	}

	if (!processed)
		return 0;

	netdev_tx_completed_queue(txq, tx_packets, tx_bytes);

	if (ring->tx.queue_stopped == 1 &&
	    gmac_tx_avail_desc(ring) > GMAC_TX_DESC_MIN_FREE) {
		ring->tx.queue_stopped = 0;
		netif_tx_wake_queue(txq);
	}

	netif_dbg(pdata, tx_done, pdata->netdev, "processed=%d\n", processed);

	return processed;
}

static int gmac_rx_poll(struct gmac_channel *channel, int budget)
{
	struct gmac_pdata *pdata = channel->pdata;
	struct gmac_ring *ring = channel->rx_ring;
	struct net_device *netdev = pdata->netdev;
	unsigned int frame_len, max_len;
	unsigned int context_next, context;
	struct gmac_desc_data *desc_data;
	struct gmac_pkt_info *pkt_info;
	unsigned int incomplete, error;
	struct gmac_hw_ops *hw_ops;
	unsigned int received = 0;
	struct napi_struct *napi;
	struct sk_buff *skb;
	struct skb_shared_hwtstamps *shhwtstamp = NULL;
	int packet_count = 0;

	hw_ops = &pdata->hw_ops;

	/* Nothing to do if there isn't a Rx ring for this channel */
	if (!ring)
		return 0;

	incomplete = 0;
	context_next = 0;

	napi = (pdata->per_channel_irq) ? &channel->napi : &pdata->napi;

	desc_data = GMAC_GET_DESC_DATA(ring, ring->cur);
	pkt_info = &ring->pkt_info;
	while (packet_count < budget) {
		memset(pkt_info, 0, sizeof(*pkt_info));
		skb = NULL;
		error = 0;

		desc_data = GMAC_GET_DESC_DATA(ring, ring->cur);

		if (gmac_rx_dirty_desc(ring) > GMAC_RX_DESC_MAX_DIRTY)
			gmac_rx_refresh(channel);

		if (hw_ops->dev_read(channel))
			break;

		received++;
		ring->cur++;

		incomplete = GMAC_GET_REG_BITS(pkt_info->attributes,
					       RX_PACKET_ATTRIBUTES_INCOMPLETE_POS,
					       RX_PACKET_ATTRIBUTES_INCOMPLETE_LEN);
		context = GMAC_GET_REG_BITS(pkt_info->attributes,
					    RX_PACKET_ATTRIBUTES_CONTEXT_POS,
					    RX_PACKET_ATTRIBUTES_CONTEXT_LEN);

		if (error || pkt_info->errors || incomplete) {
			if (pkt_info->errors)
				netif_err(pdata, rx_err, netdev,
					  "error in received packet\n");
			dev_kfree_skb(skb);
			goto next_packet;
		}

		if (!context) {
			frame_len = desc_data->trx.bytes;

			if (frame_len < GMAC_COPYBREAK_DEFAULT) {
				skb = netdev_alloc_skb_ip_align(netdev,
								frame_len);
				if (unlikely(!skb)) {
					if (net_ratelimit())
						dev_warn(pdata->dev,
							 "packet dropped\n");
					pdata->netdev->stats.rx_dropped++;
					break;
				}

				dma_sync_single_for_cpu(pdata->dev,
							desc_data->skb_dma,
							frame_len,
							DMA_FROM_DEVICE);
				skb_copy_to_linear_data(skb,
							desc_data->skb->data,
							frame_len);

				skb_put(skb, frame_len);
				dma_sync_single_for_device(pdata->dev,
							   desc_data->skb_dma,
							   frame_len,
							   DMA_FROM_DEVICE);
			} else {
				skb = desc_data->skb;
				desc_data->skb = NULL;
				dma_unmap_single(pdata->dev,
						 desc_data->skb_dma,
						 pdata->rx_buf_size,
						 DMA_FROM_DEVICE);
				desc_data->skb_dma = 0;

				skb_put(skb, frame_len);
			}
		}

		/* Be sure we don't exceed the configured MTU */
		max_len = netdev->mtu + ETH_HLEN;
		if (!(netdev->features & NETIF_F_HW_VLAN_CTAG_RX) &&
		    skb->protocol == htons(ETH_P_8021Q))
			max_len += VLAN_HLEN;

		if (skb->len > max_len) {
			netif_err(pdata, rx_err, netdev,
				  "packet length exceeds configured MTU\n");
			dev_kfree_skb(skb);
			goto next_packet;
		}

		if (netif_msg_pktdata(pdata))
			gmac_print_pkt(netdev, skb, false);

		skb_checksum_none_assert(skb);
		if (GMAC_GET_REG_BITS(pkt_info->attributes,
				      RX_PACKET_ATTRIBUTES_CSUM_DONE_POS,
				      RX_PACKET_ATTRIBUTES_CSUM_DONE_LEN))
			skb->ip_summed = CHECKSUM_UNNECESSARY;

		if (GMAC_GET_REG_BITS(pkt_info->attributes,
				      RX_PACKET_ATTRIBUTES_VLAN_CTAG_POS,
				      RX_PACKET_ATTRIBUTES_VLAN_CTAG_LEN)) {
			__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q),
					       pkt_info->vlan_ctag);
			pdata->stats.rx_vlan_packets++;
		}

		if (GMAC_GET_REG_BITS(pkt_info->attributes,
				      RX_PACKET_ATTRIBUTES_RX_TSTAMP_POS,
				      RX_PACKET_ATTRIBUTES_RX_TSTAMP_LEN)) {
			shhwtstamp = skb_hwtstamps(skb);
			memset(shhwtstamp, 0,
			       sizeof(struct skb_shared_hwtstamps));
			shhwtstamp->hwtstamp =
				ns_to_ktime(pkt_info->rx_tstamp);
			pdata->stats.rx_timestamp_packets++;
		}

		skb->dev = netdev;
		skb->protocol = eth_type_trans(skb, netdev);
		skb_record_rx_queue(skb, channel->queue_index);

		napi_gro_receive(napi, skb);

next_packet:
		packet_count++;
	}

	netif_dbg(pdata, rx_status, pdata->netdev,
		  "packet_count = %d\n", packet_count);

	return packet_count;
}

static int gmac_one_poll(struct napi_struct *napi, int budget)
{
	struct gmac_channel *channel = container_of(napi,
						    struct gmac_channel,
						    napi);
	struct gmac_pdata *pdata = channel->pdata;
	int processed = 0;

	netif_dbg(pdata, intr, pdata->netdev, "budget=%d\n", budget);

	/* Cleanup Tx ring first */
	gmac_tx_poll(channel);

	/* Process Rx ring next */
	processed = gmac_rx_poll(channel, budget);

	/* If we processed everything, we are done */
	if (processed < budget) {
		/* Turn off polling */
		napi_complete_done(napi, processed);

		/* Enable Tx and Rx interrupts */
		enable_irq(channel->dma_irq);
	}

	netif_dbg(pdata, intr, pdata->netdev, "received = %d\n", processed);

	return processed;
}

static int gmac_all_poll(struct napi_struct *napi, int budget)
{
	struct gmac_pdata *pdata = container_of(napi,
						struct gmac_pdata,
						napi);
	struct gmac_channel *channel;
	int processed, last_processed;
	int ring_budget;
	unsigned int i;

	netif_dbg(pdata, intr, pdata->netdev, "budget=%d\n", budget);

	processed = 0;
	ring_budget = budget / pdata->rx_ring_count;
	do {
		last_processed = processed;

		channel = pdata->channel_head;
		for (i = 0; i < pdata->channel_count; i++, channel++) {
			/* Cleanup Tx ring first */
			gmac_tx_poll(channel);

			/* Process Rx ring next */
			if (ring_budget > (budget - processed))
				ring_budget = budget - processed;
			processed += gmac_rx_poll(channel, ring_budget);
		}
	} while ((processed < budget) && (processed != last_processed));

	/* If we processed everything, we are done */
	if (processed < budget) {
		/* Turn off polling */
		napi_complete_done(napi, processed);

		/* Enable Tx and Rx interrupts */
		gmac_enable_rx_tx_ints(pdata);
	}

	netif_dbg(pdata, intr, pdata->netdev, "received = %d\n", processed);

	return processed;
}
