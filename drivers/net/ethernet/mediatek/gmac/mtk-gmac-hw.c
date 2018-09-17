// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2018 MediaTek Inc.
#include <linux/phy.h>
#include <linux/mdio.h>
#include <linux/bitrev.h>
#include <linux/crc32.h>
#include <linux/dcbnl.h>

#include "mtk-gmac.h"

static int gmac_tx_complete(struct gmac_dma_desc *dma_desc)
{
	return !GMAC_GET_REG_BITS_LE(dma_desc->desc3,
				     TX_NORMAL_DESC3_OWN_POS,
				     TX_NORMAL_DESC3_OWN_LEN);
}

static int gmac_disable_rx_csum(struct gmac_pdata *pdata)
{
	u32 regval;

	regval = GMAC_IOREAD(pdata, MAC_MCR);
	regval = GMAC_SET_REG_BITS(regval, MAC_MCR_IPC_POS,
				   MAC_MCR_IPC_LEN, 0);
	GMAC_IOWRITE(pdata, MAC_MCR, regval);

	return 0;
}

static int gmac_enable_rx_csum(struct gmac_pdata *pdata)
{
	u32 regval;

	regval = GMAC_IOREAD(pdata, MAC_MCR);
	regval = GMAC_SET_REG_BITS(regval, MAC_MCR_IPC_POS,
				   MAC_MCR_IPC_LEN, 1);
	GMAC_IOWRITE(pdata, MAC_MCR, regval);

	return 0;
}

static int gmac_set_mac_address(struct gmac_pdata *pdata,
				u8 *addr,
				unsigned int idx)
{
	unsigned int mac_addr_hi, mac_addr_lo;

	mac_addr_hi = (addr[5] <<  8) | (addr[4] <<  0);
	mac_addr_lo = (addr[3] << 24) | (addr[2] << 16) |
		      (addr[1] <<  8) | (addr[0] <<  0);
	mac_addr_hi = GMAC_SET_REG_BITS(mac_addr_hi,
					MAC_ADDR_HR_AE_POS,
					MAC_ADDR_HR_AE_LEN,
					1);

	GMAC_IOWRITE(pdata, MAC_ADDR_HR(idx), mac_addr_hi);
	GMAC_IOWRITE(pdata, MAC_ADDR_LR(idx), mac_addr_lo);

	return 0;
}

static void gmac_set_mac_reg(struct gmac_pdata *pdata,
			     struct netdev_hw_addr *ha,
			     unsigned int idx)
{
	unsigned int mac_addr_hi, mac_addr_lo;
	u8 *mac_addr;

	mac_addr_lo = 0;
	mac_addr_hi = 0;

	if (ha) {
		mac_addr = (u8 *)&mac_addr_lo;
		mac_addr[0] = ha->addr[0];
		mac_addr[1] = ha->addr[1];
		mac_addr[2] = ha->addr[2];
		mac_addr[3] = ha->addr[3];
		mac_addr = (u8 *)&mac_addr_hi;
		mac_addr[0] = ha->addr[4];
		mac_addr[1] = ha->addr[5];

		netif_dbg(pdata, drv, pdata->netdev,
			  "adding mac address %pM at %#x\n",
			  ha->addr, idx);

		mac_addr_hi = GMAC_SET_REG_BITS(mac_addr_hi,
						MAC_ADDR_HR_AE_POS,
						MAC_ADDR_HR_AE_LEN,
						1);
	}

	GMAC_IOWRITE(pdata, MAC_ADDR_HR(idx), mac_addr_hi);
	GMAC_IOWRITE(pdata, MAC_ADDR_LR(idx), mac_addr_lo);
}

static int gmac_enable_rx_vlan_stripping(struct gmac_pdata *pdata)
{
	u32 regval;

	regval = GMAC_IOREAD(pdata, MAC_VLANTR);
	/* Put the VLAN tag in the Rx descriptor */
	regval = GMAC_SET_REG_BITS(regval, MAC_VLANTR_EVLRXS_POS,
				   MAC_VLANTR_EVLRXS_LEN, 1);
	/* Don't check the VLAN type */
	regval = GMAC_SET_REG_BITS(regval, MAC_VLANTR_DOVLTC_POS,
				   MAC_VLANTR_DOVLTC_LEN, 1);
	/* Check only C-TAG (0x8100) packets */
	regval = GMAC_SET_REG_BITS(regval, MAC_VLANTR_ERSVLM_POS,
				   MAC_VLANTR_ERSVLM_LEN, 0);
	/* Don't consider an S-TAG (0x88A8) packet as a VLAN packet */
	regval = GMAC_SET_REG_BITS(regval, MAC_VLANTR_ESVL_POS,
				   MAC_VLANTR_ESVL_LEN, 0);
	/* Enable VLAN tag stripping */
	regval = GMAC_SET_REG_BITS(regval, MAC_VLANTR_EVLS_POS,
				   MAC_VLANTR_EVLS_LEN, 0x3);
	GMAC_IOWRITE(pdata, MAC_VLANTR, regval);

	return 0;
}

static int gmac_disable_rx_vlan_stripping(struct gmac_pdata *pdata)
{
	u32 regval;

	regval = GMAC_IOREAD(pdata, MAC_VLANTR);
	regval = GMAC_SET_REG_BITS(regval, MAC_VLANTR_EVLS_POS,
				   MAC_VLANTR_EVLS_LEN, 0);
	GMAC_IOWRITE(pdata, MAC_VLANTR, regval);

	return 0;
}

static int gmac_enable_rx_vlan_filtering(struct gmac_pdata *pdata)
{
	u32 regval;

	regval = GMAC_IOREAD(pdata, MAC_PFR);
	/* Enable VLAN filtering */
	regval = GMAC_SET_REG_BITS(regval, MAC_PFR_VTFE_POS,
				   MAC_PFR_VTFE_LEN, 1);
	GMAC_IOWRITE(pdata, MAC_PFR, regval);

	regval = GMAC_IOREAD(pdata, MAC_VLANTR);
	/* Enable VLAN Hash Table filtering */
	regval = GMAC_SET_REG_BITS(regval, MAC_VLANTR_VTHM_POS,
				   MAC_VLANTR_VTHM_LEN, 1);
	/* Disable VLAN tag inverse matching */
	regval = GMAC_SET_REG_BITS(regval, MAC_VLANTR_VTIM_POS,
				   MAC_VLANTR_VTIM_LEN, 0);
	/* Only filter on the lower 12-bits of the VLAN tag */
	regval = GMAC_SET_REG_BITS(regval, MAC_VLANTR_ETV_POS,
				   MAC_VLANTR_ETV_LEN, 1);
	/* In order for the VLAN Hash Table filtering to be effective,
	 * the VLAN tag identifier in the VLAN Tag Register must not
	 * be zero.  Set the VLAN tag identifier to "1" to enable the
	 * VLAN Hash Table filtering.  This implies that a VLAN tag of
	 * 1 will always pass filtering.
	 */
	regval = GMAC_SET_REG_BITS(regval, MAC_VLANTR_VL_POS,
				   MAC_VLANTR_VL_LEN, 1);
	GMAC_IOWRITE(pdata, MAC_VLANTR, regval);

	return 0;
}

static int gmac_disable_rx_vlan_filtering(struct gmac_pdata *pdata)
{
	u32 regval;

	regval = GMAC_IOREAD(pdata, MAC_PFR);
	/* Disable VLAN filtering */
	regval = GMAC_SET_REG_BITS(regval, MAC_PFR_VTFE_POS,
				   MAC_PFR_VTFE_LEN, 0);
	GMAC_IOWRITE(pdata, MAC_PFR, regval);

	return 0;
}

static u32 gmac_vid_crc32_le(__le16 vid_le)
{
	unsigned char *data = (unsigned char *)&vid_le;
	unsigned char data_byte = 0;
	u32 poly = 0xedb88320;
	u32 crc = ~0;
	u32 temp = 0;
	int i, bits;

	bits = get_bitmask_order(VLAN_VID_MASK);
	for (i = 0; i < bits; i++) {
		if ((i % 8) == 0)
			data_byte = data[i / 8];

		temp = ((crc & 1) ^ data_byte) & 1;
		crc >>= 1;
		data_byte >>= 1;

		if (temp)
			crc ^= poly;
	}

	return crc;
}

static int gmac_update_vlan_hash_table(struct gmac_pdata *pdata)
{
	u16 vlan_hash_table = 0;
	__le16 vid_le;
	u32 regval;
	u32 crc;
	u16 vid;

	/* Generate the VLAN Hash Table value */
	for_each_set_bit(vid, pdata->active_vlans, VLAN_N_VID) {
		/* Get the CRC32 value of the VLAN ID */
		vid_le = cpu_to_le16(vid);
		crc = bitrev32(~gmac_vid_crc32_le(vid_le)) >> 28;

		vlan_hash_table |= (1 << crc);
	}

	regval = GMAC_IOREAD(pdata, MAC_VLANHTR);
	/* Set the VLAN Hash Table filtering register */
	regval = GMAC_SET_REG_BITS(regval, MAC_VLANHTR_VLHT_POS,
				   MAC_VLANHTR_VLHT_LEN, vlan_hash_table);
	GMAC_IOWRITE(pdata, MAC_VLANHTR, regval);

	return 0;
}

static void gmac_update_vlan_id(struct gmac_pdata *pdata,
				u16 vid,
				unsigned int enable,
				unsigned int ofs)
{
	u32 regval;

	/* Set the VLAN filtering register */
	regval = GMAC_IOREAD(pdata, MAC_VLANTFR);
	regval = GMAC_SET_REG_BITS(regval, MAC_VLANTFR_VID_POS,
				   MAC_VLANTFR_VID_LEN, vid);
	regval = GMAC_SET_REG_BITS(regval, MAC_VLANTFR_VEN_POS,
				   MAC_VLANTFR_VEN_LEN, enable);
	GMAC_IOWRITE(pdata, MAC_VLANTFR, regval);

	/* Set the VLAN filtering register */
	regval = GMAC_IOREAD(pdata, MAC_VLANTR);
	regval = GMAC_SET_REG_BITS(regval, MAC_VLANTR_OFS_POS,
				   MAC_VLANTR_OFS_LEN, ofs);
	regval = GMAC_SET_REG_BITS(regval, MAC_VLANTR_CT_POS,
				   MAC_VLANTR_CT_POS, 0);
	regval = GMAC_SET_REG_BITS(regval, MAC_VLANTR_OB_POS,
				   MAC_VLANTR_OB_POS, 1);
	GMAC_IOWRITE(pdata, MAC_VLANTR, regval);

	ofs++;
}

static int gmac_update_vlan(struct gmac_pdata *pdata)
{
	u32 ofs = 0;
	u16 vid;

	/* By default, receive only VLAN pkt with VID = 1
	 * because writing 0 will pass all VLAN pkt
	 * disable check vlan tag
	 */
	for (ofs = 0; ofs < pdata->vlan_weight; ofs++)
		gmac_update_vlan_id(pdata, 1, 0, ofs);

	ofs = 0;
	/* Generate the VLAN Hash Table value */
	for_each_set_bit(vid, pdata->active_vlans, VLAN_N_VID) {
		gmac_update_vlan_id(pdata, vid, 1, ofs);
		ofs++;
	}

	return 0;
}

static int gmac_set_promiscuous_mode(struct gmac_pdata *pdata,
				     unsigned int enable)
{
	unsigned int val = enable ? 1 : 0;
	u32 regval;

	regval = GMAC_GET_REG_BITS(GMAC_IOREAD(pdata, MAC_PFR),
				   MAC_PFR_PR_POS, MAC_PFR_PR_LEN);
	if (regval == val)
		return 0;

	netif_dbg(pdata, drv, pdata->netdev, "%s promiscuous mode\n",
		  enable ? "entering" : "leaving");

	regval = GMAC_IOREAD(pdata, MAC_PFR);
	regval = GMAC_SET_REG_BITS(regval, MAC_PFR_PR_POS,
				   MAC_PFR_PR_LEN, val);
	GMAC_IOWRITE(pdata, MAC_PFR, regval);

	/* Hardware will still perform VLAN filtering in promiscuous mode */
	if (enable) {
		gmac_disable_rx_vlan_filtering(pdata);
	} else {
		if (pdata->netdev->features & NETIF_F_HW_VLAN_CTAG_FILTER)
			gmac_enable_rx_vlan_filtering(pdata);
	}

	return 0;
}

static int gmac_set_all_multicast_mode(struct gmac_pdata *pdata,
				       unsigned int enable)
{
	unsigned int val = enable ? 1 : 0;
	u32 regval;

	regval = GMAC_GET_REG_BITS(GMAC_IOREAD(pdata, MAC_PFR),
				   MAC_PFR_PM_POS, MAC_PFR_PM_LEN);
	if (regval == val)
		return 0;

	netif_dbg(pdata, drv, pdata->netdev, "%s allmulti mode\n",
		  enable ? "entering" : "leaving");

	regval = GMAC_IOREAD(pdata, MAC_PFR);
	regval = GMAC_SET_REG_BITS(regval, MAC_PFR_PM_POS,
				   MAC_PFR_PM_LEN, val);
	GMAC_IOWRITE(pdata, MAC_PFR, regval);

	return 0;
}

static void gmac_set_mac_addn_addrs(struct gmac_pdata *pdata)
{
	struct net_device *netdev = pdata->netdev;
	struct netdev_hw_addr *ha;
	unsigned int addn_macs;
	unsigned int addr_idx;

	addr_idx = 1;
	addn_macs = pdata->hw_feat.addn_mac;

	if (netdev_uc_count(netdev) > addn_macs) {
		gmac_set_promiscuous_mode(pdata, 1);
	} else {
		netdev_for_each_uc_addr(ha, netdev) {
			gmac_set_mac_reg(pdata, ha, addr_idx);
			addr_idx++;
			addn_macs--;
		}

		if (netdev_mc_count(netdev) > addn_macs) {
			gmac_set_all_multicast_mode(pdata, 1);
		} else {
			netdev_for_each_mc_addr(ha, netdev) {
				gmac_set_mac_reg(pdata, ha, addr_idx);
				addr_idx++;
				addn_macs--;
			}
		}
	}

	/* Clear remaining additional MAC address entries */
	while (addn_macs--) {
		gmac_set_mac_reg(pdata, NULL, addr_idx);
		addr_idx++;
	}
}

static void gmac_set_mac_hash_table(struct gmac_pdata *pdata)
{
	unsigned int hash_table_shift, hash_table_count;
	u32 hash_table[GMAC_MAC_HASH_TABLE_SIZE];
	struct net_device *netdev = pdata->netdev;
	struct netdev_hw_addr *ha;
	unsigned int i;
	u32 crc;

	hash_table_shift = 26 - (pdata->hw_feat.hash_table_size >> 7);
	hash_table_count = pdata->hw_feat.hash_table_size / 32;
	memset(hash_table, 0, sizeof(hash_table));

	/* Build the MAC Hash Table register values */
	netdev_for_each_uc_addr(ha, netdev) {
		crc = bitrev32(~crc32_le(~0, ha->addr, ETH_ALEN));
		crc >>= hash_table_shift;
		hash_table[crc >> 5] |= (1 << (crc & 0x1f));
	}

	netdev_for_each_mc_addr(ha, netdev) {
		crc = bitrev32(~crc32_le(~0, ha->addr, ETH_ALEN));
		crc >>= hash_table_shift;
		hash_table[crc >> 5] |= (1 << (crc & 0x1f));
	}

	/* Set the MAC Hash Table registers */
	for (i = 0; i < hash_table_count; i++)
		GMAC_IOWRITE(pdata, MAC_HTR(i), hash_table[i]);
}

static int gmac_add_mac_addresses(struct gmac_pdata *pdata)
{
	if (pdata->hw_feat.hash_table_size)
		gmac_set_mac_hash_table(pdata);
	else
		gmac_set_mac_addn_addrs(pdata);

	return 0;
}

static void gmac_config_mac_address(struct gmac_pdata *pdata)
{
	u32 regval;

	gmac_set_mac_address(pdata, pdata->netdev->dev_addr, 0);

	/* Filtering is done using perfect filtering and hash filtering */
	if (pdata->hw_feat.hash_table_size) {
		regval = GMAC_IOREAD(pdata, MAC_PFR);
		regval = GMAC_SET_REG_BITS(regval, MAC_PFR_HPF_POS,
					   MAC_PFR_HPF_LEN, 1);
		regval = GMAC_SET_REG_BITS(regval, MAC_PFR_HUC_POS,
					   MAC_PFR_HUC_LEN, 1);
		regval = GMAC_SET_REG_BITS(regval, MAC_PFR_HMC_POS,
					   MAC_PFR_HMC_LEN, 1);
		GMAC_IOWRITE(pdata, MAC_PFR, regval);
	}
}

static void gmac_config_jumbo_disable(struct gmac_pdata *pdata)
{
	u32 regval;

	regval = GMAC_IOREAD(pdata, MAC_MCR);
	regval = GMAC_SET_REG_BITS(regval, MAC_MCR_JE_POS,
				   MAC_MCR_JE_LEN, 0);
	GMAC_IOWRITE(pdata, MAC_MCR, regval);
}

static void gmac_config_checksum_offload(struct gmac_pdata *pdata)
{
	if (pdata->netdev->features & NETIF_F_RXCSUM)
		gmac_enable_rx_csum(pdata);
	else
		gmac_disable_rx_csum(pdata);
}

static void gmac_config_vlan_support(struct gmac_pdata *pdata)
{
	u32 regval;

	regval = GMAC_IOREAD(pdata, MAC_VLANIR);
	/* Indicate that VLAN Tx CTAGs come from context descriptors */
	regval = GMAC_SET_REG_BITS(regval, MAC_VLANIR_CSVL_POS,
				   MAC_VLANIR_CSVL_LEN, 0);
	regval = GMAC_SET_REG_BITS(regval, MAC_VLANIR_VLTI_POS,
				   MAC_VLANIR_VLTI_LEN, 1);
	GMAC_IOWRITE(pdata, MAC_VLANIR, regval);

	/* Set the current VLAN Hash Table register value */
	gmac_update_vlan_hash_table(pdata);

	if (pdata->netdev->features & NETIF_F_HW_VLAN_CTAG_FILTER)
		gmac_enable_rx_vlan_filtering(pdata);
	else
		gmac_disable_rx_vlan_filtering(pdata);

	if (pdata->netdev->features & NETIF_F_HW_VLAN_CTAG_RX)
		gmac_enable_rx_vlan_stripping(pdata);
	else
		gmac_disable_rx_vlan_stripping(pdata);
}

static int gmac_config_rx_mode(struct gmac_pdata *pdata)
{
	struct net_device *netdev = pdata->netdev;
	unsigned int pr_mode, am_mode;

	pr_mode = ((netdev->flags & IFF_PROMISC) != 0);
	am_mode = ((netdev->flags & IFF_ALLMULTI) != 0);

	gmac_set_promiscuous_mode(pdata, pr_mode);
	gmac_set_all_multicast_mode(pdata, am_mode);

	gmac_add_mac_addresses(pdata);

	return 0;
}

static void gmac_prepare_tx_stop(struct gmac_pdata *pdata,
				 struct gmac_channel *channel)
{
	unsigned int tx_dsr, tx_pos, tx_qidx;
	unsigned long tx_timeout;
	unsigned int tx_status;

	/* Calculate the status register to read and the position within */
	if (channel->queue_index < DMA_DSRX_FIRST_QUEUE) {
		tx_dsr = DMA_DSR0;
		tx_pos = (channel->queue_index * DMA_DSR_Q_LEN) +
			 DMA_DSR0_TPS_START;
	} else {
		tx_qidx = channel->queue_index - DMA_DSRX_FIRST_QUEUE;

		tx_dsr = DMA_DSR1 + ((tx_qidx / DMA_DSRX_QPR) * DMA_DSRX_INC);
		tx_pos = ((tx_qidx % DMA_DSRX_QPR) * DMA_DSR_Q_LEN) +
			 DMA_DSRX_TPS_START;
	}

	/* The Tx engine cannot be stopped if it is actively processing
	 * descriptors. Wait for the Tx engine to enter the stopped or
	 * suspended state.  Don't wait forever though...
	 */
	tx_timeout = jiffies + (GMAC_DMA_STOP_TIMEOUT * HZ);
	while (time_before(jiffies, tx_timeout)) {
		tx_status = GMAC_IOREAD(pdata, tx_dsr);
		tx_status = GMAC_GET_REG_BITS(tx_status, tx_pos,
					      DMA_DSR_TPS_LEN);
		if (tx_status == tx_stopped ||
		    tx_status == tx_suspended)
			break;

		usleep_range(500, 1000);
	}

	if (!time_before(jiffies, tx_timeout))
		netdev_info(pdata->netdev,
			    "timed out waiting for Tx DMA channel %u to stop\n",
			    channel->queue_index);
}

static void gmac_enable_tx(struct gmac_pdata *pdata)
{
	struct gmac_channel *channel;
	unsigned int i;
	u32 regval;

	/* Enable each Tx DMA channel */
	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			break;

		regval = GMAC_IOREAD(pdata, DMA_CH_TCR(i));
		regval = GMAC_SET_REG_BITS(regval, DMA_CH_TCR_ST_POS,
					   DMA_CH_TCR_ST_LEN, 1);
		GMAC_IOWRITE(pdata, DMA_CH_TCR(i), regval);
	}

	/* Enable each Tx queue */
	for (i = 0; i < pdata->tx_q_count; i++) {
		regval = GMAC_IOREAD(pdata, MTL_Q_TQOMR(i));
		regval = GMAC_SET_REG_BITS(regval, MTL_Q_TQOMR_TXQEN_POS,
					   MTL_Q_TQOMR_TXQEN_LEN,
					   MTL_Q_ENABLED);
		GMAC_IOWRITE(pdata, MTL_Q_TQOMR(i), regval);
	}

	/* Enable MAC Tx */
	regval = GMAC_IOREAD(pdata, MAC_MCR);
	regval = GMAC_SET_REG_BITS(regval, MAC_MCR_TE_POS,
				   MAC_MCR_TE_LEN, 1);
	GMAC_IOWRITE(pdata, MAC_MCR, regval);
}

static void gmac_disable_tx(struct gmac_pdata *pdata)
{
	struct gmac_channel *channel;
	unsigned int i;
	u32 regval;

	/* Disable each Tx DMA channel */
	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			break;
		/* Issue Tx dma stop command */
		regval = GMAC_IOREAD(pdata, DMA_CH_TCR(i));
		regval = GMAC_SET_REG_BITS(regval, DMA_CH_TCR_ST_POS,
					   DMA_CH_TCR_ST_LEN, 0);
		GMAC_IOWRITE(pdata, DMA_CH_TCR(i), regval);
		/* Waiting for Tx DMA channel stop */
		gmac_prepare_tx_stop(pdata, channel);
	}

	/* Disable MAC Tx */
	regval = GMAC_IOREAD(pdata, MAC_MCR);
	regval = GMAC_SET_REG_BITS(regval, MAC_MCR_TE_POS,
				   MAC_MCR_TE_LEN, 0);
	GMAC_IOWRITE(pdata, MAC_MCR, regval);

	/* Disable each Tx queue */
	for (i = 0; i < pdata->tx_q_count; i++) {
		regval = GMAC_IOREAD(pdata, MTL_Q_TQOMR(i));
		regval = GMAC_SET_REG_BITS(regval, MTL_Q_TQOMR_TXQEN_POS,
					   MTL_Q_TQOMR_TXQEN_LEN, 0);
		GMAC_IOWRITE(pdata, MTL_Q_TQOMR(i), regval);
	}
}

static void gmac_prepare_rx_stop(struct gmac_pdata *pdata,
				 struct gmac_channel *channel)
{
	unsigned int rx_dsr, rx_pos, rx_qidx;
	unsigned long rx_timeout;
	unsigned int rx_status;

	/* Calculate the status register to read and the position within */
	if (channel->queue_index < DMA_DSRX_FIRST_QUEUE) {
		rx_dsr = DMA_DSR0;
		rx_pos = (channel->queue_index * DMA_DSR_Q_LEN) +
			 DMA_DSR0_RPS_START;
	} else {
		rx_qidx = channel->queue_index - DMA_DSRX_FIRST_QUEUE;

		rx_dsr = DMA_DSR1 + ((rx_qidx / DMA_DSRX_QPR) * DMA_DSRX_INC);
		rx_pos = ((rx_qidx % DMA_DSRX_QPR) * DMA_DSR_Q_LEN) +
			 DMA_DSRX_RPS_START;
	}

		/* The Rx engine cannot be stopped if it is actively processing
		 * descriptors. Wait for the Rx engine to enter the stopped or
		 * suspended, waiting state.  Don't wait forever though...
		 */
		rx_timeout = jiffies + (GMAC_DMA_STOP_TIMEOUT * HZ);
		while (time_before(jiffies, rx_timeout)) {
			rx_status = GMAC_IOREAD(pdata, rx_dsr);
			rx_status = GMAC_GET_REG_BITS(rx_status, rx_pos,
						      DMA_DSR_RPS_LEN);
			if (rx_status == rx_stopped ||
			    rx_status == rx_suspended ||
			    rx_status == rx_running_waiting)
				break;

			usleep_range(500, 1000);
		}

	if (!time_before(jiffies, rx_timeout))
		netdev_info(pdata->netdev,
			    "timed out waiting for Rx queue %u to empty\n",
			    channel->queue_index);
}

static void gmac_enable_rx(struct gmac_pdata *pdata)
{
	struct gmac_channel *channel;
	unsigned int regval, i;

	/* Enable each Rx DMA channel */
	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->rx_ring)
			break;

		regval = GMAC_IOREAD(pdata, DMA_CH_RCR(i));
		regval = GMAC_SET_REG_BITS(regval, DMA_CH_RCR_SR_POS,
					   DMA_CH_RCR_SR_LEN, 1);
		GMAC_IOWRITE(pdata, DMA_CH_RCR(i), regval);
	}

	/* Enable each Rx queue */
	regval = 0;
	for (i = 0; i < pdata->rx_q_count; i++)
		regval |= (0x02 << (i << 1));

	GMAC_IOWRITE(pdata, MAC_RQC0R, regval);

	/* Enable MAC Rx */
	regval = GMAC_IOREAD(pdata, MAC_MCR);
	regval = GMAC_SET_REG_BITS(regval, MAC_MCR_CST_POS,
				   MAC_MCR_CST_LEN, 1);
	regval = GMAC_SET_REG_BITS(regval, MAC_MCR_ACS_POS,
				   MAC_MCR_ACS_LEN, 1);
	regval = GMAC_SET_REG_BITS(regval, MAC_MCR_RE_POS,
				   MAC_MCR_RE_LEN, 1);
	GMAC_IOWRITE(pdata, MAC_MCR, regval);
}

static void gmac_disable_rx(struct gmac_pdata *pdata)
{
	struct gmac_channel *channel;
	unsigned int i;
	u32 regval;

	/* Disable MAC Rx */
	regval = GMAC_IOREAD(pdata, MAC_MCR);
	regval = GMAC_SET_REG_BITS(regval, MAC_MCR_CST_POS,
				   MAC_MCR_CST_LEN, 0);
	regval = GMAC_SET_REG_BITS(regval, MAC_MCR_ACS_POS,
				   MAC_MCR_ACS_LEN, 0);
	regval = GMAC_SET_REG_BITS(regval, MAC_MCR_RE_POS,
				   MAC_MCR_RE_LEN, 0);
	GMAC_IOWRITE(pdata, MAC_MCR, regval);

	/* Disable each Rx queue */
	GMAC_IOWRITE(pdata, MAC_RQC0R, 0);

	/* Disable each Rx DMA channel */
	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->rx_ring)
			break;

		regval = GMAC_IOREAD(pdata, DMA_CH_RCR(i));
		regval = GMAC_SET_REG_BITS(regval, DMA_CH_RCR_SR_POS,
					   DMA_CH_RCR_SR_LEN, 0);
		GMAC_IOWRITE(pdata, DMA_CH_RCR(i), regval);

		/* Waiting for Rx DMA channel stop */
		gmac_prepare_rx_stop(pdata, channel);
	}
}

static void gmac_tx_start_xmit(struct gmac_channel *channel,
			       struct gmac_ring *ring)
{
	struct gmac_pdata *pdata = channel->pdata;
	struct gmac_desc_data *desc_data;
	unsigned int q = channel->queue_index;

	/* Make sure everything is written before the register write */
	wmb();

	/* Issue a poll command to Tx DMA by writing address
	 * of next immediate free descriptor
	 */
	desc_data = GMAC_GET_DESC_DATA(ring, ring->cur);
	GMAC_IOWRITE(pdata, DMA_CH_TDTR(q),
		     lower_32_bits(desc_data->dma_desc_addr));

	/* Start the Tx timer */
	if (pdata->tx_usecs && !channel->tx_timer_active) {
		channel->tx_timer_active = 1;
		mod_timer(&channel->tx_timer,
			  jiffies + usecs_to_jiffies(pdata->tx_usecs));
	}

	ring->tx.xmit_more = 0;
}

static void gmac_dev_xmit(struct gmac_channel *channel)
{
	struct gmac_pdata *pdata = channel->pdata;
	struct gmac_ring *ring = channel->tx_ring;
	unsigned int tso_context, vlan_context;
	struct gmac_desc_data *desc_data;
	struct gmac_dma_desc *dma_desc;
	struct gmac_pkt_info *pkt_info;
	unsigned int csum, tso, vlan;
	int start_index = ring->cur;
	int cur_index = ring->cur;
	unsigned int tx_set_ic;
	int i;

	pkt_info = &ring->pkt_info;
	csum = GMAC_GET_REG_BITS(pkt_info->attributes,
				 TX_PACKET_ATTRIBUTES_CSUM_ENABLE_POS,
				 TX_PACKET_ATTRIBUTES_CSUM_ENABLE_LEN);
	tso = GMAC_GET_REG_BITS(pkt_info->attributes,
				TX_PACKET_ATTRIBUTES_TSO_ENABLE_POS,
				TX_PACKET_ATTRIBUTES_TSO_ENABLE_LEN);
	vlan = GMAC_GET_REG_BITS(pkt_info->attributes,
				 TX_PACKET_ATTRIBUTES_VLAN_CTAG_POS,
				 TX_PACKET_ATTRIBUTES_VLAN_CTAG_LEN);

	if (tso && pkt_info->mss != ring->tx.cur_mss)
		tso_context = 1;
	else
		tso_context = 0;

	if (vlan && pkt_info->vlan_ctag != ring->tx.cur_vlan_ctag)
		vlan_context = 1;
	else
		vlan_context = 0;

	/* Determine if an interrupt should be generated for this Tx:
	 *   Interrupt:
	 *     - Tx frame count exceeds the frame count setting
	 *     - Addition of Tx frame count to the frame count since the
	 *       last interrupt was set exceeds the frame count setting
	 *   No interrupt:
	 *     - No frame count setting specified (ethtool -C ethX tx-frames 0)
	 *     - Addition of Tx frame count to the frame count since the
	 *       last interrupt was set does not exceed the frame count setting
	 */
	ring->coalesce_count += pkt_info->tx_packets;
	if (!pdata->tx_frames)
		tx_set_ic = 0;
	else if (pkt_info->tx_packets > pdata->tx_frames)
		tx_set_ic = 1;
	else if ((ring->coalesce_count % pdata->tx_frames) <
		 pkt_info->tx_packets)
		tx_set_ic = 1;
	else
		tx_set_ic = 0;

	desc_data = GMAC_GET_DESC_DATA(ring, cur_index);
	dma_desc = desc_data->dma_desc;

	/* Create a context descriptor if this is a TSO pkt_info */
	if (tso_context || vlan_context) {
		if (tso_context) {
			netif_dbg(pdata, tx_queued, pdata->netdev,
				  "TSO context descriptor, mss=%u\n",
				  pkt_info->mss);

			/* Set the MSS size */
			dma_desc->desc2 = GMAC_SET_REG_BITS_LE(dma_desc->desc2,
							       TX_CONTEXT_DESC2_MSS_POS,
							       TX_CONTEXT_DESC2_MSS_LEN,
							       pkt_info->mss);

			/* Mark it as a CONTEXT descriptor */
			dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
							       TX_CONTEXT_DESC3_CTXT_POS,
							       TX_CONTEXT_DESC3_CTXT_LEN,
							       1);

			/* Indicate this descriptor contains the MSS */
			dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
							       TX_CONTEXT_DESC3_TCMSSV_POS,
							       TX_CONTEXT_DESC3_TCMSSV_LEN,
							       1);

			ring->tx.cur_mss = pkt_info->mss;
		}

		if (vlan_context) {
			netif_dbg(pdata, tx_queued, pdata->netdev,
				  "VLAN context descriptor, ctag=%u\n",
				  pkt_info->vlan_ctag);

			/* Mark it as a CONTEXT descriptor */
			dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
							       TX_CONTEXT_DESC3_CTXT_POS,
							       TX_CONTEXT_DESC3_CTXT_LEN,
							       1);

			/* Set the VLAN tag */
			dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
							       TX_CONTEXT_DESC3_VT_POS,
							       TX_CONTEXT_DESC3_VT_LEN,
							       pkt_info->vlan_ctag);

			/* Indicate this descriptor contains the VLAN tag */
			dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
							       TX_CONTEXT_DESC3_VLTV_POS,
							       TX_CONTEXT_DESC3_VLTV_LEN,
							       1);

			ring->tx.cur_vlan_ctag = pkt_info->vlan_ctag;
		}

		cur_index++;
		desc_data = GMAC_GET_DESC_DATA(ring, cur_index);
		dma_desc = desc_data->dma_desc;
	}

	/* Update buffer address (for TSO this is the header) */
	dma_desc->desc0 =  cpu_to_le32(lower_32_bits(desc_data->skb_dma));

	/* Update the buffer length */
	dma_desc->desc2 = GMAC_SET_REG_BITS_LE(dma_desc->desc2,
					       TX_NORMAL_DESC2_HL_B1L_POS,
					       TX_NORMAL_DESC2_HL_B1L_LEN,
					       desc_data->skb_dma_len);

	/* VLAN tag insertion check */
	if (vlan) {
		dma_desc->desc2 = GMAC_SET_REG_BITS_LE(dma_desc->desc2,
						       TX_NORMAL_DESC2_VTIR_POS,
						       TX_NORMAL_DESC2_VTIR_LEN,
						       TX_NORMAL_DESC2_VLAN_INSERT);
		pdata->stats.tx_vlan_packets++;
	}

	/* Timestamp enablement check */
	if (GMAC_GET_REG_BITS(pkt_info->attributes,
			      TX_PACKET_ATTRIBUTES_PTP_POS,
			      TX_PACKET_ATTRIBUTES_PTP_LEN))
		dma_desc->desc2 = GMAC_SET_REG_BITS_LE(dma_desc->desc2,
						       TX_NORMAL_DESC2_TTSE_POS,
						       TX_NORMAL_DESC2_TTSE_LEN,
						       1);

	/* Mark it as First Descriptor */
	dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
					       TX_NORMAL_DESC3_FD_POS,
					       TX_NORMAL_DESC3_FD_LEN,
					       1);

	/* Mark it as a NORMAL descriptor */
	dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
					       TX_NORMAL_DESC3_CTXT_POS,
					       TX_NORMAL_DESC3_CTXT_LEN,
					       0);

	/* Set OWN bit if not the first descriptor */
	if (cur_index != start_index)
		dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
						       TX_NORMAL_DESC3_OWN_POS,
						       TX_NORMAL_DESC3_OWN_LEN,
						       1);

	if (tso) {
		/* Enable TSO */
		dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
						       TX_NORMAL_DESC3_TSE_POS,
						       TX_NORMAL_DESC3_TSE_LEN,
						       1);
		dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
						       TX_NORMAL_DESC3_TCPPL_POS,
						       TX_NORMAL_DESC3_TCPPL_LEN,
						       pkt_info->tcp_payload_len);
		dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
						       TX_NORMAL_DESC3_TCPHDRLEN_POS,
						       TX_NORMAL_DESC3_TCPHDRLEN_LEN,
						       pkt_info->tcp_header_len / 4);

		pdata->stats.tx_tso_packets++;
	} else {
		/* Enable CRC and Pad Insertion */
		dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
						       TX_NORMAL_DESC3_CPC_POS,
						       TX_NORMAL_DESC3_CPC_LEN,
						       0);

		/* Enable HW CSUM */
		if (csum)
			dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
							       TX_NORMAL_DESC3_CIC_POS,
							       TX_NORMAL_DESC3_CIC_LEN,
							       0x3);

		/* Set the total length to be transmitted */
		dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
						       TX_NORMAL_DESC3_FL_POS,
						       TX_NORMAL_DESC3_FL_LEN,
						       pkt_info->length);
	}

	for (i = cur_index - start_index + 1; i < pkt_info->desc_count; i++) {
		cur_index++;
		desc_data = GMAC_GET_DESC_DATA(ring, cur_index);
		dma_desc = desc_data->dma_desc;

		/* Update buffer address */
		dma_desc->desc0 =
			cpu_to_le32(lower_32_bits(desc_data->skb_dma));

		/* Update the buffer length */
		dma_desc->desc2 = GMAC_SET_REG_BITS_LE(dma_desc->desc2,
						       TX_NORMAL_DESC2_HL_B1L_POS,
						       TX_NORMAL_DESC2_HL_B1L_LEN,
						       desc_data->skb_dma_len);

		/* Set OWN bit */
		dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
						       TX_NORMAL_DESC3_OWN_POS,
						       TX_NORMAL_DESC3_OWN_LEN,
						       1);

		/* Mark it as NORMAL descriptor */
		dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
						       TX_NORMAL_DESC3_CTXT_POS,
						       TX_NORMAL_DESC3_CTXT_LEN,
						       0);

		/* Enable HW CSUM */
		if (csum)
			dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
							       TX_NORMAL_DESC3_CIC_POS,
							       TX_NORMAL_DESC3_CIC_LEN,
							       0x3);
	}

	/* Set LAST bit for the last descriptor */
	dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
					       TX_NORMAL_DESC3_LD_POS,
					       TX_NORMAL_DESC3_LD_LEN,
					       1);

	/* Set IC bit based on Tx coalescing settings */
	if (tx_set_ic)
		dma_desc->desc2 = GMAC_SET_REG_BITS_LE(dma_desc->desc2,
						       TX_NORMAL_DESC2_IC_POS,
						       TX_NORMAL_DESC2_IC_LEN,
						       1);

	/* Save the Tx info to report back during cleanup */
	desc_data->trx.packets = pkt_info->tx_packets;
	desc_data->trx.bytes = pkt_info->tx_bytes;

	/* In case the Tx DMA engine is running, make sure everything
	 * is written to the descriptor(s) before setting the OWN bit
	 * for the first descriptor
	 */
	dma_wmb();

	/* Set OWN bit for the first descriptor */
	desc_data = GMAC_GET_DESC_DATA(ring, start_index);
	dma_desc = desc_data->dma_desc;
	dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
					       TX_NORMAL_DESC3_OWN_POS,
					       TX_NORMAL_DESC3_OWN_LEN,
					       1);

	if (netif_msg_tx_queued(pdata))
		gmac_dump_tx_desc(pdata, ring, start_index,
				  pkt_info->desc_count, 1);

	/* Make sure ownership is written to the descriptor */
	smp_wmb();

	ring->cur = cur_index + 1;
	if (!pkt_info->skb->xmit_more ||
	    netif_xmit_stopped(netdev_get_tx_queue(pdata->netdev,
						   channel->queue_index)))
		gmac_tx_start_xmit(channel, ring);
	else
		ring->tx.xmit_more = 1;

	netif_dbg(pdata, tx_queued, pdata->netdev,
		  "%s: descriptors %u to %u written, %u:%u\n",
		  channel->name, start_index & (ring->dma_desc_count - 1),
		  (ring->cur - 1) & (ring->dma_desc_count - 1),
		  start_index,
		  ring->cur);
}

static int gmac_check_rx_tstamp(struct gmac_dma_desc *dma_desc)
{
	u32 own, ctxt;
	int ret = 1;

	own = GMAC_GET_REG_BITS_LE(dma_desc->desc3,
				   RX_CONTEXT_DESC3_OWN_POS,
				   RX_CONTEXT_DESC3_OWN_LEN);
	ctxt = GMAC_GET_REG_BITS_LE(dma_desc->desc3,
				    RX_CONTEXT_DESC3_CTXT_POS,
				    RX_CONTEXT_DESC3_CTXT_LEN);

	if (likely(!own && ctxt)) {
		if (dma_desc->desc0 == 0xffffffff &&
		    dma_desc->desc1 == 0xffffffff)
			/* Corrupted value */
			ret = -EINVAL;
		else
			/* A valid Timestamp is ready to be read */
			ret = 0;
	}

	/* Timestamp not ready */
	return ret;
}

static u64 gmac_get_rx_tstamp(struct gmac_dma_desc *dma_desc)
{
	u64 nsec;

	nsec = le32_to_cpu(dma_desc->desc1);
	nsec += le32_to_cpu(dma_desc->desc0) * 1000000000ULL;

	return nsec;
}

static int gmac_get_rx_tstamp_status(struct gmac_pdata *pdata,
				     struct gmac_dma_desc *next_desc,
				     struct gmac_pkt_info *pkt_info)
{
	int ret = -EINVAL;

	int i = 0;

	/* Check if timestamp is OK from context descriptor */
	do {
		ret = gmac_check_rx_tstamp(next_desc);
		if (ret <= 0)
			goto exit;
		i++;
	} while ((ret == 1) && (i < 10));

	if (i == 10) {
		ret = -EBUSY;
		netif_dbg(pdata, rx_status, pdata->netdev,
			  "Device has not yet updated the context desc to hold Rx time stamp\n");
	}
exit:
	if (likely(ret == 0)) {
		/* Timestamp Context Descriptor */
		pkt_info->rx_tstamp =
			gmac_get_rx_tstamp(next_desc);
		pkt_info->attributes =
			GMAC_SET_REG_BITS(pkt_info->attributes,
					  RX_PACKET_ATTRIBUTES_RX_TSTAMP_POS,
					  RX_PACKET_ATTRIBUTES_RX_TSTAMP_LEN,
					  1);
		return 1;
	}

	netif_dbg(pdata, rx_status, pdata->netdev, "RX hw timestamp corrupted\n");

	return ret;
}

static void gmac_tx_desc_reset(struct gmac_desc_data *desc_data)
{
	struct gmac_dma_desc *dma_desc = desc_data->dma_desc;

	/* Reset the Tx descriptor
	 *   Set buffer 1 (lo) address to zero
	 *   Set buffer 1 (hi) address to zero
	 *   Reset all other control bits (IC, TTSE, B2L & B1L)
	 *   Reset all other control bits (OWN, CTXT, FD, LD, CPC, CIC, etc)
	 */
	dma_desc->desc0 = 0;
	dma_desc->desc1 = 0;
	dma_desc->desc2 = 0;
	dma_desc->desc3 = 0;

	/* Make sure ownership is written to the descriptor */
	dma_wmb();
}

static void gmac_tx_desc_init(struct gmac_channel *channel)
{
	struct gmac_pdata *pdata = channel->pdata;
	struct gmac_ring *ring = channel->tx_ring;
	struct gmac_desc_data *desc_data;
	unsigned int q = channel->queue_index;
	int start_index = ring->cur;
	int i;

	/* Initialize all descriptors */
	for (i = 0; i < ring->dma_desc_count; i++) {
		desc_data = GMAC_GET_DESC_DATA(ring, i);

		/* Initialize Tx descriptor */
		gmac_tx_desc_reset(desc_data);
	}

	/* Update the total number of Tx descriptors */
	GMAC_IOWRITE(pdata, DMA_CH_TDRLR(q), ring->dma_desc_count - 1);

	/* Update the starting address of descriptor ring */
	desc_data = GMAC_GET_DESC_DATA(ring, start_index);
	GMAC_IOWRITE(pdata, DMA_CH_TDLR(q),
		     lower_32_bits(desc_data->dma_desc_addr));
}

static void gmac_rx_desc_reset(struct gmac_pdata *pdata,
			       struct gmac_desc_data *desc_data,
			       unsigned int index)
{
	struct gmac_dma_desc *dma_desc = desc_data->dma_desc;
	unsigned int rx_frames = pdata->rx_frames;
	unsigned int rx_usecs = pdata->rx_usecs;
	unsigned int inte;

	memset(dma_desc, 0, sizeof(struct gmac_dma_desc));

	if (!rx_usecs && !rx_frames) {
		/* No coalescing, interrupt for every descriptor */
		inte = 1;
	} else {
		/* Set interrupt based on Rx frame coalescing setting */
		if (rx_frames && !((index + 1) % rx_frames))
			inte = 1;
		else
			inte = 0;
	}

	/* Reset the Rx descriptor
	 * Normal Frame
	 *   Set buffer 1 address to skb dma address
	 *   Set buffer 2 address to 0 and
	 *   set control bits OWN and INTE
	 */

	dma_desc->desc0 = desc_data->skb_dma;
	dma_desc->desc1 = 0;
	dma_desc->desc2 = 0;
	dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
					       RX_NORMAL_DESC3_BUF2V_POS,
					       RX_NORMAL_DESC3_BUF2V_LEN,
					       0);

	dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
					       RX_NORMAL_DESC3_BUF1V_POS,
					       RX_NORMAL_DESC3_BUF1V_LEN,
					       1);

	dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
					       RX_NORMAL_DESC3_INTE_POS,
					       RX_NORMAL_DESC3_INTE_LEN,
					       inte);

	/* Since the Rx DMA engine is likely running, make sure everything
	 * is written to the descriptor(s) before setting the OWN bit
	 * for the descriptor
	 */
	dma_wmb();

	dma_desc->desc3 = GMAC_SET_REG_BITS_LE(dma_desc->desc3,
					       RX_NORMAL_DESC3_OWN_POS,
					       RX_NORMAL_DESC3_OWN_LEN,
					       1);

	/* Make sure ownership is written to the descriptor */
	dma_wmb();
}

static void gmac_rx_desc_init(struct gmac_channel *channel)
{
	struct gmac_pdata *pdata = channel->pdata;
	struct gmac_ring *ring = channel->rx_ring;
	unsigned int start_index = ring->cur;
	struct gmac_desc_data *desc_data;
	unsigned int q = channel->queue_index;
	unsigned int i;

	/* Initialize all descriptors */
	for (i = 0; i < ring->dma_desc_count; i++) {
		desc_data = GMAC_GET_DESC_DATA(ring, i);

		/* Initialize Rx descriptor */
		gmac_rx_desc_reset(pdata, desc_data, i);
	}

	/* Update the total number of Rx descriptors */
	GMAC_IOWRITE(pdata, DMA_CH_RDRLR(q), ring->dma_desc_count - 1);

	/* Update the starting address of descriptor ring */
	desc_data = GMAC_GET_DESC_DATA(ring, start_index);
	GMAC_IOWRITE(pdata, DMA_CH_RDLR(q),
		     lower_32_bits(desc_data->dma_desc_addr));

	/* Update the Rx Descriptor Tail Pointer */
	desc_data = GMAC_GET_DESC_DATA(ring, start_index +
				       ring->dma_desc_count - 1);
	GMAC_IOWRITE(pdata, DMA_CH_RDTR(q),
		     lower_32_bits(desc_data->dma_desc_addr));
}

static int gmac_is_context_desc(struct gmac_dma_desc *dma_desc)
{
	/* Rx and Tx share CTXT bit, so check TDES3.CTXT bit */
	return GMAC_GET_REG_BITS_LE(dma_desc->desc3,
				    TX_NORMAL_DESC3_CTXT_POS,
				    TX_NORMAL_DESC3_CTXT_LEN);
}

static int gmac_is_last_desc(struct gmac_dma_desc *dma_desc)
{
	/* Rx and Tx share LD bit, so check TDES3.LD bit */
	return GMAC_GET_REG_BITS_LE(dma_desc->desc3,
				    TX_NORMAL_DESC3_LD_POS,
				    TX_NORMAL_DESC3_LD_LEN);
}

static int gmac_is_rx_csum_error(struct gmac_dma_desc *dma_desc)
{
	/* Rx csum error, so check TDES1.IPHE/IPCB/IPCE bit */
	return GMAC_GET_REG_BITS_LE(dma_desc->desc1,
				    RX_NORMAL_DESC1_IPHE_POS,
				    RX_NORMAL_DESC1_IPHE_LEN) ||
	       GMAC_GET_REG_BITS_LE(dma_desc->desc1,
				    RX_NORMAL_DESC1_IPCB_POS,
				    RX_NORMAL_DESC1_IPCB_LEN) ||
	       GMAC_GET_REG_BITS_LE(dma_desc->desc1,
				    RX_NORMAL_DESC1_IPCE_POS,
				    RX_NORMAL_DESC1_IPCE_LEN);
}

static int gmac_is_rx_csum_valid(struct gmac_dma_desc *dma_desc)
{
	unsigned int vlan_type;

	vlan_type = GMAC_GET_REG_BITS_LE(dma_desc->desc3,
					 RX_NORMAL_DESC3_LT_POS,
					 RX_NORMAL_DESC3_LT_LEN);

	/* Rx csum error, so check TDES1.IPHE/IPCB/IPCE bit */
	return GMAC_GET_REG_BITS_LE(dma_desc->desc3,
				    RX_NORMAL_DESC3_RS0V_POS,
				    RX_NORMAL_DESC3_RS0V_LEN) &&
	       ((vlan_type == 4) || (vlan_type == 5));
}

static int gmac_disable_tx_flow_control(struct gmac_pdata *pdata)
{
	unsigned int max_q_count, q_count;
	unsigned int regval;
	unsigned int i;

	/* Clear MTL flow control */
	for (i = 0; i < pdata->rx_q_count; i++) {
		regval = GMAC_IOREAD(pdata, MTL_Q_RQOMR(i));
		regval = GMAC_SET_REG_BITS(regval, MTL_Q_RQOMR_EHFC_POS,
					   MTL_Q_RQOMR_EHFC_LEN, 0);
		GMAC_IOWRITE(pdata, MTL_Q_RQOMR(i), regval);
	}

	/* Clear MAC flow control */
	max_q_count = GMAC_MAX_FLOW_CONTROL_QUEUES;
	q_count = min_t(unsigned int, pdata->tx_q_count, max_q_count);
	for (i = 0; i < q_count; i++) {
		regval = GMAC_IOREAD(pdata, MAC_Q_TFCR(i));
		regval = GMAC_SET_REG_BITS(regval,
					   MAC_QTFCR_TFE_POS,
					   MAC_QTFCR_TFE_LEN,
					   0);
		GMAC_IOWRITE(pdata, MAC_Q_TFCR(i), regval);
	}

	return 0;
}

static int gmac_enable_tx_flow_control(struct gmac_pdata *pdata)
{
	unsigned int max_q_count, q_count;
	unsigned int regval;
	unsigned int i;

	/* Set MTL flow control */
	for (i = 0; i < pdata->rx_q_count; i++) {
		regval = GMAC_IOREAD(pdata, MTL_Q_RQOMR(i));
		regval = GMAC_SET_REG_BITS(regval, MTL_Q_RQOMR_EHFC_POS,
					   MTL_Q_RQOMR_EHFC_LEN, 1);
		GMAC_IOWRITE(pdata, MTL_Q_RQOMR(i), regval);
	}

	/* Set MAC flow control */
	max_q_count = GMAC_MAX_FLOW_CONTROL_QUEUES;
	q_count = min_t(unsigned int, pdata->tx_q_count, max_q_count);
	for (i = 0; i < q_count; i++) {
		regval = GMAC_IOREAD(pdata, MAC_Q_TFCR(i));
		/* Enable transmit flow control */
		regval = GMAC_SET_REG_BITS(regval, MAC_QTFCR_TFE_POS,
					   MAC_QTFCR_TFE_LEN, 1);
		/* Set pause time */
		regval = GMAC_SET_REG_BITS(regval, MAC_QTFCR_PT_POS,
					   MAC_QTFCR_PT_LEN, 0xffff);
		GMAC_IOWRITE(pdata, MAC_Q_TFCR(i), regval);
	}

	return 0;
}

static int gmac_disable_rx_flow_control(struct gmac_pdata *pdata)
{
	u32 regval;

	regval = GMAC_IOREAD(pdata, MAC_RFCR);
	regval = GMAC_SET_REG_BITS(regval, MAC_RFCR_RFE_POS,
				   MAC_RFCR_RFE_LEN, 0);
	GMAC_IOWRITE(pdata, MAC_RFCR, regval);

	return 0;
}

static int gmac_enable_rx_flow_control(struct gmac_pdata *pdata)
{
	u32 regval;

	regval = GMAC_IOREAD(pdata, MAC_RFCR);
	regval = GMAC_SET_REG_BITS(regval, MAC_RFCR_RFE_POS,
				   MAC_RFCR_RFE_LEN, 1);
	GMAC_IOWRITE(pdata, MAC_RFCR, regval);

	return 0;
}

static int gmac_config_tx_flow_control(struct gmac_pdata *pdata)
{
	if (pdata->tx_pause)
		gmac_enable_tx_flow_control(pdata);
	else
		gmac_disable_tx_flow_control(pdata);

	return 0;
}

static int gmac_config_rx_flow_control(struct gmac_pdata *pdata)
{
	if (pdata->rx_pause)
		gmac_enable_rx_flow_control(pdata);
	else
		gmac_disable_rx_flow_control(pdata);

	return 0;
}

static int gmac_config_rx_coalesce(struct gmac_pdata *pdata)
{
	struct gmac_channel *channel;
	unsigned int i;
	u32 regval;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->rx_ring)
			break;

		regval = GMAC_IOREAD(pdata, DMA_CH_RIWT(i));
		regval = GMAC_SET_REG_BITS(regval,
					   DMA_CH_RIWT_RWT_POS,
					   DMA_CH_RIWT_RWT_LEN,
					   pdata->rx_riwt);
		GMAC_IOWRITE(pdata, DMA_CH_RIWT(i), regval);
	}

	return 0;
}

static void gmac_config_flow_control(struct gmac_pdata *pdata)
{
	gmac_config_tx_flow_control(pdata);
	gmac_config_rx_flow_control(pdata);
}

static void gmac_config_rx_fep_enable(struct gmac_pdata *pdata)
{
	unsigned int i;
	u32 regval;

	for (i = 0; i < pdata->rx_q_count; i++) {
		regval = GMAC_IOREAD(pdata, MTL_Q_RQOMR(i));
		regval = GMAC_SET_REG_BITS(regval,
					   MTL_Q_RQOMR_FEP_POS,
					   MTL_Q_RQOMR_FEP_LEN,
					   1);
		GMAC_IOWRITE(pdata, MTL_Q_RQOMR(i), regval);
	}
}

static void gmac_config_rx_fup_enable(struct gmac_pdata *pdata)
{
	unsigned int i;
	u32 regval;

	for (i = 0; i < pdata->rx_q_count; i++) {
		regval = GMAC_IOREAD(pdata, MTL_Q_RQOMR(i));
		regval = GMAC_SET_REG_BITS(regval,
					   MTL_Q_RQOMR_FUP_POS,
					   MTL_Q_RQOMR_FUP_LEN,
					   1);
		GMAC_IOWRITE(pdata, MTL_Q_RQOMR(i), regval);
	}
}

static int gmac_config_tx_coalesce(struct gmac_pdata *pdata)
{
	return 0;
}

static void gmac_config_rx_buffer_size(struct gmac_pdata *pdata)
{
	struct gmac_channel *channel;
	unsigned int i;
	u32 regval;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->rx_ring)
			break;

		regval = GMAC_IOREAD(pdata, DMA_CH_RCR(i));
		/* for normal case, Rx Buffer size = 2048bytes */
		regval = GMAC_SET_REG_BITS(regval,
					   DMA_CH_RCR_RBSZ_POS,
					   DMA_CH_RCR_RBSZ_LEN,
					   pdata->rx_buf_size);
		GMAC_IOWRITE(pdata, DMA_CH_RCR(i), regval);
	}
}

static void gmac_config_tso_mode(struct gmac_pdata *pdata)
{
	struct gmac_channel *channel;
	unsigned int i;
	u32 regval;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			break;

		if (pdata->hw_feat.tso) {
			regval = GMAC_IOREAD(pdata, DMA_CH_TCR(i));
			regval = GMAC_SET_REG_BITS(regval,
						   DMA_CH_TCR_TSE_POS,
						   DMA_CH_TCR_TSE_LEN,
						   1);
			GMAC_IOWRITE(pdata, DMA_CH_TCR(i), regval);
		}
	}
}

static void gmac_config_sph_mode(struct gmac_pdata *pdata)
{
	struct gmac_channel *channel;
	unsigned int i;
	u32 regval;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->rx_ring)
			break;

		/* not support sph feature */
		regval = GMAC_IOREAD(pdata, DMA_CH_CR(i));
		regval = GMAC_SET_REG_BITS(regval,
					   DMA_CH_CR_SPH_POS,
					   DMA_CH_CR_SPH_LEN,
					   0);
		GMAC_IOWRITE(pdata, DMA_CH_CR(i), regval);
	}
}

static unsigned int gmac_usec_to_riwt(struct gmac_pdata *pdata,
				      unsigned int usec)
{
	unsigned long rate;
	unsigned int ret;

	rate = pdata->sysclk_rate;

	/* Convert the input usec value to the watchdog timer value. Each
	 * watchdog timer value is equivalent to 256 clock cycles.
	 * Calculate the required value as:
	 *   ( usec * ( system_clock_mhz / 10^6 ) / 256
	 */
	ret = (usec * (rate / 1000000)) / 256;

	return ret;
}

static unsigned int gmac_riwt_to_usec(struct gmac_pdata *pdata,
				      unsigned int riwt)
{
	unsigned long rate;
	unsigned int ret;

	rate = pdata->sysclk_rate;

	/* Convert the input watchdog timer value to the usec value. Each
	 * watchdog timer value is equivalent to 256 clock cycles.
	 * Calculate the required value as:
	 *   ( riwt * 256 ) / ( system_clock_mhz / 10^6 )
	 */
	ret = (riwt * 256) / (rate / 1000000);

	return ret;
}

static int gmac_config_rx_threshold(struct gmac_pdata *pdata,
				    unsigned int val)
{
	unsigned int i;
	u32 regval;

	for (i = 0; i < pdata->rx_q_count; i++) {
		regval = GMAC_IOREAD(pdata, MTL_Q_RQOMR(i));
		regval = GMAC_SET_REG_BITS(regval,
					   MTL_Q_RQOMR_RTC_POS,
					   MTL_Q_RQOMR_RTC_LEN,
					   val);
		GMAC_IOWRITE(pdata, MTL_Q_RQOMR(i), regval);
	}

	return 0;
}

static void gmac_config_mtl_mode(struct gmac_pdata *pdata)
{
	unsigned int i;
	u32 regval;

	/* Set Tx to weighted round robin scheduling algorithm */
	regval = GMAC_IOREAD(pdata, MTL_OMR);
	regval = GMAC_SET_REG_BITS(regval,
				   MTL_OMR_TSA_POS,
				   MTL_OMR_TSA_LEN,
				   MTL_TSA_WRR);
	GMAC_IOWRITE(pdata, MTL_OMR, regval);

	for (i = 0; i < pdata->hw_feat.tx_ch_cnt; i++) {
		regval = GMAC_IOREAD(pdata, MTL_Q_TQWR(i));
		regval = GMAC_SET_REG_BITS(regval,
					   MTL_Q_TQWR_QW_POS,
					   MTL_Q_TQWR_QW_LEN,
					   (0x10 + i));
		GMAC_IOWRITE(pdata, MTL_Q_TQWR(i), regval);
	}

	/* Set Rx to strict priority algorithm */
	regval = GMAC_IOREAD(pdata, MTL_OMR);
	regval = GMAC_SET_REG_BITS(regval,
				   MTL_OMR_RAA_POS,
				   MTL_OMR_RAA_LEN,
				   MTL_RAA_SP);
	GMAC_IOWRITE(pdata, MTL_OMR, regval);
}

static void gmac_config_queue_mapping(struct gmac_pdata *pdata)
{
	u32 value;

	/* Configure one to one, MTL Rx queue to DMA Rx channel mapping
	 *	ie Q0 <--> CH0, Q1 <--> CH1 ... Q7 <--> CH7
	 */
	value = (MTL_RQDCM0R_Q0MDMACH | MTL_RQDCM0R_Q1MDMACH |
		MTL_RQDCM0R_Q2MDMACH | MTL_RQDCM0R_Q3MDMACH);
	GMAC_IOWRITE(pdata, MTL_RQDCM0R, value);

	value = (MTL_RQDCM1R_Q4MDMACH | MTL_RQDCM1R_Q5MDMACH |
		MTL_RQDCM1R_Q5MDMACH | MTL_RQDCM1R_Q6MDMACH);
	GMAC_IOWRITE(pdata, MTL_RQDCM1R, value);
}

static unsigned int gmac_calculate_per_queue_fifo(unsigned int fifo_size,
						  unsigned int queue_count)
{
	unsigned int q_fifo_size;
	unsigned int p_fifo;

	/* Calculate the configured fifo size */
	q_fifo_size = 1 << (fifo_size + 7);

	/* The configured value may not be the actual amount of fifo RAM */
	q_fifo_size = min_t(unsigned int, GMAC_MAX_FIFO, q_fifo_size);

	q_fifo_size = q_fifo_size / queue_count;

	/* Each increment in the queue fifo size represents 256 bytes of
	 * fifo, with 0 representing 256 bytes. Distribute the fifo equally
	 * between the queues.
	 */
	p_fifo = fls(q_fifo_size / 256) - 1;

	p_fifo = (1 << p_fifo);

	p_fifo--;

	return p_fifo;
}

static void gmac_config_tx_fifo_size(struct gmac_pdata *pdata)
{
	unsigned int fifo_size;
	unsigned int i;
	u32 regval;

	fifo_size = gmac_calculate_per_queue_fifo(pdata->hw_feat.tx_fifo_size,
						  pdata->tx_q_count);

	for (i = 0; i < pdata->tx_q_count; i++) {
		regval = GMAC_IOREAD(pdata, MTL_Q_TQOMR(i));
		regval = GMAC_SET_REG_BITS(regval,
					   MTL_Q_TQOMR_TQS_POS,
					   MTL_Q_TQOMR_TQS_LEN,
					   fifo_size);
		GMAC_IOWRITE(pdata, MTL_Q_TQOMR(i), regval);
	}

	netif_info(pdata, drv, pdata->netdev,
		   "%d Tx hardware queues, %d byte fifo per queue\n",
		   pdata->tx_q_count, ((fifo_size + 1) * 256));
}

static void gmac_config_rx_fifo_size(struct gmac_pdata *pdata)
{
	unsigned int fifo_size;
	unsigned int i;
	u32 regval;

	fifo_size = gmac_calculate_per_queue_fifo(pdata->hw_feat.rx_fifo_size,
						  pdata->rx_q_count);

	for (i = 0; i < pdata->rx_q_count; i++) {
		regval = GMAC_IOREAD(pdata, MTL_Q_RQOMR(i));
		regval = GMAC_SET_REG_BITS(regval,
					   MTL_Q_RQOMR_RQS_POS,
					   MTL_Q_RQOMR_RQS_LEN,
					   fifo_size);
		GMAC_IOWRITE(pdata, MTL_Q_RQOMR(i), regval);
	}

	netif_info(pdata, drv, pdata->netdev,
		   "%d Rx hardware queues, %d byte fifo per queue\n",
		   pdata->rx_q_count, ((fifo_size + 1) * 256));
}

static void gmac_config_flow_control_threshold(struct gmac_pdata *pdata)
{
	unsigned int i;
	u32 regval;

	for (i = 0; i < pdata->rx_q_count; i++) {
		regval = GMAC_IOREAD(pdata, MTL_Q_RQOMR(i));
		/* Activate flow control when less than 1.5k left in fifo */
		regval = GMAC_SET_REG_BITS(regval,
					   MTL_Q_RQOMR_RFA_POS,
					   MTL_Q_RQOMR_RFA_LEN,
					   1);
		/* De-activate flow control when more than 2.5k left in fifo */
		regval = GMAC_SET_REG_BITS(regval,
					   MTL_Q_RQOMR_RFD_POS,
					   MTL_Q_RQOMR_RFD_LEN, 3);
		GMAC_IOWRITE(pdata, MTL_Q_RQOMR(i), regval);
	}
}

static int gmac_config_tx_threshold(struct gmac_pdata *pdata,
				    unsigned int val)
{
	unsigned int i;
	u32 regval;

	for (i = 0; i < pdata->tx_q_count; i++) {
		regval = GMAC_IOREAD(pdata, MTL_Q_TQOMR(i));
		regval = GMAC_SET_REG_BITS(regval,
					   MTL_Q_TQOMR_TTC_POS,
					   MTL_Q_TQOMR_TTC_LEN,
					   val);
		GMAC_IOWRITE(pdata, MTL_Q_TQOMR(i), regval);
	}

	return 0;
}

static int gmac_config_rsf_mode(struct gmac_pdata *pdata,
				unsigned int val)
{
	unsigned int i;
	u32 regval;

	for (i = 0; i < pdata->rx_q_count; i++) {
		regval = GMAC_IOREAD(pdata, MTL_Q_RQOMR(i));
		regval = GMAC_SET_REG_BITS(regval,
					   MTL_Q_RQOMR_RSF_POS,
					   MTL_Q_RQOMR_RSF_LEN, val);
		GMAC_IOWRITE(pdata, MTL_Q_RQOMR(i), regval);
	}

	return 0;
}

static int gmac_config_tsf_mode(struct gmac_pdata *pdata,
				unsigned int val)
{
	unsigned int i;
	u32 regval;

	for (i = 0; i < pdata->tx_q_count; i++) {
		regval = GMAC_IOREAD(pdata, MTL_Q_TQOMR(i));
		regval = GMAC_SET_REG_BITS(regval,
					   MTL_Q_TQOMR_TSF_POS,
					   MTL_Q_TQOMR_TSF_LEN,
					   val);
		GMAC_IOWRITE(pdata, MTL_Q_TQOMR(i), regval);
	}

	return 0;
}

static int gmac_config_osp_mode(struct gmac_pdata *pdata)
{
	struct gmac_channel *channel;
	unsigned int i;
	u32 regval;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			break;

		regval = GMAC_IOREAD(pdata, DMA_CH_TCR(i));
		regval = GMAC_SET_REG_BITS(regval,
					   DMA_CH_TCR_OSP_POS,
					   DMA_CH_TCR_OSP_LEN,
					   pdata->tx_osp_mode);
		GMAC_IOWRITE(pdata, DMA_CH_TCR(i), regval);
	}

	return 0;
}

static int gmac_config_pblx8(struct gmac_pdata *pdata)
{
	unsigned int i;
	u32 regval;

	for (i = 0; i < pdata->channel_count; i++) {
		regval = GMAC_IOREAD(pdata, DMA_CH_CR(i));
		regval = GMAC_SET_REG_BITS(regval,
					   DMA_CH_CR_PBLX8_POS,
					   DMA_CH_CR_PBLX8_LEN,
					   pdata->pblx8);
		GMAC_IOWRITE(pdata, DMA_CH_CR(i), regval);
	}

	return 0;
}

static int gmac_config_tx_pbl_val(struct gmac_pdata *pdata)
{
	struct gmac_channel *channel;
	unsigned int i;
	u32 regval;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			break;

		regval = GMAC_IOREAD(pdata, DMA_CH_TCR(i));
		regval = GMAC_SET_REG_BITS(regval,
					   DMA_CH_TCR_PBL_POS,
					   DMA_CH_TCR_PBL_LEN,
					   pdata->tx_pbl);
		GMAC_IOWRITE(pdata, DMA_CH_TCR(i), regval);
	}

	return 0;
}

static int gmac_config_rx_pbl_val(struct gmac_pdata *pdata)
{
	struct gmac_channel *channel;
	unsigned int i;
	u32 regval;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->rx_ring)
			break;

		regval = GMAC_IOREAD(pdata, DMA_CH_RCR(i));
		regval = GMAC_SET_REG_BITS(regval,
					   DMA_CH_RCR_PBL_POS,
					   DMA_CH_RCR_PBL_LEN,
					   pdata->rx_pbl);
		GMAC_IOWRITE(pdata, DMA_CH_RCR(i), regval);
	}

	return 0;
}

static void gmac_tx_mmc_int(struct gmac_pdata *pdata)
{
	unsigned int mmc_isr = GMAC_IOREAD(pdata, MMC_TISR);
	struct gmac_stats *stats = &pdata->stats;

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXOCTETCOUNT_GB_POS,
			      MMC_TISR_TXOCTETCOUNT_GB_LEN))
		stats->txoctetcount_gb +=
			GMAC_IOREAD(pdata, MMC_TXOCTETCOUNT_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXFRAMECOUNT_GB_POS,
			      MMC_TISR_TXFRAMECOUNT_GB_LEN))
		stats->txframecount_gb +=
			GMAC_IOREAD(pdata, MMC_TXPACKETCOUNT_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXBROADCASTFRAMES_G_POS,
			      MMC_TISR_TXBROADCASTFRAMES_G_LEN))
		stats->txbroadcastframes_g +=
			GMAC_IOREAD(pdata, MMC_TXBROADCASTFRAMES_G);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXMULTICASTFRAMES_G_POS,
			      MMC_TISR_TXMULTICASTFRAMES_G_LEN))
		stats->txmulticastframes_g +=
			GMAC_IOREAD(pdata, MMC_TXMULTICASTFRAMES_G);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TX64OCTETS_GB_POS,
			      MMC_TISR_TX64OCTETS_GB_LEN))
		stats->tx64octets_gb +=
			GMAC_IOREAD(pdata, MMC_TX64OCTETS_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TX65TO127OCTETS_GB_POS,
			      MMC_TISR_TX65TO127OCTETS_GB_LEN))
		stats->tx65to127octets_gb +=
			GMAC_IOREAD(pdata, MMC_TX65TO127OCTETS_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TX128TO255OCTETS_GB_POS,
			      MMC_TISR_TX128TO255OCTETS_GB_LEN))
		stats->tx128to255octets_gb +=
			GMAC_IOREAD(pdata, MMC_TX128TO255OCTETS_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TX256TO511OCTETS_GB_POS,
			      MMC_TISR_TX256TO511OCTETS_GB_LEN))
		stats->tx256to511octets_gb +=
			GMAC_IOREAD(pdata, MMC_TX256TO511OCTETS_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TX512TO1023OCTETS_GB_POS,
			      MMC_TISR_TX512TO1023OCTETS_GB_LEN))
		stats->tx512to1023octets_gb +=
			GMAC_IOREAD(pdata, MMC_TX512TO1023OCTETS_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TX1024TOMAXOCTETS_GB_POS,
			      MMC_TISR_TX1024TOMAXOCTETS_GB_LEN))
		stats->tx1024tomaxoctets_gb +=
			GMAC_IOREAD(pdata, MMC_TX1024TOMAXOCTETS_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXUNICASTFRAMES_GB_POS,
			      MMC_TISR_TXUNICASTFRAMES_GB_LEN))
		stats->txunicastframes_gb +=
			GMAC_IOREAD(pdata, MMC_TXUNICASTFRAMES_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXMULTICASTFRAMES_GB_POS,
			      MMC_TISR_TXMULTICASTFRAMES_GB_LEN))
		stats->txmulticastframes_gb +=
			GMAC_IOREAD(pdata, MMC_TXMULTICASTFRAMES_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXBROADCASTFRAMES_GB_POS,
			      MMC_TISR_TXBROADCASTFRAMES_GB_LEN))
		stats->txbroadcastframes_g +=
			GMAC_IOREAD(pdata, MMC_TXBROADCASTFRAMES_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXUNDERFLOWERROR_POS,
			      MMC_TISR_TXUNDERFLOWERROR_LEN))
		stats->txunderflowerror +=
			GMAC_IOREAD(pdata, MMC_TXUNDERFLOWERROR);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXSINGLECOL_G_POS,
			      MMC_TISR_TXSINGLECOL_G_POS))
		stats->txsinglecol_g +=
			GMAC_IOREAD(pdata, MMC_TXSINGLECOL_G);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXMULTICOL_G_POS,
			      MMC_TISR_TXMULTICOL_G_LEN))
		stats->txmulticol_g +=
			GMAC_IOREAD(pdata, MMC_TXMULTICOL_G);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXDEFERRED_POS,
			      MMC_TISR_TXDEFERRED_LEN))
		stats->txdeferred +=
			GMAC_IOREAD(pdata, MMC_TXDEFERRED);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXLATECOL_POS,
			      MMC_TISR_TXLATECOL_LEN))
		stats->txlatecol +=
			GMAC_IOREAD(pdata, MMC_TXLATECOL);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXEXESSCOL_POS,
			      MMC_TISR_TXEXESSCOL_LEN))
		stats->txexesscol +=
			GMAC_IOREAD(pdata, MMC_TXEXESSCOL);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXCARRIERERROR_POS,
			      MMC_TISR_TXCARRIERERROR_LEN))
		stats->txcarriererror +=
			GMAC_IOREAD(pdata, MMC_TXCARRIERERROR);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXOCTETCOUNT_G_POS,
			      MMC_TISR_TXOCTETCOUNT_G_LEN))
		stats->txoctetcount_g +=
			GMAC_IOREAD(pdata, MMC_TXOCTETCOUNT_G);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXFRAMECOUNT_G_POS,
			      MMC_TISR_TXFRAMECOUNT_G_LEN))
		stats->txframecount_g +=
			GMAC_IOREAD(pdata, MMC_TXPACKETSCOUNT_G);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXEXCESSDEF_POS,
			      MMC_TISR_TXEXCESSDEF_LEN))
		stats->txexcessdef +=
			GMAC_IOREAD(pdata, MMC_TXEXCESSDEF);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXPAUSEFRAMES_POS,
			      MMC_TISR_TXPAUSEFRAMES_LEN))
		stats->txpauseframes +=
			GMAC_IOREAD(pdata, MMC_TXPAUSEFRAMES);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXVLANFRAMES_G_POS,
			      MMC_TISR_TXVLANFRAMES_G_LEN))
		stats->txvlanframes_g +=
			GMAC_IOREAD(pdata, MMC_TXVLANFRAMES_G);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXOVERSIZE_G_POS,
			      MMC_TISR_TXOVERSIZE_G_LEN))
		stats->txosizeframe_g +=
			GMAC_IOREAD(pdata, MMC_TXOVERSIZE_G);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXLPIUSEC_POS,
			      MMC_TISR_TXLPIUSEC_LEN))
		stats->txlpiusec +=
			GMAC_IOREAD(pdata, MMC_TXLPIUSEC);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_TISR_TXLPITRAN_POS,
			      MMC_TISR_TXLPITRAN_LEN))
		stats->txlpitran +=
			GMAC_IOREAD(pdata, MMC_TXLPITRAN);
}

static void gmac_rx_mmc_int(struct gmac_pdata *pdata)
{
	unsigned int mmc_isr = GMAC_IOREAD(pdata, MMC_RISR);
	struct gmac_stats *stats = &pdata->stats;

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXFRAMECOUNT_GB_POS,
			      MMC_RISR_RXFRAMECOUNT_GB_LEN))
		stats->rxframecount_gb +=
			GMAC_IOREAD(pdata, MMC_RXPACKETCOUNT_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXOCTETCOUNT_GB_POS,
			      MMC_RISR_RXOCTETCOUNT_GB_LEN))
		stats->rxoctetcount_gb +=
			GMAC_IOREAD(pdata, MMC_RXOCTETCOUNT_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXOCTETCOUNT_G_POS,
			      MMC_RISR_RXOCTETCOUNT_G_LEN))
		stats->rxoctetcount_g +=
			GMAC_IOREAD(pdata, MMC_RXOCTETCOUNT_G);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXBROADCASTFRAMES_G_POS,
			      MMC_RISR_RXBROADCASTFRAMES_G_LEN))
		stats->rxbroadcastframes_g +=
			GMAC_IOREAD(pdata, MMC_RXBROADCASTFRAMES_G);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXMULTICASTFRAMES_G_POS,
			      MMC_RISR_RXMULTICASTFRAMES_G_LEN))
		stats->rxmulticastframes_g +=
			GMAC_IOREAD(pdata, MMC_RXMULTICASTFRAMES_G);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXCRCERROR_POS,
			      MMC_RISR_RXCRCERROR_LEN))
		stats->rxcrcerror +=
			GMAC_IOREAD(pdata, MMC_RXCRCERROR);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXALIGNMENTERROR_POS,
			      MMC_RISR_RXALIGNMENTERROR_LEN))
		stats->rxalignerror +=
			GMAC_IOREAD(pdata, MMC_RXALIGNMENTERROR);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXRUNTERROR_POS,
			      MMC_RISR_RXRUNTERROR_LEN))
		stats->rxrunterror +=
			GMAC_IOREAD(pdata, MMC_RXRUNTERROR);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXJABBERERROR_POS,
			      MMC_RISR_RXJABBERERROR_LEN))
		stats->rxjabbererror +=
			GMAC_IOREAD(pdata, MMC_RXJABBERERROR);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXUNDERSIZE_G_POS,
			      MMC_RISR_RXUNDERSIZE_G_LEN))
		stats->rxundersize_g +=
			GMAC_IOREAD(pdata, MMC_RXUNDERSIZE_G);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXOVERSIZE_G_POS,
			      MMC_RISR_RXOVERSIZE_G_LEN))
		stats->rxoversize_g +=
			GMAC_IOREAD(pdata, MMC_RXOVERSIZE_G);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RX64OCTETS_GB_POS,
			      MMC_RISR_RX64OCTETS_GB_LEN))
		stats->rx64octets_gb +=
			GMAC_IOREAD(pdata, MMC_RX64OCTETS_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RX65TO127OCTETS_GB_POS,
			      MMC_RISR_RX65TO127OCTETS_GB_LEN))
		stats->rx65to127octets_gb +=
			GMAC_IOREAD(pdata, MMC_RX65TO127OCTETS_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RX128TO255OCTETS_GB_POS,
			      MMC_RISR_RX128TO255OCTETS_GB_LEN))
		stats->rx128to255octets_gb +=
			GMAC_IOREAD(pdata, MMC_RX128TO255OCTETS_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RX256TO511OCTETS_GB_POS,
			      MMC_RISR_RX256TO511OCTETS_GB_LEN))
		stats->rx256to511octets_gb +=
			GMAC_IOREAD(pdata, MMC_RX256TO511OCTETS_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RX512TO1023OCTETS_GB_POS,
			      MMC_RISR_RX512TO1023OCTETS_GB_LEN))
		stats->rx512to1023octets_gb +=
			GMAC_IOREAD(pdata, MMC_RX512TO1023OCTETS_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RX1024TOMAXOCTETS_GB_POS,
			      MMC_RISR_RX1024TOMAXOCTETS_GB_LEN))
		stats->rx1024tomaxoctets_gb +=
			GMAC_IOREAD(pdata, MMC_RX1024TOMAXOCTETS_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXUNICASTFRAMES_G_POS,
			      MMC_RISR_RXUNICASTFRAMES_G_LEN))
		stats->rxunicastframes_g +=
			GMAC_IOREAD(pdata, MMC_RXUNICASTFRAMES_G);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXLENGTHERROR_POS,
			      MMC_RISR_RXLENGTHERROR_LEN))
		stats->rxlengtherror +=
			GMAC_IOREAD(pdata, MMC_RXLENGTHERROR);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXOUTOFRANGETYPE_POS,
			      MMC_RISR_RXOUTOFRANGETYPE_LEN))
		stats->rxoutofrangetype +=
			GMAC_IOREAD(pdata, MMC_RXOUTOFRANGETYPE);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXPAUSEFRAMES_POS,
			      MMC_RISR_RXPAUSEFRAMES_LEN))
		stats->rxpauseframes +=
			GMAC_IOREAD(pdata, MMC_RXPAUSEFRAMES);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXFIFOOVERFLOW_POS,
			      MMC_RISR_RXFIFOOVERFLOW_LEN))
		stats->rxfifooverflow +=
			GMAC_IOREAD(pdata, MMC_RXFIFOOVERFLOW);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXVLANFRAMES_GB_POS,
			      MMC_RISR_RXVLANFRAMES_GB_LEN))
		stats->rxvlanframes_gb +=
			GMAC_IOREAD(pdata, MMC_RXVLANFRAMES_GB);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXWATCHDOGERROR_POS,
			      MMC_RISR_RXWATCHDOGERROR_LEN))
		stats->rxwatchdogerror +=
			GMAC_IOREAD(pdata, MMC_RXWATCHDOGERROR);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXRCVERROR_POS,
			      MMC_RISR_RXRCVERROR_LEN))
		stats->rxreceiveerror +=
			GMAC_IOREAD(pdata, MMC_RXRCVERROR);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXCTRLFRAMES_POS,
			      MMC_RISR_RXCTRLFRAMES_LEN))
		stats->rxctrlframes_g +=
			GMAC_IOREAD(pdata, MMC_RXCTRLFRAMES_G);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXLPIUSEC_POS,
			      MMC_RISR_RXLPIUSEC_LEN))
		stats->rxlpiusec +=
			GMAC_IOREAD(pdata, MMC_RXLPIUSEC);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_RISR_RXLPITRAN_POS,
			      MMC_RISR_RXLPITRAN_LEN))
		stats->rxlpitran += GMAC_IOREAD(pdata, MMC_RXLPITRAN);
}

static void gmac_rxipc_mmc_int(struct gmac_pdata *pdata)
{
	unsigned int mmc_isr = GMAC_IOREAD(pdata, MMC_IPCSR);
	struct gmac_stats *stats = &pdata->stats;

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXIPV4GDPKTS_POS,
			      MMC_IPCSR_RXIPV4GDPKTS_LEN))
		stats->rxipv4_g +=
			GMAC_IOREAD(pdata, MMC_RXIPV4GDPKTS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXIPV4HDRERRPKTS_POS,
			      MMC_IPCSR_RXIPV4HDRERRPKTS_LEN))
		stats->rxipv4hderr +=
			GMAC_IOREAD(pdata, MMC_RXIPV4HDRERRPKTS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXIPV4NOPAYPKTS_POS,
			      MMC_IPCSR_RXIPV4NOPAYPKTS_LEN))
		stats->rxipv4nopay +=
			GMAC_IOREAD(pdata, MMC_RXIPV4NOPAYPKTS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXIPV4FRAGPKTS_POS,
			      MMC_IPCSR_RXIPV4FRAGPKTS_LEN))
		stats->rxipv4frag +=
			GMAC_IOREAD(pdata, MMC_RXIPV4FRAGPKTS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXIPV4UBSBLPKTS_POS,
			      MMC_IPCSR_RXIPV4UBSBLPKTS_LEN))
		stats->rxipv4udsbl +=
			GMAC_IOREAD(pdata, MMC_RXIPV4UBSBLPKTS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXIPV6GDPKTS_POS,
			      MMC_IPCSR_RXIPV6GDPKTS_LEN))
		stats->rxipv6_g +=
			GMAC_IOREAD(pdata, MMC_RXIPV6GDPKTS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXIPV6HDRERRPKTS_POS,
			      MMC_IPCSR_RXIPV6HDRERRPKTS_LEN))
		stats->rxipv6hderr +=
			GMAC_IOREAD(pdata, MMC_RXIPV6HDRERRPKTS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXIPV6NOPAYPKTS_POS,
			      MMC_IPCSR_RXIPV6NOPAYPKTS_LEN))
		stats->rxipv6nopay +=
			GMAC_IOREAD(pdata, MMC_RXIPV6NOPAYPKTS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXUDPGDPKTS_POS,
			      MMC_IPCSR_RXUDPGDPKTS_LEN))
		stats->rxudp_g +=
			GMAC_IOREAD(pdata, MMC_RXUDPGDPKTS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXUDPERRPKTS_POS,
			      MMC_IPCSR_RXUDPERRPKTS_LEN))
		stats->rxudperr +=
			GMAC_IOREAD(pdata, MMC_RXUDPERRPKTS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXTCPGDPKTS_POS,
			      MMC_IPCSR_RXTCPGDPKTS_LEN))
		stats->rxtcp_g +=
			GMAC_IOREAD(pdata, MMC_RXTCPGDPKTS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXTCPERRPKTS_POS,
			      MMC_IPCSR_RXTCPERRPKTS_LEN))
		stats->rxtcperr +=
			GMAC_IOREAD(pdata, MMC_RXTCPERRPKTS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXICMPGDPKTS_POS,
			      MMC_IPCSR_RXICMPGDPKTS_LEN))
		stats->rxicmp_g +=
			GMAC_IOREAD(pdata, MMC_RXICMPGDPKTS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXICMPERRPKTS_POS,
			      MMC_IPCSR_RXICMPERRPKTS_LEN))
		stats->rxicmperr +=
			GMAC_IOREAD(pdata, MMC_RXICMPERRPKTS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXIPV4GDOCTETS_POS,
			      MMC_IPCSR_RXIPV4GDOCTETS_LEN))
		stats->rxipv4octets_g +=
			GMAC_IOREAD(pdata, MMC_RXIPV4GDOCTETS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXIPV4GDOCTETS_POS,
			      MMC_IPCSR_RXIPV4GDOCTETS_LEN))
		stats->rxipv4hderroctets +=
			GMAC_IOREAD(pdata, MMC_RXIPV4HDRERROCTETS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXIPV4NOPAYOCTETS_POS,
			      MMC_IPCSR_RXIPV4NOPAYOCTETS_LEN))
		stats->rxipv4nopayoctets +=
			GMAC_IOREAD(pdata, MMC_RXIPV4NOPAYOCTETS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXIPV4FRAGOCTETS_POS,
			      MMC_IPCSR_RXIPV4FRAGOCTETS_LEN))
		stats->rxipv4fragoctets +=
			GMAC_IOREAD(pdata, MMC_RXIPV4FRAGOCTETS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXIPV4UDSBLOCTETS_POS,
			      MMC_IPCSR_RXIPV4UDSBLOCTETS_LEN))
		stats->rxipv4udsbloctets +=
			GMAC_IOREAD(pdata, MMC_RXIPV4UDSBLOCTETS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXIPV6GDOCTETS_POS,
			      MMC_IPCSR_RXIPV6GDOCTETS_LEN))
		stats->rxipv6octets_g +=
			GMAC_IOREAD(pdata, MMC_RXIPV6GDOCTETS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXIPV6HDRERROCTETS_POS,
			      MMC_IPCSR_RXIPV6HDRERROCTETS_LEN))
		stats->rxipv6hderroctets +=
			GMAC_IOREAD(pdata, MMC_RXIPV6HDRERROCTETS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXIPV6NOPAYOCTETS_POS,
			      MMC_IPCSR_RXIPV6NOPAYOCTETS_LEN))
		stats->rxipv6nopayoctets +=
			GMAC_IOREAD(pdata, MMC_RXIPV6NOPAYOCTETS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXUDPGDOCTETS_POS,
			      MMC_IPCSR_RXUDPGDOCTETS_LEN))
		stats->rxudpoctets_g +=
			GMAC_IOREAD(pdata, MMC_RXUDPGDOCTETS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXUDPERROCTETS_POS,
			      MMC_IPCSR_RXUDPERROCTETS_LEN))
		stats->rxudperroctets +=
			GMAC_IOREAD(pdata, MMC_RXUDPERROCTETS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXTCPGDOCTETS_POS,
			      MMC_IPCSR_RXTCPGDOCTETS_LEN))
		stats->rxtcpoctets_g +=
			GMAC_IOREAD(pdata, MMC_RXTCPGDOCTETS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXTCPERROCTETS_POS,
			      MMC_IPCSR_RXTCPERROCTETS_LEN))
		stats->rxtcperroctets +=
			GMAC_IOREAD(pdata, MMC_RXTCPERROCTETS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXICMPGDOCTETS_POS,
			      MMC_IPCSR_RXICMPGDOCTETS_LEN))
		stats->rxicmpoctets_g +=
			GMAC_IOREAD(pdata, MMC_RXICMPGDOCTETS);

	if (GMAC_GET_REG_BITS(mmc_isr,
			      MMC_IPCSR_RXICMPERROCTETS_POS,
			      MMC_IPCSR_RXICMPERROCTETS_LEN))
		stats->rxicmperroctets +=
			GMAC_IOREAD(pdata, MMC_RXICMPERROCTETS);
}

static void gmac_read_mmc_stats(struct gmac_pdata *pdata)
{
	struct gmac_stats *stats = &pdata->stats;
	u32 regval;

	/* Freeze counters */
	regval = GMAC_IOREAD(pdata, MMC_CR);
	regval = GMAC_SET_REG_BITS(regval,
				   MMC_CR_MCF_POS,
				   MMC_CR_MCF_LEN,
				   1);
	GMAC_IOWRITE(pdata, MMC_CR, regval);

	/* MMC TX counter registers */
	stats->txoctetcount_gb +=
		GMAC_IOREAD(pdata, MMC_TXOCTETCOUNT_GB);
	stats->txframecount_gb +=
		GMAC_IOREAD(pdata, MMC_TXPACKETCOUNT_GB);
	stats->txbroadcastframes_g +=
		GMAC_IOREAD(pdata, MMC_TXBROADCASTFRAMES_G);
	stats->txmulticastframes_g +=
		GMAC_IOREAD(pdata, MMC_TXMULTICASTFRAMES_G);
	stats->tx64octets_gb +=
		GMAC_IOREAD(pdata, MMC_TX64OCTETS_GB);
	stats->tx65to127octets_gb +=
		GMAC_IOREAD(pdata, MMC_TX65TO127OCTETS_GB);
	stats->tx128to255octets_gb +=
		GMAC_IOREAD(pdata, MMC_TX128TO255OCTETS_GB);
	stats->tx256to511octets_gb +=
		GMAC_IOREAD(pdata, MMC_TX256TO511OCTETS_GB);
	stats->tx512to1023octets_gb +=
		GMAC_IOREAD(pdata, MMC_TX512TO1023OCTETS_GB);
	stats->tx1024tomaxoctets_gb +=
		GMAC_IOREAD(pdata, MMC_TX1024TOMAXOCTETS_GB);
	stats->txunicastframes_gb +=
		GMAC_IOREAD(pdata, MMC_TXUNICASTFRAMES_GB);
	stats->txmulticastframes_gb +=
		GMAC_IOREAD(pdata, MMC_TXMULTICASTFRAMES_GB);
	stats->txbroadcastframes_gb +=
		GMAC_IOREAD(pdata, MMC_TXBROADCASTFRAMES_GB);
	stats->txunderflowerror +=
		GMAC_IOREAD(pdata, MMC_TXUNDERFLOWERROR);
	stats->txsinglecol_g +=
		GMAC_IOREAD(pdata, MMC_TXSINGLECOL_G);
	stats->txmulticol_g +=
		GMAC_IOREAD(pdata, MMC_TXMULTICOL_G);
	stats->txdeferred +=
		GMAC_IOREAD(pdata, MMC_TXDEFERRED);
	stats->txlatecol +=
		GMAC_IOREAD(pdata, MMC_TXLATECOL);
	stats->txexesscol +=
		GMAC_IOREAD(pdata, MMC_TXEXESSCOL);
	stats->txcarriererror +=
		GMAC_IOREAD(pdata, MMC_TXCARRIERERROR);
	stats->txoctetcount_g +=
		GMAC_IOREAD(pdata, MMC_TXOCTETCOUNT_G);
	stats->txframecount_g +=
		GMAC_IOREAD(pdata, MMC_TXPACKETSCOUNT_G);
	stats->txexcessdef +=
		GMAC_IOREAD(pdata, MMC_TXEXCESSDEF);
	stats->txpauseframes +=
		GMAC_IOREAD(pdata, MMC_TXPAUSEFRAMES);
	stats->txvlanframes_g +=
		GMAC_IOREAD(pdata, MMC_TXVLANFRAMES_G);
	stats->txosizeframe_g +=
		GMAC_IOREAD(pdata, MMC_TXOVERSIZE_G);
	stats->txlpiusec +=
		GMAC_IOREAD(pdata, MMC_TXLPIUSEC);
	stats->txlpitran +=
		GMAC_IOREAD(pdata, MMC_TXLPITRAN);

	/* MMC RX counter registers */
	stats->rxframecount_gb +=
		GMAC_IOREAD(pdata, MMC_RXPACKETCOUNT_GB);
	stats->rxoctetcount_gb +=
		GMAC_IOREAD(pdata, MMC_RXOCTETCOUNT_GB);
	stats->rxoctetcount_g +=
		GMAC_IOREAD(pdata, MMC_RXOCTETCOUNT_G);
	stats->rxbroadcastframes_g +=
		GMAC_IOREAD(pdata, MMC_RXBROADCASTFRAMES_G);
	stats->rxmulticastframes_g +=
		GMAC_IOREAD(pdata, MMC_RXMULTICASTFRAMES_G);
	stats->rxcrcerror +=
		GMAC_IOREAD(pdata, MMC_RXCRCERROR);
	stats->rxalignerror +=
		GMAC_IOREAD(pdata, MMC_RXALIGNMENTERROR);
	stats->rxrunterror +=
		GMAC_IOREAD(pdata, MMC_RXRUNTERROR);
	stats->rxjabbererror +=
		GMAC_IOREAD(pdata, MMC_RXJABBERERROR);
	stats->rxundersize_g +=
		GMAC_IOREAD(pdata, MMC_RXUNDERSIZE_G);
	stats->rxoversize_g +=
		GMAC_IOREAD(pdata, MMC_RXOVERSIZE_G);
	stats->rx64octets_gb +=
		GMAC_IOREAD(pdata, MMC_RX64OCTETS_GB);
	stats->rx65to127octets_gb +=
		GMAC_IOREAD(pdata, MMC_RX65TO127OCTETS_GB);
	stats->rx128to255octets_gb +=
		GMAC_IOREAD(pdata, MMC_RX128TO255OCTETS_GB);
	stats->rx256to511octets_gb +=
		GMAC_IOREAD(pdata, MMC_RX256TO511OCTETS_GB);
	stats->rx512to1023octets_gb +=
		GMAC_IOREAD(pdata, MMC_RX512TO1023OCTETS_GB);
	stats->rx1024tomaxoctets_gb +=
		GMAC_IOREAD(pdata, MMC_RX1024TOMAXOCTETS_GB);
	stats->rxunicastframes_g +=
		GMAC_IOREAD(pdata, MMC_RXUNICASTFRAMES_G);
	stats->rxlengtherror +=
		GMAC_IOREAD(pdata, MMC_RXLENGTHERROR);
	stats->rxoutofrangetype +=
		GMAC_IOREAD(pdata, MMC_RXOUTOFRANGETYPE);
	stats->rxpauseframes +=
		GMAC_IOREAD(pdata, MMC_RXPAUSEFRAMES);
	stats->rxfifooverflow +=
		GMAC_IOREAD(pdata, MMC_RXFIFOOVERFLOW);
	stats->rxvlanframes_gb +=
		GMAC_IOREAD(pdata, MMC_RXVLANFRAMES_GB);
	stats->rxwatchdogerror +=
		GMAC_IOREAD(pdata, MMC_RXWATCHDOGERROR);
	stats->rxreceiveerror +=
		GMAC_IOREAD(pdata, MMC_RXRCVERROR);
	stats->rxctrlframes_g +=
		GMAC_IOREAD(pdata, MMC_RXCTRLFRAMES_G);
	stats->rxlpiusec +=
		GMAC_IOREAD(pdata, MMC_RXLPIUSEC);
	stats->rxlpitran +=
		GMAC_IOREAD(pdata, MMC_RXLPITRAN);

	/* MMC RX IPC counter registers */
	stats->rxipv4_g +=
		GMAC_IOREAD(pdata, MMC_RXIPV4GDPKTS);
	stats->rxipv4hderr +=
		GMAC_IOREAD(pdata, MMC_RXIPV4HDRERRPKTS);
	stats->rxipv4nopay +=
		GMAC_IOREAD(pdata, MMC_RXIPV4NOPAYPKTS);
	stats->rxipv4frag +=
		GMAC_IOREAD(pdata, MMC_RXIPV4FRAGPKTS);
	stats->rxipv4udsbl +=
		GMAC_IOREAD(pdata, MMC_RXIPV4UBSBLPKTS);
	stats->rxipv6_g +=
		GMAC_IOREAD(pdata, MMC_RXIPV6GDPKTS);
	stats->rxipv6hderr +=
		GMAC_IOREAD(pdata, MMC_RXIPV6HDRERRPKTS);
	stats->rxipv6nopay +=
		GMAC_IOREAD(pdata, MMC_RXIPV6NOPAYPKTS);
	stats->rxudp_g +=
		GMAC_IOREAD(pdata, MMC_RXUDPGDPKTS);
	stats->rxudperr +=
		GMAC_IOREAD(pdata, MMC_RXUDPERRPKTS);
	stats->rxtcp_g +=
		GMAC_IOREAD(pdata, MMC_RXTCPGDPKTS);
	stats->rxtcperr +=
		GMAC_IOREAD(pdata, MMC_RXTCPERRPKTS);
	stats->rxicmp_g +=
		GMAC_IOREAD(pdata, MMC_RXICMPGDPKTS);
	stats->rxicmperr +=
		GMAC_IOREAD(pdata, MMC_RXICMPERRPKTS);
	stats->rxipv4octets_g +=
		GMAC_IOREAD(pdata, MMC_RXIPV4GDOCTETS);
	stats->rxipv4hderroctets +=
		GMAC_IOREAD(pdata, MMC_RXIPV4HDRERROCTETS);
	stats->rxipv4nopayoctets +=
		GMAC_IOREAD(pdata, MMC_RXIPV4NOPAYOCTETS);
	stats->rxipv4fragoctets +=
		GMAC_IOREAD(pdata, MMC_RXIPV4FRAGOCTETS);
	stats->rxipv4udsbloctets +=
		GMAC_IOREAD(pdata, MMC_RXIPV4UDSBLOCTETS);
	stats->rxipv6octets_g +=
		GMAC_IOREAD(pdata, MMC_RXIPV6GDOCTETS);
	stats->rxipv6hderroctets +=
		GMAC_IOREAD(pdata, MMC_RXIPV6HDRERROCTETS);
	stats->rxipv6nopayoctets +=
		GMAC_IOREAD(pdata, MMC_RXIPV6NOPAYOCTETS);
	stats->rxudpoctets_g +=
		GMAC_IOREAD(pdata, MMC_RXUDPGDOCTETS);
	stats->rxudperroctets +=
		GMAC_IOREAD(pdata, MMC_RXUDPERROCTETS);
	stats->rxtcpoctets_g +=
		GMAC_IOREAD(pdata, MMC_RXTCPGDOCTETS);
	stats->rxtcperroctets +=
		GMAC_IOREAD(pdata, MMC_RXTCPERROCTETS);
	stats->rxicmpoctets_g +=
		GMAC_IOREAD(pdata, MMC_RXICMPGDOCTETS);
	stats->rxicmperroctets +=
		GMAC_IOREAD(pdata, MMC_RXICMPERROCTETS);

	/* Un-freeze counters */
	regval = GMAC_IOREAD(pdata, MMC_CR);
	regval = GMAC_SET_REG_BITS(regval, MMC_CR_MCF_POS,
				   MMC_CR_MCF_LEN, 0);
	GMAC_IOWRITE(pdata, MMC_CR, regval);
}

static void gmac_config_mmc(struct gmac_pdata *pdata)
{
	unsigned int regval;

	regval = GMAC_IOREAD(pdata, MMC_CR);
	/* Set counters to reset on read */
	regval = GMAC_SET_REG_BITS(regval, MMC_CR_ROR_POS,
				   MMC_CR_ROR_LEN, 1);
	/* Reset the counters */
	regval = GMAC_SET_REG_BITS(regval, MMC_CR_CR_POS,
				   MMC_CR_CR_LEN, 1);
	GMAC_IOWRITE(pdata, MMC_CR, regval);
}

static void gmac_enable_dma_interrupts(struct gmac_pdata *pdata)
{
	unsigned int dma_ch_isr, dma_ch_ier;
	struct gmac_channel *channel;
	unsigned int i;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		/* Clear all the interrupts which are set */
		dma_ch_isr =  GMAC_IOREAD(pdata, DMA_CH_SR(i));
		GMAC_IOWRITE(pdata, DMA_CH_SR(i), dma_ch_isr);

		/* Clear all interrupt enable bits */
		dma_ch_ier = 0;

		/* Enable following interrupts
		 *   NIE  - Normal Interrupt Summary Enable
		 *   AIE  - Abnormal Interrupt Summary Enable
		 *   FBEE - Fatal Bus Error Enable
		 */
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier,
					       DMA_CH_IER_NIE_POS,
					       DMA_CH_IER_NIE_LEN, 1);
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier,
					       DMA_CH_IER_AIE_POS,
					       DMA_CH_IER_AIE_LEN, 1);
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier,
					       DMA_CH_IER_FBEE_POS,
					       DMA_CH_IER_FBEE_LEN, 1);

		if (channel->tx_ring) {
			/* Enable the following Tx interrupts
			 *   TIE  - Transmit Interrupt Enable (unless using
			 *          per channel interrupts)
			 */
			if (!pdata->per_channel_irq)
				dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier,
							       DMA_CH_IER_TIE_POS,
							       DMA_CH_IER_TIE_LEN,
							       1);
		}
		if (channel->rx_ring) {
			/* Enable following Rx interrupts
			 *   RBUE - Receive Buffer Unavailable Enable
			 *   RIE  - Receive Interrupt Enable (unless using
			 *          per channel interrupts)
			 */
			dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier,
						       DMA_CH_IER_RBUE_POS,
						       DMA_CH_IER_RBUE_LEN,
						       1);
			if (!pdata->per_channel_irq)
				dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier,
							       DMA_CH_IER_RIE_POS,
							       DMA_CH_IER_RIE_LEN,
							       1);
		}

		GMAC_IOWRITE(pdata, DMA_CH_IER(i), dma_ch_ier);
	}
}

static void gmac_enable_mtl_interrupts(struct gmac_pdata *pdata)
{
	unsigned int q_count, i;
	unsigned int regval;

	q_count = max(pdata->hw_feat.tx_q_cnt, pdata->hw_feat.rx_q_cnt);
	for (i = 0; i < q_count; i++) {
		/* No MTL interrupts to be enabled */
		regval = 0;

		/* Clear all the interrupts which are set */
		regval = GMAC_SET_REG_BITS(regval,
					   MTL_ICR_RXOVFIS_POS,
					   MTL_ICR_RXOVFIS_LEN,
					   1);
		regval = GMAC_SET_REG_BITS(regval,
					   MTL_ICR_ABPSIS_POS,
					   MTL_ICR_ABPSIS_LEN,
					   1);
		regval = GMAC_SET_REG_BITS(regval,
					   MTL_ICR_TXUNFIS_POS,
					   MTL_ICR_TXUNFIS_LEN,
					   1);
		GMAC_IOWRITE(pdata, MTL_Q_ICSR(i), regval);
	}
}

static void gmac_enable_mac_interrupts(struct gmac_pdata *pdata)
{
	unsigned int mac_ier = 0;

	/* Enable RGMII interrupt */
	mac_ier = GMAC_SET_REG_BITS(mac_ier, MAC_IER_RGMII_POS,
				    MAC_IER_RGMII_LEN, 1);
	GMAC_IOWRITE(pdata, MAC_IER, mac_ier);

	/* Enable all TX interrupts */
	GMAC_IOWRITE(pdata, MMC_TIER, 0);
	/* Enable all RX interrupts */
	GMAC_IOWRITE(pdata, MMC_RIER, 0);
	/* Enable MMC Rx Interrupts for IPC */
	GMAC_IOWRITE(pdata, MMC_IPCER, 0);
}

static int gmac_set_gmii_10_speed(struct gmac_pdata *pdata)
{
	u32 regval;

	regval = GMAC_GET_REG_BITS(GMAC_IOREAD(pdata, MAC_MCR),
				   MAC_MCR_SS_POS, MAC_MCR_SS_LEN);
	if (regval == 0x2)
		return 0;

	regval = GMAC_SET_REG_BITS(regval, MAC_MCR_SS_POS,
				   MAC_MCR_SS_LEN, 0x2);
	GMAC_IOWRITE(pdata, MAC_MCR, regval);

	return 0;
}

static int gmac_set_gmii_100_speed(struct gmac_pdata *pdata)
{
	u32 regval;

	regval = GMAC_GET_REG_BITS(GMAC_IOREAD(pdata, MAC_MCR),
				   MAC_MCR_SS_POS, MAC_MCR_SS_LEN);
	if (regval == 0x3)
		return 0;

	regval = GMAC_SET_REG_BITS(regval, MAC_MCR_SS_POS,
				   MAC_MCR_SS_LEN, 0x3);
	GMAC_IOWRITE(pdata, MAC_MCR, regval);

	return 0;
}

static int gmac_set_gmii_1000_speed(struct gmac_pdata *pdata)
{
	u32 regval;

	regval = GMAC_GET_REG_BITS(GMAC_IOREAD(pdata, MAC_MCR),
				   MAC_MCR_SS_POS, MAC_MCR_SS_LEN);
	if (regval == 0x0)
		return 0;

	regval = GMAC_SET_REG_BITS(regval, MAC_MCR_SS_POS,
				   MAC_MCR_SS_LEN, 0x0);
	GMAC_IOWRITE(pdata, MAC_MCR, regval);

	return 0;
}

static void gmac_config_mac_speed(struct gmac_pdata *pdata)
{
	switch (pdata->phy_speed) {
	case SPEED_10:
		gmac_set_gmii_10_speed(pdata);
		break;

	case SPEED_100:
		gmac_set_gmii_100_speed(pdata);
		break;

	case SPEED_1000:
		gmac_set_gmii_1000_speed(pdata);
		break;
	}
}

static int gmac_set_full_duplex(struct gmac_pdata *pdata)
{
	u32 regval;

	regval = GMAC_GET_REG_BITS(GMAC_IOREAD(pdata, MAC_MCR),
				   MAC_MCR_DM_POS, MAC_MCR_DM_LEN);
	if (regval == 0x1)
		return 0;

	regval = GMAC_SET_REG_BITS(regval, MAC_MCR_DM_POS,
				   MAC_MCR_DM_LEN, 0x1);
	GMAC_IOWRITE(pdata, MAC_MCR, regval);

	return 0;
}

static int gmac_set_half_duplex(struct gmac_pdata *pdata)
{
	u32 regval;

	regval = GMAC_GET_REG_BITS(GMAC_IOREAD(pdata, MAC_MCR),
				   MAC_MCR_DM_POS, MAC_MCR_DM_LEN);
	if (regval == 0x0)
		return 0;

	regval = GMAC_SET_REG_BITS(regval, MAC_MCR_DM_POS,
				   MAC_MCR_DM_LEN, 0x0);
	GMAC_IOWRITE(pdata, MAC_MCR, regval);

	return 0;
}

static int gmac_dev_read(struct gmac_channel *channel)
{
	struct gmac_pdata *pdata = channel->pdata;
	struct gmac_ring *ring = channel->rx_ring;
	struct net_device *netdev = pdata->netdev;
	struct gmac_desc_data *desc_data, *next_data;
	struct gmac_dma_desc *dma_desc, *next_desc;
	struct gmac_pkt_info *pkt_info;
	int ret;

	desc_data = GMAC_GET_DESC_DATA(ring, ring->cur);
	dma_desc = desc_data->dma_desc;
	pkt_info = &ring->pkt_info;

	/* Check for data availability */
	if (GMAC_GET_REG_BITS_LE(dma_desc->desc3,
				 RX_NORMAL_DESC3_OWN_POS,
				 RX_NORMAL_DESC3_OWN_LEN))
		return 1;

	/* Make sure descriptor fields are read after reading the OWN bit */
	dma_rmb();

	if (netif_msg_rx_status(pdata))
		gmac_dump_rx_desc(pdata, ring, ring->cur);

	/* Normal Descriptor, be sure Context Descriptor bit is off */
	pkt_info->attributes = GMAC_SET_REG_BITS(pkt_info->attributes,
						 RX_PACKET_ATTRIBUTES_CONTEXT_POS,
						 RX_PACKET_ATTRIBUTES_CONTEXT_LEN,
						 0);

	/* Get the pkt_info length */
	desc_data->trx.bytes = GMAC_GET_REG_BITS_LE(dma_desc->desc3,
						    RX_NORMAL_DESC3_PL_POS,
						    RX_NORMAL_DESC3_PL_LEN);

	if (!GMAC_GET_REG_BITS_LE(dma_desc->desc3,
				  RX_NORMAL_DESC3_LD_POS,
				  RX_NORMAL_DESC3_LD_LEN)) {
		/* Not all the data has been transferred for this pkt_info */
		pkt_info->attributes = GMAC_SET_REG_BITS(pkt_info->attributes,
							 RX_PACKET_ATTRIBUTES_INCOMPLETE_POS,
							 RX_PACKET_ATTRIBUTES_INCOMPLETE_LEN,
							 1);
		return 0;
	}

	/* This is the last of the data for this pkt_info */
	pkt_info->attributes = GMAC_SET_REG_BITS(pkt_info->attributes,
						 RX_PACKET_ATTRIBUTES_INCOMPLETE_POS,
						 RX_PACKET_ATTRIBUTES_INCOMPLETE_LEN,
						 0);

	/* Set checksum done indicator as appropriate */
	if (netdev->features & NETIF_F_RXCSUM)
		pkt_info->attributes = GMAC_SET_REG_BITS(pkt_info->attributes,
							 RX_PACKET_ATTRIBUTES_CSUM_DONE_POS,
							 RX_PACKET_ATTRIBUTES_CSUM_DONE_LEN,
							 1);

	if (GMAC_GET_REG_BITS_LE(dma_desc->desc3,
				 RX_NORMAL_DESC3_RS1V_POS,
				 RX_NORMAL_DESC3_RS1V_LEN)) {
		if (GMAC_GET_REG_BITS_LE(dma_desc->desc1,
					 RX_NORMAL_DESC1_TSA_POS,
					 RX_NORMAL_DESC1_TSA_LEN)){
			ring->cur++;

			next_data = GMAC_GET_DESC_DATA(ring, ring->cur);
			next_desc = next_data->dma_desc;

			ret = gmac_get_rx_tstamp_status(pdata,
							next_desc,
							pkt_info);
			if (ret == -EBUSY) {
				ring->cur--;
				return ret;
			}
		}

		if (gmac_is_rx_csum_error(dma_desc))
			pkt_info->attributes = GMAC_SET_REG_BITS(pkt_info->attributes,
								 RX_PACKET_ATTRIBUTES_CSUM_DONE_POS,
								 RX_PACKET_ATTRIBUTES_CSUM_DONE_LEN,
								 0);
	}

	if (netdev->features & NETIF_F_HW_VLAN_CTAG_RX) {
		if (gmac_is_rx_csum_valid(dma_desc)) {
			pkt_info->attributes = GMAC_SET_REG_BITS(pkt_info->attributes,
								 RX_PACKET_ATTRIBUTES_VLAN_CTAG_POS,
								 RX_PACKET_ATTRIBUTES_VLAN_CTAG_LEN,
								 1);
			pkt_info->vlan_ctag = GMAC_GET_REG_BITS_LE(dma_desc->desc0,
								   RX_NORMAL_DESC0_OVT_POS,
								   RX_NORMAL_DESC0_OVT_LEN);
			netif_dbg(pdata, rx_status, netdev, "vlan-ctag=%#06x\n",
				  pkt_info->vlan_ctag);
		}
	}

	if (GMAC_GET_REG_BITS_LE(dma_desc->desc3,
				 RX_NORMAL_DESC3_ES_POS,
				 RX_NORMAL_DESC3_ES_LEN))
		pkt_info->errors = GMAC_SET_REG_BITS(pkt_info->errors,
						     RX_PACKET_ERRORS_FRAME_POS,
						     RX_PACKET_ERRORS_FRAME_LEN,
						     1);

	netif_dbg(pdata, rx_status, netdev,
		  "%s - descriptor=%u (cur=%d)\n", channel->name,
		  ring->cur & (ring->dma_desc_count - 1), ring->cur);

	return 0;
}

static int gmac_enable_int(struct gmac_channel *channel,
			   enum gmac_int int_id)
{
	struct gmac_pdata *pdata = channel->pdata;
	unsigned int dma_ch_ier;

	dma_ch_ier = GMAC_IOREAD(pdata, DMA_CH_IER(channel->queue_index));

	switch (int_id) {
	case GMAC_INT_DMA_CH_SR_TI:
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier, DMA_CH_IER_TIE_POS,
					       DMA_CH_IER_TIE_LEN, 1);
		break;
	case GMAC_INT_DMA_CH_SR_TPS:
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier, DMA_CH_IER_TXSE_POS,
					       DMA_CH_IER_TXSE_LEN, 1);
		break;
	case GMAC_INT_DMA_CH_SR_TBU:
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier, DMA_CH_IER_TBUE_POS,
					       DMA_CH_IER_TBUE_LEN, 1);
		break;
	case GMAC_INT_DMA_CH_SR_RI:
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier, DMA_CH_IER_RIE_POS,
					       DMA_CH_IER_RIE_LEN, 1);
		break;
	case GMAC_INT_DMA_CH_SR_RBU:
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier, DMA_CH_IER_RBUE_POS,
					       DMA_CH_IER_RBUE_LEN, 1);
		break;
	case GMAC_INT_DMA_CH_SR_RPS:
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier, DMA_CH_IER_RSE_POS,
					       DMA_CH_IER_RSE_LEN, 1);
		break;
	case GMAC_INT_DMA_CH_SR_TI_RI:
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier, DMA_CH_IER_TIE_POS,
					       DMA_CH_IER_TIE_LEN, 1);
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier, DMA_CH_IER_RIE_POS,
					       DMA_CH_IER_RIE_LEN, 1);
		break;
	case GMAC_INT_DMA_CH_SR_FBE:
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier, DMA_CH_IER_FBEE_POS,
					       DMA_CH_IER_FBEE_LEN, 1);
		break;
	case GMAC_INT_DMA_ALL:
		dma_ch_ier |= channel->saved_ier;
		break;
	default:
		return -1;
	}

	GMAC_IOWRITE(pdata, DMA_CH_IER(channel->queue_index), dma_ch_ier);

	return 0;
}

static int gmac_disable_int(struct gmac_channel *channel,
			    enum gmac_int int_id)
{
	struct gmac_pdata *pdata =  channel->pdata;
	unsigned int dma_ch_ier;

	dma_ch_ier = GMAC_IOREAD(pdata, DMA_CH_IER(channel->queue_index));

	switch (int_id) {
	case GMAC_INT_DMA_CH_SR_TI:
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier, DMA_CH_IER_TIE_POS,
					       DMA_CH_IER_TIE_LEN, 0);
		break;
	case GMAC_INT_DMA_CH_SR_TPS:
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier, DMA_CH_IER_TXSE_POS,
					       DMA_CH_IER_TXSE_LEN, 0);
		break;
	case GMAC_INT_DMA_CH_SR_TBU:
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier, DMA_CH_IER_TBUE_POS,
					       DMA_CH_IER_TBUE_LEN, 0);
		break;
	case GMAC_INT_DMA_CH_SR_RI:
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier, DMA_CH_IER_RIE_POS,
					       DMA_CH_IER_RIE_LEN, 0);
		break;
	case GMAC_INT_DMA_CH_SR_RBU:
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier, DMA_CH_IER_RBUE_POS,
					       DMA_CH_IER_RBUE_LEN, 0);
		break;
	case GMAC_INT_DMA_CH_SR_RPS:
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier, DMA_CH_IER_RSE_POS,
					       DMA_CH_IER_RSE_LEN, 0);
		break;
	case GMAC_INT_DMA_CH_SR_TI_RI:
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier, DMA_CH_IER_TIE_POS,
					       DMA_CH_IER_TIE_LEN, 0);
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier, DMA_CH_IER_RIE_POS,
					       DMA_CH_IER_RIE_LEN, 0);
		break;
	case GMAC_INT_DMA_CH_SR_FBE:
		dma_ch_ier = GMAC_SET_REG_BITS(dma_ch_ier, DMA_CH_IER_FBEE_POS,
					       DMA_CH_IER_FBEE_LEN, 0);
		break;
	case GMAC_INT_DMA_ALL:
		channel->saved_ier = dma_ch_ier & GMAC_DMA_INTERRUPT_MASK;
		dma_ch_ier &= ~GMAC_DMA_INTERRUPT_MASK;
		break;
	default:
		return -1;
	}

	GMAC_IOWRITE(pdata, DMA_CH_IER(channel->queue_index), dma_ch_ier);

	return 0;
}

static int gmac_flush_tx_queues(struct gmac_pdata *pdata)
{
	unsigned int i;
	u32 regval;
	int limit;

	for (i = 0; i < pdata->tx_q_count; i++) {
		regval = GMAC_IOREAD(pdata, MTL_Q_TQOMR(i));
		regval = GMAC_SET_REG_BITS(regval, MTL_Q_TQOMR_FTQ_POS,
					   MTL_Q_TQOMR_FTQ_LEN, 1);
		GMAC_IOWRITE(pdata, MTL_Q_TQOMR(i), regval);
	}

	/* Poll Until Poll Condition */
	for (i = 0; i < pdata->tx_q_count; i++) {
		limit = 10;
		while (limit-- &&
		       GMAC_GET_REG_BITS(GMAC_IOREAD(pdata, MTL_Q_TQOMR(i)),
					 MTL_Q_TQOMR_FTQ_POS,
					 MTL_Q_TQOMR_FTQ_LEN))
			mdelay(10);

		if (limit < 0)
			return -EBUSY;
	}

	return 0;
}

static void gmac_config_dma_bus(struct gmac_pdata *pdata)
{
	u32 regval;

	regval = GMAC_IOREAD(pdata, DMA_SBMR);
	/* Set maximum read outstanding request limit*/
	regval = GMAC_SET_REG_BITS(regval,
				   DMA_SBMR_WR_OSR_LMT_POS,
				   DMA_SBMR_WR_OSR_LMT_LEN,
				   DMA_SBMR_OSR_MAX);
	regval = GMAC_SET_REG_BITS(regval,
				   DMA_SBMR_RD_OSR_LMT_POS,
				   DMA_SBMR_RD_OSR_LMT_LEN,
				   DMA_SBMR_OSR_MAX);
	/* Set the System Bus mode */
	regval = GMAC_SET_REG_BITS(regval,
				   DMA_SBMR_FB_POS,
				   DMA_SBMR_FB_LEN,
				   0);
	regval = GMAC_SET_REG_BITS(regval,
				   DMA_SBMR_BLEN_16_POS,
				   DMA_SBMR_BLEN_16_LEN,
				   1);
	regval = GMAC_SET_REG_BITS(regval,
				   DMA_SBMR_BLEN_8_POS,
				   DMA_SBMR_BLEN_8_LEN,
				   1);
	regval = GMAC_SET_REG_BITS(regval,
				   DMA_SBMR_BLEN_4_POS,
				   DMA_SBMR_BLEN_4_LEN,
				   1);
	GMAC_IOWRITE(pdata, DMA_SBMR, regval);
}

static int gmac_hw_init(struct gmac_pdata *pdata)
{
	struct gmac_desc_ops *desc_ops = &pdata->desc_ops;
	int ret;

	/* Flush Tx queues */
	ret = gmac_flush_tx_queues(pdata);
	if (ret)
		return ret;

	/* Initialize DMA related features */
	gmac_config_dma_bus(pdata);
	gmac_config_osp_mode(pdata);
	gmac_config_pblx8(pdata);
	gmac_config_tx_pbl_val(pdata);
	gmac_config_rx_pbl_val(pdata);
	gmac_config_rx_coalesce(pdata);
	gmac_config_tx_coalesce(pdata);
	gmac_config_rx_buffer_size(pdata);
	gmac_config_tso_mode(pdata);
	gmac_config_sph_mode(pdata);
	desc_ops->tx_desc_init(pdata);
	desc_ops->rx_desc_init(pdata);
	gmac_enable_dma_interrupts(pdata);

	/* Initialize MTL related features */
	gmac_config_mtl_mode(pdata);
	gmac_config_queue_mapping(pdata);
	gmac_config_tsf_mode(pdata, pdata->tx_sf_mode);
	gmac_config_rsf_mode(pdata, pdata->rx_sf_mode);
	gmac_config_tx_threshold(pdata, pdata->tx_threshold);
	gmac_config_rx_threshold(pdata, pdata->rx_threshold);
	gmac_config_tx_fifo_size(pdata);
	gmac_config_rx_fifo_size(pdata);
	gmac_config_flow_control_threshold(pdata);
	gmac_config_rx_fep_enable(pdata);
	gmac_config_rx_fup_enable(pdata);
	gmac_enable_mtl_interrupts(pdata);

	/* Initialize MAC related features */
	gmac_config_mac_address(pdata);
	gmac_config_rx_mode(pdata);
	gmac_config_jumbo_disable(pdata);
	gmac_config_flow_control(pdata);
	gmac_config_mac_speed(pdata);
	gmac_config_checksum_offload(pdata);
	gmac_config_vlan_support(pdata);
	gmac_config_mmc(pdata);
	gmac_enable_mac_interrupts(pdata);

	return 0;
}

static int gmac_hw_exit(struct gmac_pdata *pdata)
{
	u32 regval;
	int limit;

	/* Issue a software reset */
	regval = GMAC_IOREAD(pdata, DMA_MR);
	regval = GMAC_SET_REG_BITS(regval, DMA_MR_SWR_POS,
				   DMA_MR_SWR_LEN, 1);
	GMAC_IOWRITE(pdata, DMA_MR, regval);
	limit = 10;
	while (limit-- &&
	       GMAC_GET_REG_BITS(GMAC_IOREAD(pdata, DMA_MR),
				 DMA_MR_SWR_POS, DMA_MR_SWR_LEN))
		mdelay(10);

	if (limit < 0)
		return -EBUSY;

	return 0;
}

static void gmac_config_hw_timestamping(struct gmac_pdata *pdata,
					u32 data)
{
	GMAC_IOWRITE(pdata, PTP_TCR, data);
}

static void gmac_config_sub_second_increment(struct gmac_pdata *pdata,
					     u32 ptp_clock,
					     u32 *ssinc)
{
	u32 value = GMAC_IOREAD(pdata, PTP_TCR);
	unsigned long data;
	u32 reg_value = 0;

	/* Convert the ptp_clock to nano second
	 *	formula = (1/ptp_clock) * 1000000000
	 * where ptp_clock is 50MHz if fine method is used to update system
	 */
	if (GMAC_GET_REG_BITS(value,
			      PTP_TCR_TSCFUPDT_POS,
			      PTP_TCR_TSCFUPDT_LEN))
		data = (1000000000ULL / 50000000);
	else
		data = (1000000000ULL / ptp_clock);

	/* 0.465ns accuracy */
	if (!GMAC_GET_REG_BITS(value,
			       PTP_TCR_TSCTRLSSR_POS,
			       PTP_TCR_TSCTRLSSR_LEN))
		data = (data * 1000) / 465;

	reg_value = GMAC_SET_REG_BITS(reg_value,
				      PTP_SSIR_SSINC_POS,
				      PTP_SSIR_SSINC_LEN,
				      data);

	GMAC_IOWRITE(pdata, PTP_SSIR, reg_value);

	if (ssinc)
		*ssinc = data;
}

static int gmac_init_systime(struct gmac_pdata *pdata, u32 sec, u32 nsec)
{
	int limit;
	u32 value;

	GMAC_IOWRITE(pdata, PTP_STSUR, sec);
	GMAC_IOWRITE(pdata, PTP_STNSUR, nsec);

	/* issue command to initialize the system time value */
	value = GMAC_IOREAD(pdata, PTP_TCR);
	value = GMAC_SET_REG_BITS(value, PTP_TCR_TSINIT_POS,
				  PTP_TCR_TSINIT_LEN, 1);
	GMAC_IOWRITE(pdata, PTP_TCR, value);

	/* wait for present system time initialize to complete */
	limit = 10;
	while (limit-- &&
	       GMAC_GET_REG_BITS(GMAC_IOREAD(pdata, PTP_TCR),
				 PTP_TCR_TSINIT_POS, PTP_TCR_TSINIT_LEN))
		mdelay(10);

	if (limit < 0)
		return -EBUSY;

	return 0;
}

static int gmac_config_addend(struct gmac_pdata *pdata, u32 addend)
{
	u32 value;
	int limit;

	GMAC_IOWRITE(pdata, PTP_TAR, addend);
	/* issue command to update the addend value */
	value = GMAC_IOREAD(pdata, PTP_TCR);
	value = GMAC_SET_REG_BITS(value, PTP_TCR_TSADDREG_POS,
				  PTP_TCR_TSADDREG_LEN, 1);
	GMAC_IOWRITE(pdata, PTP_TCR, value);

	/* wait for present addend update to complete */
	limit = 10;
	while (limit-- &&
	       GMAC_GET_REG_BITS(GMAC_IOREAD(pdata, PTP_TCR),
				 PTP_TCR_TSADDREG_POS,
				 PTP_TCR_TSADDREG_LEN))
		mdelay(10);

	if (limit < 0)
		return -EBUSY;

	return 0;
}

static int gmac_adjust_systime(struct gmac_pdata *pdata,
			       u32 sec,
			       u32 nsec,
			       int add_sub)
{
	u32 value;
	int limit;

	if (add_sub) {
		/* If the new sec value needs to be subtracted with
		 * the system time, then MAC_STSUR reg should be
		 * programmed with (2^32 - <new_sec_value>)
		 */
		sec = (0x100000000ULL - sec);

		value = GMAC_IOREAD(pdata, PTP_TCR);
		if (GMAC_GET_REG_BITS(value,
				      PTP_TCR_TSCTRLSSR_POS,
				      PTP_TCR_TSCTRLSSR_LEN))
			nsec = (PTP_DIGITAL_ROLLOVER_MODE - nsec);
		else
			nsec = (PTP_BINARY_ROLLOVER_MODE - nsec);
	}

	GMAC_IOWRITE(pdata, PTP_STSUR, sec);

	value = 0;
	value = GMAC_SET_REG_BITS(value, PTP_STNSUR_ADDSUB_POS,
				  PTP_STNSUR_ADDSUB_LEN, add_sub);
	value = GMAC_SET_REG_BITS(value, PTP_STNSUR_TSSSS_POS,
				  PTP_STNSUR_TSSSS_LEN, nsec);
	GMAC_IOWRITE(pdata, PTP_STNSUR, value);

	/* issue command to initialize the system time value */
	value = GMAC_IOREAD(pdata, PTP_TCR);
	value = GMAC_SET_REG_BITS(value, PTP_TCR_TSUPDT_POS,
				  PTP_TCR_TSUPDT_LEN, 1);
	GMAC_IOWRITE(pdata, PTP_TCR, value);

	/* wait for present system time adjust/update to complete */
	limit = 10;
	while (limit-- &&
	       GMAC_GET_REG_BITS(GMAC_IOREAD(pdata, PTP_TCR),
				 PTP_TCR_TSUPDT_POS, PTP_TCR_TSUPDT_LEN))
		mdelay(10);

	if (limit < 0)
		return -EBUSY;

	return 0;
}

static void gmac_get_systime(struct gmac_pdata *pdata, u64 *systime)
{
	u64 ns;

	/* Get the TSSS value */
	ns = GMAC_IOREAD(pdata, PTP_STNSR);
	/* Get the TSS and convert sec time value to nanosecond */
	ns += GMAC_IOREAD(pdata, PTP_STSR) * 1000000000ULL;

	if (systime)
		*systime = ns;
}

static int gmac_get_tx_timestamp_status(struct gmac_dma_desc *dma_desc)
{
	return GMAC_GET_REG_BITS_LE(dma_desc->desc0,
				    TX_NORMAL_DESC3_TTSS_POS,
				    TX_NORMAL_DESC3_TTSS_LEN);
}

static void gmac_get_tx_timestamp(struct gmac_dma_desc *desc, u64 *ts)
{
	u64 ns;

	ns = desc->desc0;
	/* convert high/sec time stamp value to nanosecond */
	ns += (desc->desc1 * 1000000000ULL);

	*ts = ns;
}

static void gmac_get_tx_hwtstamp(struct gmac_pdata *pdata,
				 struct gmac_dma_desc *desc,
				 struct sk_buff *skb)
{
	struct skb_shared_hwtstamps shhwtstamp;
	u64 ns;

	if (!pdata->hwts_tx_en)
		return;

	/* exit if skb doesn't support hw tstamp */
	if (likely(!skb || !(skb_shinfo(skb)->tx_flags & SKBTX_IN_PROGRESS)))
		return;

	/* check tx tstamp status */
	if (gmac_get_tx_timestamp_status(desc)) {
		/* get the valid tstamp */
		gmac_get_tx_timestamp(desc, &ns);

		memset(&shhwtstamp, 0, sizeof(struct skb_shared_hwtstamps));
		shhwtstamp.hwtstamp = ns_to_ktime(ns);

		netdev_dbg(pdata->netdev,
			   "get valid TX hw timestamp %llu\n",
			   ns);
		/* pass tstamp to stack */
		skb_tstamp_tx(skb, &shhwtstamp);
		pdata->stats.tx_timestamp_packets++;
	}
}

void gmac_init_hw_ops(struct gmac_hw_ops *hw_ops)
{
	hw_ops->init = gmac_hw_init;
	hw_ops->exit = gmac_hw_exit;

	hw_ops->tx_complete = gmac_tx_complete;

	hw_ops->enable_tx = gmac_enable_tx;
	hw_ops->disable_tx = gmac_disable_tx;
	hw_ops->enable_rx = gmac_enable_rx;
	hw_ops->disable_rx = gmac_disable_rx;

	hw_ops->dev_xmit = gmac_dev_xmit;
	hw_ops->dev_read = gmac_dev_read;
	hw_ops->enable_int = gmac_enable_int;
	hw_ops->disable_int = gmac_disable_int;

	hw_ops->set_mac_address = gmac_set_mac_address;
	hw_ops->config_rx_mode = gmac_config_rx_mode;
	hw_ops->enable_rx_csum = gmac_enable_rx_csum;
	hw_ops->disable_rx_csum = gmac_disable_rx_csum;

	/* For MII speed configuration */
	hw_ops->set_gmii_10_speed = gmac_set_gmii_10_speed;
	hw_ops->set_gmii_100_speed = gmac_set_gmii_100_speed;
	hw_ops->set_gmii_1000_speed = gmac_set_gmii_1000_speed;

	hw_ops->set_full_duplex = gmac_set_full_duplex;
	hw_ops->set_half_duplex = gmac_set_half_duplex;

	/* For descriptor related operation */
	hw_ops->tx_desc_init = gmac_tx_desc_init;
	hw_ops->rx_desc_init = gmac_rx_desc_init;
	hw_ops->tx_desc_reset = gmac_tx_desc_reset;
	hw_ops->rx_desc_reset = gmac_rx_desc_reset;
	hw_ops->is_last_desc = gmac_is_last_desc;
	hw_ops->is_context_desc = gmac_is_context_desc;
	hw_ops->tx_start_xmit = gmac_tx_start_xmit;

	/* For Flow Control */
	hw_ops->config_tx_flow_control = gmac_config_tx_flow_control;
	hw_ops->config_rx_flow_control = gmac_config_rx_flow_control;

	/* For Vlan related config */
	hw_ops->enable_rx_vlan_stripping = gmac_enable_rx_vlan_stripping;
	hw_ops->disable_rx_vlan_stripping = gmac_disable_rx_vlan_stripping;
	hw_ops->enable_rx_vlan_filtering = gmac_enable_rx_vlan_filtering;
	hw_ops->disable_rx_vlan_filtering = gmac_disable_rx_vlan_filtering;
	hw_ops->update_vlan_hash_table = gmac_update_vlan_hash_table;
	hw_ops->update_vlan = gmac_update_vlan;

	/* For RX coalescing */
	hw_ops->config_rx_coalesce = gmac_config_rx_coalesce;
	hw_ops->config_tx_coalesce = gmac_config_tx_coalesce;
	hw_ops->usec_to_riwt = gmac_usec_to_riwt;
	hw_ops->riwt_to_usec = gmac_riwt_to_usec;

	/* For RX and TX threshold config */
	hw_ops->config_rx_threshold = gmac_config_rx_threshold;
	hw_ops->config_tx_threshold = gmac_config_tx_threshold;

	/* For RX and TX Store and Forward Mode config */
	hw_ops->config_rsf_mode = gmac_config_rsf_mode;
	hw_ops->config_tsf_mode = gmac_config_tsf_mode;

	/* For TX DMA Operating on Second Frame config */
	hw_ops->config_osp_mode = gmac_config_osp_mode;

	/* For RX and TX PBL config */
	hw_ops->config_rx_pbl_val = gmac_config_rx_pbl_val;
	hw_ops->config_tx_pbl_val = gmac_config_tx_pbl_val;
	hw_ops->config_pblx8 = gmac_config_pblx8;

	/* For MMC statistics support */
	hw_ops->tx_mmc_int = gmac_tx_mmc_int;
	hw_ops->rx_mmc_int = gmac_rx_mmc_int;
	hw_ops->rxipc_mmc_int = gmac_rxipc_mmc_int;
	hw_ops->read_mmc_stats = gmac_read_mmc_stats;

	/* For HW timestamping */
	hw_ops->config_hw_timestamping = gmac_config_hw_timestamping;
	hw_ops->config_sub_second_increment = gmac_config_sub_second_increment;
	hw_ops->init_systime = gmac_init_systime;
	hw_ops->config_addend = gmac_config_addend;
	hw_ops->adjust_systime = gmac_adjust_systime;
	hw_ops->get_systime = gmac_get_systime;
	hw_ops->get_tx_hwtstamp = gmac_get_tx_hwtstamp;
}

