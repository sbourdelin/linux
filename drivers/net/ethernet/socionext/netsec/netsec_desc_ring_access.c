/**
 * drivers/net/ethernet/socionext/netsec/netsec_desc_ring_access.c
 *
 *  Copyright (C) 2011-2014 Fujitsu Semiconductor Limited.
 *  Copyright (C) 2014 Linaro Ltd  Andy Green <andy.green@linaro.org>
 *  All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */

#include <linux/spinlock.h>
#include <linux/dma-mapping.h>

#include "netsec.h"

static const u32 ads_irq_set[] = {
	NETSEC_REG_NRM_TX_INTEN_SET,
	NETSEC_REG_NRM_RX_INTEN_SET,
};

static const u32 desc_ring_irq_inten_clr_reg_addr[] = {
	NETSEC_REG_NRM_TX_INTEN_CLR,
	NETSEC_REG_NRM_RX_INTEN_CLR,
};

static const u32 int_tmr_reg_addr[] = {
	NETSEC_REG_NRM_TX_TXINT_TMR,
	NETSEC_REG_NRM_RX_RXINT_TMR,
};

static const u32 rx_pkt_cnt_reg_addr[] = {
	0,
	NETSEC_REG_NRM_RX_PKTCNT,
};

static const u32 tx_pkt_cnt_reg_addr[] = {
	NETSEC_REG_NRM_TX_PKTCNT,
	0,
};

static const u32 int_pkt_cnt_reg_addr[] = {
	NETSEC_REG_NRM_TX_DONE_TXINT_PKTCNT,
	NETSEC_REG_NRM_RX_RXINT_PKTCNT,
};

static const u32 tx_done_pkt_addr[] = {
	NETSEC_REG_NRM_TX_DONE_PKTCNT,
	0,
};

static const u32 netsec_desc_mask[] = {
	[NETSEC_RING_TX] = NETSEC_GMAC_OMR_REG_ST,
	[NETSEC_RING_RX] = NETSEC_GMAC_OMR_REG_SR
};

void netsec_ring_irq_enable(struct netsec_priv *priv,
			    enum netsec_rings id, u32 irqf)
{
	netsec_writel(priv, ads_irq_set[id], irqf);
}

void netsec_ring_irq_disable(struct netsec_priv *priv,
			     enum netsec_rings id, u32 irqf)
{
	netsec_writel(priv, desc_ring_irq_inten_clr_reg_addr[id], irqf);
}

static struct sk_buff *alloc_rx_pkt_buf(struct netsec_priv *priv,
					struct netsec_frag_info *info)
{
	struct sk_buff *skb;

	skb = netdev_alloc_skb_ip_align(priv->ndev, info->len);
	if (!skb)
		return NULL;

	netsec_mark_skb_type(skb, NETSEC_RING_RX);
	info->addr = skb->data;
	info->dma_addr = dma_map_single(priv->dev, info->addr, info->len,
					DMA_FROM_DEVICE);
	if (dma_mapping_error(priv->dev, info->dma_addr)) {
		dev_kfree_skb(skb);
		return NULL;
	}

	return skb;
}

int netsec_alloc_desc_ring(struct netsec_priv *priv, enum netsec_rings id)
{
	struct netsec_desc_ring *desc = &priv->desc_ring[id];
	int ret = 0;

	desc->id = id;
	desc->len = sizeof(struct netsec_tx_de); /* rx and tx desc same size */

	spin_lock_init(&desc->spinlock_desc);

	desc->ring_vaddr = dma_zalloc_coherent(priv->dev, desc->len * DESC_NUM,
					       &desc->desc_phys, GFP_KERNEL);
	if (!desc->ring_vaddr) {
		ret = -ENOMEM;
		goto err;
	}

	desc->frag = kcalloc(DESC_NUM, sizeof(*desc->frag), GFP_KERNEL);
	if (!desc->frag) {
		ret = -ENOMEM;
		goto err;
	}

	desc->priv = kcalloc(DESC_NUM, sizeof(struct sk_buff *), GFP_KERNEL);
	if (!desc->priv) {
		ret = -ENOMEM;
		goto err;
	}

	return 0;

err:
	netsec_free_desc_ring(priv, desc);

	return ret;
}

static void netsec_uninit_pkt_desc_ring(struct netsec_priv *priv,
					struct netsec_desc_ring *desc)
{
	struct netsec_frag_info *frag;
	u32 status;
	u16 idx;

	for (idx = 0; idx < DESC_NUM; idx++) {
		frag = &desc->frag[idx];
		if (!frag->addr)
			continue;

		status = *(u32 *)(desc->ring_vaddr + desc->len * idx);

		dma_unmap_single(priv->dev, frag->dma_addr, frag->len,
				 skb_is_rx(desc->priv[idx]) ? DMA_FROM_DEVICE :
							      DMA_TO_DEVICE);
		if ((status >> NETSEC_TX_LAST) & 1)
			dev_kfree_skb(desc->priv[idx]);
	}

	memset(desc->frag, 0, sizeof(struct netsec_frag_info) * DESC_NUM);
	memset(desc->priv, 0, sizeof(struct sk_buff *) * DESC_NUM);
	memset(desc->ring_vaddr, 0, desc->len * DESC_NUM);
}

void netsec_free_desc_ring(struct netsec_priv *priv,
			   struct netsec_desc_ring *desc)
{
	if (desc->ring_vaddr && desc->frag && desc->priv)
		netsec_uninit_pkt_desc_ring(priv, desc);

	if (desc->ring_vaddr) {
		dma_free_coherent(priv->dev, desc->len * DESC_NUM,
				  desc->ring_vaddr, desc->desc_phys);
		desc->ring_vaddr = NULL;
	}
	kfree(desc->frag);
	desc->frag = NULL;
	kfree(desc->priv);
	desc->priv = NULL;
}

static void netsec_set_rx_de(struct netsec_priv *priv,
			     struct netsec_desc_ring *desc, u16 idx,
			     const struct netsec_frag_info *info,
			     struct sk_buff *skb)
{
	struct netsec_rx_de *de = desc->ring_vaddr + desc->len * idx;
	u32 attr = 1 << NETSEC_RX_PKT_OWN_FIELD | 1 << NETSEC_RX_PKT_FS_FIELD |
			       1 << NETSEC_RX_PKT_LS_FIELD;

	if (idx == DESC_NUM - 1)
		attr |= 1 << NETSEC_RX_PKT_LD_FIELD;

	de->data_buf_addr_up = info->dma_addr >> 32;
	de->data_buf_addr_lw = info->dma_addr & 0xffffffff;
	de->buf_len_info = info->len;
	/* desc->attr makes the descriptor live, so it must be physically
	 * written last after the rest of the descriptor body is already there
	 */
	wmb();
	de->attr = attr;

	desc->frag[idx].dma_addr = info->dma_addr;
	desc->frag[idx].addr = info->addr;
	desc->frag[idx].len = info->len;

	desc->priv[idx] = skb;
}

int netsec_setup_rx_desc(struct netsec_priv *priv,
			 struct netsec_desc_ring *desc)
{
	struct netsec_frag_info info;
	struct sk_buff *skb;
	int n;

	info.len = priv->rx_pkt_buf_len;

	for (n = 0; n < DESC_NUM; n++) {
		skb = alloc_rx_pkt_buf(priv, &info);
		if (!skb) {
			netsec_uninit_pkt_desc_ring(priv, desc);
			return -ENOMEM;
		}
		netsec_set_rx_de(priv, desc, n, &info, skb);
	}

	return 0;
}

static void netsec_set_tx_desc_entry(struct netsec_priv *priv,
				     struct netsec_desc_ring *desc,
				     const struct netsec_tx_pkt_ctrl *tx_ctrl,
				     bool first_flag, bool last_flag,
				     const struct netsec_frag_info *frag,
				     struct sk_buff *skb)
{
	struct netsec_tx_de tx_desc_entry;
	int idx = desc->head;

	memset(&tx_desc_entry, 0, sizeof(struct netsec_tx_de));

	tx_desc_entry.attr = (1 << NETSEC_TX_SHIFT_OWN_FIELD) |
			     (desc->id << NETSEC_TX_SHIFT_DRID_FIELD) |
			     (1 << NETSEC_TX_SHIFT_PT_FIELD) |
			     (NETSEC_RING_GMAC << NETSEC_TX_SHIFT_TDRID_FIELD) |
			     (first_flag << NETSEC_TX_SHIFT_FS_FIELD) |
			     (last_flag << NETSEC_TX_LAST) |
			     (tx_ctrl->cksum_offload_flag <<
			      NETSEC_TX_SHIFT_CO) |
			     (tx_ctrl->tcp_seg_offload_flag <<
			      NETSEC_TX_SHIFT_SO) |
			     (1 << NETSEC_TX_SHIFT_TRS_FIELD);
	if (idx == DESC_NUM - 1)
		tx_desc_entry.attr |= (1 << NETSEC_TX_SHIFT_LD_FIELD);

	tx_desc_entry.data_buf_addr_up = frag->dma_addr >> 32;
	tx_desc_entry.data_buf_addr_lw = frag->dma_addr & 0xffffffff;
	tx_desc_entry.buf_len_info = (tx_ctrl->tcp_seg_len << 16) | frag->len;

	memcpy(desc->ring_vaddr + (desc->len * idx), &tx_desc_entry, desc->len);

	desc->frag[idx].dma_addr = frag->dma_addr;
	desc->frag[idx].addr = frag->addr;
	desc->frag[idx].len = frag->len;

	desc->priv[idx] = skb;
}

static void netsec_get_rx_de(struct netsec_priv *priv,
			     struct netsec_desc_ring *desc, u16 idx,
			     struct netsec_rx_pkt_info *rxpi,
			     struct netsec_frag_info *frag, u16 *len,
			     struct sk_buff **skb)
{
	struct netsec_rx_de de;

	memset(&de, 0, sizeof(struct netsec_rx_de));
	memset(rxpi, 0, sizeof(struct netsec_rx_pkt_info));
	memcpy(&de, ((void *)desc->ring_vaddr + desc->len * idx), desc->len);

	dev_dbg(priv->dev, "%08x\n", *(u32 *)&de);
	*len = de.buf_len_info >> 16;

	rxpi->is_fragmented = (de.attr >> NETSEC_RX_PKT_FR_FIELD) & 1;
	rxpi->err_flag = (de.attr >> NETSEC_RX_PKT_ER_FIELD) & 1;
	rxpi->rx_cksum_result = (de.attr >> NETSEC_RX_PKT_CO_FIELD) & 3;
	rxpi->err_code = (de.attr >> NETSEC_RX_PKT_ERR_FIELD) &
							NETSEC_RX_PKT_ERR_MASK;
	memcpy(frag, &desc->frag[idx], sizeof(*frag));
	*skb = desc->priv[idx];
}

static void netsec_inc_desc_head_idx(struct netsec_priv *priv,
				     struct netsec_desc_ring *desc, u16 inc)
{
	u32 sum;

	sum = desc->head + inc;

	if (sum >= DESC_NUM)
		sum -= DESC_NUM;

	desc->head = sum;
	desc->full = desc->head == desc->tail;
}

static void netsec_inc_desc_tail_idx(struct netsec_priv *priv,
				     struct netsec_desc_ring *desc)
{
	u32 sum;

	sum = desc->tail + 1;

	if (sum >= DESC_NUM)
		sum -= DESC_NUM;

	desc->tail = sum;
	desc->full = false;
}

static u16 netsec_get_tx_avail_num_sub(struct netsec_priv *priv,
				       const struct netsec_desc_ring *desc)
{
	if (desc->full)
		return 0;

	if (desc->tail > desc->head)
		return desc->tail - desc->head;

	return DESC_NUM + desc->tail - desc->head;
}

static u16 netsec_get_tx_done_num_sub(struct netsec_priv *priv,
				      struct netsec_desc_ring *desc)
{
	desc->tx_done_num += netsec_readl(priv, tx_done_pkt_addr[desc->id]);

	return desc->tx_done_num;
}

static int netsec_set_irq_coalesce_param(struct netsec_priv *priv,
					 enum netsec_rings id)
{
	int max_frames, tmr;

	switch (id) {
	case NETSEC_RING_TX:
		max_frames = priv->et_coalesce.tx_max_coalesced_frames;
		tmr = priv->et_coalesce.tx_coalesce_usecs;
		break;
	case NETSEC_RING_RX:
		max_frames = priv->et_coalesce.rx_max_coalesced_frames;
		tmr = priv->et_coalesce.rx_coalesce_usecs;
		break;
	default:
		return -EINVAL;
	}

	netsec_writel(priv, int_pkt_cnt_reg_addr[id], max_frames);
	netsec_writel(priv, int_tmr_reg_addr[id], ((tmr != 0) << 31) | tmr);

	return 0;
}

int netsec_start_desc_ring(struct netsec_priv *priv, enum netsec_rings id)
{
	struct netsec_desc_ring *desc = &priv->desc_ring[id];
	int ret = 0;

	spin_lock_bh(&desc->spinlock_desc);

	if (desc->running) {
		ret = -EBUSY;
		goto err;
	}

	switch (desc->id) {
	case NETSEC_RING_RX:
		netsec_writel(priv, ads_irq_set[id], NETSEC_IRQ_RCV);
		break;
	case NETSEC_RING_TX:
		netsec_writel(priv, ads_irq_set[id], NETSEC_IRQ_EMPTY);
		break;
	}

	netsec_set_irq_coalesce_param(priv, desc->id);
	desc->running = true;

err:
	spin_unlock_bh(&desc->spinlock_desc);

	return ret;
}

void netsec_stop_desc_ring(struct netsec_priv *priv, enum netsec_rings id)
{
	struct netsec_desc_ring *desc = &priv->desc_ring[id];

	spin_lock_bh(&desc->spinlock_desc);
	if (desc->running)
		netsec_writel(priv, desc_ring_irq_inten_clr_reg_addr[id],
			      NETSEC_IRQ_RCV | NETSEC_IRQ_EMPTY |
			      NETSEC_IRQ_SND);

	desc->running = false;
	spin_unlock_bh(&desc->spinlock_desc);
}

u16 netsec_get_rx_num(struct netsec_priv *priv)
{
	struct netsec_desc_ring *desc = &priv->desc_ring[NETSEC_RING_RX];
	u32 result;

	spin_lock(&desc->spinlock_desc);
	if (desc->running) {
		result = netsec_readl(priv,
				      rx_pkt_cnt_reg_addr[NETSEC_RING_RX]);
		desc->rx_num += result;
		if (result)
			netsec_inc_desc_head_idx(priv, desc, result);
	}
	spin_unlock(&desc->spinlock_desc);

	return desc->rx_num;
}

u16 netsec_get_tx_avail_num(struct netsec_priv *priv)
{
	struct netsec_desc_ring *desc = &priv->desc_ring[NETSEC_RING_TX];
	u16 result;

	spin_lock(&desc->spinlock_desc);

	if (!desc->running) {
		netif_err(priv, drv, priv->ndev,
			  "%s: not running tx desc\n", __func__);
		result = 0;
		goto err;
	}

	result = netsec_get_tx_avail_num_sub(priv, desc);

err:
	spin_unlock(&desc->spinlock_desc);

	return result;
}

int netsec_clean_tx_desc_ring(struct netsec_priv *priv)
{
	struct netsec_desc_ring *desc = &priv->desc_ring[NETSEC_RING_TX];
	unsigned int pkts = 0, bytes = 0;
	struct netsec_frag_info *frag;
	struct netsec_tx_de *entry;
	bool is_last;

	spin_lock(&desc->spinlock_desc);

	netsec_get_tx_done_num_sub(priv, desc);

	while ((desc->tail != desc->head || desc->full) && desc->tx_done_num) {
		frag = &desc->frag[desc->tail];
		entry = desc->ring_vaddr + desc->len * desc->tail;
		is_last = (entry->attr >> NETSEC_TX_LAST) & 1;

		dma_unmap_single(priv->dev, frag->dma_addr, frag->len,
				 DMA_TO_DEVICE);
		if (is_last) {
			pkts++;
			bytes += desc->priv[desc->tail]->len;
			dev_kfree_skb(desc->priv[desc->tail]);
		}
		memset(frag, 0, sizeof(*frag));
		netsec_inc_desc_tail_idx(priv, desc);

		if (is_last)
			desc->tx_done_num--;
	}

	spin_unlock(&desc->spinlock_desc);

	priv->ndev->stats.tx_packets += pkts;
	priv->ndev->stats.tx_bytes += bytes;

	netdev_completed_queue(priv->ndev, pkts, bytes);

	return 0;
}

int netsec_clean_rx_desc_ring(struct netsec_priv *priv)
{
	struct netsec_desc_ring *desc = &priv->desc_ring[NETSEC_RING_RX];

	spin_lock(&desc->spinlock_desc);

	while (desc->full || (desc->tail != desc->head)) {
		netsec_set_rx_de(priv, desc, desc->tail,
				 &desc->frag[desc->tail],
				 desc->priv[desc->tail]);
		desc->rx_num--;
		netsec_inc_desc_tail_idx(priv, desc);
	}

	spin_unlock(&desc->spinlock_desc);

	return 0;
}

int netsec_set_tx_pkt_data(struct netsec_priv *priv,
			   const struct netsec_tx_pkt_ctrl *tx_ctrl,
			   u8 count_frags, const struct netsec_frag_info *info,
			   struct sk_buff *skb)
{
	struct netsec_desc_ring *desc;
	u32 sum_len = 0;
	unsigned int i;
	int ret = 0;

	if (tx_ctrl->tcp_seg_offload_flag && !tx_ctrl->cksum_offload_flag)
		return -EINVAL;

	if (tx_ctrl->tcp_seg_offload_flag) {
		if (tx_ctrl->tcp_seg_len == 0)
			return -EINVAL;

		if (priv->param.use_jumbo_pkt_flag) {
			if (tx_ctrl->tcp_seg_len > NETSEC_TCP_JUMBO_SEG_LEN_MAX)
				return -EINVAL;
		} else {
			if (tx_ctrl->tcp_seg_len > NETSEC_TCP_SEG_LEN_MAX)
				return -EINVAL;
		}
	} else {
		if (tx_ctrl->tcp_seg_len)
			return -EINVAL;
	}

	if (!count_frags)
		return -ERANGE;

	for (i = 0; i < count_frags; i++) {
		if ((info[i].len == 0) || (info[i].len > 0xffff)) {
			netif_err(priv, drv, priv->ndev,
				  "%s: bad info len\n", __func__);
			return -EINVAL;
		}
		sum_len += info[i].len;
	}

	if (!tx_ctrl->tcp_seg_offload_flag) {
		if (priv->param.use_jumbo_pkt_flag) {
			if (sum_len > NETSEC_MAX_TX_JUMBO_PKT_LEN)
				return -EINVAL;
		} else {
			if (sum_len > NETSEC_MAX_TX_PKT_LEN)
				return -EINVAL;
		}
	}

	desc = &priv->desc_ring[NETSEC_RING_TX];
	spin_lock(&desc->spinlock_desc);

	if (!desc->running) {
		ret = -ENODEV;
		goto end;
	}

	smp_rmb(); /* we need to see a consistent view of pending tx count */
	if (count_frags > netsec_get_tx_avail_num_sub(priv, desc)) {
		ret = -EBUSY;
		goto end;
	}

	for (i = 0; i < count_frags; i++) {
		netsec_set_tx_desc_entry(priv, desc, tx_ctrl, i == 0,
					 i == count_frags - 1, &info[i], skb);
		netsec_inc_desc_head_idx(priv, desc, 1);
	}

	wmb(); /* ensure the descriptor is flushed */
	netsec_writel(priv, tx_pkt_cnt_reg_addr[NETSEC_RING_TX], 1);

end:
	spin_unlock(&desc->spinlock_desc);

	return ret;
}

int netsec_get_rx_pkt_data(struct netsec_priv *priv,
			   struct netsec_rx_pkt_info *rxpi,
			   struct netsec_frag_info *frag, u16 *len,
			   struct sk_buff **skb)
{
	struct netsec_desc_ring *desc = &priv->desc_ring[NETSEC_RING_RX];
	struct netsec_frag_info info;
	struct sk_buff *tmp_skb;
	int ret = 0;

	spin_lock(&desc->spinlock_desc);

	if (desc->rx_num == 0) {
		dev_err(priv->dev, "%s 0 len rx\n", __func__);
		ret = -EINVAL;
		goto err;
	}

	info.len = priv->rx_pkt_buf_len;
	rmb(); /* we need to ensure we only see current data in descriptor */
	tmp_skb = alloc_rx_pkt_buf(priv, &info);
	if (!tmp_skb) {
		netsec_set_rx_de(priv, desc, desc->tail,
				 &desc->frag[desc->tail],
				 desc->priv[desc->tail]);
		ret = -ENOMEM;
	} else {
		netsec_get_rx_de(priv, desc, desc->tail, rxpi, frag, len, skb);
		netsec_set_rx_de(priv, desc, desc->tail, &info, tmp_skb);
	}

	netsec_inc_desc_tail_idx(priv, desc);
	desc->rx_num--;

err:
	spin_unlock(&desc->spinlock_desc);

	return ret;
}
