/*
 * Huawei HiNIC PCI Express Linux driver
 * Copyright(c) 2017 Huawei Technologies Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/u64_stats_sync.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/skbuff.h>
#include <linux/smp.h>
#include <asm/byteorder.h>

#include "hinic_common.h"
#include "hinic_hw_if.h"
#include "hinic_hw_wq.h"
#include "hinic_hw_qp.h"
#include "hinic_hw_dev.h"
#include "hinic_dev.h"
#include "hinic_tx.h"

#define TX_IRQ_NO_PENDING		0
#define TX_IRQ_NO_COALESC		0
#define TX_IRQ_NO_LLI_TIMER		0
#define TX_IRQ_NO_CREDIT		0
#define TX_IRQ_NO_RESEND_TIMER		0

#define CI_UPDATE_NO_PENDING		0
#define CI_UPDATE_NO_COALESC		0

#define HW_CONS_IDX(sq)		be16_to_cpu(*(u16 *)((sq)->hw_ci_addr))

#define MIN_SKB_LEN		64

/**
 * hinic_txq_clean_stats - Clean the statistics of specific queue
 * @txq: Logical Tx Queue
 **/
void hinic_txq_clean_stats(struct hinic_txq *txq)
{
	struct hinic_txq_stats *txq_stats = &txq->txq_stats;

	u64_stats_update_begin(&txq_stats->syncp);
	txq_stats->pkts = 0;
	txq_stats->bytes = 0;
	txq_stats->tx_busy = 0;
	txq_stats->tx_wake = 0;
	txq_stats->tx_dropped = 0;
	u64_stats_update_end(&txq_stats->syncp);
}

/**
 * txq_stats_init - Initialize the statistics of specific queue
 * @txq: Logical Tx Queue
 **/
static void txq_stats_init(struct hinic_txq *txq)
{
	struct hinic_txq_stats *txq_stats = &txq->txq_stats;

	u64_stats_init(&txq_stats->syncp);
	hinic_txq_clean_stats(txq);
}

/**
 * tx_map_skb - dma mapping for skb and return sges
 * @nic_dev: nic device
 * @skb: the skb
 * @sges: returned sges
 *
 * Return 0 - Success, negative - Failure
 **/
static int tx_map_skb(struct hinic_dev *nic_dev, struct sk_buff *skb,
		      struct hinic_sge *sges)
{
	struct hinic_hwdev *hwdev = nic_dev->hwdev;
	struct hinic_hwif *hwif = hwdev->hwif;
	struct pci_dev *pdev = hwif->pdev;
	dma_addr_t dma_addr;
	struct skb_frag_struct *frag;
	int i, j;

	dma_addr = dma_map_single(&pdev->dev, skb->data, skb_headlen(skb),
				  DMA_TO_DEVICE);
	if (dma_mapping_error(&pdev->dev, dma_addr)) {
		dev_err(&pdev->dev, "Failed to map Tx skb data\n");
		return -EFAULT;
	}

	hinic_set_sge(&sges[0], dma_addr, skb_headlen(skb));

	for (i = 0 ; i < skb_shinfo(skb)->nr_frags; i++) {
		frag = &(skb_shinfo(skb)->frags[i]);

		dma_addr = skb_frag_dma_map(&pdev->dev, frag, 0,
					    skb_frag_size(frag),
					    DMA_TO_DEVICE);
		if (dma_mapping_error(&pdev->dev, dma_addr)) {
			dev_err(&pdev->dev, "Failed to map Tx skb frag\n");
			goto tx_map_err;
		}

		hinic_set_sge(&sges[i + 1], dma_addr, skb_frag_size(frag));
	}

	return 0;

tx_map_err:
	for (j = 0; j < i; j++)
		dma_unmap_page(&pdev->dev, hinic_sge_to_dma(&sges[j + 1]),
			       sges[j + 1].len, DMA_TO_DEVICE);

	dma_unmap_single(&pdev->dev, hinic_sge_to_dma(&sges[0]), sges[0].len,
			 DMA_TO_DEVICE);
	return -EFAULT;
}

/**
 * tx_unmap_skb - unmap the dma address of the skb
 * @nic_dev: nic device
 * @skb: the skb
 * @sges: the sges that are connected to the skb
 **/
static void tx_unmap_skb(struct hinic_dev *nic_dev, struct sk_buff *skb,
			 struct hinic_sge *sges)
{
	struct hinic_hwdev *hwdev = nic_dev->hwdev;
	struct hinic_hwif *hwif = hwdev->hwif;
	struct pci_dev *pdev = hwif->pdev;
	int i;

	for (i = 0; i < skb_shinfo(skb)->nr_frags ; i++)
		dma_unmap_page(&pdev->dev, hinic_sge_to_dma(&sges[i + 1]),
			       sges[i + 1].len, DMA_TO_DEVICE);

	dma_unmap_single(&pdev->dev, hinic_sge_to_dma(&sges[0]), sges[0].len,
			 DMA_TO_DEVICE);
}

netdev_tx_t hinic_xmit_frame(struct sk_buff *skb, struct net_device *netdev)
{
	struct hinic_dev *nic_dev = netdev_priv(netdev);
	struct hinic_hwdev *hwdev = nic_dev->hwdev;
	struct hinic_hwif *hwif = hwdev->hwif;
	struct pci_dev *pdev = hwif->pdev;
	int qpn = skb->queue_mapping;
	int err = NETDEV_TX_OK, cos = 0;
	struct netdev_queue *netdev_txq = netdev_get_tx_queue(netdev, qpn);
	struct hinic_txq *txq = &nic_dev->txqs[qpn];
	struct hinic_sq *sq = txq->sq;
	struct hinic_wq *wq = sq->wq;
	struct hinic_qp *qp = container_of(sq, struct hinic_qp, sq);
	struct hinic_sge *sges = txq->sges;
	void *wqe;
	unsigned int wqe_size;
	u16 prod_idx;
	int nr_sges;

	if (skb->len < MIN_SKB_LEN) {
		if (skb_pad(skb, MIN_SKB_LEN - skb->len)) {
			dev_err(&netdev->dev, "Failed to pad skb\n");
			goto skb_error;
		}

		skb->len = MIN_SKB_LEN;
	}

	nr_sges = skb_shinfo(skb)->nr_frags + 1;
	if (nr_sges > txq->max_sges) {
		dev_err(&pdev->dev, "Too many Tx sges\n");
		goto skb_error;
	}

	err = tx_map_skb(nic_dev, skb, sges);
	if (err) {
		dev_err(&pdev->dev, "Failed to map Tx sges\n");
		goto skb_error;
	}

	wqe_size = HINIC_SQ_WQE_SIZE(nr_sges);

	wqe = hinic_sq_get_wqe(txq->sq, wqe_size, &prod_idx);
	if (!wqe) {
		dev_err(&pdev->dev, "Failed to get SQ WQE\n");
		tx_unmap_skb(nic_dev, skb, sges);

		netif_stop_subqueue(netdev, qp->q_id);

		u64_stats_update_begin(&txq->txq_stats.syncp);
		txq->txq_stats.tx_busy++;
		u64_stats_update_end(&txq->txq_stats.syncp);
		err = NETDEV_TX_BUSY;
		goto flush_skbs;
	}

	hinic_sq_prepare_wqe(txq->sq, prod_idx, wqe, sges, nr_sges);

	hinic_sq_write_wqe(txq->sq, prod_idx, wqe, skb, wqe_size);

	/* increment prod_idx to the next */
	prod_idx += ALIGN(wqe_size, wq->wqebb_size) / wq->wqebb_size;

flush_skbs:
	if ((!skb->xmit_more) || (netif_xmit_stopped(netdev_txq)))
		hinic_sq_write_db(txq->sq, prod_idx, cos);

	return err;

skb_error:
	dev_kfree_skb_any(skb);

	u64_stats_update_begin(&txq->txq_stats.syncp);
	txq->txq_stats.tx_dropped++;
	u64_stats_update_end(&txq->txq_stats.syncp);
	return err;
}

/**
 * tx_free_skb - unmap and free skb
 * @nic_dev: nic device
 * @skb: the skb
 * @sges: the sges that are connected to the skb
 **/
static void tx_free_skb(struct hinic_dev *nic_dev, struct sk_buff *skb,
			struct hinic_sge *sges)
{
	tx_unmap_skb(nic_dev, skb, sges);

	dev_kfree_skb_any(skb);
}

/**
 * free_all_rx_skbs - free all skbs in tx queue
 * @txq: tx queue
 **/
static void free_all_tx_skbs(struct hinic_txq *txq)
{
	struct hinic_dev *nic_dev = netdev_priv(txq->netdev);
	struct hinic_sq *sq = txq->sq;
	struct hinic_sq_wqe *wqe;
	struct sk_buff *skb;
	u16 ci;
	int wqe_size, nr_sges;

	while ((wqe = hinic_sq_read_wqe(sq, (void **)&skb, &wqe_size, &ci))) {
		nr_sges = skb_shinfo(skb)->nr_frags + 1;

		hinic_sq_get_sges(wqe, txq->free_sges, nr_sges);

		hinic_sq_put_wqe(sq, wqe_size);

		tx_free_skb(nic_dev, skb, txq->free_sges);
	}
}

/**
 * free_tx_poll - free finished tx skbs in tx queue that connected to napi
 * @napi: napi
 * @budget: number of tx
 *
 * Return 0 - Success, negative - Failure
 **/
static int free_tx_poll(struct napi_struct *napi, int budget)
{
	struct hinic_txq *txq = container_of(napi, struct hinic_txq, napi);
	struct hinic_dev *nic_dev = netdev_priv(txq->netdev);
	struct hinic_sq *sq = txq->sq;
	struct hinic_qp *qp = container_of(sq, struct hinic_qp, sq);
	struct hinic_wq *wq = sq->wq;
	struct netdev_queue *netdev_txq;
	struct hinic_sq_wqe *wqe;
	struct sk_buff *skb;
	u64 tx_bytes = 0;
	unsigned int wqe_size;
	int nr_sges, pkts = 0;
	u16 hw_ci, sw_ci, q_id = qp->q_id;

	do {
		hw_ci = HW_CONS_IDX(sq) & wq->mask;

		wqe = hinic_sq_read_wqe(sq, (void **)&skb, &wqe_size,
					&sw_ci);
		if ((!wqe) ||
		    (((hw_ci - sw_ci) & wq->mask) * wq->wqebb_size < wqe_size))
			break;

		tx_bytes += skb->len;
		pkts++;

		nr_sges = skb_shinfo(skb)->nr_frags + 1;

		hinic_sq_get_sges(wqe, txq->free_sges, nr_sges);

		hinic_sq_put_wqe(sq, wqe_size);

		tx_free_skb(nic_dev, skb, txq->free_sges);
	} while (pkts < budget);

	if (__netif_subqueue_stopped(nic_dev->netdev, q_id) &&
	    hinic_get_sq_free_wqebbs(sq) >= HINIC_MIN_TX_NUM_WQEBBS(sq)) {
		netdev_txq = netdev_get_tx_queue(txq->netdev, q_id);

		__netif_tx_lock(netdev_txq, smp_processor_id());

		netif_wake_subqueue(nic_dev->netdev, q_id);

		__netif_tx_unlock(netdev_txq);

		u64_stats_update_begin(&txq->txq_stats.syncp);
		txq->txq_stats.tx_wake++;
		u64_stats_update_end(&txq->txq_stats.syncp);
	}

	u64_stats_update_begin(&txq->txq_stats.syncp);
	txq->txq_stats.bytes += tx_bytes;
	txq->txq_stats.pkts += pkts;
	u64_stats_update_end(&txq->txq_stats.syncp);

	if (pkts < budget) {
		napi_complete(napi);
		enable_irq(sq->irq);
		return pkts;
	}

	return budget;
}

static void tx_napi_add(struct hinic_txq *txq, int weight)
{
	netif_napi_add(txq->netdev, &txq->napi, free_tx_poll, weight);
	napi_enable(&txq->napi);
}

static void tx_napi_del(struct hinic_txq *txq)
{
	napi_disable(&txq->napi);
	netif_napi_del(&txq->napi);
}

static irqreturn_t tx_irq(int irq, void *data)
{
	struct hinic_txq *txq = (struct hinic_txq *)data;
	struct hinic_dev *nic_dev = netdev_priv(txq->netdev);
	struct hinic_sq *sq = txq->sq;

	/* Disable the interrupt until napi will be completed */
	disable_irq_nosync(sq->irq);

	hinic_hwdev_msix_cnt_set(nic_dev->hwdev, sq->msix_entry);

	napi_schedule(&txq->napi);
	return IRQ_HANDLED;
}

static int tx_request_irq(struct hinic_txq *txq)
{
	struct hinic_dev *nic_dev = netdev_priv(txq->netdev);
	struct hinic_sq *sq = txq->sq;
	int err;

	tx_napi_add(txq, nic_dev->tx_weight);

	hinic_hwdev_msix_set(nic_dev->hwdev, sq->msix_entry,
			     TX_IRQ_NO_PENDING, TX_IRQ_NO_COALESC,
			     TX_IRQ_NO_LLI_TIMER, TX_IRQ_NO_CREDIT,
			     TX_IRQ_NO_RESEND_TIMER);

	err = request_irq(sq->irq, tx_irq, 0, txq->irq_name, txq);
	if (err) {
		pr_err("Failed to request Tx irq\n");
		tx_napi_del(txq);
		return err;
	}

	return 0;
}

static void tx_free_irq(struct hinic_txq *txq)
{
	struct hinic_sq *sq = txq->sq;

	free_irq(sq->irq, txq);
	tx_napi_del(txq);
}

/**
 * hinic_init_txq - Initialize the Tx Queue
 * @txq: Logical Tx Queue
 * @sq: Hardware Tx Queue to connect the Logical queue with
 * @netdev: network device to connect the Logical queue with
 *
 * Return 0 - Success, negative - Failure
 **/
int hinic_init_txq(struct hinic_txq *txq, struct hinic_sq *sq,
		   struct net_device *netdev)
{
	struct hinic_dev *nic_dev = netdev_priv(netdev);
	struct hinic_hwdev *hwdev = nic_dev->hwdev;
	struct hinic_hwif *hwif = hwdev->hwif;
	struct pci_dev *pdev = hwif->pdev;
	struct hinic_qp *qp = container_of(sq, struct hinic_qp, sq);
	size_t sges_size;
	int err, irqname_len;

	txq->netdev = netdev;
	txq->sq = sq;

	txq_stats_init(txq);

	txq->max_sges = HINIC_MAX_SQ_BUFDESCS;

	sges_size = txq->max_sges * sizeof(*txq->sges);
	txq->sges = kzalloc(sges_size, GFP_KERNEL);
	if (!txq->sges)
		return -ENOMEM;

	sges_size = txq->max_sges * sizeof(*txq->free_sges);
	txq->free_sges = kzalloc(sges_size, GFP_KERNEL);
	if (!txq->free_sges) {
		err = -ENOMEM;
		goto alloc_free_sges_err;
	}

	irqname_len = snprintf(NULL, 0, "hinic_txq%d", qp->q_id) + 1;
	txq->irq_name = kzalloc(irqname_len, GFP_KERNEL);
	if (!txq->irq_name) {
		err = -ENOMEM;
		goto alloc_irqname_err;
	}

	sprintf(txq->irq_name, "hinic_txq%d", qp->q_id);

	err = hinic_hwdev_hw_ci_addr_set(hwdev, sq, CI_UPDATE_NO_PENDING,
					 CI_UPDATE_NO_COALESC);
	if (err) {
		dev_err(&pdev->dev, "Failed to set HW CI for qid = %d\n",
			qp->q_id);
		goto hw_ci_err;
	}

	err = tx_request_irq(txq);
	if (err) {
		dev_err(&pdev->dev, "Failed to request Tx irq\n");
		goto req_tx_irq_err;
	}

	return 0;

req_tx_irq_err:
hw_ci_err:
	kfree(txq->irq_name);

alloc_irqname_err:
	kfree(txq->free_sges);

alloc_free_sges_err:
	kfree(txq->sges);
	return err;
}

/**
 * hinic_clean_txq - Clean the Tx Queue
 * @txq: Logical Tx Queue
 **/
void hinic_clean_txq(struct hinic_txq *txq)
{
	tx_free_irq(txq);

	free_all_tx_skbs(txq);

	kfree(txq->irq_name);
	kfree(txq->free_sges);
	kfree(txq->sges);
}
