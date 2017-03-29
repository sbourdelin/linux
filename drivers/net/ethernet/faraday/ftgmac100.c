/*
 * Faraday FTGMAC100 Gigabit Ethernet
 *
 * (C) Copyright 2009-2011 Faraday Technology
 * Po-Yu Chuang <ratbert@faraday-tech.com>
 *
 * Largely rewritten by
 *
 * Benjamin Herrenschmidt, copyright 2017, IBM Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <net/ip.h>
#include <net/ncsi.h>

#include "ftgmac100.h"

#define DRV_NAME	"ftgmac100"
#define DRV_VERSION	"1.0"

/* Arbitrary values, I am not sure the HW has limits */
#define MAX_RX_QUEUE_ENTRIES	1024
#define MAX_TX_QUEUE_ENTRIES	1024
#define MIN_RX_QUEUE_ENTRIES	32
#define MIN_TX_QUEUE_ENTRIES	32

/* Defaults */
#define DEF_RX_QUEUE_ENTRIES	128
#define DEF_TX_QUEUE_ENTRIES	128

/* We don't do jumbo frames */
#define MAX_PKT_SIZE		1536
#define RX_BUF_SIZE		MAX_PKT_SIZE  /* must be smaller than 0x3fff */

/* Min number of tx ring entries before stopping queue */
#define TX_THRESHOLD		(MAX_SKB_FRAGS + 1)

struct ftgmac100 {
	/* Registers */
	struct resource *res;
	void __iomem *base;

	/* Rx ring */
	unsigned int rx_q_entries;
	struct ftgmac100_rxdes *rxdes;
	dma_addr_t rxdes_dma;
	struct sk_buff **rx_skbs;
	unsigned int rx_pointer;

	/* Tx ring */
	struct ftgmac100_txdes *txdes;
	dma_addr_t txdes_dma;
	unsigned int tx_q_entries;
	struct sk_buff **tx_skbs;
	unsigned int tx_clean_pointer;
	unsigned int tx_pointer;

	/* Used to signal the reset task of ring change request */
	unsigned int new_rx_q_entries;
	unsigned int new_tx_q_entries;

	/* Scratch page to use when rx skb alloc fails */
	void *rx_scratch;
	dma_addr_t rx_scratch_dma;

	/* Component structures */
	struct net_device *ndev;
	struct device *dev;
	struct ncsi_dev *ncsidev;
	struct napi_struct napi;
	struct work_struct reset_task;
	struct mii_bus *mii_bus;

	/* Link management */
	int cur_speed;
	int cur_duplex;
	bool use_ncsi;

	/* Multicast filter settings */
	u32 maht0;
	u32 maht1;

	/* Tells the reset task to skip  */
	bool stopping;
	bool need_mac_restart;

	/* Flow control settings */
	bool tx_pause;
	bool rx_pause;
	bool aneg_pause;

	u32 rxdes0_edorr_mask;
	u32 txdes0_edotr_mask;
};

static int ftgmac100_alloc_rx_buf(struct ftgmac100 *priv,
				  unsigned int entry, gfp_t gfp)
{
	struct net_device *ndev = priv->ndev;
	struct ftgmac100_rxdes *rxdes = &priv->rxdes[entry];
	struct sk_buff *skb;
	dma_addr_t map;
	int err = 0;

	skb = netdev_alloc_skb_ip_align(ndev, RX_BUF_SIZE);
	if (unlikely(!skb)) {
		if (net_ratelimit())
			netdev_err(ndev, "failed to allocate rx skb\n");
		err = -ENOMEM;
		map = priv->rx_scratch_dma;
	} else {
		map = dma_map_single(priv->dev, skb->data, RX_BUF_SIZE,
				     DMA_FROM_DEVICE);
		if (unlikely(dma_mapping_error(priv->dev, map))) {
			if (net_ratelimit())
				netdev_err(ndev, "failed to map rx page\n");
			dev_kfree_skb_any(skb);
			map = priv->rx_scratch_dma;
			skb = NULL;
			err = -ENOMEM;
		}
	}

	/* Store skb */
	priv->rx_skbs[entry] = skb;

	/* Store DMA address into RX desc */
	ftgmac100_rxdes_set_dma_addr(rxdes, map);

	/* Ensure the above is ordered vs clearing the OWN bit */
	dma_wmb();

	/* Clean rxdes0 (which resets own bit) */
	rxdes->rxdes0 &= cpu_to_le32(priv->rxdes0_edorr_mask);

	return err;
}

static void ftgmac100_free_tx_packet(struct ftgmac100 *priv,
				     unsigned int pointer,
				     struct ftgmac100_txdes *txdes)
{
	struct sk_buff *skb = priv->tx_skbs[pointer];
	dma_addr_t map = ftgmac100_txdes_get_dma_addr(txdes);

	if (ftgmac100_txdes_get_first_segment(txdes)) {
		size_t len = skb_headlen(skb);

		if (skb_shinfo(skb)->nr_frags == 0 && len < ETH_ZLEN)
			len = ETH_ZLEN;
		dma_unmap_single(priv->dev, map, len, DMA_TO_DEVICE);
	} else {
		dma_unmap_page(priv->dev, map,
			       ftgmac100_txdes_get_buffer_size(txdes),
			       DMA_TO_DEVICE);
	}

	if (ftgmac100_txdes_get_last_segment(txdes))
		dev_kfree_skb_any(skb);
	priv->tx_skbs[pointer] = NULL;
}

static int ftgmac100_next_rx_pointer(struct ftgmac100 *priv, int pointer)
{
	return (pointer + 1) & (priv->rx_q_entries - 1);
}

static void ftgmac100_rx_packet_error(struct ftgmac100 *priv,
				      struct ftgmac100_rxdes *rxdes)
{
	struct net_device *ndev = priv->ndev;

	if (ftgmac100_rxdes_rx_error(rxdes))
		ndev->stats.rx_errors++;

	if (ftgmac100_rxdes_crc_error(rxdes))
		ndev->stats.rx_crc_errors++;

	if (ftgmac100_rxdes_frame_too_long(rxdes) ||
	    ftgmac100_rxdes_runt(rxdes) ||
	    ftgmac100_rxdes_odd_nibble(rxdes))
		ndev->stats.rx_length_errors++;
}

static bool ftgmac100_rx_packet(struct ftgmac100 *priv, int *processed)
{
	struct net_device *ndev = priv->ndev;
	struct ftgmac100_rxdes *rxdes;
	struct sk_buff *skb;
	unsigned int pointer, size;
	dma_addr_t map;

	/* Grab next RX descriptor */
	pointer = priv->rx_pointer;
	rxdes = &priv->rxdes[pointer];

	/* Do we have a packet ? */
	if (!ftgmac100_rxdes_packet_ready(rxdes))
		return false;

	/* We don't cope with fragmented RX packets */
	if (unlikely(!ftgmac100_rxdes_first_segment(rxdes) ||
		     !ftgmac100_rxdes_last_segment(rxdes)))
		goto drop;

	/* Any error (other than csum offload) flagged ? */
	if (unlikely(ftgmac100_rxdes_any_error(rxdes))) {
		ftgmac100_rx_packet_error(priv, rxdes);
		goto drop;
	}

	/* Grab the corresponding skb */
	skb = priv->rx_skbs[pointer];
	if (unlikely(!skb)) {
		netdev_err(ndev, "Missing skb in rx ring !\n");
		goto drop;
	}

	/* Grab received size */
	size = ftgmac100_rxdes_data_length(rxdes);
	skb_put(skb, size);

	/* Tear down DMA mapping, do necessary cache management */
	map = ftgmac100_rxdes_get_dma_addr(rxdes);

#if defined(CONFIG_ARM) && !defined(CONFIG_ARM_DMA_USE_IOMMU)
	/* When we don't have an iommu, we can save cycles by not
	 * invalidating the cache for the part of the packet that
	 * wasn't received.
	 */
	dma_unmap_single(priv->dev, map, size, DMA_FROM_DEVICE);
#else
	dma_unmap_single(priv->dev, map, RX_BUF_SIZE, DMA_FROM_DEVICE);
#endif

	/* Grab protocol and handle rx csum */
	skb->protocol = eth_type_trans(skb, ndev);
	if ((ndev->features & NETIF_F_RXCSUM) &&
	    !ftgmac100_rxdes_csum_err(rxdes))
		skb->ip_summed = CHECKSUM_UNNECESSARY;
	else
		skb->ip_summed = CHECKSUM_NONE;

	/* Some stats ... */
	if (unlikely(ftgmac100_rxdes_multicast(rxdes)))
		ndev->stats.multicast++;
	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += size;

	/* Resplenish rx ring */
	ftgmac100_alloc_rx_buf(priv, pointer, GFP_ATOMIC);
	priv->rx_pointer = ftgmac100_next_rx_pointer(priv, pointer);

	/* push packet to protocol stack */
	if (skb->ip_summed == CHECKSUM_NONE)
		netif_receive_skb(skb);
	else
		napi_gro_receive(&priv->napi, skb);

	(*processed)++;
	return true;

 drop:
	/* Clean rxdes0 (which resets own bit) */
	rxdes->rxdes0 &= cpu_to_le32(priv->rxdes0_edorr_mask);
	priv->rx_pointer = ftgmac100_next_rx_pointer(priv, priv->rx_pointer);
	ndev->stats.rx_dropped++;
	return true;
}

static int ftgmac100_next_tx_pointer(struct ftgmac100 *priv, int pointer)
{
	return (pointer + 1) & (priv->tx_q_entries - 1);
}

static u32 ftgmac100_tx_buf_avail(struct ftgmac100 *priv)
{
	if (priv->tx_clean_pointer <= priv->tx_pointer)
		return priv->tx_clean_pointer + (priv->tx_q_entries - 1)
			- priv->tx_pointer;
	else
		return priv->tx_clean_pointer - priv->tx_pointer - 1;
}

static bool ftgmac100_tx_buf_cleanable(struct ftgmac100 *priv)
{
	return priv->tx_pointer != priv->tx_clean_pointer;
}

static bool ftgmac100_tx_complete_packet(struct ftgmac100 *priv)
{
	struct net_device *ndev = priv->ndev;
	struct ftgmac100_txdes *txdes;
	struct sk_buff *skb;
	unsigned int pointer = priv->tx_clean_pointer;

	txdes = &priv->txdes[pointer];
	if (ftgmac100_txdes_owned_by_dma(txdes))
		return false;

	if (ftgmac100_txdes_get_last_segment(txdes)) {
		skb = priv->tx_skbs[pointer];
		ndev->stats.tx_packets++;
		ndev->stats.tx_bytes += skb->len;
	}
	ftgmac100_free_tx_packet(priv, priv->tx_clean_pointer, txdes);

	/* Clear except end of ring bit */
	txdes->txdes0 &= cpu_to_le32(priv->txdes0_edotr_mask);
	txdes->txdes1 = 0;

	priv->tx_clean_pointer = ftgmac100_next_tx_pointer(priv, pointer);

	return true;
}

static irqreturn_t ftgmac100_interrupt(int irq __always_unused, void *dev_id)
{
	struct net_device *ndev = dev_id;
	struct ftgmac100 *priv = netdev_priv(ndev);
	unsigned int status, new_mask = FTGMAC100_INT_BAD;

	/* Fetch and clear interrupt bits, process abnormal ones */
	status = ioread32(priv->base + FTGMAC100_OFFSET_ISR);
	iowrite32(status, priv->base + FTGMAC100_OFFSET_ISR);
	if (unlikely(status & FTGMAC100_INT_BAD)) {
		/* RX buffer unavailable */
		if (status & FTGMAC100_INT_NO_RXBUF)
			ndev->stats.rx_over_errors++;

		/* Received packet lost due to RX FIFO full */
		if (status & FTGMAC100_INT_RPKT_LOST)
			ndev->stats.rx_fifo_errors++;

		/* AHB error -> Reset the chip */
		if (status & FTGMAC100_INT_AHB_ERR) {
			if (net_ratelimit())
				netdev_warn(ndev, "AHB bus error ! Resetting chip.\n");
			iowrite32(0, priv->base + FTGMAC100_OFFSET_IER);
			schedule_work(&priv->reset_task);
			return IRQ_HANDLED;
		}

		/* We may need to restart the MAC after such errors, delay
		 * this until after we have freed some Rx buffers though
		 */
		priv->need_mac_restart = true;

		/* Disable those errors until we restart */
		new_mask &= ~status;
	}


	/* Only enable "bad" interrupts while NAPI is on */
	iowrite32(new_mask, priv->base + FTGMAC100_OFFSET_IER);

	/* Schedule NAPI bh */
	napi_schedule_irqoff(&priv->napi);

	return IRQ_HANDLED;
}

static int ftgmac100_hard_start_xmit(struct sk_buff *skb,
				     struct net_device *ndev)
{
	struct ftgmac100 *priv = netdev_priv(ndev);
	struct ftgmac100_txdes *txdes, *first;
	int nfrags;
	int pointer, len, i, j;
	dma_addr_t map;

	if (unlikely(skb->len > MAX_PKT_SIZE)) {
		if (net_ratelimit())
			netdev_dbg(ndev, "tx packet too big\n");
		goto drop;
	}

	/* The HW doesn't pad small frames */
	if (skb_padto(skb, ETH_ZLEN) < 0) {
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	/* Do we have a limit on #fragments ? I yet have to get a reply
	 * from Aspeed. If there's one I haven't hit it.
	 */
	nfrags = skb_shinfo(skb)->nr_frags;

	/* Get header len and pad for non-fragmented packets */
	len = skb_headlen(skb);
	if (nfrags == 0 && len < ETH_ZLEN)
		len = ETH_ZLEN;

	/* Map the packet head */
	map = dma_map_single(priv->dev, skb->data, len, DMA_TO_DEVICE);
	if (dma_mapping_error(priv->dev, map)) {
		if (net_ratelimit())
			netdev_err(ndev, "map tx packet head failed\n");
		goto drop;
	}

	/* Grab the next free tx descriptor */
	pointer = priv->tx_pointer;
	txdes = first = &priv->txdes[pointer];

	/* Setup it up. We don't set the OWN bit yet. */
	priv->tx_skbs[pointer] = skb;
	ftgmac100_txdes_set_dma_addr(txdes, map);
	ftgmac100_txdes_set_buffer_size(txdes, len);
	ftgmac100_txdes_set_first_segment(txdes);

	/* Setup HW checksumming */
	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		__be16 protocol = skb->protocol;

		if (protocol == cpu_to_be16(ETH_P_IP)) {
			u8 ip_proto = ip_hdr(skb)->protocol;

			ftgmac100_txdes_set_ipcs(txdes);
			if (ip_proto == IPPROTO_TCP)
				ftgmac100_txdes_set_tcpcs(txdes);
			else if (ip_proto == IPPROTO_UDP)
				ftgmac100_txdes_set_udpcs(txdes);
		} else if (skb_checksum_help(skb))
			goto drop;
	}

	/* Next descriptor */
	pointer = ftgmac100_next_tx_pointer(priv, pointer);

	/* Add the fragments */
	for (i = 0; i < nfrags; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];

		len = frag->size;

		/* Map it */
		map = skb_frag_dma_map(priv->dev, frag, 0, len,
				       DMA_TO_DEVICE);
		if (dma_mapping_error(priv->dev, map))
			goto dma_err;

		/* Setup descriptor */
		priv->tx_skbs[pointer] = skb;
		txdes = &priv->txdes[pointer];
		ftgmac100_txdes_set_dma_addr(txdes, map);
		ftgmac100_txdes_set_buffer_size(txdes, len);
		ftgmac100_txdes_set_dma_own(txdes);

		/* The spec is unclear, whether these need to be in the
		 * first descriptor only or all of them. Gor now, do all
		 * of them.
		 */
#define CSUM_MASK cpu_to_le32(FTGMAC100_TXDES1_TCP_CHKSUM | \
			      FTGMAC100_TXDES1_UDP_CHKSUM | \
			      FTGMAC100_TXDES1_IP_CHKSUM)
		txdes->txdes1 |= first->txdes1 & CSUM_MASK;
		pointer = ftgmac100_next_tx_pointer(priv, pointer);
	}

	/* Tag last fragment */
	ftgmac100_txdes_set_last_segment(txdes);

	/* Set the own bit on the first descriptor, this can cause the
	 * HW to transmit, it needs to be ordered after all previous
	 * stores.
	 */
	dma_wmb();
	ftgmac100_txdes_set_dma_own(first);

	/* Update next TX pointer */
	priv->tx_pointer = pointer;

	/* If there isn't enough room for all the fragments of a new packet
	 * in the TX ring, stop the queue. The sequence below is race free
	 * vs. a concurrent restart in ftgmac100_poll()
	 */
	if (unlikely(ftgmac100_tx_buf_avail(priv) <= TX_THRESHOLD)) {
		netif_stop_queue(ndev);
		/* Order the queue stop with the test below */
		smp_mb();
		if (ftgmac100_tx_buf_avail(priv) > TX_THRESHOLD)
			netif_wake_queue(ndev);
	}

	/* Poke transmitter to read the updated TX descriptors */
	iowrite32(1, priv->base + FTGMAC100_OFFSET_NPTXPD);

	return NETDEV_TX_OK;

 dma_err:
	if (net_ratelimit())
		netdev_dbg(ndev, "map tx fragment failed\n");

	/* Free head */
	pointer = priv->tx_pointer;
	ftgmac100_free_tx_packet(priv, pointer, first);

	/* Then all fragments */
	for (j = 0; j < i; j++) {
		pointer = ftgmac100_next_tx_pointer(priv, pointer);
		txdes = &priv->txdes[pointer];
		ftgmac100_free_tx_packet(priv, pointer, txdes);
	}

	/* This cannot be reached if we successfully mapped the
	 * last fragment, so we know ftgmac100_free_tx_packet()
	 * hasn't freed the skb yet.
	 */
 drop:
	/* Drop the packet */
	dev_kfree_skb_any(skb);
	ndev->stats.tx_dropped++;

	return NETDEV_TX_OK;
}

static void ftgmac100_start_mac(struct ftgmac100 *priv)
{
	u32 maccr = ioread32(priv->base + FTGMAC100_OFFSET_MACCR);

	priv->need_mac_restart = false;

	/* Keep the original GMAC and FAST bits */
	maccr &= (FTGMAC100_MACCR_FAST_MODE | FTGMAC100_MACCR_GIGA_MODE);

	/* Add all the main enable bits */
	maccr |= FTGMAC100_MACCR_TXDMA_EN	|
		 FTGMAC100_MACCR_RXDMA_EN	|
		 FTGMAC100_MACCR_TXMAC_EN	|
		 FTGMAC100_MACCR_RXMAC_EN	|
		 FTGMAC100_MACCR_CRC_APD	|
		 FTGMAC100_MACCR_PHY_LINK_LEVEL	|
		 FTGMAC100_MACCR_RX_RUNT	|
		 FTGMAC100_MACCR_RX_BROADPKT;

	/* Add other bits as needed */
	if (priv->cur_duplex == DUPLEX_FULL)
		maccr |= FTGMAC100_MACCR_FULLDUP;
	if (priv->ndev->flags & IFF_PROMISC)
		maccr |= FTGMAC100_MACCR_RX_ALL;
	if (priv->ndev->flags & IFF_ALLMULTI)
		maccr |= FTGMAC100_MACCR_RX_MULTIPKT;
	else if (netdev_mc_count(priv->ndev))
		maccr |= FTGMAC100_MACCR_HT_MULTI_EN;

	/* Hit the HW */
	iowrite32(maccr, priv->base + FTGMAC100_OFFSET_MACCR);
}

static void ftgmac100_stop_mac(struct ftgmac100 *priv)
{
	iowrite32(0, priv->base + FTGMAC100_OFFSET_MACCR);
}

static int ftgmac100_poll(struct napi_struct *napi, int budget)
{
	struct ftgmac100 *priv = container_of(napi, struct ftgmac100, napi);
	struct net_device *ndev = priv->ndev;
	struct netdev_queue *txq;
	int work_done = 0;
	bool more;

	/* Handle Tx packet reclaim */
	if (ftgmac100_tx_buf_cleanable(priv)) {
		while (ftgmac100_tx_buf_cleanable(priv) &&
		       ftgmac100_tx_complete_packet(priv))
			;
		/* Restart queue if needed */
		smp_mb();
		if (unlikely(netif_queue_stopped(ndev) &&
			     ftgmac100_tx_buf_avail(priv) > TX_THRESHOLD)) {
			txq = netdev_get_tx_queue(ndev, 0);
			__netif_tx_lock(txq, smp_processor_id());
			if (netif_queue_stopped(ndev) &&
			    ftgmac100_tx_buf_avail(priv) > TX_THRESHOLD)
				netif_wake_queue(ndev);
			__netif_tx_unlock(txq);
		}
	}

	/* Handle RX packets */
	do
		more = ftgmac100_rx_packet(priv, &work_done);
	while (more && work_done < budget);

	/* The interrupt is telling us to kick the MAC back to life
	 * after an RX overflow
	 */
	if (unlikely(priv->need_mac_restart)) {
		ftgmac100_start_mac(priv);

		/* Re-enable "bad" interrupts */
		iowrite32(FTGMAC100_INT_BAD,
			  priv->base + FTGMAC100_OFFSET_IER);
	}

	/* As long as we are waiting for transmit packets to be
	 * completed we keep NAPI going
	 */
	if (ftgmac100_tx_buf_cleanable(priv))
		work_done = budget;

	/* Are we done ? */
	if (work_done < budget) {
		/* NAPI's over for now */
		napi_complete(napi);

		/* Enable all interrupts */
		iowrite32(FTGMAC100_INT_ALL,
			  priv->base + FTGMAC100_OFFSET_IER);
	}

	return work_done;
}

static void ftgmac100_write_mac_addr(struct ftgmac100 *priv, const u8 *mac)
{
	unsigned int maddr = mac[0] << 8 | mac[1];
	unsigned int laddr = mac[2] << 24 | mac[3] << 16 | mac[4] << 8 | mac[5];

	iowrite32(maddr, priv->base + FTGMAC100_OFFSET_MAC_MADR);
	iowrite32(laddr, priv->base + FTGMAC100_OFFSET_MAC_LADR);
}

static void ftgmac100_config_pause(struct ftgmac100 *priv)
{
	u32 fcr = FTGMAC100_FCR_PAUSE_TIME(16);

	/* Throttle tx queue when receiving pause frames */
	if (priv->rx_pause)
		fcr |= FTGMAC100_FCR_FC_EN;

	/* Enables sending pause frames when the RX queue is past a
	 * certain threshold.
	 */
	if (priv->tx_pause)
		fcr |= FTGMAC100_FCR_FCTHR_EN;

	iowrite32(fcr, priv->base + FTGMAC100_OFFSET_FCR);
}

static void ftgmac100_init_hw(struct ftgmac100 *priv)
{
	u32 reg, rfifo_sz, tfifo_sz;

	/* Clear stale interrupts */
	reg = ioread32(priv->base + FTGMAC100_OFFSET_ISR);
	iowrite32(reg, priv->base + FTGMAC100_OFFSET_ISR);

	/* Setup RX ring buffer base */
	iowrite32(priv->rxdes_dma, priv->base + FTGMAC100_OFFSET_RXR_BADR);

	/* Setup TX ring buffer base */
	iowrite32(priv->txdes_dma, priv->base + FTGMAC100_OFFSET_NPTXR_BADR);

	/* Configure RX buffer size */
	iowrite32(FTGMAC100_RBSR_SIZE(RX_BUF_SIZE),
		  priv->base + FTGMAC100_OFFSET_RBSR);

	/* Set RX descriptor autopoll */
	iowrite32(FTGMAC100_APTC_RXPOLL_CNT(1),
		  priv->base + FTGMAC100_OFFSET_APTC);

	/* Configure descriptor sizes and increase burst sizes according
	 * to values in Aspeed SDK. The FIFO arbitration is enabled and
	 * the thresholds set based on the recommended values in the
	 * AST2400 specification.
	 */
	iowrite32(FTGMAC100_DBLAC_RXDES_SIZE(2) |   /* 2*8 bytes RX descs */
		  FTGMAC100_DBLAC_TXDES_SIZE(2) |   /* 2*8 bytes TX descs */
		  FTGMAC100_DBLAC_RXBURST_SIZE(3) | /* 512 bytes max RX bursts */
		  FTGMAC100_DBLAC_TXBURST_SIZE(3) | /* 512 bytes max TX bursts */
		  FTGMAC100_DBLAC_RX_THR_EN |       /* Enable fifo threshold arb */
		  FTGMAC100_DBLAC_RXFIFO_HTHR(6) |  /* 6/8 of FIFO high threshold */
		  FTGMAC100_DBLAC_RXFIFO_LTHR(2),   /* 2/8 of FIFO low threshold */
		  priv->base + FTGMAC100_OFFSET_DBLAC);

	/* Interrupt mitigation configured for 1 interrupt/packet. HW interrupt
	 * mitigation doesn't seem to provide any benefit with NAPI so leave
	 * it at that.
	 */
	iowrite32(FTGMAC100_ITC_RXINT_THR(1) |
		  FTGMAC100_ITC_TXINT_THR(1),
		  priv->base + FTGMAC100_OFFSET_ITC);

	/* Configure FIFO sizes in the TPAFCR register */
	reg = ioread32(priv->base + FTGMAC100_OFFSET_FEAR);
	rfifo_sz = reg & 0x00000007;
	tfifo_sz = (reg >> 3) & 0x00000007;
	reg = ioread32(priv->base + FTGMAC100_OFFSET_TPAFCR);
	reg &= ~0x3f000000;
	reg |= (tfifo_sz << 27);
	reg |= (rfifo_sz << 24);
	iowrite32(reg, priv->base + FTGMAC100_OFFSET_TPAFCR);

	/* Write MAC address */
	ftgmac100_write_mac_addr(priv, priv->ndev->dev_addr);

	/* Write multicast filters */
	iowrite32(priv->maht0, priv->base + FTGMAC100_OFFSET_MAHT0);
	iowrite32(priv->maht1, priv->base + FTGMAC100_OFFSET_MAHT1);
}

static int ftgmac100_reset_mac(struct ftgmac100 *priv, u32 maccr)
{
	struct net_device *ndev = priv->ndev;
	int i;

	/* NOTE: reset clears all registers */
	iowrite32(maccr, priv->base + FTGMAC100_OFFSET_MACCR);
	iowrite32(maccr | FTGMAC100_MACCR_SW_RST,
		  priv->base + FTGMAC100_OFFSET_MACCR);
	for (i = 0; i < 50; i++) {
		maccr = ioread32(priv->base + FTGMAC100_OFFSET_MACCR);
		if (!(maccr & FTGMAC100_MACCR_SW_RST))
			return 0;
		udelay(1);
	}

	netdev_err(ndev, "Hardware reset failed\n");
	return -EIO;
}

static int ftgmac100_reset_and_config_mac(struct ftgmac100 *priv)
{
	u32 maccr = 0;

	switch (priv->cur_speed) {
	case SPEED_10:
	case 0: /* no link */
		break;

	case SPEED_100:
		maccr |= FTGMAC100_MACCR_FAST_MODE;
		break;

	case SPEED_1000:
		maccr |= FTGMAC100_MACCR_GIGA_MODE;
		break;
	default:
		netdev_err(priv->ndev, "Unknown speed %d !\n", priv->cur_speed);
		break;
	}

	/* (Re)initialize the queue pointers */
	priv->rx_pointer = 0;
	priv->tx_clean_pointer = 0;
	priv->tx_pointer = 0;

	/* The doc says reset twice with 10us interval */
	if (ftgmac100_reset_mac(priv, maccr))
		return -EIO;
	usleep_range(10, 1000);
	return ftgmac100_reset_mac(priv, maccr);
}

static void ftgmac100_calc_mc_hash(struct ftgmac100 *priv)
{
	struct netdev_hw_addr *ha;

	priv->maht1 = 0;
	priv->maht0 = 0;
	netdev_for_each_mc_addr(ha, priv->ndev) {
		u32 crc_val = ether_crc_le(ETH_ALEN, ha->addr);

		crc_val = (~(crc_val >> 2)) & 0x3f;
		if (crc_val >= 32)
			priv->maht1 |= 1ul << (crc_val - 32);
		else
			priv->maht0 |= 1ul << (crc_val);
	}
}

static void ftgmac100_set_rx_mode(struct net_device *ndev)
{
	struct ftgmac100 *priv = netdev_priv(ndev);

	/* If we get passed some MC addresses, setup the hash filter */
	if (netdev_mc_count(ndev)) {
		ftgmac100_calc_mc_hash(priv);
		iowrite32(priv->maht0, priv->base + FTGMAC100_OFFSET_MAHT0);
		iowrite32(priv->maht1, priv->base + FTGMAC100_OFFSET_MAHT1);
	}

	/* Reconfigure MACCR */
	ftgmac100_start_mac(priv);
}

static int ftgmac100_set_mac_addr(struct net_device *ndev, void *p)
{
	struct ftgmac100 *priv = netdev_priv(ndev);
	int ret;

	ret = eth_prepare_mac_addr_change(ndev, p);
	if (ret < 0)
		return ret;
	ftgmac100_write_mac_addr(priv, p);
	eth_commit_mac_addr_change(ndev, p);
	return 0;
}

static int ftgmac100_do_ioctl(struct net_device *ndev, struct ifreq *ifr,
			      int cmd)
{
	if (!ndev->phydev)
		return -ENXIO;

	return phy_mii_ioctl(ndev->phydev, ifr, cmd);
}

static void ftgmac100_get_drvinfo(struct net_device *ndev,
				  struct ethtool_drvinfo *info)
{
	strlcpy(info->driver, DRV_NAME, sizeof(info->driver));
	strlcpy(info->version, DRV_VERSION, sizeof(info->version));
	strlcpy(info->bus_info, dev_name(&ndev->dev), sizeof(info->bus_info));
}

static int ftgmac100_nway_reset(struct net_device *ndev)
{
	if (!ndev->phydev)
		return -ENXIO;
	return phy_start_aneg(ndev->phydev);
}

static void ftgmac100_get_ringparam(struct net_device *ndev,
				    struct ethtool_ringparam *ering)
{
	struct ftgmac100 *priv = netdev_priv(ndev);

	memset(ering, 0, sizeof(*ering));
	ering->rx_max_pending = MAX_RX_QUEUE_ENTRIES;
	ering->tx_max_pending = MAX_TX_QUEUE_ENTRIES;
	ering->rx_pending = priv->rx_q_entries;
	ering->tx_pending = priv->tx_q_entries;
}

static int ftgmac100_set_ringparam(struct net_device *ndev,
				   struct ethtool_ringparam *ering)
{
	struct ftgmac100 *priv = netdev_priv(ndev);

	if (ering->rx_pending > MAX_RX_QUEUE_ENTRIES ||
	    ering->tx_pending > MAX_TX_QUEUE_ENTRIES ||
	    ering->rx_pending < MIN_RX_QUEUE_ENTRIES ||
	    ering->tx_pending < MIN_TX_QUEUE_ENTRIES ||
	    !is_power_of_2(ering->rx_pending) ||
	    !is_power_of_2(ering->tx_pending))
		return -EINVAL;

	priv->new_rx_q_entries = ering->rx_pending;
	priv->new_tx_q_entries = ering->tx_pending;
	if (netif_running(ndev))
		schedule_work(&priv->reset_task);

	return 0;
}

static void ftgmac100_get_pauseparam(struct net_device *ndev,
				     struct ethtool_pauseparam *pause)
{
	struct ftgmac100 *priv = netdev_priv(ndev);

	pause->autoneg = priv->aneg_pause;
	pause->tx_pause = priv->tx_pause;
	pause->rx_pause = priv->rx_pause;
}

static int ftgmac100_set_pauseparam(struct net_device *ndev,
				    struct ethtool_pauseparam *pause)
{
	struct ftgmac100 *priv = netdev_priv(ndev);
	struct phy_device *phydev = ndev->phydev;

	priv->aneg_pause = pause->autoneg;
	priv->tx_pause = pause->tx_pause;
	priv->rx_pause = pause->rx_pause;

	if (phydev) {
		phydev->advertising &= ~ADVERTISED_Pause;
		phydev->advertising &= ~ADVERTISED_Asym_Pause;

		if (pause->rx_pause) {
			phydev->advertising |= ADVERTISED_Pause;
			phydev->advertising |= ADVERTISED_Asym_Pause;
		}

		if (pause->tx_pause)
			phydev->advertising ^= ADVERTISED_Asym_Pause;
	}
	if (netif_running(ndev)) {
		if (phydev && priv->aneg_pause)
			phy_start_aneg(phydev);
		else
			ftgmac100_config_pause(priv);
	}

	return 0;
}

static const struct ethtool_ops ftgmac100_ethtool_ops = {
	.get_drvinfo		= ftgmac100_get_drvinfo,
	.get_link		= ethtool_op_get_link,
	.get_link_ksettings	= phy_ethtool_get_link_ksettings,
	.set_link_ksettings	= phy_ethtool_set_link_ksettings,
	.nway_reset		= ftgmac100_nway_reset,
	.get_ringparam		= ftgmac100_get_ringparam,
	.set_ringparam		= ftgmac100_set_ringparam,
	.get_pauseparam		= ftgmac100_get_pauseparam,
	.set_pauseparam		= ftgmac100_set_pauseparam,
};

static void ftgmac100_free_tx_buffers(struct ftgmac100 *priv)
{
	int i;

	/* Free all tx buffers */
	for (i = 0; i < priv->tx_q_entries; i++) {
		struct ftgmac100_txdes *txdes = &priv->txdes[i];

		if (!priv->tx_skbs[i])
			continue;
		ftgmac100_free_tx_packet(priv, i, txdes);
	}
}

static void ftgmac100_free_rx_buffers(struct ftgmac100 *priv)
{
	int i;

	/* Free all RX buffers */
	for (i = 0; i < priv->rx_q_entries; i++) {
		struct ftgmac100_rxdes *rxdes = &priv->rxdes[i];
		struct sk_buff *skb = priv->rx_skbs[i];
		dma_addr_t map = ftgmac100_rxdes_get_dma_addr(rxdes);

		if (!skb)
			continue;

		priv->rx_skbs[i] = NULL;
		dma_unmap_page(priv->dev, map, RX_BUF_SIZE, DMA_FROM_DEVICE);
		dev_kfree_skb_any(skb);
	}
}

static void ftgmac100_free_descriptors(struct ftgmac100 *priv)
{
	/* Free skb arrays */
	kfree(priv->rx_skbs);
	kfree(priv->tx_skbs);

	/* Free descriptor arrays */
	if (priv->rxdes)
		dma_free_coherent(priv->dev, MAX_RX_QUEUE_ENTRIES *
				  sizeof(struct ftgmac100_rxdes),
				  priv->rxdes, priv->rxdes_dma);
	priv->rxdes = NULL;
	if (priv->txdes)
		dma_free_coherent(priv->dev, MAX_TX_QUEUE_ENTRIES *
				  sizeof(struct ftgmac100_txdes),
				  priv->txdes, priv->txdes_dma);
	priv->txdes = NULL;

	/* Free scratch packet buffer */
	if (priv->rx_scratch)
		dma_free_coherent(priv->dev, RX_BUF_SIZE,
				  priv->rx_scratch, priv->rx_scratch_dma);
}

static void ftgmac100_init_descriptors(struct ftgmac100 *priv)
{
	int i;

	/* Update entries counts */
	priv->rx_q_entries = priv->new_rx_q_entries;
	priv->tx_q_entries = priv->new_tx_q_entries;

	/* Clean all rx and tx descriptors and set the end-of-ring
	 * marker on the last entry. For the RX descriptor, populate
	 * all entries with a DMA address pointing to the scratch
	 * page.
	 */
	for (i = 0; i < priv->rx_q_entries; i++) {
		priv->rxdes[i].rxdes0 = 0;
		ftgmac100_rxdes_set_dma_addr(&priv->rxdes[i],
					     priv->rx_scratch_dma);
	}
	priv->rxdes[i - 1].rxdes0 = cpu_to_le32(priv->rxdes0_edorr_mask);
	for (i = 0; i < priv->tx_q_entries; i++)
		priv->txdes[i].txdes0 = 0;
	priv->txdes[i - 1].txdes0 = cpu_to_le32(priv->txdes0_edotr_mask);
}

static int ftgmac100_alloc_descriptors(struct ftgmac100 *priv)
{
	/* Allocate skb arrays */
	priv->rx_skbs = kcalloc(MAX_RX_QUEUE_ENTRIES, sizeof(void *),
				GFP_KERNEL);
	if (!priv->rx_skbs)
		return -ENOMEM;
	priv->tx_skbs = kcalloc(MAX_TX_QUEUE_ENTRIES, sizeof(void *),
				GFP_KERNEL);
	if (!priv->tx_skbs)
		return -ENOMEM;

	/* Allocate descriptor arrays */
	priv->rxdes = dma_zalloc_coherent(priv->dev,
					  MAX_RX_QUEUE_ENTRIES *
					  sizeof(struct ftgmac100_rxdes),
					  &priv->rxdes_dma, GFP_KERNEL);
	if (!priv->rxdes)
		return -ENOMEM
;	priv->txdes = dma_zalloc_coherent(priv->dev,
					  MAX_TX_QUEUE_ENTRIES *
					  sizeof(struct ftgmac100_txdes),
					  &priv->txdes_dma, GFP_KERNEL);
	if (!priv->txdes)
		return -ENOMEM;

	/* Allocate scratch packet buffer */
	priv->rx_scratch = dma_alloc_coherent(priv->dev,
					      RX_BUF_SIZE,
					      &priv->rx_scratch_dma,
					      GFP_KERNEL);
	if (!priv->rx_scratch)
		return -ENOMEM;

	return 0;
}

static int ftgmac100_alloc_rx_buffers(struct ftgmac100 *priv)
{
	int i;

	/* Populate RX ring */
	for (i = 0; i < priv->rx_q_entries; i++) {
		/* Give up on error, the entries have been pre-populated
		 * with the address of the scratch page
		 */
		if (ftgmac100_alloc_rx_buf(priv, i, GFP_KERNEL))
			return -ENOMEM;
	}
	return 0;
}

static void ftgmac100_init_all(struct ftgmac100 *priv)
{
	if (!netif_running(priv->ndev))
		return;

	/* Re-init descriptors (adjust queue sizes) */
	ftgmac100_init_descriptors(priv);

	/* Realloc rx descriptors */
	ftgmac100_alloc_rx_buffers(priv);

	/* Reinit and restart HW */
	ftgmac100_init_hw(priv);
	ftgmac100_config_pause(priv);
	ftgmac100_start_mac(priv);

	/* Re-enable the device */
	napi_enable(&priv->napi);
	netif_start_queue(priv->ndev);

	/* Enable all interrupts */
	iowrite32(FTGMAC100_INT_ALL, priv->base + FTGMAC100_OFFSET_IER);
}

static int ftgmac100_open(struct net_device *ndev)
{
	struct ftgmac100 *priv = netdev_priv(ndev);
	int err;

	/* Clear stale stopping flag */
	priv->stopping = false;

	/* Allocate ring buffers and populate Rx ring */
	err = ftgmac100_alloc_descriptors(priv);
	if (err) {
		netdev_err(ndev, "failed to allocate descriptors\n");
		goto err_alloc;
	}

	/* When using NC-SI we force the speed to 100Mbit/s full duplex,
	 *
	 * Otherwise we leave it set to 0 (no link), the link
	 * message from the PHY layer will handle setting it up to
	 * something else if needed.
	 */
	if (priv->use_ncsi) {
		priv->cur_duplex = DUPLEX_FULL;
		priv->cur_speed = SPEED_100;
	} else {
		priv->cur_duplex = 0;
		priv->cur_speed = 0;
	}

	/* Reset the hardware */
	err = ftgmac100_reset_and_config_mac(priv);
	if (err)
		goto err_hw;

	/* Initialize NAPI */
	netif_napi_add(ndev, &priv->napi, ftgmac100_poll, NAPI_POLL_WEIGHT);

	/* Disable all interrupts */
	iowrite32(0, priv->base + FTGMAC100_OFFSET_IER);

	/* Grab our interrupt */
	err = request_irq(ndev->irq, ftgmac100_interrupt, 0,
			  ndev->name, ndev);
	if (err) {
		netdev_err(ndev, "failed to request irq %d\n", ndev->irq);
		goto err_irq;
	}

	/* Start thing up */
	ftgmac100_init_all(priv);
	if (ndev->phydev) {
		/* If we have a PHY, start polling */
		phy_start(ndev->phydev);
	} else if (priv->use_ncsi) {
		/* If using NC-SI, set our carrier on and start the stack */
		netif_carrier_on(ndev);

		/* Start the NCSI device */
		err = ncsi_start_dev(priv->ncsidev);
		if (err)
			goto err_ncsi;
	}

	return 0;

 err_ncsi:
	napi_disable(&priv->napi);
	netif_stop_queue(ndev);
	free_irq(ndev->irq, ndev);
 err_irq:
	netif_napi_del(&priv->napi);
 err_hw:
 err_alloc:
	iowrite32(0, priv->base + FTGMAC100_OFFSET_IER);
	ftgmac100_free_tx_buffers(priv);
	ftgmac100_free_rx_buffers(priv);
	ftgmac100_free_descriptors(priv);
	return err;
}

static int ftgmac100_stop(struct net_device *ndev)
{
	struct ftgmac100 *priv = netdev_priv(ndev);

	/* Block reset task */
	priv->stopping = true;

	/* Kill any pending one */
	cancel_work_sync(&priv->reset_task);

	/* Disable all interrupts */
	iowrite32(0, priv->base + FTGMAC100_OFFSET_IER);

	/* Stop the PHY or NCSI */
	if (ndev->phydev)
		phy_stop(ndev->phydev);
	else if (priv->use_ncsi)
		ncsi_stop_dev(priv->ncsidev);

	/* Stop the network stack */
	netif_stop_queue(ndev);
	napi_disable(&priv->napi);
	netif_napi_del(&priv->napi);

	/* Stop the HW */
	ftgmac100_stop_mac(priv);

	/* No more IRQ for us */
	free_irq(ndev->irq, ndev);

	/* Free everything */
	ftgmac100_free_tx_buffers(priv);
	ftgmac100_free_rx_buffers(priv);
	ftgmac100_free_descriptors(priv);

	return 0;
}

static void ftgmac100_reset_task(struct work_struct *work)
{
	struct ftgmac100 *priv = container_of(work, struct ftgmac100, reset_task);
	struct net_device *ndev = priv->ndev;
	int err;

	/* Adapter is going down */
	if (priv->stopping)
		return;

	netdev_dbg(ndev, "Resetting NIC...\n");

	/* Block PHY polling */
	if (ndev->phydev)
		mutex_lock(&ndev->phydev->lock);

	rtnl_lock();

	/* Check if link state changed again */
	if (priv->cur_speed == 0)
		goto bail;

	/* Stop the network stack */
	netif_trans_update(ndev);
	napi_disable(&priv->napi);
	netif_tx_disable(ndev);

	/* Stop and reset the MAC */
	ftgmac100_stop_mac(priv);
	err = ftgmac100_reset_and_config_mac(priv);
	if (err) {
		/* Not much we can do ... it might come back... */
		netdev_err(ndev, "attempting to continue...\n");
	}

	/* Free all rx and tx buffers */
	ftgmac100_free_tx_buffers(priv);
	ftgmac100_free_rx_buffers(priv);

	/* Setup everything and restart chip */
	ftgmac100_init_all(priv);

	netdev_dbg(ndev, "Reset done !\n");
 bail:
	rtnl_unlock();

	/* Unblock PHY polling */
	if (ndev->phydev)
		mutex_unlock(&ndev->phydev->lock);
}

static void ftgmac100_tx_timeout(struct net_device *ndev)
{
	struct ftgmac100 *priv = netdev_priv(ndev);

	/* Disable all interrupts */
	iowrite32(0, priv->base + FTGMAC100_OFFSET_IER);

	/* Do the reset outside of interrupt context */
	schedule_work(&priv->reset_task);
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void ftgmac100_netpoll(struct net_device *ndev)
{
	unsigned long flags;

	disable_irq(dev->irq);
	local_irq_save(flags);
	ftgmac100_interrupt(dev->irq, ndev);
	local_irq_restore(flags);
	enable_irq(dev->irq);
}
#endif

static const struct net_device_ops ftgmac100_netdev_ops = {
	.ndo_open		= ftgmac100_open,
	.ndo_stop		= ftgmac100_stop,
	.ndo_start_xmit		= ftgmac100_hard_start_xmit,
	.ndo_set_mac_address	= ftgmac100_set_mac_addr,
	.ndo_set_rx_mode	= ftgmac100_set_rx_mode,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_do_ioctl		= ftgmac100_do_ioctl,
	.ndo_tx_timeout		= ftgmac100_tx_timeout,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= ftgmac100_poll_controller,
#endif
};

static const char *ftgmac100_fctrl_string(struct ftgmac100 *priv)
{
	if (priv->tx_pause && priv->rx_pause)
		return "rx/tx";
	else if (priv->rx_pause)
		return "rx";
	else if (priv->tx_pause)
		return "tx";
	else
		return "no";
}

static void ftgmac100_adjust_link(struct net_device *ndev)
{
	struct ftgmac100 *priv = netdev_priv(ndev);
	struct phy_device *phydev = ndev->phydev;
	bool tx_pause, rx_pause;

	/* Link is down */
	if (!phydev->link) {
		if (priv->cur_speed)
			netdev_info(ndev, "Link down\n");
		priv->cur_speed = 0;

		/* We just stop the MAC, we'll reset the adapter
		 * if/when the link comes back up
		 */
		ftgmac100_stop_mac(priv);
		return;
	}

	/* Grab pause settings from PHY if configured to do so */
	if (priv->aneg_pause) {
		rx_pause = tx_pause = phydev->pause;
		if (phydev->asym_pause)
			tx_pause = !rx_pause;
	} else {
		rx_pause = priv->rx_pause;
		tx_pause = priv->tx_pause;
	}

	/* Link hasn't changed, do nothing */
	if (phydev->speed == priv->cur_speed &&
	    phydev->duplex == priv->cur_duplex &&
	    rx_pause == priv->rx_pause &&
	    tx_pause == priv->tx_pause)
		return;

	priv->cur_speed = phydev->speed;
	priv->cur_duplex = phydev->duplex;
	priv->rx_pause = rx_pause;
	priv->tx_pause = tx_pause;

	netdev_info(ndev, "Link up at %d Mbit/s %s duplex %s flow ctrl\n",
		    priv->cur_speed,
		    phydev->duplex == DUPLEX_FULL ? "full" : "half",
		    ftgmac100_fctrl_string(priv));


	/* Disable all interrupts */
	iowrite32(0, priv->base + FTGMAC100_OFFSET_IER);

	/* Reset the adapter asynchronously */
	schedule_work(&priv->reset_task);
}

static int ftgmac100_mii_probe(struct ftgmac100 *priv, phy_interface_t intf)
{
	struct net_device *ndev = priv->ndev;
	struct phy_device *phydev;

	phydev = phy_find_first(priv->mii_bus);
	if (!phydev) {
		netdev_info(ndev, "%s: no PHY found\n", ndev->name);
		return -ENODEV;
	}

	phydev = phy_connect(ndev, phydev_name(phydev),
			     &ftgmac100_adjust_link, intf);

	if (IS_ERR(phydev)) {
		netdev_err(ndev, "%s: Could not attach to PHY\n", ndev->name);
		return PTR_ERR(phydev);
	}

	/* Indicate that we support PAUSE frames (see comment in
	 * Documentation/networking/phy.txt)
	 */
	phydev->supported |= SUPPORTED_Pause | SUPPORTED_Asym_Pause;

	phydev->advertising = phydev->supported;
	phy_attached_info(phydev);

	return 0;
}

static int ftgmac100_mii_read(struct mii_bus *bus, int phy_addr, int regnum)
{
	struct net_device *ndev = bus->priv;
	struct ftgmac100 *priv = netdev_priv(ndev);
	unsigned int phycr;
	int i;

	phycr = ioread32(priv->base + FTGMAC100_OFFSET_PHYCR);

	/* preserve MDC cycle threshold */
	phycr &= FTGMAC100_PHYCR_MDC_CYCTHR_MASK;

	phycr |= FTGMAC100_PHYCR_PHYAD(phy_addr) |
		 FTGMAC100_PHYCR_REGAD(regnum) |
		 FTGMAC100_PHYCR_MIIRD;

	iowrite32(phycr, priv->base + FTGMAC100_OFFSET_PHYCR);

	for (i = 0; i < 10; i++) {
		phycr = ioread32(priv->base + FTGMAC100_OFFSET_PHYCR);

		if ((phycr & FTGMAC100_PHYCR_MIIRD) == 0) {
			int data;

			data = ioread32(priv->base + FTGMAC100_OFFSET_PHYDATA);
			return FTGMAC100_PHYDATA_MIIRDATA(data);
		}

		udelay(100);
	}

	netdev_err(ndev, "mdio read timed out\n");
	return -EIO;
}

static int ftgmac100_mii_write(struct mii_bus *bus, int phy_addr,
			       int regnum, u16 value)
{
	struct net_device *ndev = bus->priv;
	struct ftgmac100 *priv = netdev_priv(ndev);
	unsigned int phycr;
	int data;
	int i;

	phycr = ioread32(priv->base + FTGMAC100_OFFSET_PHYCR);

	/* preserve MDC cycle threshold */
	phycr &= FTGMAC100_PHYCR_MDC_CYCTHR_MASK;

	phycr |= FTGMAC100_PHYCR_PHYAD(phy_addr) |
		 FTGMAC100_PHYCR_REGAD(regnum) |
		 FTGMAC100_PHYCR_MIIWR;

	data = FTGMAC100_PHYDATA_MIIWDATA(value);

	iowrite32(data, priv->base + FTGMAC100_OFFSET_PHYDATA);
	iowrite32(phycr, priv->base + FTGMAC100_OFFSET_PHYCR);

	for (i = 0; i < 10; i++) {
		phycr = ioread32(priv->base + FTGMAC100_OFFSET_PHYCR);

		if ((phycr & FTGMAC100_PHYCR_MIIWR) == 0)
			return 0;

		udelay(100);
	}

	netdev_err(ndev, "mdio write timed out\n");
	return -EIO;
}

static int ftgmac100_setup_mdio(struct net_device *ndev)
{
	struct ftgmac100 *priv = netdev_priv(ndev);
	struct platform_device *pdev = to_platform_device(priv->dev);
	phy_interface_t phy_intf = PHY_INTERFACE_MODE_RGMII;
	struct device_node *np = pdev->dev.of_node;
	const char *intf_prop = NULL;
	int i, err = 0;
	u32 reg;

	/* initialize mdio bus */
	priv->mii_bus = mdiobus_alloc();
	if (!priv->mii_bus)
		return -EIO;

	if (np && (of_device_is_compatible(np, "aspeed,ast2400-mac") ||
		   of_device_is_compatible(np, "aspeed,ast2500-mac"))) {
		/* This driver supports the old MDIO interface */
		reg = ioread32(priv->base + FTGMAC100_OFFSET_REVR);
		reg &= ~FTGMAC100_REVR_NEW_MDIO_INTERFACE;
		iowrite32(reg, priv->base + FTGMAC100_OFFSET_REVR);
	}

	/* Note: When using RGMII mode, we simply pass "RGMII" to the
	 *       PHY and assume that u-boot will have configured the
	 *       clock delays appropriately for the system.
	 *
	 *       The implementation of the MAC in the Aspeed chips
	 *       supports sub-ns programable delays that need to be
	 *       configured in the SCU while the MAC IP block is in
	 *       reset.
	 *
	 *       If needed in the future, we can support configuring this
	 *       here based on device-tree properties but unless absolutely
	 *       needed I'd rather avoid poking at the SCU registers from
	 *       this driver.
	 */
	if (np)
		intf_prop = of_get_property(np, "phy-mode", NULL);
	if (intf_prop) {
		if (!strcasecmp(intf_prop, "rmii"))
			phy_intf = PHY_INTERFACE_MODE_RMII;
		else if (strcasecmp(intf_prop, "rgmii"))
			netdev_warn(ndev, "Unsupported PHY interface '%s'\n",
				    intf_prop);
	}
	priv->mii_bus->name = "ftgmac100_mdio";
	snprintf(priv->mii_bus->id, MII_BUS_ID_SIZE, "%s-%d",
		 pdev->name, pdev->id);
	priv->mii_bus->priv = priv->ndev;
	priv->mii_bus->read = ftgmac100_mii_read;
	priv->mii_bus->write = ftgmac100_mii_write;

	for (i = 0; i < PHY_MAX_ADDR; i++)
		priv->mii_bus->irq[i] = PHY_POLL;

	err = mdiobus_register(priv->mii_bus);
	if (err) {
		dev_err(priv->dev, "Cannot register MDIO bus!\n");
		goto err_register_mdiobus;
	}

	err = ftgmac100_mii_probe(priv, phy_intf);
	if (err) {
		dev_err(priv->dev, "MII Probe failed!\n");
		goto err_mii_probe;
	}

	return 0;

err_mii_probe:
	mdiobus_unregister(priv->mii_bus);
err_register_mdiobus:
	mdiobus_free(priv->mii_bus);
	return err;
}

static void ftgmac100_destroy_mdio(struct net_device *ndev)
{
	struct ftgmac100 *priv = netdev_priv(ndev);

	if (!ndev->phydev)
		return;

	phy_disconnect(ndev->phydev);
	mdiobus_unregister(priv->mii_bus);
	mdiobus_free(priv->mii_bus);
}

static void ftgmac100_ncsi_handler(struct ncsi_dev *nd)
{
	if (unlikely(nd->state != ncsi_dev_state_functional))
		return;

	netdev_info(nd->dev, "NCSI interface %s\n",
		    nd->link_up ? "up" : "down");
}

static void ftgmac100_initial_mac(struct ftgmac100 *priv)
{
	u8 mac[ETH_ALEN];
	unsigned int m;
	unsigned int l;
	void *addr;

	addr = device_get_mac_address(priv->dev, mac, ETH_ALEN);
	if (addr) {
		ether_addr_copy(priv->ndev->dev_addr, mac);
		dev_info(priv->dev, "Read MAC address %pM from device tree\n",
			 mac);
		return;
	}

	m = ioread32(priv->base + FTGMAC100_OFFSET_MAC_MADR);
	l = ioread32(priv->base + FTGMAC100_OFFSET_MAC_LADR);

	mac[0] = (m >> 8) & 0xff;
	mac[1] = m & 0xff;
	mac[2] = (l >> 24) & 0xff;
	mac[3] = (l >> 16) & 0xff;
	mac[4] = (l >> 8) & 0xff;
	mac[5] = l & 0xff;

	if (is_valid_ether_addr(mac)) {
		ether_addr_copy(priv->ndev->dev_addr, mac);
		dev_info(priv->dev, "Read MAC address %pM from chip\n", mac);
	} else {
		eth_hw_addr_random(priv->ndev);
		dev_info(priv->dev, "Generated random MAC address %pM\n",
			 priv->ndev->dev_addr);
	}
}

static int ftgmac100_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct net_device *ndev;
	struct ftgmac100 *priv;
	struct device_node *np;
	int irq, err = 0;

	if (!pdev)
		return -ENODEV;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENXIO;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	/* setup net_device */
	ndev = alloc_etherdev(sizeof(*priv));
	if (!ndev) {
		err = -ENOMEM;
		goto err_alloc_etherdev;
	}

	SET_NETDEV_DEV(ndev, &pdev->dev);

	ndev->ethtool_ops = &ftgmac100_ethtool_ops;
	ndev->netdev_ops = &ftgmac100_netdev_ops;
	ndev->irq = irq;

	platform_set_drvdata(pdev, ndev);

	/* setup private data */
	priv = netdev_priv(ndev);
	priv->ndev = ndev;
	priv->dev = &pdev->dev;
	priv->maht1 = 0;
	priv->maht0 = 0;
	INIT_WORK(&priv->reset_task, ftgmac100_reset_task);

	np = pdev->dev.of_node;
	if (np && (of_device_is_compatible(np, "aspeed,ast2400-mac") ||
		   of_device_is_compatible(np, "aspeed,ast2500-mac"))) {
		priv->rxdes0_edorr_mask = BIT(30);
		priv->txdes0_edotr_mask = BIT(30);
	} else {
		priv->rxdes0_edorr_mask = BIT(15);
		priv->txdes0_edotr_mask = BIT(15);
	}

	/* map io memory */
	priv->res = request_mem_region(res->start, resource_size(res),
				       dev_name(&pdev->dev));
	if (!priv->res) {
		dev_err(&pdev->dev, "Could not reserve memory region\n");
		err = -ENOMEM;
		goto err_req_mem;
	}

	priv->base = ioremap(res->start, resource_size(res));
	if (!priv->base) {
		dev_err(&pdev->dev, "Failed to ioremap ethernet registers\n");
		err = -EIO;
		goto err_ioremap;
	}

	/* Enable pause */
	priv->tx_pause = true;
	priv->rx_pause = true;
	priv->aneg_pause = true;

	/* MAC address from chip or random one */
	ftgmac100_initial_mac(priv);

	if (np && of_get_property(np, "use-ncsi", NULL)) {
		if (!IS_ENABLED(CONFIG_NET_NCSI)) {
			dev_err(&pdev->dev, "NCSI stack not enabled\n");
			goto err_ncsi_dev;
		}

		dev_info(&pdev->dev, "Using NCSI interface\n");
		priv->use_ncsi = true;
		priv->ncsidev = ncsi_register_dev(ndev, ftgmac100_ncsi_handler);
		if (!priv->ndev)
			goto err_ncsi_dev;
	} else {
		priv->use_ncsi = false;
		err = ftgmac100_setup_mdio(ndev);
		if (err)
			goto err_setup_mdio;
	}

	/* Default ring sizes */
	priv->rx_q_entries = priv->new_rx_q_entries = DEF_RX_QUEUE_ENTRIES;
	priv->tx_q_entries = priv->new_tx_q_entries = DEF_TX_QUEUE_ENTRIES;

	/* Setup feature set */
	ndev->hw_features = NETIF_F_RXCSUM | NETIF_F_IP_CSUM |
		NETIF_F_GRO | NETIF_F_SG;
	/* AST2400  doesn't have working HW checksum generation */
	if (np && of_device_is_compatible(np, "aspeed,ast2400-mac"))
		ndev->hw_features &= ~NETIF_F_IP_CSUM;
	if (np && of_get_property(np, "no-hw-checksum", NULL))
		ndev->hw_features &= ~(NETIF_F_IP_CSUM | NETIF_F_RXCSUM);
	ndev->features |= ndev->hw_features;

	/* register network device */
	err = register_netdev(ndev);
	if (err) {
		dev_err(&pdev->dev, "Failed to register netdev\n");
		goto err_register_netdev;
	}

	netdev_info(ndev, "irq %d, mapped at %p\n", ndev->irq, priv->base);

	return 0;

err_ncsi_dev:
err_register_netdev:
	ftgmac100_destroy_mdio(ndev);
err_setup_mdio:
	iounmap(priv->base);
err_ioremap:
	release_resource(priv->res);
err_req_mem:
	free_netdev(ndev);
err_alloc_etherdev:
	return err;
}

static int ftgmac100_remove(struct platform_device *pdev)
{
	struct net_device *ndev;
	struct ftgmac100 *priv;

	ndev = platform_get_drvdata(pdev);
	priv = netdev_priv(ndev);

	/* Preent the reset task from kicking early on */
	priv->stopping = true;

	/* Close & unregister the netdevice, at this points
	 * the interrupt will be disabled
	 */
	unregister_netdev(ndev);

	/* There's a small chance the reset task will have been re-queued,
	 * during stop, make sure it's gone before we free the structure.
	 */
	cancel_work_sync(&priv->reset_task);

	ftgmac100_destroy_mdio(ndev);

	iounmap(priv->base);
	release_resource(priv->res);
	free_netdev(ndev);
	return 0;
}

static const struct of_device_id ftgmac100_of_match[] = {
	{ .compatible = "faraday,ftgmac100" },
	{ }
};
MODULE_DEVICE_TABLE(of, ftgmac100_of_match);

static struct platform_driver ftgmac100_driver = {
	.probe	= ftgmac100_probe,
	.remove	= ftgmac100_remove,
	.driver	= {
		.name		= DRV_NAME,
		.of_match_table	= ftgmac100_of_match,
	},
};
module_platform_driver(ftgmac100_driver);

MODULE_AUTHOR("Po-Yu Chuang <ratbert@faraday-tech.com>");
MODULE_DESCRIPTION("FTGMAC100 driver");
MODULE_LICENSE("GPL");
