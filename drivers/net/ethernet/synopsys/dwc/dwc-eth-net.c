/*
 * Synopsys DesignWare Ethernet Driver
 *
 * Copyright (c) 2014-2016 Synopsys, Inc. (www.synopsys.com)
 *
 * This file is free software; you may copy, redistribute and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or (at
 * your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *     The Synopsys DWC ETHER XGMAC Software Driver and documentation
 *     (hereinafter "Software") is an unsupported proprietary work of Synopsys,
 *     Inc. unless otherwise expressly agreed to in writing between Synopsys
 *     and you.
 *
 *     The Software IS NOT an item of Licensed Software or Licensed Product
 *     under any End User Software License Agreement or Agreement for Licensed
 *     Product with Synopsys or any supplement thereto.  Permission is hereby
 *     granted, free of charge, to any person obtaining a copy of this software
 *     annotated with this license and the Software, to deal in the Software
 *     without restriction, including without limitation the rights to use,
 *     copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 *     of the Software, and to permit persons to whom the Software is furnished
 *     to do so, subject to the following conditions:
 *
 *     The above copyright notice and this permission notice shall be included
 *     in all copies or substantial portions of the Software.
 *
 *     THIS SOFTWARE IS BEING DISTRIBUTED BY SYNOPSYS SOLELY ON AN "AS IS"
 *     BASIS AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 *     TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *     PARTICULAR PURPOSE ARE HEREBY DISCLAIMED. IN NO EVENT SHALL SYNOPSYS
 *     BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *     CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *     SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *     INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *     CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *     ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 *     THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/netdevice.h>
#include <linux/tcp.h>

#include "dwc-eth.h"
#include "dwc-eth-regacc.h"

static int dwc_eth_one_poll(struct napi_struct *, int);
static int dwc_eth_all_poll(struct napi_struct *, int);

static inline unsigned int dwc_eth_tx_avail_desc(struct dwc_eth_ring *ring)
{
	return (ring->dma_desc_count - (ring->cur - ring->dirty));
}

static inline unsigned int dwc_eth_rx_dirty_desc(struct dwc_eth_ring *ring)
{
	return (ring->cur - ring->dirty);
}

static int dwc_eth_maybe_stop_tx_queue(
			struct dwc_eth_channel *channel,
			struct dwc_eth_ring *ring,
			unsigned int count)
{
	struct dwc_eth_pdata *pdata = channel->pdata;

	if (count > dwc_eth_tx_avail_desc(ring)) {
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

static void dwc_eth_prep_tx_tstamp(struct dwc_eth_pdata *pdata,
				   struct sk_buff *skb,
				   struct dwc_eth_pkt_info *pkt_info)
{
	unsigned long flags;

	if (DWC_ETH_GET_BITS(pkt_info->attributes, TX_PACKET_ATTRIBUTES, PTP)) {
		spin_lock_irqsave(&pdata->tstamp_lock, flags);
		if (pdata->tx_tstamp_skb) {
			/* Another timestamp in progress, ignore this one */
			DWC_ETH_SET_BITS(pkt_info->attributes,
					 TX_PACKET_ATTRIBUTES, PTP, 0);
		} else {
			pdata->tx_tstamp_skb = skb_get(skb);
			skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
		}
		spin_unlock_irqrestore(&pdata->tstamp_lock, flags);
	}

	if (!DWC_ETH_GET_BITS(pkt_info->attributes, TX_PACKET_ATTRIBUTES, PTP))
		skb_tx_timestamp(skb);
}

static void dwc_eth_prep_vlan(struct sk_buff *skb,
			      struct dwc_eth_pkt_info *pkt_info)
{
	if (skb_vlan_tag_present(skb))
		pkt_info->vlan_ctag = skb_vlan_tag_get(skb);
}

static int dwc_eth_prep_tso(struct sk_buff *skb,
			    struct dwc_eth_pkt_info *pkt_info)
{
	int ret;

	if (!DWC_ETH_GET_BITS(pkt_info->attributes,
			      TX_PACKET_ATTRIBUTES, TSO_ENABLE))
		return 0;

	ret = skb_cow_head(skb, 0);
	if (ret)
		return ret;

	pkt_info->header_len = skb_transport_offset(skb) + tcp_hdrlen(skb);
	pkt_info->tcp_header_len = tcp_hdrlen(skb);
	pkt_info->tcp_payload_len = skb->len - pkt_info->header_len;
	pkt_info->mss = skb_shinfo(skb)->gso_size;
	DBGPR("  pkt_info->header_len=%u\n", pkt_info->header_len);
	DBGPR("  pkt_info->tcp_header_len=%u, pkt_info->tcp_payload_len=%u\n",
	      pkt_info->tcp_header_len, pkt_info->tcp_payload_len);
	DBGPR("  pkt_info->mss=%u\n", pkt_info->mss);

	/* Update the number of packets that will ultimately be transmitted
	 * along with the extra bytes for each extra packet
	 */
	pkt_info->tx_packets = skb_shinfo(skb)->gso_segs;
	pkt_info->tx_bytes += (pkt_info->tx_packets - 1) * pkt_info->header_len;

	return 0;
}

static int dwc_eth_is_tso(struct sk_buff *skb)
{
	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return 0;

	if (!skb_is_gso(skb))
		return 0;

	DBGPR("  TSO packet to be processed\n");

	return 1;
}

static void dwc_eth_prep_tx_pkt(struct dwc_eth_pdata *pdata,
				struct dwc_eth_ring *ring,
				struct sk_buff *skb,
				struct dwc_eth_pkt_info *pkt_info)
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

	if (dwc_eth_is_tso(skb)) {
		/* TSO requires an extra descriptor if mss is different */
		if (skb_shinfo(skb)->gso_size != ring->tx.cur_mss) {
			context_desc = 1;
			pkt_info->desc_count++;
		}

		/* TSO requires an extra descriptor for TSO header */
		pkt_info->desc_count++;

		DWC_ETH_SET_BITS(pkt_info->attributes, TX_PACKET_ATTRIBUTES,
				 TSO_ENABLE, 1);
		DWC_ETH_SET_BITS(pkt_info->attributes, TX_PACKET_ATTRIBUTES,
				 CSUM_ENABLE, 1);
	} else if (skb->ip_summed == CHECKSUM_PARTIAL)
		DWC_ETH_SET_BITS(pkt_info->attributes, TX_PACKET_ATTRIBUTES,
				 CSUM_ENABLE, 1);

	if (skb_vlan_tag_present(skb)) {
		/* VLAN requires an extra descriptor if tag is different */
		if (skb_vlan_tag_get(skb) != ring->tx.cur_vlan_ctag)
			/* We can share with the TSO context descriptor */
			if (!context_desc) {
				context_desc = 1;
				pkt_info->desc_count++;
			}

		DWC_ETH_SET_BITS(pkt_info->attributes, TX_PACKET_ATTRIBUTES,
				 VLAN_CTAG, 1);
	}

	if ((skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP) &&
	    (pdata->tstamp_config.tx_type == HWTSTAMP_TX_ON))
		DWC_ETH_SET_BITS(pkt_info->attributes, TX_PACKET_ATTRIBUTES,
				 PTP, 1);

	for (len = skb_headlen(skb); len;) {
		pkt_info->desc_count++;
		len -= min_t(unsigned int, len, pdata->tx_max_buf_size);
	}

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		frag = &skb_shinfo(skb)->frags[i];
		for (len = skb_frag_size(frag); len; ) {
			pkt_info->desc_count++;
			len -= min_t(unsigned int, len, pdata->tx_max_buf_size);
		}
	}
}

static int dwc_eth_calc_rx_buf_size(struct net_device *netdev, unsigned int mtu)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	unsigned int rx_buf_size;

	if (mtu > DWC_ETH_JUMBO_PACKET_MTU) {
		netdev_alert(netdev, "MTU exceeds maximum supported value\n");
		return -EINVAL;
	}

	rx_buf_size = mtu + ETH_HLEN + ETH_FCS_LEN + VLAN_HLEN;
	rx_buf_size = clamp_val(rx_buf_size, pdata->rx_min_buf_size, PAGE_SIZE);

	rx_buf_size = (rx_buf_size + pdata->rx_buf_align - 1) &
		      ~(pdata->rx_buf_align - 1);

	return rx_buf_size;
}

static void dwc_eth_enable_rx_tx_ints(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;
	struct dwc_eth_channel *channel;
	enum dwc_eth_int int_id;
	unsigned int i;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (channel->tx_ring && channel->rx_ring)
			int_id = DWC_ETH_INT_DMA_CH_SR_TI_RI;
		else if (channel->tx_ring)
			int_id = DWC_ETH_INT_DMA_CH_SR_TI;
		else if (channel->rx_ring)
			int_id = DWC_ETH_INT_DMA_CH_SR_RI;
		else
			continue;

		hw_ops->enable_int(channel, int_id);
	}
}

static void dwc_eth_disable_rx_tx_ints(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;
	struct dwc_eth_channel *channel;
	enum dwc_eth_int int_id;
	unsigned int i;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (channel->tx_ring && channel->rx_ring)
			int_id = DWC_ETH_INT_DMA_CH_SR_TI_RI;
		else if (channel->tx_ring)
			int_id = DWC_ETH_INT_DMA_CH_SR_TI;
		else if (channel->rx_ring)
			int_id = DWC_ETH_INT_DMA_CH_SR_RI;
		else
			continue;

		hw_ops->disable_int(channel, int_id);
	}
}

static irqreturn_t dwc_eth_isr(int irq, void *data)
{
	struct dwc_eth_pdata *pdata = data;
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;
	struct dwc_eth_channel *channel;
	unsigned int dma_isr, dma_ch_isr;
	unsigned int mac_isr, mac_tssr;
	unsigned int i;

	/* The DMA interrupt status register also reports MAC and MTL
	 * interrupts. So for polling mode, we just need to check for
	 * this register to be non-zero
	 */
	dma_isr = DWC_ETH_IOREAD(pdata, DMA_ISR);
	if (!dma_isr)
		goto isr_done;

	netif_dbg(pdata, intr, pdata->netdev, "DMA_ISR=%#010x\n", dma_isr);

	for (i = 0; i < pdata->channel_count; i++) {
		if (!(dma_isr & (1 << i)))
			continue;

		channel = pdata->channel_head + i;

		dma_ch_isr = DWC_ETH_DMA_IOREAD(channel, DMA_CH_SR);
		netif_dbg(pdata, intr, pdata->netdev, "DMA_CH%u_ISR=%#010x\n",
			  i, dma_ch_isr);

		/* The TI or RI interrupt bits may still be set even if using
		 * per channel DMA interrupts. Check to be sure those are not
		 * enabled before using the private data napi structure.
		 */
		if (!pdata->per_channel_irq &&
		    (DWC_ETH_GET_BITS(dma_ch_isr, DMA_CH_SR, TI) ||
		     DWC_ETH_GET_BITS(dma_ch_isr, DMA_CH_SR, RI))) {
			if (napi_schedule_prep(&pdata->napi)) {
				/* Disable Tx and Rx interrupts */
				dwc_eth_disable_rx_tx_ints(pdata);

				/* Turn on polling */
				__napi_schedule_irqoff(&pdata->napi);
			}
		}

		if (DWC_ETH_GET_BITS(dma_ch_isr, DMA_CH_SR, RBU))
			pdata->stats.rx_buffer_unavailable++;

		/* Restart the device on a Fatal Bus Error */
		if (DWC_ETH_GET_BITS(dma_ch_isr, DMA_CH_SR, FBE))
			schedule_work(&pdata->restart_work);

		/* Clear all interrupt signals */
		DWC_ETH_DMA_IOWRITE(channel, DMA_CH_SR, dma_ch_isr);
	}

	if (DWC_ETH_GET_BITS(dma_isr, DMA_ISR, MACIS)) {
		mac_isr = DWC_ETH_IOREAD(pdata, MAC_ISR);

		if (DWC_ETH_GET_BITS(mac_isr, MAC_ISR, MMCTXIS))
			hw_ops->tx_mmc_int(pdata);

		if (DWC_ETH_GET_BITS(mac_isr, MAC_ISR, MMCRXIS))
			hw_ops->rx_mmc_int(pdata);

		if (DWC_ETH_GET_BITS(mac_isr, MAC_ISR, TSIS)) {
			mac_tssr = DWC_ETH_IOREAD(pdata, MAC_TSSR);

			if (DWC_ETH_GET_BITS(mac_tssr, MAC_TSSR, TXTSC)) {
				/* Read Tx Timestamp to clear interrupt */
				pdata->tx_tstamp =
					hw_ops->get_tx_tstamp(pdata);
				queue_work(pdata->dev_workqueue,
					   &pdata->tx_tstamp_work);
			}
		}
	}

isr_done:
	return IRQ_HANDLED;
}

static irqreturn_t dwc_eth_dma_isr(int irq, void *data)
{
	struct dwc_eth_channel *channel = data;

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

static void dwc_eth_tx_timer(unsigned long data)
{
	struct dwc_eth_channel *channel = (struct dwc_eth_channel *)data;
	struct dwc_eth_pdata *pdata = channel->pdata;
	struct napi_struct *napi;

	TRACE("-->");

	napi = (pdata->per_channel_irq) ? &channel->napi : &pdata->napi;

	if (napi_schedule_prep(napi)) {
		/* Disable Tx and Rx interrupts */
		if (pdata->per_channel_irq)
			disable_irq_nosync(channel->dma_irq);
		else
			dwc_eth_disable_rx_tx_ints(pdata);

		/* Turn on polling */
		__napi_schedule(napi);
	}

	channel->tx_timer_active = 0;

	TRACE("<--");
}

static void dwc_eth_init_timers(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			break;

		setup_timer(&channel->tx_timer, dwc_eth_tx_timer,
			    (unsigned long)channel);
	}
}

static void dwc_eth_stop_timers(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			break;

		del_timer_sync(&channel->tx_timer);
	}
}

static void dwc_eth_napi_enable(struct dwc_eth_pdata *pdata, unsigned int add)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	if (pdata->per_channel_irq) {
		channel = pdata->channel_head;
		for (i = 0; i < pdata->channel_count; i++, channel++) {
			if (add)
				netif_napi_add(pdata->netdev, &channel->napi,
					       dwc_eth_one_poll,
					       NAPI_POLL_WEIGHT);

			napi_enable(&channel->napi);
		}
	} else {
		if (add)
			netif_napi_add(pdata->netdev, &pdata->napi,
				       dwc_eth_all_poll, NAPI_POLL_WEIGHT);

		napi_enable(&pdata->napi);
	}
}

static void dwc_eth_napi_disable(struct dwc_eth_pdata *pdata, unsigned int del)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	if (pdata->per_channel_irq) {
		channel = pdata->channel_head;
		for (i = 0; i < pdata->channel_count; i++, channel++) {
			napi_disable(&channel->napi);

			if (del)
				netif_napi_del(&channel->napi);
		}
	} else {
		napi_disable(&pdata->napi);

		if (del)
			netif_napi_del(&pdata->napi);
	}
}

static int dwc_eth_request_irqs(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	struct net_device *netdev = pdata->netdev;
	unsigned int i;
	int ret;

	ret = devm_request_irq(pdata->dev, pdata->dev_irq, dwc_eth_isr,
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
				       dwc_eth_dma_isr, 0,
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

static void dwc_eth_free_irqs(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	devm_free_irq(pdata->dev, pdata->dev_irq, pdata);

	if (!pdata->per_channel_irq)
		return;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++)
		devm_free_irq(pdata->dev, channel->dma_irq, channel);
}

static void dwc_eth_free_tx_data(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_desc_ops *desc_ops = &pdata->desc_ops;
	struct dwc_eth_channel *channel;
	struct dwc_eth_ring *ring;
	struct dwc_eth_desc_data *desc_data;
	unsigned int i, j;

	TRACE("-->");

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		ring = channel->tx_ring;
		if (!ring)
			break;

		for (j = 0; j < ring->dma_desc_count; j++) {
			desc_data = DWC_ETH_GET_DESC_DATA(ring, j);
			desc_ops->unmap_desc_data(pdata, desc_data);
		}
	}

	TRACE("<--");
}

static void dwc_eth_free_rx_data(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_desc_ops *desc_ops = &pdata->desc_ops;
	struct dwc_eth_channel *channel;
	struct dwc_eth_ring *ring;
	struct dwc_eth_desc_data *desc_data;
	unsigned int i, j;

	TRACE("-->");

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		ring = channel->rx_ring;
		if (!ring)
			break;

		for (j = 0; j < ring->dma_desc_count; j++) {
			desc_data = DWC_ETH_GET_DESC_DATA(ring, j);
			desc_ops->unmap_desc_data(pdata, desc_data);
		}
	}

	TRACE("<--");
}

static void dwc_eth_adjust_link(struct net_device *netdev)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;
	struct phy_device *phydev = pdata->phydev;
	int new_state = 0;

	if (!phydev)
		return;

	if (phydev->link) {
		/* Flow control support */
		if (pdata->pause_autoneg) {
			if (phydev->pause || phydev->asym_pause) {
				pdata->tx_pause = 1;
				pdata->rx_pause = 1;
			} else {
				pdata->tx_pause = 0;
				pdata->rx_pause = 0;
			}
		}

		if (pdata->tx_pause != pdata->phy_tx_pause) {
			hw_ops->config_tx_flow_control(pdata);
			pdata->phy_tx_pause = pdata->tx_pause;
		}

		if (pdata->rx_pause != pdata->phy_rx_pause) {
			hw_ops->config_rx_flow_control(pdata);
			pdata->phy_rx_pause = pdata->rx_pause;
		}

		/* Speed support */
		if (phydev->speed != pdata->phy_speed) {
			new_state = 1;

			switch (phydev->speed) {
			case SPEED_100000:
				hw_ops->set_xlgmii_100000_speed(pdata);
				break;

			case SPEED_50000:
				hw_ops->set_xlgmii_50000_speed(pdata);
				break;

			case SPEED_40000:
				hw_ops->set_xlgmii_40000_speed(pdata);
				break;

			case SPEED_25000:
				hw_ops->set_xlgmii_25000_speed(pdata);
				break;

			case SPEED_10000:
				hw_ops->set_xgmii_10000_speed(pdata);
				break;

			case SPEED_2500:
				hw_ops->set_gmii_2500_speed(pdata);
				break;

			case SPEED_1000:
				hw_ops->set_gmii_1000_speed(pdata);
				break;
			}
			pdata->phy_speed = phydev->speed;
		}

		if (phydev->link != pdata->phy_link) {
			new_state = 1;
			pdata->phy_link = 1;
		}
	} else if (pdata->phy_link) {
		new_state = 1;
		pdata->phy_link = 0;
		pdata->phy_speed = SPEED_UNKNOWN;
	}

	if (new_state && netif_msg_link(pdata))
		phy_print_status(phydev);
}

static int dwc_eth_phy_init(struct dwc_eth_pdata *pdata)
{
	struct net_device *netdev = pdata->netdev;
	struct phy_device *phydev = pdata->phydev;
	int ret;

	if (!pdata->phydev)
		return -ENODEV;

	pdata->phy_link = -1;
	pdata->phy_speed = SPEED_UNKNOWN;
	pdata->phy_tx_pause = pdata->tx_pause;
	pdata->phy_rx_pause = pdata->rx_pause;

	ret = phy_connect_direct(netdev, phydev, &dwc_eth_adjust_link,
				 pdata->phy_mode);
	if (ret) {
		netdev_err(netdev, "phy_connect_direct failed\n");
		return ret;
	}

	if (!phydev->drv || (phydev->drv->phy_id == 0)) {
		netdev_err(netdev, "phy_id not valid\n");
		ret = -ENODEV;
		goto err_phy_connect;
	}
	netif_dbg(pdata, ifup, pdata->netdev,
		  "phy_connect_direct succeeded for PHY %s\n",
		  dev_name(&phydev->mdio.dev));

	return 0;

err_phy_connect:
	phy_disconnect(phydev);

	return ret;
}

static void dwc_eth_phy_exit(struct dwc_eth_pdata *pdata)
{
	if (!pdata->phydev)
		return;

	phy_disconnect(pdata->phydev);
}

static int dwc_eth_start(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;
	struct net_device *netdev = pdata->netdev;
	int ret;

	TRACE("-->");

	hw_ops->init(pdata);

	if (pdata->phydev)
		phy_start(pdata->phydev);

	dwc_eth_napi_enable(pdata, 1);

	ret = dwc_eth_request_irqs(pdata);
	if (ret)
		goto err_napi;

	hw_ops->enable_tx(pdata);
	hw_ops->enable_rx(pdata);

	netif_tx_start_all_queues(netdev);

	TRACE("<--");

	return 0;

err_napi:
	dwc_eth_napi_disable(pdata, 1);

	if (pdata->phydev)
		phy_stop(pdata->phydev);

	hw_ops->exit(pdata);

	return ret;
}

static void dwc_eth_stop(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;
	struct dwc_eth_channel *channel;
	struct net_device *netdev = pdata->netdev;
	struct netdev_queue *txq;
	unsigned int i;

	TRACE("-->");

	netif_tx_stop_all_queues(netdev);

	dwc_eth_stop_timers(pdata);
	flush_workqueue(pdata->dev_workqueue);

	hw_ops->disable_tx(pdata);
	hw_ops->disable_rx(pdata);

	dwc_eth_free_irqs(pdata);

	dwc_eth_napi_disable(pdata, 1);

	if (pdata->phydev)
		phy_stop(pdata->phydev);

	hw_ops->exit(pdata);

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			continue;

		txq = netdev_get_tx_queue(netdev, channel->queue_index);
		netdev_tx_reset_queue(txq);
	}

	TRACE("<--");
}

static void dwc_eth_restart_dev(struct dwc_eth_pdata *pdata)
{
	TRACE("-->");

	/* If not running, "restart" will happen on open */
	if (!netif_running(pdata->netdev))
		return;

	dwc_eth_stop(pdata);

	dwc_eth_free_tx_data(pdata);
	dwc_eth_free_rx_data(pdata);

	dwc_eth_start(pdata);

	TRACE("<--");
}

static void dwc_eth_restart(struct work_struct *work)
{
	struct dwc_eth_pdata *pdata = container_of(work,
						   struct dwc_eth_pdata,
						   restart_work);

	rtnl_lock();

	dwc_eth_restart_dev(pdata);

	rtnl_unlock();
}

static void dwc_eth_tx_tstamp(struct work_struct *work)
{
	struct dwc_eth_pdata *pdata = container_of(work,
						   struct dwc_eth_pdata,
						   tx_tstamp_work);
	struct skb_shared_hwtstamps hwtstamps;
	u64 nsec;
	unsigned long flags;

	if (pdata->tx_tstamp) {
		nsec = timecounter_cyc2time(&pdata->tstamp_tc,
					    pdata->tx_tstamp);

		memset(&hwtstamps, 0, sizeof(hwtstamps));
		hwtstamps.hwtstamp = ns_to_ktime(nsec);
		skb_tstamp_tx(pdata->tx_tstamp_skb, &hwtstamps);
	}

	dev_kfree_skb_any(pdata->tx_tstamp_skb);

	spin_lock_irqsave(&pdata->tstamp_lock, flags);
	pdata->tx_tstamp_skb = NULL;
	spin_unlock_irqrestore(&pdata->tstamp_lock, flags);
}

static int dwc_eth_get_hwtstamp_settings(struct dwc_eth_pdata *pdata,
					 struct ifreq *ifreq)
{
	if (copy_to_user(ifreq->ifr_data, &pdata->tstamp_config,
			 sizeof(pdata->tstamp_config)))
		return -EFAULT;

	return 0;
}

static int dwc_eth_set_hwtstamp_settings(struct dwc_eth_pdata *pdata,
					 struct ifreq *ifreq)
{
	struct hwtstamp_config config;
	unsigned int mac_tscr;

	if (copy_from_user(&config, ifreq->ifr_data, sizeof(config)))
		return -EFAULT;

	if (config.flags)
		return -EINVAL;

	mac_tscr = 0;

	switch (config.tx_type) {
	case HWTSTAMP_TX_OFF:
		break;

	case HWTSTAMP_TX_ON:
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSENA, 1);
		break;

	default:
		return -ERANGE;
	}

	switch (config.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		break;

	case HWTSTAMP_FILTER_ALL:
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSENALL, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSENA, 1);
		break;

	/* PTP v2, UDP, any kind of event packet */
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSVER2ENA, 1);
	/* PTP v1, UDP, any kind of event packet */
	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSIPV4ENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSIPV6ENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, SNAPTYPSEL, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSENA, 1);
		break;

	/* PTP v2, UDP, Sync packet */
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSVER2ENA, 1);
	/* PTP v1, UDP, Sync packet */
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSIPV4ENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSIPV6ENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSEVNTENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSENA, 1);
		break;

	/* PTP v2, UDP, Delay_req packet */
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSVER2ENA, 1);
	/* PTP v1, UDP, Delay_req packet */
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSIPV4ENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSIPV6ENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSEVNTENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSMSTRENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSENA, 1);
		break;

	/* 802.AS1, Ethernet, any kind of event packet */
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, AV8021ASMEN, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, SNAPTYPSEL, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSENA, 1);
		break;

	/* 802.AS1, Ethernet, Sync packet */
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, AV8021ASMEN, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSEVNTENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSENA, 1);
		break;

	/* 802.AS1, Ethernet, Delay_req packet */
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, AV8021ASMEN, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSMSTRENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSEVNTENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSENA, 1);
		break;

	/* PTP v2/802.AS1, any layer, any kind of event packet */
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSVER2ENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSIPENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSIPV4ENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSIPV6ENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, SNAPTYPSEL, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSENA, 1);
		break;

	/* PTP v2/802.AS1, any layer, Sync packet */
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSVER2ENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSIPENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSIPV4ENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSIPV6ENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSEVNTENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSENA, 1);
		break;

	/* PTP v2/802.AS1, any layer, Delay_req packet */
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSVER2ENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSIPENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSIPV4ENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSIPV6ENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSMSTRENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSEVNTENA, 1);
		DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSENA, 1);
		break;

	default:
		return -ERANGE;
	}

	pdata->hw_ops.config_tstamp(pdata, mac_tscr);

	memcpy(&pdata->tstamp_config, &config, sizeof(config));

	return 0;
}

static int dwc_eth_open(struct net_device *netdev)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	struct dwc_eth_desc_ops *desc_ops = &pdata->desc_ops;
	int ret;

	TRACE("-->");

	/* Initialize the phy */
	if (pdata->mdio_en) {
		ret = dwc_eth_phy_init(pdata);
		if (ret)
			return ret;
	}

	/* Calculate the Rx buffer size before allocating rings */
	ret = dwc_eth_calc_rx_buf_size(netdev, netdev->mtu);
	if (ret < 0)
		goto err_phy_init;
	pdata->rx_buf_size = ret;

	/* Allocate the channels and rings */
	ret = desc_ops->alloc_channles_and_rings(pdata);
	if (ret)
		goto err_phy_init;

	INIT_WORK(&pdata->restart_work, dwc_eth_restart);
	INIT_WORK(&pdata->tx_tstamp_work, dwc_eth_tx_tstamp);
	dwc_eth_init_timers(pdata);

	ret = dwc_eth_start(pdata);
	if (ret)
		goto err_channels_and_rings;

	TRACE("<--");

	return 0;

err_channels_and_rings:
	desc_ops->free_channels_and_rings(pdata);

err_phy_init:
	dwc_eth_phy_exit(pdata);

	return ret;
}

static int dwc_eth_close(struct net_device *netdev)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	struct dwc_eth_desc_ops *desc_ops = &pdata->desc_ops;

	TRACE("-->");

	/* Stop the device */
	dwc_eth_stop(pdata);

	/* Free the channels and rings */
	desc_ops->free_channels_and_rings(pdata);

	/* Release the phy */
	dwc_eth_phy_exit(pdata);

	TRACE("<--");

	return 0;
}

static void dwc_eth_tx_timeout(struct net_device *netdev)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);

	netdev_warn(netdev, "tx timeout, device restarting\n");
	schedule_work(&pdata->restart_work);
}

static int dwc_eth_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	struct dwc_eth_desc_ops *desc_ops = &pdata->desc_ops;
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;
	struct dwc_eth_channel *channel;
	struct dwc_eth_ring *ring;
	struct dwc_eth_pkt_info *tx_pkt_info;
	struct netdev_queue *txq;
	int ret;

	TRACE("-->");
	DBGPR("  skb->len = %d\n", skb->len);

	channel = pdata->channel_head + skb->queue_mapping;
	txq = netdev_get_tx_queue(netdev, channel->queue_index);
	ring = channel->tx_ring;
	tx_pkt_info = &ring->pkt_info;

	ret = NETDEV_TX_OK;

	if (skb->len == 0) {
		netif_err(pdata, tx_err, netdev,
			  "empty skb received from stack\n");
		dev_kfree_skb_any(skb);
		goto tx_return;
	}

	/* Prepare preliminary packet info for TX */
	memset(tx_pkt_info, 0, sizeof(*tx_pkt_info));
	dwc_eth_prep_tx_pkt(pdata, ring, skb, tx_pkt_info);

	/* Check that there are enough descriptors available */
	ret = dwc_eth_maybe_stop_tx_queue(channel, ring,
					  tx_pkt_info->desc_count);
	if (ret)
		goto tx_return;

	ret = dwc_eth_prep_tso(skb, tx_pkt_info);
	if (ret) {
		netif_err(pdata, tx_err, netdev,
			  "error processing TSO packet\n");
		dev_kfree_skb_any(skb);
		goto tx_return;
	}
	dwc_eth_prep_vlan(skb, tx_pkt_info);

	if (!desc_ops->map_tx_skb(channel, skb)) {
		dev_kfree_skb_any(skb);
		goto tx_return;
	}

	dwc_eth_prep_tx_tstamp(pdata, skb, tx_pkt_info);

	/* Report on the actual number of bytes (to be) sent */
	netdev_tx_sent_queue(txq, tx_pkt_info->tx_bytes);

	/* Configure required descriptor fields for transmission */
	hw_ops->dev_xmit(channel);

	if (netif_msg_pktdata(pdata))
		dwc_eth_print_pkt(netdev, skb, true);

	/* Stop the queue in advance if there may not be enough descriptors */
	dwc_eth_maybe_stop_tx_queue(channel, ring, pdata->tx_max_desc_nr);

	ret = NETDEV_TX_OK;

tx_return:
	return ret;
}

static struct rtnl_link_stats64 *dwc_eth_get_stats64(
				struct net_device *netdev,
				struct rtnl_link_stats64 *s)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	struct dwc_eth_stats *pstats = &pdata->stats;

	TRACE("-->");

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

	TRACE("<--");

	return s;
}

static int dwc_eth_set_mac_address(struct net_device *netdev, void *addr)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;
	struct sockaddr *saddr = addr;

	TRACE("-->");

	if (!is_valid_ether_addr(saddr->sa_data))
		return -EADDRNOTAVAIL;

	memcpy(netdev->dev_addr, saddr->sa_data, netdev->addr_len);

	hw_ops->set_mac_address(pdata, netdev->dev_addr);

	TRACE("<--");

	return 0;
}

static int dwc_eth_ioctl(struct net_device *netdev,
			 struct ifreq *ifreq, int cmd)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	int ret;

	TRACE("-->");

	if (!netif_running(netdev))
		return -ENODEV;

	switch (cmd) {
	case SIOCGHWTSTAMP:
		ret = dwc_eth_get_hwtstamp_settings(pdata, ifreq);
		break;

	case SIOCSHWTSTAMP:
		ret = dwc_eth_set_hwtstamp_settings(pdata, ifreq);
		break;

	default:
		ret = -EOPNOTSUPP;
	}

	TRACE("<--");

	return ret;
}

static int dwc_eth_change_mtu(struct net_device *netdev, int mtu)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	int ret;

	TRACE("-->");

	ret = dwc_eth_calc_rx_buf_size(netdev, mtu);
	if (ret < 0)
		return ret;

	pdata->rx_buf_size = ret;
	netdev->mtu = mtu;

	dwc_eth_restart_dev(pdata);

	TRACE("<--");

	return 0;
}

static int dwc_eth_vlan_rx_add_vid(struct net_device *netdev,
				   __be16 proto,
				   u16 vid)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;

	TRACE("-->");

	set_bit(vid, pdata->active_vlans);
	hw_ops->update_vlan_hash_table(pdata);

	TRACE("<--");

	return 0;
}

static int dwc_eth_vlan_rx_kill_vid(struct net_device *netdev,
				    __be16 proto,
				    u16 vid)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;

	TRACE("-->");

	clear_bit(vid, pdata->active_vlans);
	hw_ops->update_vlan_hash_table(pdata);

	TRACE("<--");

	return 0;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void dwc_eth_poll_controller(struct net_device *netdev)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	struct dwc_eth_channel *channel;
	unsigned int i;

	TRACE("-->");

	if (pdata->per_channel_irq) {
		channel = pdata->channel_head;
		for (i = 0; i < pdata->channel_count; i++, channel++)
			dwc_eth_dma_isr(channel->dma_irq, channel);
	} else {
		disable_irq(pdata->dev_irq);
		dwc_eth_isr(pdata->dev_irq, pdata);
		enable_irq(pdata->dev_irq);
	}

	TRACE("<--");
}
#endif /* CONFIG_NET_POLL_CONTROLLER */

static int dwc_eth_setup_tc(struct net_device *netdev,
			    u32 handle, __be16 proto,
			    struct tc_to_netdev *tc_to_netdev)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	u8 tc;

	if (tc_to_netdev->type != TC_SETUP_MQPRIO)
		return -EINVAL;

	tc = tc_to_netdev->tc;

	if (tc > pdata->hw_feat.tc_cnt)
		return -EINVAL;

	pdata->num_tcs = tc;
	pdata->hw_ops.config_tc(pdata);

	return 0;
}

static int dwc_eth_set_features(struct net_device *netdev,
				netdev_features_t features)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;
	netdev_features_t rxhash, rxcsum, rxvlan, rxvlan_filter;
	int ret = 0;

	TRACE("-->");

	rxhash = pdata->netdev_features & NETIF_F_RXHASH;
	rxcsum = pdata->netdev_features & NETIF_F_RXCSUM;
	rxvlan = pdata->netdev_features & NETIF_F_HW_VLAN_CTAG_RX;
	rxvlan_filter = pdata->netdev_features & NETIF_F_HW_VLAN_CTAG_FILTER;

	if ((features & NETIF_F_RXHASH) && !rxhash)
		ret = hw_ops->enable_rss(pdata);
	else if (!(features & NETIF_F_RXHASH) && rxhash)
		ret = hw_ops->disable_rss(pdata);
	if (ret)
		return ret;

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

	TRACE("<--");

	return 0;
}

static void dwc_eth_set_rx_mode(struct net_device *netdev)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;

	TRACE("-->");

	hw_ops->config_rx_mode(pdata);

	TRACE("<--");
}

static const struct net_device_ops dwc_eth_netdev_ops = {
	.ndo_open		= dwc_eth_open,
	.ndo_stop		= dwc_eth_close,
	.ndo_start_xmit		= dwc_eth_xmit,
	.ndo_tx_timeout		= dwc_eth_tx_timeout,
	.ndo_get_stats64	= dwc_eth_get_stats64,
	.ndo_change_mtu		= dwc_eth_change_mtu,
	.ndo_set_mac_address	= dwc_eth_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_do_ioctl		= dwc_eth_ioctl,
	.ndo_vlan_rx_add_vid	= dwc_eth_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid	= dwc_eth_vlan_rx_kill_vid,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= dwc_eth_poll_controller,
#endif
	.ndo_setup_tc		= dwc_eth_setup_tc,
	.ndo_set_features	= dwc_eth_set_features,
	.ndo_set_rx_mode	= dwc_eth_set_rx_mode,
};

const struct net_device_ops *dwc_eth_get_netdev_ops(void)
{
	return &dwc_eth_netdev_ops;
}

static void dwc_eth_rx_refresh(struct dwc_eth_channel *channel)
{
	struct dwc_eth_pdata *pdata = channel->pdata;
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;
	struct dwc_eth_desc_ops *desc_ops = &pdata->desc_ops;
	struct dwc_eth_ring *ring = channel->rx_ring;
	struct dwc_eth_desc_data *desc_data;

	while (ring->dirty != ring->cur) {
		desc_data = DWC_ETH_GET_DESC_DATA(ring, ring->dirty);

		/* Reset desc_data values */
		desc_ops->unmap_desc_data(pdata, desc_data);

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
	desc_data = DWC_ETH_GET_DESC_DATA(ring, ring->dirty - 1);
	DWC_ETH_DMA_IOWRITE(channel, DMA_CH_RDTR_LO,
			    lower_32_bits(desc_data->dma_desc_addr));
}

static struct sk_buff *dwc_eth_create_skb(struct dwc_eth_pdata *pdata,
					  struct napi_struct *napi,
					  struct dwc_eth_desc_data *desc_data,
					  unsigned int len)
{
	struct sk_buff *skb;
	u8 *packet;
	unsigned int copy_len;

	skb = napi_alloc_skb(napi, desc_data->rx.hdr.dma_len);
	if (!skb)
		return NULL;

	/* Start with the header buffer which may contain just the header
	 * or the header plus data
	 */
	dma_sync_single_range_for_cpu(pdata->dev, desc_data->rx.hdr.dma_base,
				      desc_data->rx.hdr.dma_off,
				      desc_data->rx.hdr.dma_len,
				      DMA_FROM_DEVICE);

	packet = page_address(desc_data->rx.hdr.pa.pages) +
		 desc_data->rx.hdr.pa.pages_offset;
	copy_len = (desc_data->rx.hdr_len) ? desc_data->rx.hdr_len : len;
	copy_len = min(desc_data->rx.hdr.dma_len, copy_len);
	skb_copy_to_linear_data(skb, packet, copy_len);
	skb_put(skb, copy_len);

	len -= copy_len;
	if (len) {
		/* Add the remaining data as a frag */
		dma_sync_single_range_for_cpu(pdata->dev,
					      desc_data->rx.buf.dma_base,
					      desc_data->rx.buf.dma_off,
					      desc_data->rx.buf.dma_len,
					      DMA_FROM_DEVICE);

		skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags,
				desc_data->rx.buf.pa.pages,
				desc_data->rx.buf.pa.pages_offset,
				len, desc_data->rx.buf.dma_len);
		desc_data->rx.buf.pa.pages = NULL;
	}

	return skb;
}

static int dwc_eth_tx_poll(struct dwc_eth_channel *channel)
{
	struct dwc_eth_pdata *pdata = channel->pdata;
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;
	struct dwc_eth_desc_ops *desc_ops = &pdata->desc_ops;
	struct dwc_eth_ring *ring = channel->tx_ring;
	struct dwc_eth_desc_data *desc_data;
	struct dwc_eth_dma_desc *dma_desc;
	struct net_device *netdev = pdata->netdev;
	struct netdev_queue *txq;
	int processed = 0;
	unsigned int tx_packets = 0, tx_bytes = 0;
	unsigned int cur;

	TRACE("-->");

	/* Nothing to do if there isn't a Tx ring for this channel */
	if (!ring)
		return 0;

	cur = ring->cur;

	/* Be sure we get ring->cur before accessing descriptor data */
	smp_rmb();

	txq = netdev_get_tx_queue(netdev, channel->queue_index);

	while ((processed < pdata->tx_desc_max_proc) &&
	       (ring->dirty != cur)) {
		desc_data = DWC_ETH_GET_DESC_DATA(ring, ring->dirty);
		dma_desc = desc_data->dma_desc;

		if (!hw_ops->tx_complete(dma_desc))
			break;

		/* Make sure descriptor fields are read after reading
		 * the OWN bit
		 */
		dma_rmb();

		if (netif_msg_tx_done(pdata))
			dwc_eth_dump_tx_desc(pdata, ring, ring->dirty, 1, 0);

		if (hw_ops->is_last_desc(dma_desc)) {
			tx_packets += desc_data->tx.packets;
			tx_bytes += desc_data->tx.bytes;
		}

		/* Free the SKB and reset the descriptor for re-use */
		desc_ops->unmap_desc_data(pdata, desc_data);
		hw_ops->tx_desc_reset(desc_data);

		processed++;
		ring->dirty++;
	}

	if (!processed)
		return 0;

	netdev_tx_completed_queue(txq, tx_packets, tx_bytes);

	if ((ring->tx.queue_stopped == 1) &&
	    (dwc_eth_tx_avail_desc(ring) > pdata->tx_desc_min_free)) {
		ring->tx.queue_stopped = 0;
		netif_tx_wake_queue(txq);
	}

	DBGPR("  processed=%d\n", processed);
	TRACE("<--");

	return processed;
}

static int dwc_eth_rx_poll(struct dwc_eth_channel *channel, int budget)
{
	struct dwc_eth_pdata *pdata = channel->pdata;
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;
	struct dwc_eth_ring *ring = channel->rx_ring;
	struct dwc_eth_desc_data *desc_data;
	struct dwc_eth_pkt_info *pkt_info;
	struct net_device *netdev = pdata->netdev;
	struct napi_struct *napi;
	struct sk_buff *skb;
	struct skb_shared_hwtstamps *hwtstamps;
	unsigned int incomplete, error, context_next, context;
	unsigned int len, dma_desc_len, max_len;
	unsigned int received = 0;
	int packet_count = 0;

	TRACE("-->");
	DBGPR("  budget=%d\n", budget);

	/* Nothing to do if there isn't a Rx ring for this channel */
	if (!ring)
		return 0;

	incomplete = 0;
	context_next = 0;

	napi = (pdata->per_channel_irq) ? &channel->napi : &pdata->napi;

	desc_data = DWC_ETH_GET_DESC_DATA(ring, ring->cur);
	pkt_info = &ring->pkt_info;
	while (packet_count < budget) {
		DBGPR("  cur = %d\n", ring->cur);

		/* First time in loop see if we need to restore state */
		if (!received && desc_data->state_saved) {
			skb = desc_data->state.skb;
			error = desc_data->state.error;
			len = desc_data->state.len;
		} else {
			memset(pkt_info, 0, sizeof(*pkt_info));
			skb = NULL;
			error = 0;
			len = 0;
		}

read_again:
		desc_data = DWC_ETH_GET_DESC_DATA(ring, ring->cur);

		if (dwc_eth_rx_dirty_desc(ring) > pdata->rx_desc_max_dirty)
			dwc_eth_rx_refresh(channel);

		if (hw_ops->dev_read(channel))
			break;

		received++;
		ring->cur++;

		incomplete = DWC_ETH_GET_BITS(pkt_info->attributes,
					      RX_PACKET_ATTRIBUTES,
					      INCOMPLETE);
		context_next = DWC_ETH_GET_BITS(pkt_info->attributes,
						RX_PACKET_ATTRIBUTES,
						CONTEXT_NEXT);
		context = DWC_ETH_GET_BITS(pkt_info->attributes,
					   RX_PACKET_ATTRIBUTES,
					   CONTEXT);

		/* Earlier error, just drain the remaining data */
		if ((incomplete || context_next) && error)
			goto read_again;

		if (error || pkt_info->errors) {
			if (pkt_info->errors)
				netif_err(pdata, rx_err, netdev,
					  "error in received packet\n");
			dev_kfree_skb(skb);
			goto next_packet;
		}

		if (!context) {
			/* Length is cumulative, get this descriptor's length */
			dma_desc_len = desc_data->rx.len - len;
			len += dma_desc_len;

			if (dma_desc_len && !skb) {
				skb = dwc_eth_create_skb(pdata, napi, desc_data,
							 dma_desc_len);
				if (!skb)
					error = 1;
			} else if (dma_desc_len) {
				dma_sync_single_range_for_cpu(
						pdata->dev,
						desc_data->rx.buf.dma_base,
						desc_data->rx.buf.dma_off,
						desc_data->rx.buf.dma_len,
						DMA_FROM_DEVICE);

				skb_add_rx_frag(
					skb, skb_shinfo(skb)->nr_frags,
					desc_data->rx.buf.pa.pages,
					desc_data->rx.buf.pa.pages_offset,
					dma_desc_len,
					desc_data->rx.buf.dma_len);
				desc_data->rx.buf.pa.pages = NULL;
			}
		}

		if (incomplete || context_next)
			goto read_again;

		if (!skb)
			goto next_packet;

		/* Be sure we don't exceed the configured MTU */
		max_len = netdev->mtu + ETH_HLEN;
		if (!(netdev->features & NETIF_F_HW_VLAN_CTAG_RX) &&
		    (skb->protocol == htons(ETH_P_8021Q)))
			max_len += VLAN_HLEN;

		if (skb->len > max_len) {
			netif_err(pdata, rx_err, netdev,
				  "packet length exceeds configured MTU\n");
			dev_kfree_skb(skb);
			goto next_packet;
		}

		if (netif_msg_pktdata(pdata))
			dwc_eth_print_pkt(netdev, skb, false);

		skb_checksum_none_assert(skb);
		if (DWC_ETH_GET_BITS(pkt_info->attributes,
				     RX_PACKET_ATTRIBUTES, CSUM_DONE))
			skb->ip_summed = CHECKSUM_UNNECESSARY;

		if (DWC_ETH_GET_BITS(pkt_info->attributes,
				     RX_PACKET_ATTRIBUTES, VLAN_CTAG))
			__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q),
					       pkt_info->vlan_ctag);

		if (DWC_ETH_GET_BITS(pkt_info->attributes,
				     RX_PACKET_ATTRIBUTES, RX_TSTAMP)) {
			u64 nsec;

			nsec = timecounter_cyc2time(&pdata->tstamp_tc,
						    pkt_info->rx_tstamp);
			hwtstamps = skb_hwtstamps(skb);
			hwtstamps->hwtstamp = ns_to_ktime(nsec);
		}

		if (DWC_ETH_GET_BITS(pkt_info->attributes,
				     RX_PACKET_ATTRIBUTES, RSS_HASH))
			skb_set_hash(skb, pkt_info->rss_hash,
				     pkt_info->rss_hash_type);

		skb->dev = netdev;
		skb->protocol = eth_type_trans(skb, netdev);
		skb_record_rx_queue(skb, channel->queue_index);

		napi_gro_receive(napi, skb);

next_packet:
		packet_count++;
	}

	/* Check if we need to save state before leaving */
	if (received && (incomplete || context_next)) {
		desc_data = DWC_ETH_GET_DESC_DATA(ring, ring->cur);
		desc_data->state_saved = 1;
		desc_data->state.skb = skb;
		desc_data->state.len = len;
		desc_data->state.error = error;
	}

	DBGPR("  packet_count = %d\n", packet_count);
	TRACE("<--");

	return packet_count;
}

static int dwc_eth_one_poll(struct napi_struct *napi, int budget)
{
	struct dwc_eth_channel *channel = container_of(napi,
						struct dwc_eth_channel,
						napi);
	int processed = 0;

	TRACE("-->");
	DBGPR("  budget=%d\n", budget);

	/* Cleanup Tx ring first */
	dwc_eth_tx_poll(channel);

	/* Process Rx ring next */
	processed = dwc_eth_rx_poll(channel, budget);

	/* If we processed everything, we are done */
	if (processed < budget) {
		/* Turn off polling */
		napi_complete_done(napi, processed);

		/* Enable Tx and Rx interrupts */
		enable_irq(channel->dma_irq);
	}

	DBGPR("  received = %d\n", processed);
	TRACE("<--");

	return processed;
}

static int dwc_eth_all_poll(struct napi_struct *napi, int budget)
{
	struct dwc_eth_pdata *pdata = container_of(napi,
						   struct dwc_eth_pdata,
						   napi);
	struct dwc_eth_channel *channel;
	int ring_budget;
	int processed, last_processed;
	unsigned int i;

	TRACE("-->");
	DBGPR("  budget=%d\n", budget);

	processed = 0;
	ring_budget = budget / pdata->rx_ring_count;
	do {
		last_processed = processed;

		channel = pdata->channel_head;
		for (i = 0; i < pdata->channel_count; i++, channel++) {
			/* Cleanup Tx ring first */
			dwc_eth_tx_poll(channel);

			/* Process Rx ring next */
			if (ring_budget > (budget - processed))
				ring_budget = budget - processed;
			processed += dwc_eth_rx_poll(channel, ring_budget);
		}
	} while ((processed < budget) && (processed != last_processed));

	/* If we processed everything, we are done */
	if (processed < budget) {
		/* Turn off polling */
		napi_complete_done(napi, processed);

		/* Enable Tx and Rx interrupts */
		dwc_eth_enable_rx_tx_ints(pdata);
	}

	DBGPR("  received = %d\n", processed);
	TRACE("<--");

	return processed;
}

void dwc_eth_dump_tx_desc(struct dwc_eth_pdata *pdata,
			  struct dwc_eth_ring *ring,
			  unsigned int idx,
			  unsigned int count,
			  unsigned int flag)
{
	struct dwc_eth_desc_data *desc_data;
	struct dwc_eth_dma_desc *dma_desc;

	while (count--) {
		desc_data = DWC_ETH_GET_DESC_DATA(ring, idx);
		dma_desc = desc_data->dma_desc;

		netdev_dbg(pdata->netdev, "TX: dma_desc=%p, dma_desc_addr=%pad\n",
			   desc_data->dma_desc, &desc_data->dma_desc_addr);
		netdev_dbg(pdata->netdev,
			   "TX_NORMAL_DESC[%d %s] = %08x:%08x:%08x:%08x\n", idx,
			   (flag == 1) ? "QUEUED FOR TX" : "TX BY DEVICE",
			   le32_to_cpu(dma_desc->desc0),
			   le32_to_cpu(dma_desc->desc1),
			   le32_to_cpu(dma_desc->desc2),
			   le32_to_cpu(dma_desc->desc3));

		idx++;
	}
}

void dwc_eth_dump_rx_desc(struct dwc_eth_pdata *pdata,
			  struct dwc_eth_ring *ring,
			  unsigned int idx)
{
	struct dwc_eth_desc_data *desc_data;
	struct dwc_eth_dma_desc *dma_desc;

	desc_data = DWC_ETH_GET_DESC_DATA(ring, idx);
	dma_desc = desc_data->dma_desc;

	netdev_dbg(pdata->netdev, "RX: dma_desc=%p, dma_desc_addr=%pad\n",
		   desc_data->dma_desc, &desc_data->dma_desc_addr);
	netdev_dbg(pdata->netdev,
		   "RX_NORMAL_DESC[%d RX BY DEVICE] = %08x:%08x:%08x:%08x\n",
		   idx,
		   le32_to_cpu(dma_desc->desc0),
		   le32_to_cpu(dma_desc->desc1),
		   le32_to_cpu(dma_desc->desc2),
		   le32_to_cpu(dma_desc->desc3));
}

void dwc_eth_print_pkt(struct net_device *netdev,
		       struct sk_buff *skb, bool tx_rx)
{
	struct ethhdr *eth = (struct ethhdr *)skb->data;
	unsigned char *buf = skb->data;
	unsigned char buffer[128];
	unsigned int i, j;

	netdev_dbg(netdev, "\n************** SKB dump ****************\n");

	netdev_dbg(netdev, "%s packet of %d bytes\n",
		   (tx_rx ? "TX" : "RX"), skb->len);

	netdev_dbg(netdev, "Dst MAC addr: %pM\n", eth->h_dest);
	netdev_dbg(netdev, "Src MAC addr: %pM\n", eth->h_source);
	netdev_dbg(netdev, "Protocol: %#06hx\n", ntohs(eth->h_proto));

	for (i = 0, j = 0; i < skb->len;) {
		j += snprintf(buffer + j, sizeof(buffer) - j, "%02hhx",
			      buf[i++]);

		if ((i % 32) == 0) {
			netdev_dbg(netdev, "  %#06x: %s\n", i - 32, buffer);
			j = 0;
		} else if ((i % 16) == 0) {
			buffer[j++] = ' ';
			buffer[j++] = ' ';
		} else if ((i % 4) == 0) {
			buffer[j++] = ' ';
		}
	}
	if (i % 32)
		netdev_dbg(netdev, "  %#06x: %s\n", i - (i % 32), buffer);

	netdev_dbg(netdev, "\n************** SKB dump ****************\n");
}

void dwc_eth_get_all_hw_features(struct dwc_eth_pdata *pdata)
{
	unsigned int mac_hfr0, mac_hfr1, mac_hfr2;
	struct dwc_eth_hw_features *hw_feat = &pdata->hw_feat;

	TRACE("-->");

	mac_hfr0 = DWC_ETH_IOREAD(pdata, MAC_HWF0R);
	mac_hfr1 = DWC_ETH_IOREAD(pdata, MAC_HWF1R);
	mac_hfr2 = DWC_ETH_IOREAD(pdata, MAC_HWF2R);

	memset(hw_feat, 0, sizeof(*hw_feat));

	hw_feat->version = DWC_ETH_IOREAD(pdata, MAC_VR);

	/* Hardware feature register 0 */
	hw_feat->phyifsel    = DWC_ETH_GET_BITS(mac_hfr0, MAC_HWF0R, PHYIFSEL);
	hw_feat->vlhash      = DWC_ETH_GET_BITS(mac_hfr0, MAC_HWF0R, VLHASH);
	hw_feat->sma         = DWC_ETH_GET_BITS(mac_hfr0, MAC_HWF0R, SMASEL);
	hw_feat->rwk         = DWC_ETH_GET_BITS(mac_hfr0, MAC_HWF0R, RWKSEL);
	hw_feat->mgk         = DWC_ETH_GET_BITS(mac_hfr0, MAC_HWF0R, MGKSEL);
	hw_feat->mmc         = DWC_ETH_GET_BITS(mac_hfr0, MAC_HWF0R, MMCSEL);
	hw_feat->aoe         = DWC_ETH_GET_BITS(mac_hfr0, MAC_HWF0R, ARPOFFSEL);
	hw_feat->ts          = DWC_ETH_GET_BITS(mac_hfr0, MAC_HWF0R, TSSEL);
	hw_feat->eee         = DWC_ETH_GET_BITS(mac_hfr0, MAC_HWF0R, EEESEL);
	hw_feat->tx_coe      = DWC_ETH_GET_BITS(mac_hfr0, MAC_HWF0R, TXCOESEL);
	hw_feat->rx_coe      = DWC_ETH_GET_BITS(mac_hfr0, MAC_HWF0R, RXCOESEL);
	hw_feat->addn_mac    = DWC_ETH_GET_BITS(mac_hfr0, MAC_HWF0R,
						ADDMACADRSEL);
	hw_feat->ts_src      = DWC_ETH_GET_BITS(mac_hfr0, MAC_HWF0R, TSSTSSEL);
	hw_feat->sa_vlan_ins = DWC_ETH_GET_BITS(mac_hfr0, MAC_HWF0R, SAVLANINS);

	/* Hardware feature register 1 */
	hw_feat->rx_fifo_size  = DWC_ETH_GET_BITS(mac_hfr1, MAC_HWF1R,
						  RXFIFOSIZE);
	hw_feat->tx_fifo_size  = DWC_ETH_GET_BITS(mac_hfr1, MAC_HWF1R,
						  TXFIFOSIZE);
	hw_feat->adv_ts_hi     = DWC_ETH_GET_BITS(mac_hfr1, MAC_HWF1R,
						  ADVTHWORD);
	hw_feat->dma_width     = DWC_ETH_GET_BITS(mac_hfr1, MAC_HWF1R, ADDR64);
	hw_feat->dcb           = DWC_ETH_GET_BITS(mac_hfr1, MAC_HWF1R, DCBEN);
	hw_feat->sph           = DWC_ETH_GET_BITS(mac_hfr1, MAC_HWF1R, SPHEN);
	hw_feat->tso           = DWC_ETH_GET_BITS(mac_hfr1, MAC_HWF1R, TSOEN);
	hw_feat->dma_debug     = DWC_ETH_GET_BITS(mac_hfr1, MAC_HWF1R, DBGMEMA);
	hw_feat->rss           = DWC_ETH_GET_BITS(mac_hfr1, MAC_HWF1R, RSSEN);
	hw_feat->tc_cnt	       = DWC_ETH_GET_BITS(mac_hfr1, MAC_HWF1R, NUMTC);
	hw_feat->hash_table_size = DWC_ETH_GET_BITS(mac_hfr1, MAC_HWF1R,
						    HASHTBLSZ);
	hw_feat->l3l4_filter_num = DWC_ETH_GET_BITS(mac_hfr1, MAC_HWF1R,
						    L3L4FNUM);

	/* Hardware feature register 2 */
	hw_feat->rx_q_cnt     = DWC_ETH_GET_BITS(mac_hfr2, MAC_HWF2R, RXQCNT);
	hw_feat->tx_q_cnt     = DWC_ETH_GET_BITS(mac_hfr2, MAC_HWF2R, TXQCNT);
	hw_feat->rx_ch_cnt    = DWC_ETH_GET_BITS(mac_hfr2, MAC_HWF2R, RXCHCNT);
	hw_feat->tx_ch_cnt    = DWC_ETH_GET_BITS(mac_hfr2, MAC_HWF2R, TXCHCNT);
	hw_feat->pps_out_num  = DWC_ETH_GET_BITS(mac_hfr2, MAC_HWF2R,
						 PPSOUTNUM);
	hw_feat->aux_snap_num = DWC_ETH_GET_BITS(mac_hfr2, MAC_HWF2R,
						 AUXSNAPNUM);

	/* Translate the Hash Table size into actual number */
	switch (hw_feat->hash_table_size) {
	case 0:
		break;
	case 1:
		hw_feat->hash_table_size = 64;
		break;
	case 2:
		hw_feat->hash_table_size = 128;
		break;
	case 3:
		hw_feat->hash_table_size = 256;
		break;
	}

	/* Translate the address width setting into actual number */
	switch (hw_feat->dma_width) {
	case 0:
		hw_feat->dma_width = 32;
		break;
	case 1:
		hw_feat->dma_width = 40;
		break;
	case 2:
		hw_feat->dma_width = 48;
		break;
	default:
		hw_feat->dma_width = 32;
	}

	/* The Queue, Channel and TC counts are zero based so increment them
	 * to get the actual number
	 */
	hw_feat->rx_q_cnt++;
	hw_feat->tx_q_cnt++;
	hw_feat->rx_ch_cnt++;
	hw_feat->tx_ch_cnt++;
	hw_feat->tc_cnt++;

	TRACE("<--");
}

void dwc_eth_print_all_hw_features(struct dwc_eth_pdata *pdata)
{
	char *str = NULL;

	TRACE("-->");

	DBGPR("\n");
	DBGPR("=====================================================\n");
	DBGPR("\n");
	DBGPR("HW support following features\n");
	DBGPR("\n");
	/* HW Feature Register0 */
	DBGPR("VLAN Hash Filter Selected                   : %s\n",
	      pdata->hw_feat.vlhash ? "YES" : "NO");
	DBGPR("SMA (MDIO) Interface                        : %s\n",
	      pdata->hw_feat.sma ? "YES" : "NO");
	DBGPR("PMT Remote Wake-up Packet Enable            : %s\n",
	      pdata->hw_feat.rwk ? "YES" : "NO");
	DBGPR("PMT Magic Packet Enable                     : %s\n",
	      pdata->hw_feat.mgk ? "YES" : "NO");
	DBGPR("RMON/MMC Module Enable                      : %s\n",
	      pdata->hw_feat.mmc ? "YES" : "NO");
	DBGPR("ARP Offload Enabled                         : %s\n",
	      pdata->hw_feat.aoe ? "YES" : "NO");
	DBGPR("IEEE 1588-2008 Timestamp Enabled            : %s\n",
	      pdata->hw_feat.ts ? "YES" : "NO");
	DBGPR("Energy Efficient Ethernet Enabled           : %s\n",
	      pdata->hw_feat.eee ? "YES" : "NO");
	DBGPR("Transmit Checksum Offload Enabled           : %s\n",
	      pdata->hw_feat.tx_coe ? "YES" : "NO");
	DBGPR("Receive Checksum Offload Enabled            : %s\n",
	      pdata->hw_feat.rx_coe ? "YES" : "NO");
	DBGPR("Additional MAC Addresses 1-31 Selected      : %s\n",
	      pdata->hw_feat.addn_mac ? "YES" : "NO");

	switch (pdata->hw_feat.ts_src) {
	case 0:
		str = "RESERVED";
		break;
	case 1:
		str = "INTERNAL";
		break;
	case 2:
		str = "EXTERNAL";
		break;
	case 3:
		str = "BOTH";
		break;
	}
	DBGPR("Timestamp System Time Source                : %s\n", str);

	DBGPR("Source Address or VLAN Insertion Enable     : %s\n",
	      pdata->hw_feat.sa_vlan_ins ? "YES" : "NO");

	/* HW Feature Register1 */
	switch (pdata->hw_feat.rx_fifo_size) {
	case 0:
		str = "128 bytes";
		break;
	case 1:
		str = "256 bytes";
		break;
	case 2:
		str = "512 bytes";
		break;
	case 3:
		str = "1 KBytes";
		break;
	case 4:
		str = "2 KBytes";
		break;
	case 5:
		str = "4 KBytes";
		break;
	case 6:
		str = "8 KBytes";
		break;
	case 7:
		str = "16 KBytes";
		break;
	case 8:
		str = "32 kBytes";
		break;
	case 9:
		str = "64 KBytes";
		break;
	case 10:
		str = "128 KBytes";
		break;
	case 11:
		str = "256 KBytes";
		break;
	default:
		str = "RESERVED";
	}
	DBGPR("MTL Receive FIFO Size                       : %s\n", str);

	switch (pdata->hw_feat.tx_fifo_size) {
	case 0:
		str = "128 bytes";
		break;
	case 1:
		str = "256 bytes";
		break;
	case 2:
		str = "512 bytes";
		break;
	case 3:
		str = "1 KBytes";
		break;
	case 4:
		str = "2 KBytes";
		break;
	case 5:
		str = "4 KBytes";
		break;
	case 6:
		str = "8 KBytes";
		break;
	case 7:
		str = "16 KBytes";
		break;
	case 8:
		str = "32 kBytes";
		break;
	case 9:
		str = "64 KBytes";
		break;
	case 10:
		str = "128 KBytes";
		break;
	case 11:
		str = "256 KBytes";
		break;
	default:
		str = "RESERVED";
	}
	DBGPR("MTL Transmit FIFO Size                      : %s\n", str);

	DBGPR("IEEE 1588 High Word Register Enable         : %s\n",
	      pdata->hw_feat.adv_ts_hi ? "YES" : "NO");
	DBGPR("Address width                               : %u\n",
	      pdata->hw_feat.dma_width);
	DBGPR("DCB Feature Enable                          : %s\n",
	      pdata->hw_feat.dcb ? "YES" : "NO");
	DBGPR("Split Header Feature Enable                 : %s\n",
	      pdata->hw_feat.sph ? "YES" : "NO");
	DBGPR("TCP Segmentation Offload Enable             : %s\n",
	      pdata->hw_feat.tso ? "YES" : "NO");
	DBGPR("DMA Debug Registers Enabled                 : %s\n",
	      pdata->hw_feat.dma_debug ? "YES" : "NO");
	DBGPR("RSS Feature Enabled                         : %s\n",
	      pdata->hw_feat.rss ? "YES" : "NO");
	DBGPR("Number of Traffic classes                   : %u\n",
	      (pdata->hw_feat.tc_cnt));
	DBGPR("Hash Table Size                             : %u\n",
	      pdata->hw_feat.hash_table_size);
	DBGPR("Total number of L3 or L4 Filters            : %u L3/L4 Filter\n",
	      pdata->hw_feat.l3l4_filter_num);

	/* HW Feature Register2 */
	DBGPR("Number of MTL Receive Queues                : %u\n",
	      pdata->hw_feat.rx_q_cnt);
	DBGPR("Number of MTL Transmit Queues               : %u\n",
	      pdata->hw_feat.tx_q_cnt);
	DBGPR("Number of DMA Receive Channels              : %u\n",
	      pdata->hw_feat.rx_ch_cnt);
	DBGPR("Number of DMA Transmit Channels             : %u\n",
	      pdata->hw_feat.tx_ch_cnt);

	switch (pdata->hw_feat.pps_out_num) {
	case 0:
		str = "No PPS output";
		break;
	case 1:
		str = "1 PPS output";
		break;
	case 2:
		str = "2 PPS output";
		break;
	case 3:
		str = "3 PPS output";
		break;
	case 4:
		str = "4 PPS output";
		break;
	default:
		str = "RESERVED";
	}
	DBGPR("Number of PPS Outputs                       : %s\n", str);

	switch (pdata->hw_feat.aux_snap_num) {
	case 0:
		str = "No auxiliary input";
		break;
	case 1:
		str = "1 auxiliary input";
		break;
	case 2:
		str = "2 auxiliary input";
		break;
	case 3:
		str = "3 auxiliary input";
		break;
	case 4:
		str = "4 auxiliary input";
		break;
	default:
		str = "RESERVED";
	}
	DBGPR("Number of Auxiliary Snapshot Inputs         : %s", str);

	DBGPR("\n");
	DBGPR("=====================================================\n");
	DBGPR("\n");

	TRACE("<--");
}

int dwc_eth_powerdown(struct net_device *netdev, unsigned int caller)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;
	unsigned long flags;

	TRACE("-->");

	if (!netif_running(netdev) ||
	    (caller == DWC_ETH_IOCTL_CONTEXT && pdata->power_down)) {
		netdev_alert(netdev, "Device is already powered down\n");
		return -EINVAL;
	}

	if (pdata->phydev)
		phy_stop(pdata->phydev);

	spin_lock_irqsave(&pdata->lock, flags);

	if (caller == DWC_ETH_DRIVER_CONTEXT)
		netif_device_detach(netdev);

	netif_tx_stop_all_queues(netdev);

	dwc_eth_stop_timers(pdata);
	flush_workqueue(pdata->dev_workqueue);

	hw_ops->powerdown_tx(pdata);
	hw_ops->powerdown_rx(pdata);

	dwc_eth_napi_disable(pdata, 0);

	pdata->power_down = 1;

	spin_unlock_irqrestore(&pdata->lock, flags);

	TRACE("<--");

	return 0;
}

int dwc_eth_powerup(struct net_device *netdev, unsigned int caller)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;
	unsigned long flags;

	TRACE("-->");

	if (!netif_running(netdev) ||
	    (caller == DWC_ETH_IOCTL_CONTEXT && !pdata->power_down)) {
		netdev_alert(netdev, "Device is already powered up\n");
		return -EINVAL;
	}

	spin_lock_irqsave(&pdata->lock, flags);

	pdata->power_down = 0;

	if (pdata->phydev)
		phy_start(pdata->phydev);

	dwc_eth_napi_enable(pdata, 0);

	hw_ops->powerup_tx(pdata);
	hw_ops->powerup_rx(pdata);

	if (caller == DWC_ETH_DRIVER_CONTEXT)
		netif_device_attach(netdev);

	netif_tx_start_all_queues(netdev);

	spin_unlock_irqrestore(&pdata->lock, flags);

	TRACE("<--");

	return 0;
}
