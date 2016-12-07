/*
 * Synopsys DesignWare Core Enterprise Ethernet (XLGMAC) Driver
 *
 * Copyright (C) 2015-2016 Synopsys, Inc. (www.synopsys.com)
 *
 * Author: Jie Deng <jiedeng@synopsys.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __DWC_XLGMAC_H__
#define __DWC_XLGMAC_H__

#include "dwc-eth.h"

#define XLGMAC_DRV_NAME			"dwc-xlgmac"
#define XLGMAC_DRV_VERSION		"1.0.0"
#define XLGMAC_DRV_DESC			"Synopsys DWC XLGMAC Driver"

/* Descriptor related defines */
#define XLGMAC_TX_DESC_CNT		512
#define XLGMAC_TX_DESC_MIN_FREE		(XLGMAC_TX_DESC_CNT >> 3)
#define XLGMAC_TX_DESC_MAX_PROC		(XLGMAC_TX_DESC_CNT >> 1)
#define XLGMAC_RX_DESC_CNT		512
#define XLGMAC_RX_DESC_MAX_DIRTY	(XLGMAC_RX_DESC_CNT >> 3)

#define XLGMAC_TX_MAX_BUF_SIZE		(0x3fff & ~(64 - 1))

/* Descriptors required for maximum contiguous TSO/GSO packet */
#define XLGMAC_TX_MAX_SPLIT	((GSO_MAX_SIZE / XLGMAC_TX_MAX_BUF_SIZE) + 1)

/* Maximum possible descriptors needed for an SKB:
 * - Maximum number of SKB frags
 * - Maximum descriptors for contiguous TSO/GSO packet
 * - Possible context descriptor
 * - Possible TSO header descriptor
 */
#define XLGMAC_TX_MAX_DESC_NR	(MAX_SKB_FRAGS + XLGMAC_TX_MAX_SPLIT + 2)

#define XLGMAC_RX_MIN_BUF_SIZE	(ETH_FRAME_LEN + ETH_FCS_LEN + VLAN_HLEN)
#define XLGMAC_RX_BUF_ALIGN	64
#define XLGMAC_SKB_ALLOC_SIZE	256
#define XLGMAC_SPH_HDSMS_SIZE	2	/* Keep in sync with SKB_ALLOC_SIZE */

#define XLGMAC_DMA_STOP_TIMEOUT	5

/* DMA cache settings - Outer sharable, write-back, write-allocate */
#define XLGMAC_DMA_OS_AXDOMAIN		0x2
#define XLGMAC_DMA_OS_ARCACHE		0xb
#define XLGMAC_DMA_OS_AWCACHE		0xf

/* DMA cache settings - System, no caches used */
#define XLGMAC_DMA_SYS_AXDOMAIN		0x3
#define XLGMAC_DMA_SYS_ARCACHE		0x0
#define XLGMAC_DMA_SYS_AWCACHE		0x0

/* Default coalescing parameters */
#define XLGMAC_INIT_DMA_TX_USECS	1000
#define XLGMAC_INIT_DMA_TX_FRAMES	25

#define XLGMAC_MAX_DMA_RIWT		0xff
#define XLGMAC_INIT_DMA_RX_USECS	30
#define XLGMAC_INIT_DMA_RX_FRAMES	25

/* Flow control queue count */
#define XLGMAC_MAX_FLOW_CONTROL_QUEUES	8

/* Maximum MAC address hash table size (256 bits = 8 bytes) */
#define XLGMAC_MAC_HASH_TABLE_SIZE	8

/* Timestamp support - values based on 50MHz PTP clock
 *   50MHz => 20 nsec
 */
#define XLGMAC_TSTAMP_SSINC		20
#define XLGMAC_TSTAMP_SNSINC		0

#define XLGMAC_MDIO_RD_TIMEOUT		10000

#define XLGMAC_SYSCLOCK			62500000 /* System clock is 62.5MHz */

void xlgmac_init_hw_ops(struct dwc_eth_hw_ops *hw_ops);

#endif /* __DWC_XLGMAC_H__ */
