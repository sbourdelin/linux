/*   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; version 2 of the License
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   Copyright (C) 2009-2015 John Crispin <blogic@openwrt.org>
 *   Copyright (C) 2009-2015 Felix Fietkau <nbd@openwrt.org>
 *   Copyright (C) 2013-2015 Michael Lee <igvtee@gmail.com>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/skbuff.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/clk.h>
#include <linux/of_net.h>
#include <linux/of_mdio.h>
#include <linux/if_vlan.h>
#include <linux/reset.h>
#include <linux/tcp.h>
#include <linux/io.h>
#include <linux/bug.h>

#include <asm/mach-ralink/ralink_regs.h>

#include "mtk_eth_soc.h"
#include "mdio.h"
#include "ethtool.h"

#define	MAX_RX_LENGTH		1536
#define FE_RX_ETH_HLEN		(VLAN_ETH_HLEN + VLAN_HLEN + ETH_FCS_LEN)
#define FE_RX_HLEN		(NET_SKB_PAD + FE_RX_ETH_HLEN + NET_IP_ALIGN)
#define DMA_DUMMY_DESC		0xffffffff
#define FE_DEFAULT_MSG_ENABLE \
		(NETIF_MSG_DRV | \
		NETIF_MSG_PROBE | \
		NETIF_MSG_LINK | \
		NETIF_MSG_TIMER | \
		NETIF_MSG_IFDOWN | \
		NETIF_MSG_IFUP | \
		NETIF_MSG_RX_ERR | \
		NETIF_MSG_TX_ERR)

#define TX_DMA_DESP2_DEF	(TX_DMA_LS0 | TX_DMA_DONE)
#define NEXT_TX_DESP_IDX(X)	(((X) + 1) & (ring->tx_ring_size - 1))
#define NEXT_RX_DESP_IDX(X)	(((X) + 1) & (ring->rx_ring_size - 1))

#define SYSC_REG_RSTCTRL	0x34

static int fe_msg_level = -1;
module_param_named(msg_level, fe_msg_level, int, 0);
MODULE_PARM_DESC(msg_level, "Message level (-1=defaults,0=none,...,16=all)");

static const u16 fe_reg_table_default[FE_REG_COUNT] = {
	[FE_REG_PDMA_GLO_CFG] = FE_PDMA_GLO_CFG,
	[FE_REG_PDMA_RST_CFG] = FE_PDMA_RST_CFG,
	[FE_REG_DLY_INT_CFG] = FE_DLY_INT_CFG,
	[FE_REG_TX_BASE_PTR0] = FE_TX_BASE_PTR0,
	[FE_REG_TX_MAX_CNT0] = FE_TX_MAX_CNT0,
	[FE_REG_TX_CTX_IDX0] = FE_TX_CTX_IDX0,
	[FE_REG_TX_DTX_IDX0] = FE_TX_DTX_IDX0,
	[FE_REG_RX_BASE_PTR0] = FE_RX_BASE_PTR0,
	[FE_REG_RX_MAX_CNT0] = FE_RX_MAX_CNT0,
	[FE_REG_RX_CALC_IDX0] = FE_RX_CALC_IDX0,
	[FE_REG_RX_DRX_IDX0] = FE_RX_DRX_IDX0,
	[FE_REG_FE_INT_ENABLE] = FE_FE_INT_ENABLE,
	[FE_REG_FE_INT_STATUS] = FE_FE_INT_STATUS,
	[FE_REG_FE_DMA_VID_BASE] = FE_DMA_VID0,
	[FE_REG_FE_COUNTER_BASE] = FE_GDMA1_TX_GBCNT,
	[FE_REG_FE_RST_GL] = FE_FE_RST_GL,
};

static const u16 *fe_reg_table = fe_reg_table_default;

struct fe_work_t {
	int bitnr;
	void (*action)(struct fe_priv *);
};

static void __iomem *fe_base;

void fe_w32(u32 val, unsigned reg)
{
	__raw_writel(val, fe_base + reg);
}

u32 fe_r32(unsigned reg)
{
	return __raw_readl(fe_base + reg);
}

static void fe_reg_w32(u32 val, enum fe_reg reg)
{
	fe_w32(val, fe_reg_table[reg]);
}

static u32 fe_reg_r32(enum fe_reg reg)
{
	return fe_r32(fe_reg_table[reg]);
}

void fe_reset(u32 reset_bits)
{
	u32 t;

	t = rt_sysc_r32(SYSC_REG_RSTCTRL);
	t |= reset_bits;
	rt_sysc_w32(t, SYSC_REG_RSTCTRL);
	usleep_range(10, 20);

	t &= ~reset_bits;
	rt_sysc_w32(t, SYSC_REG_RSTCTRL);
	usleep_range(10, 20);
}

static inline void fe_irq_ack(struct fe_priv *priv, u32 mask)
{
	if (priv->soc->dma_type & FE_PDMA)
		fe_reg_w32(mask, FE_REG_FE_INT_STATUS);
	if (priv->soc->dma_type & FE_QDMA)
		fe_w32(mask, FE_QFE_INT_STATUS);
}

static inline u32 fe_irq_pending(struct fe_priv *priv)
{
	u32 status = 0;

	if (priv->soc->dma_type & FE_PDMA)
		status |= fe_reg_r32(FE_REG_FE_INT_STATUS);

	if (priv->soc->dma_type & FE_QDMA)
		status |= fe_r32(FE_QFE_INT_STATUS);

	return status;
}

static void fe_irq_ack_status(struct fe_priv *priv, u32 mask)
{
	u32 status_reg = FE_REG_FE_INT_STATUS;

	if (fe_reg_table[FE_REG_FE_INT_STATUS2])
		status_reg = FE_REG_FE_INT_STATUS2;

	fe_reg_w32(mask, status_reg);
}

static u32 fe_irq_pending_status(struct fe_priv *priv)
{
	u32 status_reg = FE_REG_FE_INT_STATUS;

	if (fe_reg_table[FE_REG_FE_INT_STATUS2])
		status_reg = FE_REG_FE_INT_STATUS2;

	return fe_reg_r32(status_reg);
}

static inline void fe_irq_disable(struct fe_priv *priv, u32 mask)
{
	u32 val;

	if (priv->soc->dma_type & FE_PDMA) {
		val = fe_reg_r32(FE_REG_FE_INT_ENABLE);
		fe_reg_w32(val & ~mask, FE_REG_FE_INT_ENABLE);
		/* flush write */
		fe_reg_r32(FE_REG_FE_INT_ENABLE);
	}

	if (priv->soc->dma_type & FE_QDMA) {
		val = fe_r32(FE_QFE_INT_ENABLE);
		fe_w32(val & ~mask, FE_QFE_INT_ENABLE);
		/* flush write */
		fe_r32(FE_QFE_INT_ENABLE);
	}
}

static inline void fe_irq_enable(struct fe_priv *priv, u32 mask)
{
	u32 val;

	if (priv->soc->dma_type & FE_PDMA) {
		val = fe_reg_r32(FE_REG_FE_INT_ENABLE);
		fe_reg_w32(val | mask, FE_REG_FE_INT_ENABLE);
		/* flush write */
		fe_reg_r32(FE_REG_FE_INT_ENABLE);
	}

	if (priv->soc->dma_type & FE_QDMA) {
		val = fe_r32(FE_QFE_INT_ENABLE);
		fe_w32(val | mask, FE_QFE_INT_ENABLE);
		/* flush write */
		fe_r32(FE_QFE_INT_ENABLE);
	}
}

static inline u32 fe_irq_enabled(struct fe_priv *priv)
{
	u32 enabled = 0;

	if (priv->soc->dma_type & FE_PDMA)
		enabled |= fe_reg_r32(FE_REG_FE_INT_ENABLE);

	if (priv->soc->dma_type & FE_QDMA)
		enabled |= fe_reg_r32(FE_QFE_INT_ENABLE);

	return enabled;
}

static inline void fe_hw_set_macaddr(struct fe_priv *priv, unsigned char *mac)
{
	unsigned long flags;

	spin_lock_irqsave(&priv->page_lock, flags);
	fe_w32((mac[0] << 8) | mac[1], FE_GDMA1_MAC_ADRH);
	fe_w32((mac[2] << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5],
	       FE_GDMA1_MAC_ADRL);
	spin_unlock_irqrestore(&priv->page_lock, flags);
}

static int fe_set_mac_address(struct net_device *dev, void *p)
{
	int ret = eth_mac_addr(dev, p);

	if (!ret) {
		struct fe_priv *priv = netdev_priv(dev);

		if (priv->soc->set_mac)
			priv->soc->set_mac(priv, dev->dev_addr);
		else
			fe_hw_set_macaddr(priv, p);
	}

	return ret;
}

static inline int fe_max_frag_size(int mtu)
{
	/* make sure buf_size will be at least MAX_RX_LENGTH */
	if (mtu + FE_RX_ETH_HLEN < MAX_RX_LENGTH)
		mtu = MAX_RX_LENGTH - FE_RX_ETH_HLEN;

	return SKB_DATA_ALIGN(FE_RX_HLEN + mtu) +
		SKB_DATA_ALIGN(sizeof(struct skb_shared_info));
}

static inline int fe_max_buf_size(int frag_size)
{
	int buf_size = frag_size - NET_SKB_PAD - NET_IP_ALIGN -
		       SKB_DATA_ALIGN(sizeof(struct skb_shared_info));

	WARN_ON(buf_size < MAX_RX_LENGTH);
	return buf_size;
}

static inline void fe_get_rxd(struct fe_rx_dma *rxd, struct fe_rx_dma *dma_rxd)
{
	rxd->rxd1 = READ_ONCE(dma_rxd->rxd1);
	rxd->rxd2 = READ_ONCE(dma_rxd->rxd2);
	rxd->rxd3 = READ_ONCE(dma_rxd->rxd3);
	rxd->rxd4 = READ_ONCE(dma_rxd->rxd4);
}

static inline void fe_set_txd_pdma(struct fe_tx_dma *txd,
				   struct fe_tx_dma *dma_txd)
{
	WRITE_ONCE(dma_txd->txd1, txd->txd1);
	WRITE_ONCE(dma_txd->txd3, txd->txd3);
	WRITE_ONCE(dma_txd->txd4, txd->txd4);
	/* clean dma done flag last */
	WRITE_ONCE(dma_txd->txd2, txd->txd2);
}

static void fe_clean_rx(struct fe_priv *priv, struct fe_rx_ring *ring)
{
	int i;

	if (ring->rx_data) {
		for (i = 0; i < ring->rx_ring_size; i++)
			if (ring->rx_data[i]) {
				if (ring->rx_dma && ring->rx_dma[i].rxd1)
					dma_unmap_single(&priv->netdev->dev,
							 ring->rx_dma[i].rxd1,
							 ring->rx_buf_size,
							 DMA_FROM_DEVICE);
				put_page(virt_to_head_page(ring->rx_data[i]));
			}

		kfree(ring->rx_data);
		ring->rx_data = NULL;
	}

	if (ring->rx_dma) {
		dma_free_coherent(&priv->netdev->dev,
				  ring->rx_ring_size * sizeof(*ring->rx_dma),
				  ring->rx_dma,
				  ring->rx_phys);
		ring->rx_dma = NULL;
	}
}

static int fe_dma_rx_alloc(struct fe_priv *priv, struct fe_rx_ring *ring)
{
	struct net_device *netdev = priv->netdev;
	int i, pad;

	ring->frag_size = fe_max_frag_size(ETH_DATA_LEN);
	ring->rx_buf_size = fe_max_buf_size(ring->frag_size);
	ring->rx_ring_size = NUM_DMA_DESC;
	if (priv->flags & FE_FLAG_NAPI_WEIGHT)
		ring->rx_ring_size *= 4;

	ring->rx_data = kcalloc(ring->rx_ring_size, sizeof(*ring->rx_data),
			GFP_KERNEL);
	if (!ring->rx_data)
		goto no_rx_mem;

	for (i = 0; i < ring->rx_ring_size; i++) {
		ring->rx_data[i] = netdev_alloc_frag(ring->frag_size);
		if (!ring->rx_data[i])
			goto no_rx_mem;
	}

	ring->rx_dma = dma_alloc_coherent(&netdev->dev,
			ring->rx_ring_size * sizeof(*ring->rx_dma),
			&ring->rx_phys,
			GFP_ATOMIC | __GFP_ZERO);
	if (!ring->rx_dma)
		goto no_rx_mem;

	if (priv->flags & FE_FLAG_RX_2B_OFFSET)
		pad = 0;
	else
		pad = NET_IP_ALIGN;
	for (i = 0; i < ring->rx_ring_size; i++) {
		dma_addr_t dma_addr = dma_map_single(&netdev->dev,
				ring->rx_data[i] + NET_SKB_PAD + pad,
				ring->rx_buf_size,
				DMA_FROM_DEVICE);
		if (unlikely(dma_mapping_error(&netdev->dev, dma_addr)))
			goto no_rx_mem;
		ring->rx_dma[i].rxd1 = (unsigned int)dma_addr;

		if (priv->flags & FE_FLAG_RX_SG_DMA)
			ring->rx_dma[i].rxd2 = RX_DMA_PLEN0(ring->rx_buf_size);
		else
			ring->rx_dma[i].rxd2 = RX_DMA_LSO;
	}
	ring->rx_calc_idx = ring->rx_ring_size - 1;
	/* make sure that all changes to the dma ring are flushed before we
	 * continue
	 */
	wmb();

	return 0;

no_rx_mem:
	return -ENOMEM;
}

static void fe_txd_unmap(struct device *dev, struct fe_tx_buf *tx_buf)
{
	if (tx_buf->flags & FE_TX_FLAGS_SINGLE0) {
		dma_unmap_single(dev,
				 dma_unmap_addr(tx_buf, dma_addr0),
				 dma_unmap_len(tx_buf, dma_len0),
				 DMA_TO_DEVICE);
	} else if (tx_buf->flags & FE_TX_FLAGS_PAGE0) {
		dma_unmap_page(dev,
			       dma_unmap_addr(tx_buf, dma_addr0),
			       dma_unmap_len(tx_buf, dma_len0),
			       DMA_TO_DEVICE);
	}
	if (tx_buf->flags & FE_TX_FLAGS_PAGE1)
		dma_unmap_page(dev,
			       dma_unmap_addr(tx_buf, dma_addr1),
			       dma_unmap_len(tx_buf, dma_len1),
			       DMA_TO_DEVICE);

	tx_buf->flags = 0;
	if (tx_buf->skb && (tx_buf->skb != (struct sk_buff *)DMA_DUMMY_DESC))
		dev_kfree_skb_any(tx_buf->skb);
	tx_buf->skb = NULL;
}

static void fe_pdma_tx_clean(struct fe_priv *priv)
{
	int i;
	struct device *dev = &priv->netdev->dev;
	struct fe_tx_ring *ring = &priv->tx_ring;

	if (ring->tx_buf) {
		for (i = 0; i < ring->tx_ring_size; i++)
			fe_txd_unmap(dev, &ring->tx_buf[i]);
		kfree(ring->tx_buf);
		ring->tx_buf = NULL;
	}

	if (ring->tx_dma) {
		dma_free_coherent(dev,
				  ring->tx_ring_size * sizeof(*ring->tx_dma),
				  ring->tx_dma,
				  ring->tx_phys);
		ring->tx_dma = NULL;
	}
}

static void fe_qdma_tx_clean(struct fe_priv *priv)
{
	int i;
	struct device *dev = &priv->netdev->dev;
	struct fe_tx_ring *ring = &priv->tx_ring;

	if (ring->tx_buf) {
		for (i = 0; i < ring->tx_ring_size; i++)
			fe_txd_unmap(dev, &ring->tx_buf[i]);
		kfree(ring->tx_buf);
		ring->tx_buf = NULL;
	}

	if (ring->tx_dma) {
		dma_free_coherent(dev,
				  ring->tx_ring_size * sizeof(*ring->tx_dma),
				  ring->tx_dma,
				  ring->tx_phys);
		ring->tx_dma = NULL;
	}
}

void fe_stats_update(struct fe_priv *priv)
{
	struct fe_hw_stats *hwstats = priv->hw_stats;
	unsigned int base = fe_reg_table[FE_REG_FE_COUNTER_BASE];
	u64 stats;

	u64_stats_update_begin(&hwstats->syncp);

	if (IS_ENABLED(CONFIG_SOC_MT7621)) {
		hwstats->rx_bytes			+= fe_r32(base);
		stats					=  fe_r32(base + 0x04);
		if (stats)
			hwstats->rx_bytes		+= (stats << 32);
		hwstats->rx_packets			+= fe_r32(base + 0x08);
		hwstats->rx_overflow			+= fe_r32(base + 0x10);
		hwstats->rx_fcs_errors			+= fe_r32(base + 0x14);
		hwstats->rx_short_errors		+= fe_r32(base + 0x18);
		hwstats->rx_long_errors			+= fe_r32(base + 0x1c);
		hwstats->rx_checksum_errors		+= fe_r32(base + 0x20);
		hwstats->rx_flow_control_packets	+= fe_r32(base + 0x24);
		hwstats->tx_skip			+= fe_r32(base + 0x28);
		hwstats->tx_collisions			+= fe_r32(base + 0x2c);
		hwstats->tx_bytes			+= fe_r32(base + 0x30);
		stats					=  fe_r32(base + 0x34);
		if (stats)
			hwstats->tx_bytes		+= (stats << 32);
		hwstats->tx_packets			+= fe_r32(base + 0x38);
	} else {
		hwstats->tx_bytes			+= fe_r32(base);
		hwstats->tx_packets			+= fe_r32(base + 0x04);
		hwstats->tx_skip			+= fe_r32(base + 0x08);
		hwstats->tx_collisions			+= fe_r32(base + 0x0c);
		hwstats->rx_bytes			+= fe_r32(base + 0x20);
		hwstats->rx_packets			+= fe_r32(base + 0x24);
		hwstats->rx_overflow			+= fe_r32(base + 0x28);
		hwstats->rx_fcs_errors			+= fe_r32(base + 0x2c);
		hwstats->rx_short_errors		+= fe_r32(base + 0x30);
		hwstats->rx_long_errors			+= fe_r32(base + 0x34);
		hwstats->rx_checksum_errors		+= fe_r32(base + 0x38);
		hwstats->rx_flow_control_packets	+= fe_r32(base + 0x3c);
	}

	u64_stats_update_end(&hwstats->syncp);
}

static struct rtnl_link_stats64 *fe_get_stats64(struct net_device *dev,
				struct rtnl_link_stats64 *storage)
{
	struct fe_priv *priv = netdev_priv(dev);
	struct fe_hw_stats *hwstats = priv->hw_stats;
	unsigned int base = fe_reg_table[FE_REG_FE_COUNTER_BASE];
	unsigned int start;

	if (!base) {
		netdev_stats_to_stats64(storage, &dev->stats);
		return storage;
	}

	if (netif_running(dev) && netif_device_present(dev)) {
		if (spin_trylock(&hwstats->stats_lock)) {
			fe_stats_update(priv);
			spin_unlock(&hwstats->stats_lock);
		}
	}

	do {
		start = u64_stats_fetch_begin_irq(&hwstats->syncp);
		storage->rx_packets = hwstats->rx_packets;
		storage->tx_packets = hwstats->tx_packets;
		storage->rx_bytes = hwstats->rx_bytes;
		storage->tx_bytes = hwstats->tx_bytes;
		storage->collisions = hwstats->tx_collisions;
		storage->rx_length_errors = hwstats->rx_short_errors +
			hwstats->rx_long_errors;
		storage->rx_over_errors = hwstats->rx_overflow;
		storage->rx_crc_errors = hwstats->rx_fcs_errors;
		storage->rx_errors = hwstats->rx_checksum_errors;
		storage->tx_aborted_errors = hwstats->tx_skip;
	} while (u64_stats_fetch_retry_irq(&hwstats->syncp, start));

	storage->tx_errors = priv->netdev->stats.tx_errors;
	storage->rx_dropped = priv->netdev->stats.rx_dropped;
	storage->tx_dropped = priv->netdev->stats.tx_dropped;

	return storage;
}

static int fe_vlan_rx_add_vid(struct net_device *dev,
			      __be16 proto, u16 vid)
{
	struct fe_priv *priv = netdev_priv(dev);
	u32 idx = (vid & 0xf);
	u32 vlan_cfg;

	if (!((fe_reg_table[FE_REG_FE_DMA_VID_BASE]) &&
	      (dev->features & NETIF_F_HW_VLAN_CTAG_TX)))
		return 0;

	if (test_bit(idx, &priv->vlan_map)) {
		netdev_warn(dev, "disable tx vlan offload\n");
		dev->wanted_features &= ~NETIF_F_HW_VLAN_CTAG_TX;
		netdev_update_features(dev);
	} else {
		vlan_cfg = fe_r32(fe_reg_table[FE_REG_FE_DMA_VID_BASE] +
				((idx >> 1) << 2));
		if (idx & 0x1) {
			vlan_cfg &= 0xffff;
			vlan_cfg |= (vid << 16);
		} else {
			vlan_cfg &= 0xffff0000;
			vlan_cfg |= vid;
		}
		fe_w32(vlan_cfg, fe_reg_table[FE_REG_FE_DMA_VID_BASE] +
				((idx >> 1) << 2));
		set_bit(idx, &priv->vlan_map);
	}

	return 0;
}

static int fe_vlan_rx_kill_vid(struct net_device *dev,
			       __be16 proto, u16 vid)
{
	struct fe_priv *priv = netdev_priv(dev);
	u32 idx = (vid & 0xf);

	if (!((fe_reg_table[FE_REG_FE_DMA_VID_BASE]) &&
	      (dev->features & NETIF_F_HW_VLAN_CTAG_TX)))
		return 0;

	clear_bit(idx, &priv->vlan_map);

	return 0;
}

static inline u32 fe_pdma_empty_txd(struct fe_tx_ring *ring)
{
	barrier();
	return (u32)(ring->tx_ring_size -
			((ring->tx_next_idx - ring->tx_free_idx) &
			 (ring->tx_ring_size - 1)));
}

static int fe_skb_padto(struct sk_buff *skb, struct fe_priv *priv)
{
	unsigned int len;
	int ret;

	if (unlikely(skb->len >= VLAN_ETH_ZLEN))
		return 0;

	if ((priv->flags & FE_FLAG_PADDING_64B) &&
	    !(priv->flags & FE_FLAG_PADDING_BUG))
		return 0;

	if (skb_vlan_tag_present(skb))
		len = ETH_ZLEN;
	else if (skb->protocol == cpu_to_be16(ETH_P_8021Q))
		len = VLAN_ETH_ZLEN;
	else if (!(priv->flags & FE_FLAG_PADDING_64B))
		len = ETH_ZLEN;
	else
		return 0;

	if (skb->len >= len)
		return 0;

	ret = skb_pad(skb, len - skb->len);
	if (ret < 0)
		return ret;
	skb->len = len;
	skb_set_tail_pointer(skb, len);

	return ret;
}

static int fe_pdma_tx_map(struct sk_buff *skb, struct net_device *dev,
			  int tx_num, struct fe_tx_ring *ring, bool gso)
{
	struct fe_priv *priv = netdev_priv(dev);
	struct skb_frag_struct *frag;
	struct fe_tx_dma txd, *ptxd;
	struct fe_tx_buf *tx_buf;
	dma_addr_t mapped_addr;
	unsigned int nr_frags;
	u32 def_txd4;
	int i, j, k, frag_size, frag_map_size, offset;

	if (fe_skb_padto(skb, priv)) {
		netif_warn(priv, tx_err, dev, "tx padding failed!\n");
		return -1;
	}

	tx_buf = &ring->tx_buf[ring->tx_next_idx];
	memset(tx_buf, 0, sizeof(*tx_buf));
	memset(&txd, 0, sizeof(txd));
	nr_frags = skb_shinfo(skb)->nr_frags;

	/* init tx descriptor */
	def_txd4 = priv->soc->txd4;
	txd.txd4 = def_txd4;

	/* TX Checksum offload */
	if (skb->ip_summed == CHECKSUM_PARTIAL)
		txd.txd4 |= TX_DMA_CHKSUM;

	/* VLAN header offload */
	if (skb_vlan_tag_present(skb)) {
		u16 tag = skb_vlan_tag_get(skb);

		txd.txd4 |= TX_DMA_INS_VLAN |
			    ((tag >> VLAN_PRIO_SHIFT) << 4) |
			    (tag & 0xF);
	}

	mapped_addr = dma_map_single(&dev->dev, skb->data,
				     skb_headlen(skb), DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(&dev->dev, mapped_addr)))
		return -1;

	txd.txd1 = mapped_addr;
	txd.txd2 = TX_DMA_PLEN0(skb_headlen(skb));

	tx_buf->flags |= FE_TX_FLAGS_SINGLE0;
	dma_unmap_addr_set(tx_buf, dma_addr0, mapped_addr);
	dma_unmap_len_set(tx_buf, dma_len0, skb_headlen(skb));

	/* TX SG offload */
	j = ring->tx_next_idx;
	k = 0;
	for (i = 0; i < nr_frags; i++) {
		offset = 0;
		frag = &skb_shinfo(skb)->frags[i];
		frag_size = skb_frag_size(frag);

		while (frag_size > 0) {
			frag_map_size = min(frag_size, TX_DMA_BUF_LEN);
			mapped_addr = skb_frag_dma_map(&dev->dev, frag, offset,
						       frag_map_size,
						       DMA_TO_DEVICE);
			if (unlikely(dma_mapping_error(&dev->dev, mapped_addr)))
				goto err_dma;

			if (k & 0x1) {
				j = NEXT_TX_DESP_IDX(j);
				txd.txd1 = mapped_addr;
				txd.txd2 = TX_DMA_PLEN0(frag_map_size);
				txd.txd4 = def_txd4;

				tx_buf = &ring->tx_buf[j];
				memset(tx_buf, 0, sizeof(*tx_buf));

				tx_buf->flags |= FE_TX_FLAGS_PAGE0;
				dma_unmap_addr_set(tx_buf, dma_addr0,
						   mapped_addr);
				dma_unmap_len_set(tx_buf, dma_len0,
						  frag_map_size);
			} else {
				txd.txd3 = mapped_addr;
				txd.txd2 |= TX_DMA_PLEN1(frag_map_size);

				tx_buf->skb = (struct sk_buff *)DMA_DUMMY_DESC;
				tx_buf->flags |= FE_TX_FLAGS_PAGE1;
				dma_unmap_addr_set(tx_buf, dma_addr1,
						   mapped_addr);
				dma_unmap_len_set(tx_buf, dma_len1,
						  frag_map_size);

				if (!((i == (nr_frags - 1)) &&
				      (frag_map_size == frag_size))) {
					fe_set_txd_pdma(&txd, &ring->tx_dma[j]);
					memset(&txd, 0, sizeof(txd));
				}
			}
			frag_size -= frag_map_size;
			offset += frag_map_size;
			k++;
		}
	}

	/* set last segment */
	if (k & 0x1)
		txd.txd2 |= TX_DMA_LS1;
	else
		txd.txd2 |= TX_DMA_LS0;
	fe_set_txd_pdma(&txd, &ring->tx_dma[j]);

	/* store skb to cleanup */
	tx_buf->skb = skb;

	netdev_sent_queue(dev, skb->len);
	skb_tx_timestamp(skb);

	ring->tx_next_idx = NEXT_TX_DESP_IDX(j);
	/* make sure that all changes to the dma ring are flushed before we
	 * continue
	 */
	wmb();
	atomic_set(&ring->tx_free_count, fe_pdma_empty_txd(ring));

	if (netif_xmit_stopped(netdev_get_tx_queue(dev, 0)) || !skb->xmit_more)
		fe_reg_w32(ring->tx_next_idx, FE_REG_TX_CTX_IDX0);

	return 0;

err_dma:
	j = ring->tx_next_idx;
	for (i = 0; i < tx_num; i++) {
		ptxd = &ring->tx_dma[j];
		tx_buf = &ring->tx_buf[j];

		/* unmap dma */
		fe_txd_unmap(&dev->dev, tx_buf);

		ptxd->txd2 = TX_DMA_DESP2_DEF;
		j = NEXT_TX_DESP_IDX(j);
	}
	/* make sure that all changes to the dma ring are flushed before we
	 * continue
	 */
	wmb();
	return -1;
}

static void *fe_qdma_phys_to_virt(struct fe_tx_ring *ring, u32 desc)
{
	void *ret = ring->tx_dma;

	return ret + (desc - ring->tx_phys);
}

static int fe_qdma_desc_to_index(struct fe_tx_ring *ring,
				 struct fe_tx_dma *desc)
{
	return desc - ring->tx_dma;
}

static struct fe_tx_dma *fe_tx_next_qdma(struct fe_tx_ring *ring,
					 struct fe_tx_dma *txd)
{
	return fe_qdma_phys_to_virt(ring, txd->txd2);
}

static struct fe_tx_buf *fe_desc_to_tx_buf(struct fe_tx_ring *ring,
					   struct fe_tx_dma *txd)
{
	int idx = fe_qdma_desc_to_index(ring, txd);

	return &ring->tx_buf[idx];
}

static int fe_qdma_tx_map(struct sk_buff *skb, struct net_device *dev,
			  int tx_num, struct fe_tx_ring *ring, bool gso)
{
	struct fe_priv *priv = netdev_priv(dev);
	struct fe_tx_dma *itxd, *txd;
	struct fe_tx_buf *tx_buf;
	dma_addr_t mapped_addr;
	unsigned int nr_frags;
	int i, n_desc = 1;
	u32 txd4 = priv->soc->txd4;

	itxd = ring->tx_next_free;
	if (itxd == ring->tx_last_free)
		return -ENOMEM;

	tx_buf = fe_desc_to_tx_buf(ring, itxd);
	memset(tx_buf, 0, sizeof(*tx_buf));

	if (gso)
		txd4 |= TX_DMA_TSO;

	/* TX Checksum offload */
	if (skb->ip_summed == CHECKSUM_PARTIAL)
		txd4 |= TX_DMA_CHKSUM;

	/* VLAN header offload */
	if (skb_vlan_tag_present(skb))
		txd4 |= TX_DMA_INS_VLAN_MT7621 | skb_vlan_tag_get(skb);

	mapped_addr = dma_map_single(&dev->dev, skb->data,
				     skb_headlen(skb), DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(&dev->dev, mapped_addr)))
		return -ENOMEM;

	WRITE_ONCE(itxd->txd1, mapped_addr);
	tx_buf->flags |= FE_TX_FLAGS_SINGLE0;
	dma_unmap_addr_set(tx_buf, dma_addr0, mapped_addr);
	dma_unmap_len_set(tx_buf, dma_len0, skb_headlen(skb));

	/* TX SG offload */
	txd = itxd;
	nr_frags = skb_shinfo(skb)->nr_frags;
	for (i = 0; i < nr_frags; i++) {
		struct skb_frag_struct *frag = &skb_shinfo(skb)->frags[i];
		unsigned int offset = 0;
		int frag_size = skb_frag_size(frag);

		while (frag_size) {
			bool last_frag = false;
			unsigned int frag_map_size;

			txd = fe_tx_next_qdma(ring, txd);
			if (txd == ring->tx_last_free)
				goto err_dma;

			n_desc++;
			frag_map_size = min(frag_size, TX_DMA_BUF_LEN);
			mapped_addr = skb_frag_dma_map(&dev->dev, frag, offset,
						       frag_map_size,
						       DMA_TO_DEVICE);
			if (unlikely(dma_mapping_error(&dev->dev, mapped_addr)))
				goto err_dma;

			if (i == nr_frags - 1 &&
			    (frag_size - frag_map_size) == 0)
				last_frag = true;

			WRITE_ONCE(txd->txd1, mapped_addr);
			WRITE_ONCE(txd->txd3, (QDMA_TX_SWC |
					       TX_DMA_PLEN0(frag_map_size) |
					       last_frag * TX_DMA_LS0));
			WRITE_ONCE(txd->txd4, 0);

			tx_buf->skb = (struct sk_buff *)DMA_DUMMY_DESC;
			tx_buf = fe_desc_to_tx_buf(ring, txd);
			memset(tx_buf, 0, sizeof(*tx_buf));

			tx_buf->flags |= FE_TX_FLAGS_PAGE0;
			dma_unmap_addr_set(tx_buf, dma_addr0, mapped_addr);
			dma_unmap_len_set(tx_buf, dma_len0, frag_map_size);
			frag_size -= frag_map_size;
			offset += frag_map_size;
		}
	}

	/* store skb to cleanup */
	tx_buf->skb = skb;

	WRITE_ONCE(itxd->txd4, txd4);
	WRITE_ONCE(itxd->txd3, (QDMA_TX_SWC | TX_DMA_PLEN0(skb_headlen(skb)) |
				(!nr_frags * TX_DMA_LS0)));

	netdev_sent_queue(dev, skb->len);
	skb_tx_timestamp(skb);

	ring->tx_next_free = fe_tx_next_qdma(ring, txd);
	atomic_sub(n_desc, &ring->tx_free_count);

	/* make sure that all changes to the dma ring are flushed before we
	 * continue
	 */
	wmb();

	if (netif_xmit_stopped(netdev_get_tx_queue(dev, 0)) || !skb->xmit_more)
		fe_w32(txd->txd2, FE_QTX_CTX_PTR);

	return 0;

err_dma:
	do {
		tx_buf = fe_desc_to_tx_buf(ring, txd);

		/* unmap dma */
		fe_txd_unmap(&dev->dev, tx_buf);

		itxd->txd3 = TX_DMA_DESP2_DEF;
		itxd = fe_tx_next_qdma(ring, itxd);
	} while (itxd != txd);

	return -ENOMEM;
}

static inline int fe_cal_txd_req(struct sk_buff *skb)
{
	int i, nfrags;
	struct skb_frag_struct *frag;

	nfrags = 1;
	if (skb_is_gso(skb)) {
		for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
			frag = &skb_shinfo(skb)->frags[i];
			nfrags += DIV_ROUND_UP(frag->size, TX_DMA_BUF_LEN);
		}
	} else {
		nfrags += skb_shinfo(skb)->nr_frags;
	}

	return DIV_ROUND_UP(nfrags, 2);
}

static int fe_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct fe_priv *priv = netdev_priv(dev);
	struct fe_tx_ring *ring = &priv->tx_ring;
	struct net_device_stats *stats = &dev->stats;
	int tx_num;
	int len = skb->len;
	bool gso = false;

	tx_num = fe_cal_txd_req(skb);
	if (unlikely(atomic_read(&ring->tx_free_count) <= tx_num)) {
		netif_stop_queue(dev);
		netif_err(priv, tx_queued, dev,
			  "Tx Ring full when queue awake!\n");
		return NETDEV_TX_BUSY;
	}

	/* TSO: fill MSS info in tcp checksum field */
	if (skb_is_gso(skb)) {
		if (skb_cow_head(skb, 0)) {
			netif_warn(priv, tx_err, dev,
				   "GSO expand head fail.\n");
			goto drop;
		}

		if (skb_shinfo(skb)->gso_type &
				(SKB_GSO_TCPV4 | SKB_GSO_TCPV6)) {
			gso = true;
			tcp_hdr(skb)->check = htons(skb_shinfo(skb)->gso_size);
		}
	}

	if (ring->tx_map(skb, dev, tx_num, ring, gso) < 0)
		goto drop;

	stats->tx_packets++;
	stats->tx_bytes += len;

	if (unlikely(atomic_read(&ring->tx_free_count) <= ring->tx_thresh)) {
		netif_stop_queue(dev);
		smp_mb();
		if (unlikely(atomic_read(&ring->tx_free_count) >
			     ring->tx_thresh))
			netif_wake_queue(dev);
	}

	return NETDEV_TX_OK;

drop:
	stats->tx_dropped++;
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}

static int fe_poll_rx(struct napi_struct *napi, int budget,
		      struct fe_priv *priv, u32 rx_intr)
{
	struct net_device *netdev = priv->netdev;
	struct net_device_stats *stats = &netdev->stats;
	struct fe_soc_data *soc = priv->soc;
	struct fe_rx_ring *ring = &priv->rx_ring_p;
	int idx = ring->rx_calc_idx;
	u32 checksum_bit;
	struct sk_buff *skb;
	u8 *data, *new_data;
	struct fe_rx_dma *rxd, trxd;
	int done = 0, pad;

	if (netdev->features & NETIF_F_RXCSUM)
		checksum_bit = soc->checksum_bit;
	else
		checksum_bit = 0;

	if (priv->flags & FE_FLAG_RX_2B_OFFSET)
		pad = 0;
	else
		pad = NET_IP_ALIGN;

	while (done < budget) {
		unsigned int pktlen;
		dma_addr_t dma_addr;

		idx = NEXT_RX_DESP_IDX(idx);
		rxd = &ring->rx_dma[idx];
		data = ring->rx_data[idx];

		fe_get_rxd(&trxd, rxd);
		if (!(trxd.rxd2 & RX_DMA_DONE))
			break;

		/* alloc new buffer */
		new_data = napi_alloc_frag(ring->frag_size);
		if (unlikely(!new_data)) {
			stats->rx_dropped++;
			goto release_desc;
		}
		dma_addr = dma_map_single(&netdev->dev,
					  new_data + NET_SKB_PAD + pad,
					  ring->rx_buf_size,
					  DMA_FROM_DEVICE);
		if (unlikely(dma_mapping_error(&netdev->dev, dma_addr))) {
			put_page(virt_to_head_page(new_data));
			goto release_desc;
		}

		/* receive data */
		skb = build_skb(data, ring->frag_size);
		if (unlikely(!skb)) {
			put_page(virt_to_head_page(new_data));
			goto release_desc;
		}
		skb_reserve(skb, NET_SKB_PAD + NET_IP_ALIGN);

		dma_unmap_single(&netdev->dev, trxd.rxd1,
				 ring->rx_buf_size, DMA_FROM_DEVICE);
		pktlen = RX_DMA_GET_PLEN0(trxd.rxd2);
		skb->dev = netdev;
		skb_put(skb, pktlen);
		if (trxd.rxd4 & checksum_bit)
			skb->ip_summed = CHECKSUM_UNNECESSARY;
		else
			skb_checksum_none_assert(skb);
		skb->protocol = eth_type_trans(skb, netdev);

		stats->rx_packets++;
		stats->rx_bytes += pktlen;

		if (netdev->features & NETIF_F_HW_VLAN_CTAG_RX &&
		    trxd.rxd2 & TX_DMA_TAG) {
			u16 vid = trxd.rxd3 & TX_DMA_TAG_MASK;

			__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), vid);
		}
		napi_gro_receive(napi, skb);

		ring->rx_data[idx] = new_data;
		rxd->rxd1 = (unsigned int)dma_addr;

release_desc:
		if (priv->flags & FE_FLAG_RX_SG_DMA)
			rxd->rxd2 = RX_DMA_PLEN0(ring->rx_buf_size);
		else
			rxd->rxd2 = RX_DMA_LSO;

		ring->rx_calc_idx = idx;
		/* make sure that all changes to the dma ring are flushed before
		 * we continue
		 */
		wmb();
		fe_reg_w32(ring->rx_calc_idx, FE_REG_RX_CALC_IDX0);
		done++;
	}

	if (done < budget)
		fe_irq_ack(priv, rx_intr);

	return done;
}

static int fe_pdma_tx_poll(struct fe_priv *priv, int budget, bool *tx_again,
			   unsigned int *bytes)
{
	struct net_device *netdev = priv->netdev;
	struct device *dev = &netdev->dev;
	struct sk_buff *skb;
	struct fe_tx_buf *tx_buf;
	int done = 0;
	u32 idx, hwidx;
	struct fe_tx_ring *ring = &priv->tx_ring;

	idx = ring->tx_free_idx;
	hwidx = fe_reg_r32(FE_REG_TX_DTX_IDX0);

	while ((idx != hwidx) && budget) {
		tx_buf = &ring->tx_buf[idx];
		skb = tx_buf->skb;

		if (!skb)
			break;

		if (skb != (struct sk_buff *)DMA_DUMMY_DESC) {
			*bytes += skb->len;
			done++;
			budget--;
		}
		fe_txd_unmap(dev, tx_buf);
		idx = NEXT_TX_DESP_IDX(idx);
	}
	ring->tx_free_idx = idx;
	atomic_set(&ring->tx_free_count, fe_pdma_empty_txd(ring));

	/* read hw index again make sure no new tx packet */
	if (idx != hwidx || idx != fe_reg_r32(FE_REG_TX_DTX_IDX0))
		*tx_again = 1;

	return done;
}

static int fe_qdma_tx_poll(struct fe_priv *priv, int budget, bool *tx_again,
			   unsigned int *bytes)
{
	struct net_device *netdev = priv->netdev;
	struct fe_tx_ring *ring = &priv->tx_ring;
	struct device *dev = &netdev->dev;
	struct fe_tx_dma *desc;
	struct sk_buff *skb;
	struct fe_tx_buf *tx_buf;
	int done = 0;
	u32 cpu, dma;
	static int condition;

	cpu = fe_r32(FE_QTX_CRX_PTR);
	dma = fe_r32(FE_QTX_DRX_PTR);

	desc = fe_qdma_phys_to_virt(ring, cpu);

	while ((cpu != dma) && budget) {
		u32 next_cpu = desc->txd2;

		desc = fe_tx_next_qdma(ring, desc);
		if ((desc->txd3 & QDMA_TX_OWNER_CPU) == 0)
			break;

		tx_buf = fe_desc_to_tx_buf(ring, desc);
		skb = tx_buf->skb;
		if (!skb) {
			condition = 1;
			break;
		}

		if (skb != (struct sk_buff *)DMA_DUMMY_DESC) {
			*bytes += skb->len;
			done++;
			budget--;
		}
		fe_txd_unmap(dev, tx_buf);

		ring->tx_last_free->txd2 = next_cpu;
		ring->tx_last_free = desc;
		atomic_inc(&ring->tx_free_count);

		cpu = next_cpu;
	}

	fe_w32(cpu, FE_QTX_CRX_PTR);

	/* read hw index again make sure no new tx packet */
	if (cpu != dma || cpu != fe_r32(FE_QTX_DRX_PTR))
		*tx_again = true;

	return done;
}

static int fe_poll_tx(struct fe_priv *priv, int budget, u32 tx_intr,
		      bool *tx_again)
{
	struct fe_tx_ring *ring = &priv->tx_ring;
	struct net_device *netdev = priv->netdev;
	int done, bytes_compl = 0;

	done = priv->tx_ring.tx_poll(priv, budget, tx_again, &bytes_compl);
	if (!*tx_again)
		fe_irq_ack(priv, tx_intr);

	if (!done)
		return 0;

	netdev_completed_queue(netdev, done, bytes_compl);
	smp_mb();
	if (unlikely(!netif_queue_stopped(netdev)))
		return done;

	if (atomic_read(&ring->tx_free_count) > ring->tx_thresh)
		netif_wake_queue(netdev);

	return done;
}

static int fe_poll(struct napi_struct *napi, int budget)
{
	struct fe_priv *priv = container_of(napi, struct fe_priv, rx_napi);
	struct fe_hw_stats *hwstat = priv->hw_stats;
	u32 status, fe_status, mask, tx_intr, rx_intr, status_intr;
	int tx_done, rx_done;
	bool tx_again = false;

	status = fe_irq_pending(priv);
	fe_status = fe_irq_pending_status(priv);
	tx_intr = priv->soc->tx_int;
	rx_intr = priv->soc->rx_int;
	status_intr = priv->soc->status_int;
	tx_done = 0;
	rx_done = 0;
	tx_again = 0;

	if (status & tx_intr)
		tx_done = fe_poll_tx(priv, budget, tx_intr, &tx_again);

	if (status & rx_intr)
		rx_done = fe_poll_rx(napi, budget, priv, rx_intr);

	if (unlikely(fe_status & status_intr)) {
		if (hwstat && spin_trylock(&hwstat->stats_lock)) {
			fe_stats_update(priv);
			spin_unlock(&hwstat->stats_lock);
		}
		fe_irq_ack_status(priv, status_intr);
	}

	if (unlikely(netif_msg_intr(priv))) {
		mask = fe_irq_enabled(priv);
		netdev_info(priv->netdev,
			    "done tx %d, rx %d, intr 0x%08x/0x%x\n",
			    tx_done, rx_done, status, mask);
	}

	if (tx_again || rx_done == budget)
		return budget;

	status = fe_irq_pending(priv);
	if (status & (tx_intr | rx_intr))
		return budget;

	napi_complete(napi);
	fe_irq_enable(priv, tx_intr | rx_intr);

	return rx_done;
}

static int fe_pdma_tx_alloc(struct fe_priv *priv)
{
	int i;
	struct fe_tx_ring *ring = &priv->tx_ring;

	ring->tx_ring_size = NUM_DMA_DESC;
	if (priv->flags & FE_FLAG_NAPI_WEIGHT)
		ring->tx_ring_size *= 4;

	ring->tx_free_idx = 0;
	ring->tx_next_idx = 0;
	ring->tx_thresh = max((unsigned long)ring->tx_ring_size >> 2,
			      MAX_SKB_FRAGS);

	ring->tx_buf = kcalloc(ring->tx_ring_size, sizeof(*ring->tx_buf),
			GFP_KERNEL);
	if (!ring->tx_buf)
		goto no_tx_mem;

	ring->tx_dma = dma_alloc_coherent(&priv->netdev->dev,
			ring->tx_ring_size * sizeof(*ring->tx_dma),
			&ring->tx_phys,
			GFP_ATOMIC | __GFP_ZERO);
	if (!ring->tx_dma)
		goto no_tx_mem;

	for (i = 0; i < ring->tx_ring_size; i++) {
		ring->tx_dma[i].txd2 = TX_DMA_DESP2_DEF;
		ring->tx_dma[i].txd4 = priv->soc->txd4;
	}

	atomic_set(&ring->tx_free_count, fe_pdma_empty_txd(ring));
	ring->tx_map = fe_pdma_tx_map;
	ring->tx_poll = fe_pdma_tx_poll;
	ring->tx_clean = fe_pdma_tx_clean;

	/* make sure that all changes to the dma ring are flushed before we
	 * continue
	 */
	wmb();

	fe_reg_w32(ring->tx_phys, FE_REG_TX_BASE_PTR0);
	fe_reg_w32(ring->tx_ring_size, FE_REG_TX_MAX_CNT0);
	fe_reg_w32(0, FE_REG_TX_CTX_IDX0);
	fe_reg_w32(FE_PST_DTX_IDX0, FE_REG_PDMA_RST_CFG);

	return 0;

no_tx_mem:
	return -ENOMEM;
}

/* the qdma core needs scratch memory to be setup */
static int fq_init_fq_dma(struct fe_priv *priv)
{
	unsigned int phy_ring_head, phy_ring_tail, phy_scratch_head;
	struct fe_tx_dma *ring_head;
	int cnt = NUM_DMA_DESC;
	dma_addr_t dma_addr;
	void *scratch_head;
	int i;

	ring_head = dma_alloc_coherent(&priv->netdev->dev,
				       cnt * sizeof(struct fe_tx_dma),
				       &phy_ring_head,
				       GFP_ATOMIC | __GFP_ZERO);
	if (unlikely(!ring_head))
		return -ENOMEM;

	scratch_head = kcalloc(cnt, QDMA_PAGE_SIZE,
			       GFP_KERNEL);
	dma_addr = dma_map_single(&priv->netdev->dev,
				  scratch_head, cnt * QDMA_PAGE_SIZE,
				  DMA_FROM_DEVICE);
	if (unlikely(dma_mapping_error(&priv->netdev->dev, dma_addr)))
		return -ENOMEM;

	memset(ring_head, 0x0, sizeof(struct fe_tx_dma) * cnt);
	phy_ring_tail = phy_ring_head + (sizeof(struct fe_tx_dma) * (cnt - 1));

	for (i = 0; i < cnt; i++) {
		ring_head[i].txd1 = (phy_scratch_head + (i * QDMA_PAGE_SIZE));
		if (i < cnt - 1)
			ring_head[i].txd2 = (phy_ring_head +
					((i + 1) * sizeof(struct fe_tx_dma)));
		ring_head[i].txd3 = TX_QDMA_SDL(QDMA_PAGE_SIZE);
	}

	fe_w32(phy_ring_head, FE_QDMA_FQ_HEAD);
	fe_w32(phy_ring_tail, FE_QDMA_FQ_TAIL);
	fe_w32((cnt << 16) | cnt, FE_QDMA_FQ_CNT);
	fe_w32(QDMA_PAGE_SIZE << 16, FE_QDMA_FQ_BLEN);

	return 0;
}

static int fe_qdma_tx_alloc_tx(struct fe_priv *priv)
{
	struct fe_tx_ring *ring = &priv->tx_ring;
	int i;

	ring->tx_ring_size = NUM_DMA_DESC;
	if (priv->flags & FE_FLAG_NAPI_WEIGHT)
		ring->tx_ring_size *= 4;

	ring->tx_buf = kcalloc(ring->tx_ring_size, sizeof(*ring->tx_buf),
			GFP_KERNEL);
	if (!ring->tx_buf)
		goto no_tx_mem;

	ring->tx_dma = dma_alloc_coherent(&priv->netdev->dev,
			ring->tx_ring_size * sizeof(*ring->tx_dma),
			&ring->tx_phys,
			GFP_ATOMIC | __GFP_ZERO);
	if (!ring->tx_dma)
		goto no_tx_mem;

	memset(ring->tx_dma, 0, ring->tx_ring_size * sizeof(*ring->tx_dma));
	for (i = 0; i < ring->tx_ring_size; i++) {
		int next = (i + 1) % ring->tx_ring_size;
		u32 next_ptr = ring->tx_phys + next * sizeof(*ring->tx_dma);

		ring->tx_dma[i].txd2 = next_ptr;
		ring->tx_dma[i].txd3 = TX_DMA_DESP2_DEF;
	}

	atomic_set(&ring->tx_free_count, ring->tx_ring_size - 2);
	ring->tx_next_free = &ring->tx_dma[0];
	ring->tx_last_free = &ring->tx_dma[ring->tx_ring_size - 2];
	ring->tx_thresh = max((unsigned long)ring->tx_ring_size >> 2,
			      MAX_SKB_FRAGS);

	ring->tx_map = fe_qdma_tx_map;
	ring->tx_poll = fe_qdma_tx_poll;
	ring->tx_clean = fe_qdma_tx_clean;

	/* make sure that all changes to the dma ring are flushed before we
	 * continue
	 */
	wmb();

	fe_w32(ring->tx_phys, FE_QTX_CTX_PTR);
	fe_w32(ring->tx_phys, FE_QTX_DTX_PTR);
	fe_w32(ring->tx_phys + ((ring->tx_ring_size - 1) * sizeof(*ring->tx_dma)),
	       FE_QTX_CRX_PTR);
	fe_w32(ring->tx_phys + ((ring->tx_ring_size - 1) * sizeof(*ring->tx_dma)),
	       FE_QTX_DRX_PTR);

	return 0;

no_tx_mem:
	return -ENOMEM;
}

static int fe_qdma_init(struct fe_priv *priv)
{
	int err;

	err = fq_init_fq_dma(priv);
	if (err)
		return err;

	err = fe_qdma_tx_alloc_tx(priv);
	if (err)
		return err;

	err = fe_dma_rx_alloc(priv, &priv->rx_ring_q);
	if (err)
		return err;

	fe_w32(priv->rx_ring_q.rx_phys, FE_QRX_BASE_PTR0);
	fe_w32(priv->rx_ring_q.rx_ring_size, FE_QRX_MAX_CNT0);
	fe_w32(priv->rx_ring_q.rx_calc_idx, FE_QRX_CRX_IDX0);
	fe_w32(FE_PST_DRX_IDX0, FE_QDMA_RST_IDX);

	err = fe_dma_rx_alloc(priv, &priv->rx_ring_p);
	if (err)
		return err;

	fe_reg_w32(priv->rx_ring_p.rx_phys, FE_REG_RX_BASE_PTR0);
	fe_reg_w32(priv->rx_ring_p.rx_ring_size, FE_REG_RX_MAX_CNT0);
	fe_reg_w32(priv->rx_ring_p.rx_calc_idx, FE_REG_RX_CALC_IDX0);
	fe_reg_w32(FE_PST_DRX_IDX0, FE_REG_PDMA_RST_CFG);

	/* Enable randon early drop and set drop threshold automatically */
	fe_w32(0x174444, FE_QDMA_FC_THRES);
	fe_w32(0x0, FE_QDMA_HRED2);

	return 0;
}

static int fe_pdma_init(struct fe_priv *priv)
{
	struct fe_rx_ring *ring = &priv->rx_ring_p;
	int err;

	err = fe_pdma_tx_alloc(priv);
	if (err)
		return err;

	err = fe_dma_rx_alloc(priv, ring);
	if (err)
		return err;

	fe_reg_w32(ring->rx_phys, FE_REG_RX_BASE_PTR0);
	fe_reg_w32(ring->rx_ring_size, FE_REG_RX_MAX_CNT0);
	fe_reg_w32(ring->rx_calc_idx, FE_REG_RX_CALC_IDX0);
	fe_reg_w32(FE_PST_DRX_IDX0, FE_REG_PDMA_RST_CFG);

	return 0;
}

static void fe_dma_free(struct fe_priv *priv)
{
	priv->tx_ring.tx_clean(priv);
	netdev_reset_queue(priv->netdev);
	fe_clean_rx(priv, &priv->rx_ring_p);
	fe_clean_rx(priv, &priv->rx_ring_q);
}

static void fe_tx_timeout(struct net_device *dev)
{
	struct fe_priv *priv = netdev_priv(dev);
	struct fe_tx_ring *ring = &priv->tx_ring;

	priv->netdev->stats.tx_errors++;
	netif_err(priv, tx_err, dev,
		  "transmit timed out\n");
	if (priv->soc->dma_type & FE_PDMA) {
		netif_info(priv, drv, dev, "pdma_cfg:%08x\n",
			   fe_reg_r32(FE_REG_PDMA_GLO_CFG));
		netif_info(priv, drv, dev, "tx_ring=%d, "
			   "base=%08x, max=%u, ctx=%u, dtx=%u, fdx=%hu, next=%hu\n",
			   0, fe_reg_r32(FE_REG_TX_BASE_PTR0),
			   fe_reg_r32(FE_REG_TX_MAX_CNT0),
			   fe_reg_r32(FE_REG_TX_CTX_IDX0),
			   fe_reg_r32(FE_REG_TX_DTX_IDX0),
			   ring->tx_free_idx,
			   ring->tx_next_idx);
	}
	if (priv->soc->dma_type & FE_QDMA) {
		netif_info(priv, drv, dev, "qdma_cfg:%08x\n",
			   fe_r32(FE_QDMA_GLO_CFG));
		netif_info(priv, drv, dev, "tx_ring=%d, "
			   "ctx=%08x, dtx=%08x, crx=%08x, drx=%08x, free=%hu\n",
			   0, fe_r32(FE_QTX_CTX_PTR),
			   fe_r32(FE_QTX_DTX_PTR),
			   fe_r32(FE_QTX_CRX_PTR),
			   fe_r32(FE_QTX_DRX_PTR),
			   atomic_read(&ring->tx_free_count));
	}
	netif_info(priv, drv, dev,
		   "rx_ring=%d, base=%08x, max=%u, calc=%u, drx=%u\n",
		   0, fe_reg_r32(FE_REG_RX_BASE_PTR0),
		   fe_reg_r32(FE_REG_RX_MAX_CNT0),
		   fe_reg_r32(FE_REG_RX_CALC_IDX0),
		   fe_reg_r32(FE_REG_RX_DRX_IDX0));

	if (!test_and_set_bit(FE_FLAG_RESET_PENDING, priv->pending_flags))
		schedule_work(&priv->pending_work);
}

static irqreturn_t fe_handle_irq(int irq, void *dev)
{
	struct fe_priv *priv = netdev_priv(dev);
	u32 status, int_mask;

	status = fe_irq_pending(priv);
	if (unlikely(!status))
		return IRQ_NONE;

	int_mask = (priv->soc->rx_int | priv->soc->tx_int);
	if (likely(status & int_mask)) {
		if (likely(napi_schedule_prep(&priv->rx_napi)))
			__napi_schedule(&priv->rx_napi);
	} else {
		fe_irq_ack(priv, status);
	}
	fe_irq_disable(priv, int_mask);

	return IRQ_HANDLED;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void fe_poll_controller(struct net_device *dev)
{
	struct fe_priv *priv = netdev_priv(dev);
	u32 int_mask = priv->soc->tx_int | priv->soc->rx_int;

	fe_irq_disable(priv, int_mask);
	fe_handle_irq(dev->irq, dev);
	fe_irq_enable(priv, int_mask);
}
#endif

int fe_set_clock_cycle(struct fe_priv *priv)
{
	unsigned long sysclk = priv->sysclk;

	sysclk /= FE_US_CYC_CNT_DIVISOR;
	sysclk <<= FE_US_CYC_CNT_SHIFT;

	fe_w32((fe_r32(FE_FE_GLO_CFG) &
			~(FE_US_CYC_CNT_MASK << FE_US_CYC_CNT_SHIFT)) |
			sysclk,
			FE_FE_GLO_CFG);
	return 0;
}

void fe_fwd_config(struct fe_priv *priv)
{
	u32 fwd_cfg;

	fwd_cfg = fe_r32(FE_GDMA1_FWD_CFG);

	/* disable jumbo frame */
	if (priv->flags & FE_FLAG_JUMBO_FRAME)
		fwd_cfg &= ~FE_GDM1_JMB_EN;

	/* set unicast/multicast/broadcast frame to cpu */
	fwd_cfg &= ~0xffff;

	fe_w32(fwd_cfg, FE_GDMA1_FWD_CFG);
}

static void fe_rxcsum_config(bool enable)
{
	if (enable)
		fe_w32(fe_r32(FE_GDMA1_FWD_CFG) | (FE_GDM1_ICS_EN |
					FE_GDM1_TCS_EN | FE_GDM1_UCS_EN),
				FE_GDMA1_FWD_CFG);
	else
		fe_w32(fe_r32(FE_GDMA1_FWD_CFG) & ~(FE_GDM1_ICS_EN |
					FE_GDM1_TCS_EN | FE_GDM1_UCS_EN),
				FE_GDMA1_FWD_CFG);
}

static void fe_txcsum_config(bool enable)
{
	if (enable)
		fe_w32(fe_r32(FE_CDMA_CSG_CFG) | (FE_ICS_GEN_EN |
					FE_TCS_GEN_EN | FE_UCS_GEN_EN),
				FE_CDMA_CSG_CFG);
	else
		fe_w32(fe_r32(FE_CDMA_CSG_CFG) & ~(FE_ICS_GEN_EN |
					FE_TCS_GEN_EN | FE_UCS_GEN_EN),
				FE_CDMA_CSG_CFG);
}

void fe_csum_config(struct fe_priv *priv)
{
	struct net_device *dev = priv_netdev(priv);

	fe_txcsum_config((dev->features & NETIF_F_IP_CSUM));
	fe_rxcsum_config((dev->features & NETIF_F_RXCSUM));
}

static int fe_hw_init(struct net_device *dev)
{
	struct fe_priv *priv = netdev_priv(dev);
	int i, err;

	err = devm_request_irq(priv->device, dev->irq, fe_handle_irq, 0,
			       dev_name(priv->device), dev);
	if (err)
		return err;

	if (priv->soc->set_mac)
		priv->soc->set_mac(priv, dev->dev_addr);
	else
		fe_hw_set_macaddr(priv, dev->dev_addr);

	/* disable delay interrupt */
	fe_reg_w32(0, FE_REG_DLY_INT_CFG);

	fe_irq_disable(priv, priv->soc->tx_int | priv->soc->rx_int);

	/* frame engine will push VLAN tag regarding to VIDX field in Tx desc */
	if (fe_reg_table[FE_REG_FE_DMA_VID_BASE])
		for (i = 0; i < 16; i += 2)
			fe_w32(((i + 1) << 16) + i,
			       fe_reg_table[FE_REG_FE_DMA_VID_BASE] +
			       (i * 2));

	if (priv->soc->fwd_config(priv))
		netdev_err(dev, "unable to get clock\n");

	if (fe_reg_table[FE_REG_FE_RST_GL]) {
		fe_reg_w32(1, FE_REG_FE_RST_GL);
		fe_reg_w32(0, FE_REG_FE_RST_GL);
	}

	return 0;
}

static int fe_open(struct net_device *dev)
{
	struct fe_priv *priv = netdev_priv(dev);
	unsigned long flags;
	u32 val;
	int err;

	if (priv->soc->dma_type == FE_PDMA)
		err = fe_pdma_init(priv);
	else
		err = fe_qdma_init(priv);
	if (err) {
		fe_dma_free(priv);
		return err;
	}

	spin_lock_irqsave(&priv->page_lock, flags);

	val = FE_TX_WB_DDONE | FE_RX_DMA_EN | FE_TX_DMA_EN;
	if (priv->flags & FE_FLAG_RX_2B_OFFSET)
		val |= FE_RX_2B_OFFSET;
	val |= priv->soc->pdma_glo_cfg;

	if (priv->soc->dma_type & FE_PDMA)
		fe_reg_w32(val, FE_REG_PDMA_GLO_CFG);

	if (priv->soc->dma_type & FE_QDMA)
		fe_w32(val, FE_QDMA_GLO_CFG);

	spin_unlock_irqrestore(&priv->page_lock, flags);

	if (priv->phy)
		priv->phy->start(priv);

	if (priv->soc->has_carrier && priv->soc->has_carrier(priv))
		netif_carrier_on(dev);

	napi_enable(&priv->rx_napi);
	fe_irq_enable(priv, priv->soc->tx_int | priv->soc->rx_int);
	netif_start_queue(dev);

	return 0;
}

static void fe_stop_dma(struct fe_priv *priv, u32 glo_cfg)
{
	unsigned long flags;
	u32 val;
	int i;

	spin_lock_irqsave(&priv->page_lock, flags);
	val = fe_r32(glo_cfg);
	fe_w32(val & ~(FE_TX_WB_DDONE | FE_RX_DMA_EN | FE_TX_DMA_EN), glo_cfg);
	spin_unlock_irqrestore(&priv->page_lock, flags);

	/* wait dma stop */
	for (i = 0; i < 10; i++) {
		val = fe_r32(glo_cfg);
		if (val & (FE_TX_DMA_BUSY | FE_RX_DMA_BUSY)) {
			msleep(20);
			continue;
		}
		break;
	}
}

static int fe_stop(struct net_device *dev)
{
	struct fe_priv *priv = netdev_priv(dev);

	netif_tx_disable(dev);
	fe_irq_disable(priv, priv->soc->tx_int | priv->soc->rx_int);
	napi_disable(&priv->rx_napi);

	if (priv->phy)
		priv->phy->stop(priv);

	if (priv->soc->dma_type & FE_PDMA)
		fe_stop_dma(priv, priv->soc->reg_table[FE_REG_PDMA_GLO_CFG]);

	if (priv->soc->dma_type & FE_QDMA)
		fe_stop_dma(priv, FE_QDMA_GLO_CFG);

	fe_dma_free(priv);

	return 0;
}

static int __init fe_init(struct net_device *dev)
{
	struct fe_priv *priv = netdev_priv(dev);
	struct device_node *port;
	const char *mac_addr;
	int err;

	priv->soc->reset_fe();

	if (priv->soc->switch_init)
		if (priv->soc->switch_init(priv)) {
			netdev_err(dev, "failed to initialize switch core\n");
			return -ENODEV;
		}

	mac_addr = of_get_mac_address(priv->device->of_node);
	if (mac_addr)
		ether_addr_copy(dev->dev_addr, mac_addr);

	/* If the mac address is invalid, use random mac address  */
	if (!is_valid_ether_addr(dev->dev_addr)) {
		random_ether_addr(dev->dev_addr);
		dev_err(priv->device, "generated random MAC address %pM\n",
			dev->dev_addr);
	}

	err = fe_mdio_init(priv);
	if (err)
		return err;

	if (priv->soc->port_init)
		for_each_child_of_node(priv->device->of_node, port)
			if (of_device_is_compatible(port,
						    "mediatek,eth-port") &&
			    of_device_is_available(port))
				priv->soc->port_init(priv, port);

	if (priv->phy) {
		err = priv->phy->connect(priv);
		if (err)
			goto err_phy_disconnect;
	}

	err = fe_hw_init(dev);
	if (!err)
		return 0;

err_phy_disconnect:
	if (priv->phy)
		priv->phy->disconnect(priv);
	fe_mdio_cleanup(priv);

	return err;
}

static void fe_uninit(struct net_device *dev)
{
	struct fe_priv *priv = netdev_priv(dev);

	if (priv->phy)
		priv->phy->disconnect(priv);
	fe_mdio_cleanup(priv);

	fe_irq_disable(priv, ~0);
	free_irq(dev->irq, dev);
}

static int fe_do_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct fe_priv *priv = netdev_priv(dev);

	if (!priv->phy_dev)
		return -ENODEV;

	switch (cmd) {
	case SIOCGMIIPHY:
	case SIOCGMIIREG:
	case SIOCSMIIREG:
		return phy_mii_ioctl(priv->phy_dev, ifr, cmd);
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int fe_change_mtu(struct net_device *dev, int new_mtu)
{
	struct fe_priv *priv = netdev_priv(dev);
	int frag_size, old_mtu;
	u32 fwd_cfg;

	if (!(priv->flags & FE_FLAG_JUMBO_FRAME))
		return eth_change_mtu(dev, new_mtu);

	frag_size = fe_max_frag_size(new_mtu);
	if (new_mtu < 68 || frag_size > PAGE_SIZE)
		return -EINVAL;

	old_mtu = dev->mtu;
	dev->mtu = new_mtu;

	/* return early if the buffer sizes will not change */
	if (old_mtu <= ETH_DATA_LEN && new_mtu <= ETH_DATA_LEN)
		return 0;
	if (old_mtu > ETH_DATA_LEN && new_mtu > ETH_DATA_LEN)
		return 0;

	if (new_mtu <= ETH_DATA_LEN)
		priv->rx_ring_p.frag_size = fe_max_frag_size(ETH_DATA_LEN);
	else
		priv->rx_ring_p.frag_size = PAGE_SIZE;
	priv->rx_ring_p.rx_buf_size =
				fe_max_buf_size(priv->rx_ring_p.frag_size);

	if (!netif_running(dev))
		return 0;

	fe_stop(dev);
	fwd_cfg = fe_r32(FE_GDMA1_FWD_CFG);
	if (new_mtu <= ETH_DATA_LEN) {
		fwd_cfg &= ~FE_GDM1_JMB_EN;
	} else {
		fwd_cfg &= ~(FE_GDM1_JMB_LEN_MASK << FE_GDM1_JMB_LEN_SHIFT);
		fwd_cfg |= (DIV_ROUND_UP(frag_size, 1024) <<
				FE_GDM1_JMB_LEN_SHIFT) | FE_GDM1_JMB_EN;
	}
	fe_w32(fwd_cfg, FE_GDMA1_FWD_CFG);

	return fe_open(dev);
}

static const struct net_device_ops fe_netdev_ops = {
	.ndo_init		= fe_init,
	.ndo_uninit		= fe_uninit,
	.ndo_open		= fe_open,
	.ndo_stop		= fe_stop,
	.ndo_start_xmit		= fe_start_xmit,
	.ndo_set_mac_address	= fe_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_do_ioctl		= fe_do_ioctl,
	.ndo_change_mtu		= fe_change_mtu,
	.ndo_tx_timeout		= fe_tx_timeout,
	.ndo_get_stats64        = fe_get_stats64,
	.ndo_vlan_rx_add_vid	= fe_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid	= fe_vlan_rx_kill_vid,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= fe_poll_controller,
#endif
};

static void fe_reset_pending(struct fe_priv *priv)
{
	struct net_device *dev = priv->netdev;
	int err;

	rtnl_lock();
	fe_stop(dev);

	err = fe_open(dev);
	if (err) {
		netif_alert(priv, ifup, dev,
			    "Driver up/down cycle failed, closing device.\n");
		dev_close(dev);
	}
	rtnl_unlock();
}

static const struct fe_work_t fe_work[] = {
	{FE_FLAG_RESET_PENDING, fe_reset_pending},
};

static void fe_pending_work(struct work_struct *work)
{
	struct fe_priv *priv = container_of(work, struct fe_priv, pending_work);
	int i;
	bool pending;

	for (i = 0; i < ARRAY_SIZE(fe_work); i++) {
		pending = test_and_clear_bit(fe_work[i].bitnr,
					     priv->pending_flags);
		if (pending)
			fe_work[i].action(priv);
	}
}

static int fe_probe(struct platform_device *pdev)
{
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	const struct of_device_id *match;
	struct fe_soc_data *soc;
	struct net_device *netdev;
	struct fe_priv *priv;
	struct clk *sysclk;
	int err, napi_weight;

	device_reset(&pdev->dev);

	match = of_match_device(of_fe_match, &pdev->dev);
	soc = (struct fe_soc_data *)match->data;

	if (soc->reg_table)
		fe_reg_table = soc->reg_table;
	else
		soc->reg_table = fe_reg_table;

	fe_base = devm_ioremap_resource(&pdev->dev, res);
	if (!fe_base) {
		err = -EADDRNOTAVAIL;
		goto err_out;
	}

	netdev = alloc_etherdev(sizeof(*priv));
	if (!netdev) {
		dev_err(&pdev->dev, "alloc_etherdev failed\n");
		err = -ENOMEM;
		goto err_iounmap;
	}

	SET_NETDEV_DEV(netdev, &pdev->dev);
	netdev->netdev_ops = &fe_netdev_ops;
	netdev->base_addr = (unsigned long)fe_base;

	netdev->irq = platform_get_irq(pdev, 0);
	if (netdev->irq < 0) {
		dev_err(&pdev->dev, "no IRQ resource found\n");
		err = -ENXIO;
		goto err_free_dev;
	}

	if (soc->init_data)
		soc->init_data(soc, netdev);
	netdev->vlan_features = netdev->hw_features &
		~(NETIF_F_HW_VLAN_CTAG_TX | NETIF_F_HW_VLAN_CTAG_RX);
	netdev->features |= netdev->hw_features;

	/* fake rx vlan filter func. to support tx vlan offload func */
	if (fe_reg_table[FE_REG_FE_DMA_VID_BASE])
		netdev->features |= NETIF_F_HW_VLAN_CTAG_FILTER;

	priv = netdev_priv(netdev);
	spin_lock_init(&priv->page_lock);
	if (fe_reg_table[FE_REG_FE_COUNTER_BASE]) {
		priv->hw_stats = kzalloc(sizeof(*priv->hw_stats), GFP_KERNEL);
		if (!priv->hw_stats) {
			err = -ENOMEM;
			goto err_free_dev;
		}
		spin_lock_init(&priv->hw_stats->stats_lock);
	}

	sysclk = devm_clk_get(&pdev->dev, NULL);
	if (!IS_ERR(sysclk)) {
		priv->sysclk = clk_get_rate(sysclk);
	} else if ((priv->flags & FE_FLAG_CALIBRATE_CLK)) {
		dev_err(&pdev->dev, "this soc needs a clk for calibration\n");
		err = -ENXIO;
		goto err_free_dev;
	}

	priv->switch_np = of_parse_phandle(pdev->dev.of_node,
					   "mediatek,switch", 0);
	if ((priv->flags & FE_FLAG_HAS_SWITCH) && !priv->switch_np) {
		dev_err(&pdev->dev, "failed to read switch phandle\n");
		err = -ENODEV;
		goto err_free_dev;
	}

	priv->netdev = netdev;
	priv->device = &pdev->dev;
	priv->soc = soc;
	priv->msg_enable = netif_msg_init(fe_msg_level, FE_DEFAULT_MSG_ENABLE);
	INIT_WORK(&priv->pending_work, fe_pending_work);

	napi_weight = 32;
	if (priv->flags & FE_FLAG_NAPI_WEIGHT)
		napi_weight *= 4;
	netif_napi_add(netdev, &priv->rx_napi, fe_poll, napi_weight);
	fe_set_ethtool_ops(netdev);

	err = register_netdev(netdev);
	if (err) {
		dev_err(&pdev->dev, "error bringing up device\n");
		goto err_free_dev;
	}

	platform_set_drvdata(pdev, netdev);

	netif_info(priv, probe, netdev, "mediatek frame engine at 0x%08lx, irq %d\n",
		   netdev->base_addr, netdev->irq);

	return 0;

err_free_dev:
	free_netdev(netdev);
err_iounmap:
	devm_iounmap(&pdev->dev, fe_base);
err_out:
	return err;
}

static int fe_remove(struct platform_device *pdev)
{
	struct net_device *dev = platform_get_drvdata(pdev);
	struct fe_priv *priv = netdev_priv(dev);

	netif_napi_del(&priv->rx_napi);
	kfree(priv->hw_stats);

	cancel_work_sync(&priv->pending_work);

	unregister_netdev(dev);
	free_netdev(dev);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_driver fe_driver = {
	.probe = fe_probe,
	.remove = fe_remove,
	.driver = {
		.name = "mtk_soc_eth",
		.owner = THIS_MODULE,
		.of_match_table = of_fe_match,
	},
};

module_platform_driver(fe_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("John Crispin <blogic@openwrt.org>");
MODULE_DESCRIPTION("Ethernet driver for Ralink SoC");
