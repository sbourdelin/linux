/* Broadcom NetXtreme-C/E network driver.
 *
 * Copyright (c) 2016-2017 Broadcom Limited
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 */

#ifndef BNXT_XDP_H
#define BNXT_XDP_H

#ifdef CONFIG_BNXT_XDP
bool bnxt_rx_xdp(struct bnxt_rx_ring_info *rxr, u16 cons, void *data,
		 u8 *data_ptr, unsigned int len, dma_addr_t dma_addr,
		 u8 *event);

int bnxt_xdp(struct net_device *dev, struct netdev_xdp *xdp);

#else
static inline bool bnxt_rx_xdp(struct bnxt_rx_ring_info *rxr, u16 cons,
			       void *data, u8 *data_ptr, unsigned int len,
			       dma_addr_t dma_addr, u8 *event)
{
	return false;
}
#endif
#endif
