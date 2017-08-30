/**
 * drivers/net/ethernet/socionext/netsec/netsec_netdev.c
 *
 *  Copyright (C) 2013-2014 Fujitsu Semiconductor Limited.
 *  Copyright (C) 2014 Linaro Ltd  Andy Green <andy.green@linaro.org>
 *  All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */

#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <net/tcp.h>
#include <net/ip6_checksum.h>
#include <linux/pm_runtime.h>

#include "netsec.h"

#define WAIT_FW_RDY_TIMEOUT 50

static const u32 desc_ring_irq_status_reg_addr[] = {
	NETSEC_REG_NRM_TX_STATUS,
	NETSEC_REG_NRM_RX_STATUS,
};

static const u32 desc_ads[] = {
	NETSEC_REG_NRM_TX_CONFIG,
	NETSEC_REG_NRM_RX_CONFIG,
};

static const u32 netsec_desc_start_reg_addr_up[] = {
	NETSEC_REG_NRM_TX_DESC_START_UP,
	NETSEC_REG_NRM_RX_DESC_START_UP,
};

static const u32 netsec_desc_start_reg_addr_lw[] = {
	NETSEC_REG_NRM_TX_DESC_START_LW,
	NETSEC_REG_NRM_RX_DESC_START_LW,
};

static int netsec_wait_for_ring_config_ready(struct netsec_priv *priv, int ring)
{
	int timeout = WAIT_FW_RDY_TIMEOUT;

	while (--timeout && (netsec_readl(priv, desc_ads[ring]) &
			    NETSEC_REG_DESC_RING_CONFIG_CFG_UP))
		usleep_range(1000, 2000);

	if (!timeout) {
		netif_err(priv, hw, priv->ndev,
			  "%s: timeout\n", __func__);
		return -ETIMEDOUT;
	}

	return 0;
}

static u32 netsec_calc_pkt_ctrl_reg_param(const struct netsec_pkt_ctrlaram
					*pkt_ctrlaram_p)
{
	u32 param = NETSEC_PKT_CTRL_REG_MODE_NRM;

	if (pkt_ctrlaram_p->log_chksum_er_flag)
		param |= NETSEC_PKT_CTRL_REG_LOG_CHKSUM_ER;

	if (pkt_ctrlaram_p->log_hd_imcomplete_flag)
		param |= NETSEC_PKT_CTRL_REG_LOG_HD_INCOMPLETE;

	if (pkt_ctrlaram_p->log_hd_er_flag)
		param |= NETSEC_PKT_CTRL_REG_LOG_HD_ER;

	return param;
}

static int netsec_configure_normal_mode(struct netsec_priv *priv)
{
	int ret = 0;
	u32 value;

	/* save scb set value  */
	priv->scb_set_normal_tx_paddr = (phys_addr_t)netsec_readl(priv,
			netsec_desc_start_reg_addr_up[NETSEC_RING_TX]) << 32;
	priv->scb_set_normal_tx_paddr |= (phys_addr_t)netsec_readl(priv,
			netsec_desc_start_reg_addr_lw[NETSEC_RING_TX]);

	/* set desc_start addr */
	netsec_writel(priv, netsec_desc_start_reg_addr_up[NETSEC_RING_RX],
		      priv->desc_ring[NETSEC_RING_RX].desc_phys >> 32);
	netsec_writel(priv, netsec_desc_start_reg_addr_lw[NETSEC_RING_RX],
		      priv->desc_ring[NETSEC_RING_RX].desc_phys & 0xffffffff);

	netsec_writel(priv, netsec_desc_start_reg_addr_up[NETSEC_RING_TX],
		      priv->desc_ring[NETSEC_RING_TX].desc_phys >> 32);
	netsec_writel(priv, netsec_desc_start_reg_addr_lw[NETSEC_RING_TX],
		      priv->desc_ring[NETSEC_RING_TX].desc_phys & 0xffffffff);

	/* set normal tx desc ring config */
	value = (cpu_to_le32(1) == 1) << NETSEC_REG_DESC_ENDIAN |
		NETSEC_REG_DESC_RING_CONFIG_CFG_UP |
		NETSEC_REG_DESC_RING_CONFIG_CH_RST;
	netsec_writel(priv, desc_ads[NETSEC_RING_TX], value);

	value = (cpu_to_le32(1) == 1) << NETSEC_REG_DESC_ENDIAN |
		NETSEC_REG_DESC_RING_CONFIG_CFG_UP |
		NETSEC_REG_DESC_RING_CONFIG_CH_RST;
	netsec_writel(priv, desc_ads[NETSEC_RING_RX], value);

	if (netsec_wait_for_ring_config_ready(priv, NETSEC_RING_TX) ||
	    netsec_wait_for_ring_config_ready(priv, NETSEC_RING_RX))
		return -ETIMEDOUT;

	return ret;
}

static int netsec_change_mode_to_normal(struct netsec_priv *priv)
{
	u32 value;

	priv->scb_pkt_ctrl_reg = netsec_readl(priv, NETSEC_REG_PKT_CTRL);

	value = netsec_calc_pkt_ctrl_reg_param(&priv->param.pkt_ctrlaram);

	if (priv->param.use_jumbo_pkt_flag)
		value |= NETSEC_PKT_CTRL_REG_EN_JUMBO;

	value |= NETSEC_PKT_CTRL_REG_MODE_NRM;

	/* change to normal mode */
	netsec_writel(priv, NETSEC_REG_DMA_MH_CTRL, MH_CTRL__MODE_TRANS);
	netsec_writel(priv, NETSEC_REG_PKT_CTRL, value);

	/* Wait Change mode Complete */
	usleep_range(2000, 10000);

	return 0;
}

static int netsec_change_mode_to_taiki(struct netsec_priv *priv)
{
	int ret = 0;
	u32 value;

	netsec_writel(priv, netsec_desc_start_reg_addr_up[NETSEC_RING_TX],
		      priv->scb_set_normal_tx_paddr >> 32);
	netsec_writel(priv, netsec_desc_start_reg_addr_lw[NETSEC_RING_TX],
		      priv->scb_set_normal_tx_paddr & 0xffffffff);

	value = NETSEC_REG_DESC_RING_CONFIG_CFG_UP |
		NETSEC_REG_DESC_RING_CONFIG_CH_RST;

	netsec_writel(priv, desc_ads[NETSEC_RING_TX], value);

	if (netsec_wait_for_ring_config_ready(priv, NETSEC_RING_TX))
		return -ETIMEDOUT;

	netsec_writel(priv, NETSEC_REG_DMA_MH_CTRL, MH_CTRL__MODE_TRANS);
	netsec_writel(priv, NETSEC_REG_PKT_CTRL, priv->scb_pkt_ctrl_reg);

	/* Wait Change mode Complete */
	usleep_range(2000, 10000);

	return ret;
}

static int netsec_clear_modechange_irq(struct netsec_priv *priv, u32 value)
{
	netsec_writel(priv, NETSEC_REG_MODE_TRANS_COMP_STATUS,
		      (value & (NETSEC_MODE_TRANS_COMP_IRQ_N2T |
		      NETSEC_MODE_TRANS_COMP_IRQ_T2N)));
	return 0;
}

static int netsec_hw_configure_to_normal(struct netsec_priv *priv)
{
	int err;

	err = netsec_configure_normal_mode(priv);
	if (err) {
		netif_err(priv, drv, priv->ndev,
			  "%s: normal conf fail\n", __func__);
		return err;
	}
	err = netsec_change_mode_to_normal(priv);
	if (err) {
		netif_err(priv, drv, priv->ndev,
			  "%s: normal set fail\n", __func__);
		return err;
	}

	return err;
}

static int netsec_hw_configure_to_taiki(struct netsec_priv *priv)
{
	int ret;

	ret = netsec_change_mode_to_taiki(priv);
	if (ret) {
		netif_err(priv, drv, priv->ndev,
			  "%s: taiki set fail\n", __func__);
		return ret;
	}

	/* Clear mode change complete IRQ */
	ret = netsec_clear_modechange_irq(priv, NETSEC_MODE_TRANS_COMP_IRQ_T2N
					  | NETSEC_MODE_TRANS_COMP_IRQ_N2T);

	if (ret)
		netif_err(priv, drv, priv->ndev,
			  "%s: clear mode fail\n", __func__);

	return ret;
}

static void netsec_ring_irq_clr(struct netsec_priv *priv,
				unsigned int id, u32 value)
{
	netsec_writel(priv, desc_ring_irq_status_reg_addr[id],
		      value & (NETSEC_IRQ_EMPTY | NETSEC_IRQ_ERR));
}

static void netsec_napi_tx_processing(struct napi_struct *napi_p)
{
	struct netsec_priv *priv = container_of(napi_p,
						struct netsec_priv, napi);

	netsec_ring_irq_clr(priv, NETSEC_RING_TX, NETSEC_IRQ_EMPTY);
	netsec_clean_tx_desc_ring(priv);

	if (netif_queue_stopped(priv->ndev) &&
	    netsec_get_tx_avail_num(priv) >= NETSEC_NETDEV_TX_PKT_SCAT_NUM_MAX)
		netif_wake_queue(priv->ndev);
}

int netsec_netdev_napi_poll(struct napi_struct *napi_p, int budget)
{
	struct netsec_priv *priv = container_of(napi_p,
						struct netsec_priv, napi);
	struct net_device *ndev = priv->ndev;
	struct netsec_rx_pkt_info rx_info;
	int ret, done = 0, rx_num = 0;
	struct netsec_frag_info frag;
	struct sk_buff *skb;
	u16 len;

	netsec_napi_tx_processing(napi_p);

	while (done < budget) {
		if (!rx_num) {
			rx_num = netsec_get_rx_num(priv);
			if (!rx_num)
				break;
		}
		done++;
		rx_num--;
		ret = netsec_get_rx_pkt_data(priv, &rx_info, &frag, &len, &skb);
		if (unlikely(ret == -ENOMEM)) {
			netif_err(priv, drv, priv->ndev,
				  "%s: rx fail %d\n", __func__, ret);
			ndev->stats.rx_dropped++;
			continue;
		}
		dma_unmap_single(priv->dev, frag.dma_addr, frag.len,
				 DMA_FROM_DEVICE);
		skb_put(skb, len);
		skb->protocol = eth_type_trans(skb, priv->ndev);

		if (priv->rx_cksum_offload_flag &&
		    rx_info.rx_cksum_result == NETSEC_RX_CKSUM_OK)
			skb->ip_summed = CHECKSUM_UNNECESSARY;

		napi_gro_receive(napi_p, skb);

		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += len;
	}

	if (done == budget)
		return budget;

	napi_complete(napi_p);
	netsec_writel(priv, NETSEC_REG_INTEN_SET,
		      NETSEC_IRQ_TX | NETSEC_IRQ_RX);

	return done;
}

static netdev_tx_t netsec_netdev_start_xmit(struct sk_buff *skb,
					    struct net_device *ndev)
{
	struct netsec_priv *priv = netdev_priv(ndev);
	struct netsec_tx_pkt_ctrl tx_ctrl;
	u16 pend_tx, tso_seg_len = 0;
	skb_frag_t *frag;
	int count_frags;
	int ret, i;

	memset(&tx_ctrl, 0, sizeof(struct netsec_tx_pkt_ctrl));

	netsec_ring_irq_clr(priv, NETSEC_RING_TX, NETSEC_IRQ_EMPTY);

	count_frags = skb_shinfo(skb)->nr_frags + 1;

	if (skb->ip_summed == CHECKSUM_PARTIAL)
		tx_ctrl.cksum_offload_flag = true;

	if (skb_is_gso(skb))
		tso_seg_len = skb_shinfo(skb)->gso_size;

	if (tso_seg_len > 0) {
		if (skb->protocol == htons(ETH_P_IP)) {
			ip_hdr(skb)->tot_len = 0;
			tcp_hdr(skb)->check =
				~tcp_v4_check(0, ip_hdr(skb)->saddr,
					      ip_hdr(skb)->daddr, 0);
		} else {
			ipv6_hdr(skb)->payload_len = 0;
			tcp_hdr(skb)->check =
				~csum_ipv6_magic(&ipv6_hdr(skb)->saddr,
						 &ipv6_hdr(skb)->daddr,
						 0, IPPROTO_TCP, 0);
		}

		tx_ctrl.tcp_seg_offload_flag = true;
		tx_ctrl.tcp_seg_len = tso_seg_len;
	}

	priv->tx_info[0].dma_addr = dma_map_single(priv->dev, skb->data,
						   skb_headlen(skb),
						   DMA_TO_DEVICE);
	if (dma_mapping_error(priv->dev, priv->tx_info[0].dma_addr)) {
		netif_err(priv, drv, priv->ndev,
			  "%s: DMA mapping failed\n", __func__);
		return NETDEV_TX_OK;
	}
	priv->tx_info[0].addr = skb->data;
	priv->tx_info[0].len = skb_headlen(skb);

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		frag = &skb_shinfo(skb)->frags[i];
		priv->tx_info[i + 1].dma_addr =
			skb_frag_dma_map(priv->dev, frag, 0,
					 skb_frag_size(frag), DMA_TO_DEVICE);
		priv->tx_info[i + 1].addr = skb_frag_address(frag);
		priv->tx_info[i + 1].len = frag->size;
	}

	netsec_mark_skb_type(skb, NETSEC_RING_TX);

	ret = netsec_set_tx_pkt_data(priv, &tx_ctrl, count_frags,
				     priv->tx_info, skb);
	if (ret) {
		netif_info(priv, drv, priv->ndev,
			   "set tx pkt failed %d\n", ret);
		for (i = 0; i < count_frags; i++)
			dma_unmap_single(priv->dev, priv->tx_info[i].dma_addr,
					 priv->tx_info[i].len, DMA_TO_DEVICE);
		ndev->stats.tx_dropped++;

		return NETDEV_TX_OK;
	}

	netdev_sent_queue(priv->ndev, skb->len);

	spin_lock(&priv->tx_queue_lock);
	pend_tx = netsec_get_tx_avail_num(priv);

	if (pend_tx < NETSEC_NETDEV_TX_PKT_SCAT_NUM_MAX) {
		netsec_ring_irq_enable(priv, NETSEC_RING_TX, NETSEC_IRQ_EMPTY);
		netif_stop_queue(ndev);
		goto err;
	}
	if (pend_tx <= DESC_NUM - 2) {
		netsec_ring_irq_enable(priv, NETSEC_RING_TX, NETSEC_IRQ_EMPTY);
		goto err;
	}
	netsec_ring_irq_disable(priv, NETSEC_RING_TX, NETSEC_IRQ_EMPTY);

err:
	spin_unlock(&priv->tx_queue_lock);

	return NETDEV_TX_OK;
}

static int netsec_netdev_set_features(struct net_device *ndev,
				      netdev_features_t features)
{
	struct netsec_priv *priv = netdev_priv(ndev);

	priv->rx_cksum_offload_flag = !!(features & NETIF_F_RXCSUM);

	return 0;
}

static void netsec_phy_adjust_link(struct net_device *ndev)
{
	struct netsec_priv *priv = netdev_priv(ndev);

	if (priv->actual_link_speed == ndev->phydev->speed &&
	    priv->actual_duplex == ndev->phydev->duplex)
		return;

	netsec_stop_gmac(priv);
	netsec_start_gmac(priv);
}

static irqreturn_t netsec_irq_handler(int irq, void *dev_id)
{
	struct netsec_priv *priv = dev_id;
	u32 status = netsec_readl(priv, NETSEC_REG_TOP_STATUS) &
		     netsec_readl(priv, NETSEC_REG_TOP_INTEN);

	if (!status)
		return IRQ_NONE;

	if (status & (NETSEC_IRQ_TX | NETSEC_IRQ_RX)) {
		netsec_writel(priv, NETSEC_REG_INTEN_CLR,
			      status & (NETSEC_IRQ_TX | NETSEC_IRQ_RX));
		napi_schedule(&priv->napi);
	}

	return IRQ_HANDLED;
}

static int netsec_netdev_open(struct net_device *ndev)
{
	struct netsec_priv *priv = netdev_priv(ndev);
	struct phy_device *phydev = NULL;
	u32 scb_irq_temp;
	int ret, n;

	scb_irq_temp = netsec_readl(priv, NETSEC_REG_TOP_INTEN);

	for (n = 0; n <= NETSEC_RING_MAX; n++) {
		ret = netsec_alloc_desc_ring(priv, n);
		if (ret) {
			netif_err(priv, probe, priv->ndev,
				  "%s: alloc ring failed\n", __func__);
			goto err;
		}
	}

	ret = netsec_setup_rx_desc(priv, &priv->desc_ring[NETSEC_RING_RX]);
	if (ret) {
		netif_err(priv, probe, priv->ndev,
			  "%s: fail setup ring\n", __func__);
		goto err1;
	}

	pm_runtime_get_sync(priv->dev);

	netsec_writel(priv, NETSEC_REG_INTEN_CLR, scb_irq_temp);

	ret = netsec_hw_configure_to_normal(priv);
	if (ret) {
		netif_err(priv, probe, priv->ndev,
			  "%s: normal fail %d\n", __func__, ret);
		goto err1;
	}

	ret = request_irq(priv->ndev->irq, netsec_irq_handler,
			  IRQF_SHARED, "netsec", priv);
	if (ret) {
		netif_err(priv, drv, priv->ndev, "request_irq failed\n");
		goto err1;
	}
	priv->irq_registered = true;

	ret = netsec_clean_rx_desc_ring(priv);
	if (ret) {
		netif_err(priv, drv, priv->ndev,
			  "%s: clean rx desc fail\n", __func__);
		goto err2;
	}

	ret = netsec_clean_tx_desc_ring(priv);
	if (ret) {
		netif_err(priv, drv, priv->ndev,
			  "%s: clean tx desc fail\n", __func__);
		goto err2;
	}

	netsec_ring_irq_clr(priv, NETSEC_RING_TX, NETSEC_IRQ_EMPTY);

	phydev = of_phy_connect(priv->ndev, priv->phy_np,
				&netsec_phy_adjust_link, 0,
				priv->phy_interface);
	if (!phydev) {
		netif_err(priv, link, priv->ndev, "missing PHY\n");
		goto err2;
	}

	phy_start_aneg(phydev);

	netsec_ring_irq_disable(priv, NETSEC_RING_TX, NETSEC_IRQ_EMPTY);

	netsec_start_gmac(priv);
	napi_enable(&priv->napi);
	netif_start_queue(ndev);

	netsec_writel(priv, NETSEC_REG_INTEN_SET,
		      NETSEC_IRQ_TX | NETSEC_IRQ_RX);

	return 0;

err2:
	pm_runtime_put_sync(priv->dev);
	free_irq(priv->ndev->irq, priv);
	priv->irq_registered = false;
err1:
	for (n = 0; n <= NETSEC_RING_MAX; n++)
		netsec_free_desc_ring(priv, &priv->desc_ring[n]);
err:
	netsec_writel(priv, NETSEC_REG_INTEN_SET, scb_irq_temp);

	pm_runtime_put_sync(priv->dev);

	return ret;
}

static int netsec_netdev_stop(struct net_device *ndev)
{
	struct netsec_priv *priv = netdev_priv(ndev);
	int n;

	phy_stop(ndev->phydev);
	phy_disconnect(ndev->phydev);

	netif_stop_queue(priv->ndev);
	napi_disable(&priv->napi);

	netsec_writel(priv, NETSEC_REG_INTEN_CLR, ~0);
	netsec_stop_gmac(priv);
	WARN_ON(netsec_hw_configure_to_taiki(priv));

	pm_runtime_put_sync(priv->dev);

	for (n = 0; n <= NETSEC_RING_MAX; n++)
		netsec_free_desc_ring(priv, &priv->desc_ring[n]);

	free_irq(priv->ndev->irq, priv);
	priv->irq_registered = false;

	return 0;
}

const struct net_device_ops netsec_netdev_ops = {
	.ndo_open		= netsec_netdev_open,
	.ndo_stop		= netsec_netdev_stop,
	.ndo_start_xmit		= netsec_netdev_start_xmit,
	.ndo_set_features	= netsec_netdev_set_features,
	.ndo_set_mac_address    = eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};
