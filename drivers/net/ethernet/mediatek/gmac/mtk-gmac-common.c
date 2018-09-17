// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2018 MediaTek Inc.
#include "mtk-gmac.h"

static int debug = -1;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "MediaTek Message Level (-1: default, 0=none,...,16=all)");
static const u32 default_msg_level = (NETIF_MSG_DRV | NETIF_MSG_PROBE |
				      NETIF_MSG_LINK | NETIF_MSG_IFUP |
				      NETIF_MSG_IFDOWN | NETIF_MSG_TIMER);

void gmac_dump_tx_desc(struct gmac_pdata *pdata, struct gmac_ring *ring,
		       unsigned int idx, unsigned int count, unsigned int flag)
{
	struct gmac_desc_data *desc_data;
	struct gmac_dma_desc *dma_desc;

	while (count--) {
		desc_data = GMAC_GET_DESC_DATA(ring, idx);
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

void gmac_dump_rx_desc(struct gmac_pdata *pdata,
		       struct gmac_ring *ring,
		       unsigned int idx)
{
	struct gmac_desc_data *desc_data;
	struct gmac_dma_desc *dma_desc;

	desc_data = GMAC_GET_DESC_DATA(ring, idx);
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

void gmac_print_pkt(struct net_device *netdev,
		    struct sk_buff *skb,
		    bool tx_rx)
{
	struct ethhdr *eth = (struct ethhdr *)skb->data;
	unsigned char buffer[128];
	unsigned int i;

	netdev_dbg(netdev, "\n************** SKB dump ****************\n");

	netdev_dbg(netdev, "%s packet of %d bytes\n",
		   (tx_rx ? "TX" : "RX"), skb->len);

	netdev_dbg(netdev, "Dst MAC addr: %pM\n", eth->h_dest);
	netdev_dbg(netdev, "Src MAC addr: %pM\n", eth->h_source);
	netdev_dbg(netdev, "Protocol: %#06hx\n", ntohs(eth->h_proto));

	for (i = 0; i < skb->len; i += 32) {
		unsigned int len = min(skb->len - i, 32U);

		hex_dump_to_buffer(&skb->data[i], len, 32, 1,
				   buffer, sizeof(buffer), false);
		netdev_dbg(netdev, "  %#06x: %s\n", i, buffer);
	}

	netdev_dbg(netdev, "\n************** SKB dump ****************\n");
}

static void gmac_default_config(struct gmac_pdata *pdata)
{
	pdata->tx_osp_mode	= DMA_OSP_ENABLE;
	pdata->tx_sf_mode	= MTL_TSF_ENABLE;
	pdata->rx_sf_mode	= MTL_RSF_DISABLE;
	pdata->pblx8		= DMA_PBL_X8_ENABLE;
	pdata->tx_pbl		= DMA_PBL_32;
	pdata->rx_pbl		= DMA_PBL_32;
	pdata->tx_threshold	= MTL_TX_THRESHOLD_128;
	pdata->rx_threshold	= MTL_RX_THRESHOLD_128;
	pdata->tx_pause		= 1;
	pdata->rx_pause		= 1;
	pdata->phy_speed	= SPEED_1000;
	pdata->sysclk_rate	= GMAC_SYSCLOCK;

	strlcpy(pdata->drv_name, GMAC_DRV_NAME, sizeof(pdata->drv_name));
	strlcpy(pdata->drv_ver, GMAC_DRV_VERSION, sizeof(pdata->drv_ver));
}

static void gmac_init_all_ops(struct gmac_pdata *pdata)
{
	gmac_init_desc_ops(&pdata->desc_ops);
	gmac_init_hw_ops(&pdata->hw_ops);
}

static void gmac_get_all_hw_features(struct gmac_pdata *pdata)
{
	struct gmac_hw_features *hw_feat = &pdata->hw_feat;
	unsigned int mac_hfr0, mac_hfr1, mac_hfr2;

	mac_hfr0 = GMAC_IOREAD(pdata, MAC_HWF0R);
	mac_hfr1 = GMAC_IOREAD(pdata, MAC_HWF1R);
	mac_hfr2 = GMAC_IOREAD(pdata, MAC_HWF2R);

	memset(hw_feat, 0, sizeof(*hw_feat));

	hw_feat->version = GMAC_IOREAD(pdata, MAC_VR);

	/* Hardware feature register 0 */
	hw_feat->mii		= GMAC_GET_REG_BITS(mac_hfr0,
						    MAC_HW_FEAT_MIISEL_POS,
						    MAC_HW_FEAT_MIISEL_LEN);
	hw_feat->gmii		= GMAC_GET_REG_BITS(mac_hfr0,
						    MAC_HW_FEAT_GMIISEL_POS,
						    MAC_HW_FEAT_GMIISEL_LEN);
	hw_feat->hd		= GMAC_GET_REG_BITS(mac_hfr0,
						    MAC_HW_FEAT_HDSEL_POS,
						    MAC_HW_FEAT_HDSEL_LEN);
	hw_feat->pcs		= GMAC_GET_REG_BITS(mac_hfr0,
						    MAC_HW_FEAT_PCSSEL_POS,
						    MAC_HW_FEAT_PCSSEL_LEN);
	hw_feat->vlhash		= GMAC_GET_REG_BITS(mac_hfr0,
						    MAC_HW_FEAT_VLHASH_POS,
						    MAC_HW_FEAT_VLHASH_LEN);
	hw_feat->sma		= GMAC_GET_REG_BITS(mac_hfr0,
						    MAC_HW_FEAT_SMASEL_POS,
						    MAC_HW_FEAT_SMASEL_LEN);
	hw_feat->rwk		= GMAC_GET_REG_BITS(mac_hfr0,
						    MAC_HW_FEAT_RWKSEL_POS,
						    MAC_HW_FEAT_RWKSEL_LEN);
	hw_feat->mgk		= GMAC_GET_REG_BITS(mac_hfr0,
						    MAC_HW_FEAT_MGKSEL_POS,
						    MAC_HW_FEAT_MGKSEL_LEN);
	hw_feat->mmc		= GMAC_GET_REG_BITS(mac_hfr0,
						    MAC_HW_FEAT_MMCSEL_POS,
						    MAC_HW_FEAT_MMCSEL_LEN);
	hw_feat->aoe		= GMAC_GET_REG_BITS(mac_hfr0,
						    MAC_HW_FEAT_ARPOFFSEL_POS,
						    MAC_HW_FEAT_ARPOFFSEL_LEN);
	hw_feat->ts		= GMAC_GET_REG_BITS(mac_hfr0,
						    MAC_HW_FEAT_TSSEL_POS,
						    MAC_HW_FEAT_TSSEL_LEN);
	hw_feat->eee		= GMAC_GET_REG_BITS(mac_hfr0,
						    MAC_HW_FEAT_EEESEL_POS,
						    MAC_HW_FEAT_EEESEL_LEN);
	hw_feat->tx_coe		= GMAC_GET_REG_BITS(mac_hfr0,
						    MAC_HW_FEAT_TXCOSEL_POS,
						    MAC_HW_FEAT_TXCOSEL_LEN);
	hw_feat->rx_coe		= GMAC_GET_REG_BITS(mac_hfr0,
						    MAC_HW_FEAT_RXCOESEL_POS,
						    MAC_HW_FEAT_RXCOESEL_LEN);
	hw_feat->addn_mac	= GMAC_GET_REG_BITS(mac_hfr0,
						    MAC_HW_FEAT_ADDMAC_POS,
						    MAC_HW_FEAT_ADDMAC_LEN);
	hw_feat->ts_src		= GMAC_GET_REG_BITS(mac_hfr0,
						    MAC_HW_FEAT_TSSTSSEL_POS,
						    MAC_HW_FEAT_TSSTSSEL_LEN);
	hw_feat->sa_vlan_ins	= GMAC_GET_REG_BITS(mac_hfr0,
						    MAC_HW_FEAT_SAVLANINS_POS,
						    MAC_HW_FEAT_SAVLANINS_LEN);
	hw_feat->phyifsel	= GMAC_GET_REG_BITS(mac_hfr0,
						    MAC_HW_FEAT_ACTPHYSEL_POS,
						    MAC_HW_FEAT_ACTPHYSEL_LEN);

	/* Hardware feature register 1 */
	hw_feat->rx_fifo_size	= GMAC_GET_REG_BITS(mac_hfr1,
						    MAC_HW_RXFIFOSIZE_POS,
						    MAC_HW_RXFIFOSIZE_LEN);
	hw_feat->tx_fifo_size	= GMAC_GET_REG_BITS(mac_hfr1,
						    MAC_HW_TXFIFOSIZE_POS,
						    MAC_HW_TXFIFOSIZE_LEN);
	hw_feat->one_step_en	= GMAC_GET_REG_BITS(mac_hfr1,
						    MAC_HW_OSTEN_POS,
						    MAC_HW_OSTEN_LEN);
	hw_feat->ptp_offload	= GMAC_GET_REG_BITS(mac_hfr1,
						    MAC_HW_PTOEN_POS,
						    MAC_HW_PTOEN_LEN);
	hw_feat->adv_ts_hi	= GMAC_GET_REG_BITS(mac_hfr1,
						    MAC_HW_ADVTHWORD_POS,
						    MAC_HW_ADVTHWORD_LEN);
	hw_feat->dma_width	= GMAC_GET_REG_BITS(mac_hfr1,
						    MAC_HW_ADDR64_POS,
						    MAC_HW_ADDR64_LEN);
	hw_feat->dcb		= GMAC_GET_REG_BITS(mac_hfr1,
						    MAC_HW_DCBEN_POS,
						    MAC_HW_DCBEN_LEN);
	hw_feat->sph		= GMAC_GET_REG_BITS(mac_hfr1,
						    MAC_HW_SPHEN_POS,
						    MAC_HW_SPHEN_LEN);
	hw_feat->tso		= GMAC_GET_REG_BITS(mac_hfr1,
						    MAC_HW_TSOEN_POS,
						    MAC_HW_TSOEN_LEN);
	hw_feat->dma_debug	= GMAC_GET_REG_BITS(mac_hfr1,
						    MAC_HW_DMADEBUGEN_POS,
						    MAC_HW_DMADEBUGEN_LEN);
	hw_feat->av		= GMAC_GET_REG_BITS(mac_hfr1,
						    MAC_HW_AV_POS,
						    MAC_HW_AV_LEN);
	hw_feat->rav		= GMAC_GET_REG_BITS(mac_hfr1,
						    MAC_HW_RAV_POS,
						    MAC_HW_RAV_LEN);
	hw_feat->pouost		= GMAC_GET_REG_BITS(mac_hfr1,
						    MAC_HW_POUOST_POS,
						    MAC_HW_POUOST_LEN);
	hw_feat->hash_table_size = GMAC_GET_REG_BITS(mac_hfr1,
						     MAC_HW_HASHTBLSZ_POS,
						     MAC_HW_HASHTBLSZ_LEN);
	hw_feat->l3l4_filter_num = GMAC_GET_REG_BITS(mac_hfr1,
						     MAC_HW_L3L4FNUM_POS,
						     MAC_HW_L3L4FNUM_LEN);

	/* Hardware feature register 2 */
	hw_feat->rx_q_cnt	= GMAC_GET_REG_BITS(mac_hfr2,
						    MAC_HW_FEAT_RXQCNT_POS,
						    MAC_HW_FEAT_RXQCNT_LEN);
	hw_feat->tx_q_cnt	= GMAC_GET_REG_BITS(mac_hfr2,
						    MAC_HW_FEAT_TXQCNT_POS,
						    MAC_HW_FEAT_TXQCNT_LEN);
	hw_feat->rx_ch_cnt	= GMAC_GET_REG_BITS(mac_hfr2,
						    MAC_HW_FEAT_RXCHCNT_POS,
						    MAC_HW_FEAT_RXCHCNT_LEN);
	hw_feat->tx_ch_cnt	= GMAC_GET_REG_BITS(mac_hfr2,
						    MAC_HW_FEAT_TXCHCNT_POS,
						    MAC_HW_FEAT_TXCHCNT_LEN);
	hw_feat->pps_out_num	= GMAC_GET_REG_BITS(mac_hfr2,
						    MAC_HW_FEAT_PPSOUTNUM_POS,
						    MAC_HW_FEAT_PPSOUTNUM_LEN);
	hw_feat->aux_snap_num	= GMAC_GET_REG_BITS(mac_hfr2,
						    MAC_HW_FEAT_AUXSNAPNUM_POS,
						    MAC_HW_FEAT_AUXSNAPNUM_LEN);

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

	/* The Queue and Channel counts are zero based so increment them
	 * to get the actual number
	 */
	hw_feat->rx_q_cnt++;
	hw_feat->tx_q_cnt++;
	hw_feat->rx_ch_cnt++;
	hw_feat->tx_ch_cnt++;
}

static void gmac_print_all_hw_features(struct gmac_pdata *pdata)
{
	char *str = NULL;

	netif_info(pdata, probe, pdata->netdev, "\n");
	netif_info(pdata, probe, pdata->netdev,
		   "=====================================================\n");
	netif_info(pdata, probe, pdata->netdev, "\n");
	netif_info(pdata, probe, pdata->netdev,
		   "HW support following features\n");
	netif_info(pdata, probe, pdata->netdev, "\n");
	/* HW Feature Register0 */
	netif_info(pdata, probe, pdata->netdev,
		   "10/100 Mbps Support                         : %s\n",
		   pdata->hw_feat.mii ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "1000 Mbps Support                           : %s\n",
		   pdata->hw_feat.gmii ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "Half-duplex Support                         : %s\n",
		   pdata->hw_feat.hd ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "PCS Registers(TBI/SGMII/RTBI PHY interface) : %s\n",
		   pdata->hw_feat.pcs ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "VLAN Hash Filter Selected                   : %s\n",
		   pdata->hw_feat.vlhash ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "SMA (MDIO) Interface                        : %s\n",
		   pdata->hw_feat.sma ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "PMT Remote Wake-up Packet Enable            : %s\n",
		   pdata->hw_feat.rwk ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "PMT Magic Packet Enable                     : %s\n",
		   pdata->hw_feat.mgk ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "RMON/MMC Module Enable                      : %s\n",
		   pdata->hw_feat.mmc ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "ARP Offload Enabled                         : %s\n",
		   pdata->hw_feat.aoe ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "IEEE 1588-2008 Timestamp Enabled            : %s\n",
		   pdata->hw_feat.ts ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "Energy Efficient Ethernet Enabled           : %s\n",
		   pdata->hw_feat.eee ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "Transmit Checksum Offload Enabled           : %s\n",
		   pdata->hw_feat.tx_coe ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "Receive Checksum Offload Enabled            : %s\n",
		   pdata->hw_feat.rx_coe ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "Additional MAC Addresses Selected           : %s\n",
		   pdata->hw_feat.addn_mac ? "YES" : "NO");

	if (pdata->hw_feat.addn_mac)
		pdata->max_addr_reg_cnt = pdata->hw_feat.addn_mac;
	else
		pdata->max_addr_reg_cnt = 1;

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
	netif_info(pdata, probe, pdata->netdev,
		   "Timestamp System Time Source                : %s\n", str);

	netif_info(pdata, probe, pdata->netdev,
		   "Source Address or VLAN Insertion Enable     : %s\n",
		   pdata->hw_feat.sa_vlan_ins ? "YES" : "NO");

	switch (pdata->hw_feat.phyifsel) {
	case 0:
		str = "GMII/MII";
		break;
	case 1:
		str = "RGMII";
		break;
	case 2:
		str = "SGMII";
		break;
	case 3:
		str = "TBI";
		break;
	case 4:
		str = "RMII";
		break;
	case 5:
		str = "RTBI";
		break;
	case 6:
		str = "SMII";
		break;
	case 7:
		str = "RevMII";
		break;
	default:
		str = "RESERVED";
	}
	netif_info(pdata, probe, pdata->netdev,
		   "Active PHY Selected                         : %s\n",
		   str);

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
	netif_info(pdata, probe, pdata->netdev,
		   "MTL Receive FIFO Size                       : %s\n",
		   str);

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
	netif_info(pdata, probe, pdata->netdev,
		   "MTL Transmit FIFO Size                      : %s\n",
		   str);
	netif_info(pdata, probe, pdata->netdev,
		   "One-Step Timingstamping Enable              : %s\n",
		   pdata->hw_feat.one_step_en ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "PTP Offload Enable                          : %s\n",
		   pdata->hw_feat.ptp_offload ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "IEEE 1588 High Word Register Enable         : %s\n",
		   pdata->hw_feat.adv_ts_hi ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "DMA Address width                           : %u\n",
		   pdata->hw_feat.dma_width);
	pdata->dma_width = pdata->hw_feat.dma_width + 1;
	netif_info(pdata, probe, pdata->netdev,
		   "DCB Feature Enable                          : %s\n",
		   pdata->hw_feat.dcb ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "Split Header Feature Enable                 : %s\n",
		   pdata->hw_feat.sph ? "YES" : "NO");
	pdata->rx_sph = pdata->hw_feat.sph ? 1 : 0;
	netif_info(pdata, probe, pdata->netdev,
		   "TCP Segmentation Offload Enable             : %s\n",
		   pdata->hw_feat.tso ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "DMA Debug Registers Enabled                 : %s\n",
		   pdata->hw_feat.dma_debug ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "Audio-Vedio Bridge Feature Enabled          : %s\n",
		   pdata->hw_feat.av ? "YES" : "NO");
	netif_info(pdata, probe, pdata->netdev,
		   "Rx Side AV Feature Enabled                  : %s\n",
		   (pdata->hw_feat.rav ? "YES" : "NO"));
	netif_info(pdata, probe, pdata->netdev,
		   "One-Step for PTP over UDP/IP Feature        : %s\n",
		   (pdata->hw_feat.pouost ? "YES" : "NO"));
	netif_info(pdata, probe, pdata->netdev,
		   "Hash Table Size                             : %u\n",
		   pdata->hw_feat.hash_table_size);
	netif_info(pdata, probe, pdata->netdev,
		   "Total number of L3 or L4 Filters            : %u\n",
		   pdata->hw_feat.l3l4_filter_num);

	/* HW Feature Register2 */
	netif_info(pdata, probe, pdata->netdev,
		   "Number of MTL Receive Queues                : %u\n",
		   pdata->hw_feat.rx_q_cnt);
	netif_info(pdata, probe, pdata->netdev,
		   "Number of MTL Transmit Queues               : %u\n",
		   pdata->hw_feat.tx_q_cnt);
	netif_info(pdata, probe, pdata->netdev,
		   "Number of DMA Receive Channels              : %u\n",
		   pdata->hw_feat.rx_ch_cnt);
	netif_info(pdata, probe, pdata->netdev,
		   "Number of DMA Transmit Channels             : %u\n",
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
	netif_info(pdata, probe, pdata->netdev,
		   "Number of PPS Outputs                       : %s\n",
		   str);

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
	netif_info(pdata, probe, pdata->netdev,
		   "Number of Auxiliary Snapshot Inputs         : %s",
		   str);

	netif_info(pdata, probe, pdata->netdev, "\n");
	netif_info(pdata, probe, pdata->netdev,
		   "=====================================================\n");
	netif_info(pdata, probe, pdata->netdev, "\n");
}

static int gmac_init(struct gmac_pdata *pdata)
{
	struct gmac_hw_ops *hw_ops = &pdata->hw_ops;
	struct net_device *netdev = pdata->netdev;
	struct plat_gmac_data *plat = pdata->plat;
	int ret;

	/* power on PHY */
	ret = gpio_request(pdata->phy_rst, "phy_rst");
	if (ret < 0) {
		dev_err(pdata->dev, "Unable to allocate PHY Reset");
		return ret;
	}
	gpio_direction_output(pdata->phy_rst, 1);

	/* Set the PHY mode, delay macro from top
	 * it should be set before mac reset
	 */
	plat->gmac_set_interface(plat);
	plat->gmac_set_delay(plat);

	ret = plat->gmac_clk_enable(plat);
	if (ret) {
		dev_err(pdata->dev, "gmac clk enable failed\n");
		return ret;
	}

	/* Set default configuration data */
	gmac_default_config(pdata);

	/* Set all the function pointers */
	gmac_init_all_ops(pdata);

	/* Issue software reset to device */
	hw_ops->exit(pdata);

	/* Populate the hardware features */
	gmac_get_all_hw_features(pdata);

	/* Set the DMA mask, 4GB mode enabled */
	ret = dma_set_mask_and_coherent(pdata->dev,
					DMA_BIT_MASK(pdata->dma_width));
	if (ret) {
		dev_err(pdata->dev, "dma_set_mask_and_coherent failed");
		return ret;
	}

	/* Channel and ring params initializtion
	 *  pdata->channel_count;
	 *  pdata->tx_ring_count;
	 *  pdata->rx_ring_count;
	 *  pdata->tx_desc_count;
	 *  pdata->rx_desc_count;
	 */
	BUILD_BUG_ON_NOT_POWER_OF_2(GMAC_TX_DESC_CNT);
	BUILD_BUG_ON_NOT_POWER_OF_2(GMAC_RX_DESC_CNT);
	pdata->tx_desc_count = GMAC_TX_DESC_CNT;
	pdata->rx_desc_count = GMAC_RX_DESC_CNT;

	pdata->tx_ring_count = min_t(unsigned int, pdata->hw_feat.tx_ch_cnt,
				     pdata->hw_feat.tx_q_cnt);
	pdata->tx_q_count = pdata->tx_ring_count;
	ret = netif_set_real_num_tx_queues(netdev, pdata->tx_q_count);
	if (ret) {
		dev_err(pdata->dev, "error setting real tx queue count\n");
		return ret;
	}

	pdata->rx_ring_count = min_t(unsigned int, pdata->hw_feat.rx_ch_cnt,
				     pdata->hw_feat.rx_q_cnt);
	pdata->rx_q_count = pdata->rx_ring_count;
	ret = netif_set_real_num_rx_queues(netdev, pdata->rx_q_count);
	if (ret) {
		dev_err(pdata->dev, "error setting real rx queue count\n");
		return ret;
	}

	pdata->channel_count =
		max_t(unsigned int, pdata->tx_ring_count, pdata->rx_ring_count);

	/* Set device operations */
	netdev->netdev_ops = gmac_get_netdev_ops();
	netdev->ethtool_ops = gmac_get_ethtool_ops();

	/* Set device features */
	if (pdata->hw_feat.tso) {
		netdev->hw_features = NETIF_F_TSO;
		netdev->hw_features |= NETIF_F_TSO6;
		netdev->hw_features |= NETIF_F_SG;
		netdev->hw_features |= NETIF_F_IP_CSUM;
		netdev->hw_features |= NETIF_F_IPV6_CSUM;
	} else if (pdata->hw_feat.tx_coe) {
		netdev->hw_features = NETIF_F_IP_CSUM;
		netdev->hw_features |= NETIF_F_IPV6_CSUM;
	}

	if (pdata->hw_feat.rx_coe) {
		netdev->hw_features |= NETIF_F_RXCSUM;
		netdev->hw_features |= NETIF_F_GRO;
	}

	netdev->vlan_features |= netdev->hw_features;

	netdev->hw_features |= NETIF_F_HW_VLAN_CTAG_RX;
	if (pdata->hw_feat.sa_vlan_ins)
		netdev->hw_features |= NETIF_F_HW_VLAN_CTAG_TX;
	if (pdata->hw_feat.vlhash)
		netdev->hw_features |= NETIF_F_HW_VLAN_CTAG_FILTER;

	netdev->features |= netdev->hw_features;
	pdata->netdev_features = netdev->features;

	netdev->priv_flags |= IFF_UNICAST_FLT;

	/* Use default watchdog timeout */
	netdev->watchdog_timeo = 0;

	/* Tx coalesce parameters initialization */
	pdata->tx_usecs = GMAC_INIT_DMA_TX_USECS;
	pdata->tx_frames = GMAC_INIT_DMA_TX_FRAMES;

	/* Rx coalesce parameters initialization */
	pdata->rx_riwt = hw_ops->usec_to_riwt(pdata, GMAC_INIT_DMA_RX_USECS);
	pdata->rx_usecs = GMAC_INIT_DMA_RX_USECS;
	pdata->rx_frames = GMAC_INIT_DMA_RX_FRAMES;

	return 0;
}

int gmac_drv_probe(struct device *dev,
		   struct plat_gmac_data *plat,
		   struct gmac_resources *res)
{
	struct gmac_pdata *pdata;
	struct net_device *netdev;
	int ret = 0;

	netdev = alloc_etherdev_mq(sizeof(struct gmac_pdata),
				   GMAC_MAX_DMA_CHANNELS);
	if (!netdev) {
		dev_err(dev, "Unable to alloc new net device\n");
		return -ENOMEM;
	}

	SET_NETDEV_DEV(netdev, dev);
	dev_set_drvdata(dev, netdev);
	pdata = netdev_priv(netdev);
	pdata->dev = dev;
	pdata->netdev = netdev;
	pdata->plat = plat;
	pdata->mac_regs = res->base_addr;
	pdata->dev_irq = res->irq;
	pdata->phy_rst = res->phy_rst;
	netdev->base_addr = (unsigned long)res->base_addr;
	netdev->irq = res->irq;

	if (res->mac_addr)
		ether_addr_copy(netdev->dev_addr, res->mac_addr);

	/* Check if the MAC address is valid, if not get a random one */
	if (!is_valid_ether_addr(netdev->dev_addr)) {
		pr_info("no valid MAC address supplied, using a random one\n");
		eth_hw_addr_random(pdata->netdev);
	}

	pdata->msg_enable = netif_msg_init(debug, default_msg_level);
	ret = gmac_init(pdata);
	if (ret) {
		dev_err(dev, "gmac init failed\n");
		goto err_free_netdev;
	}

	ret = mdio_register(netdev);
	if (ret < 0) {
		dev_err(dev, "MDIO bus (id %d) registration failed\n",
			pdata->bus_id);
		goto err_free_netdev;
	}

	ret = register_netdev(netdev);
	if (ret) {
		dev_err(dev, "net device registration failed\n");
		goto err_free_netdev;
	}

	gmac_print_all_hw_features(pdata);

	return 0;

err_free_netdev:
	free_netdev(netdev);

	return ret;
}

int gmac_drv_remove(struct device *dev)
{
	struct net_device *netdev = dev_get_drvdata(dev);
	struct gmac_pdata *pdata = netdev_priv(netdev);
	struct plat_gmac_data *plat = pdata->plat;

	plat->gmac_clk_disable(plat);
	unregister_netdev(netdev);
	free_netdev(netdev);

	return 0;
}

