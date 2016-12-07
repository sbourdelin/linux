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

#include <linux/phy.h>
#include <linux/mdio.h>
#include <linux/clk.h>
#include <linux/bitrev.h>
#include <linux/crc32.h>

#include "dwc-eth.h"
#include "dwc-eth-regacc.h"

static int dwc_eth_tx_complete(struct dwc_eth_dma_desc *dma_desc)
{
	return !DWC_ETH_GET_BITS_LE(dma_desc->desc3, TX_NORMAL_DESC3, OWN);
}

static int dwc_eth_disable_rx_csum(struct dwc_eth_pdata *pdata)
{
	DWC_ETH_IOWRITE_BITS(pdata, MAC_RCR, IPC, 0);

	return 0;
}

static int dwc_eth_enable_rx_csum(struct dwc_eth_pdata *pdata)
{
	DWC_ETH_IOWRITE_BITS(pdata, MAC_RCR, IPC, 1);

	return 0;
}

static int dwc_eth_set_mac_address(struct dwc_eth_pdata *pdata, u8 *addr)
{
	unsigned int mac_addr_hi, mac_addr_lo;

	mac_addr_hi = (addr[5] <<  8) | (addr[4] <<  0);
	mac_addr_lo = (addr[3] << 24) | (addr[2] << 16) |
		      (addr[1] <<  8) | (addr[0] <<  0);

	DWC_ETH_IOWRITE(pdata, MAC_MACA0HR, mac_addr_hi);
	DWC_ETH_IOWRITE(pdata, MAC_MACA0LR, mac_addr_lo);

	return 0;
}

static void dwc_eth_set_mac_reg(struct dwc_eth_pdata *pdata,
				struct netdev_hw_addr *ha,
				unsigned int *mac_reg)
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
			  ha->addr, *mac_reg);

		DWC_ETH_SET_BITS(mac_addr_hi, MAC_MACA1HR, AE, 1);
	}

	DWC_ETH_IOWRITE(pdata, *mac_reg, mac_addr_hi);
	*mac_reg += MAC_MACA_INC;
	DWC_ETH_IOWRITE(pdata, *mac_reg, mac_addr_lo);
	*mac_reg += MAC_MACA_INC;
}

static int dwc_eth_enable_rx_vlan_stripping(struct dwc_eth_pdata *pdata)
{
	/* Put the VLAN tag in the Rx descriptor */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_VLANTR, EVLRXS, 1);

	/* Don't check the VLAN type */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_VLANTR, DOVLTC, 1);

	/* Check only C-TAG (0x8100) packets */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_VLANTR, ERSVLM, 0);

	/* Don't consider an S-TAG (0x88A8) packet as a VLAN packet */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_VLANTR, ESVL, 0);

	/* Enable VLAN tag stripping */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_VLANTR, EVLS, 0x3);

	return 0;
}

static int dwc_eth_disable_rx_vlan_stripping(struct dwc_eth_pdata *pdata)
{
	DWC_ETH_IOWRITE_BITS(pdata, MAC_VLANTR, EVLS, 0);

	return 0;
}

static int dwc_eth_enable_rx_vlan_filtering(struct dwc_eth_pdata *pdata)
{
	/* Enable VLAN filtering */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_PFR, VTFE, 1);

	/* Enable VLAN Hash Table filtering */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_VLANTR, VTHM, 1);

	/* Disable VLAN tag inverse matching */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_VLANTR, VTIM, 0);

	/* Only filter on the lower 12-bits of the VLAN tag */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_VLANTR, ETV, 1);

	/* In order for the VLAN Hash Table filtering to be effective,
	 * the VLAN tag identifier in the VLAN Tag Register must not
	 * be zero.  Set the VLAN tag identifier to "1" to enable the
	 * VLAN Hash Table filtering.  This implies that a VLAN tag of
	 * 1 will always pass filtering.
	 */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_VLANTR, VL, 1);

	return 0;
}

static int dwc_eth_disable_rx_vlan_filtering(struct dwc_eth_pdata *pdata)
{
	/* Disable VLAN filtering */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_PFR, VTFE, 0);

	return 0;
}

static u32 dwc_eth_vid_crc32_le(__le16 vid_le)
{
	u32 poly = 0xedb88320;	/* CRCPOLY_LE */
	u32 crc = ~0;
	u32 temp = 0;
	unsigned char *data = (unsigned char *)&vid_le;
	unsigned char data_byte = 0;
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

static int dwc_eth_update_vlan_hash_table(struct dwc_eth_pdata *pdata)
{
	u32 crc;
	u16 vid;
	__le16 vid_le;
	u16 vlan_hash_table = 0;

	/* Generate the VLAN Hash Table value */
	for_each_set_bit(vid, pdata->active_vlans, VLAN_N_VID) {
		/* Get the CRC32 value of the VLAN ID */
		vid_le = cpu_to_le16(vid);
		crc = bitrev32(~dwc_eth_vid_crc32_le(vid_le)) >> 28;

		vlan_hash_table |= (1 << crc);
	}

	/* Set the VLAN Hash Table filtering register */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_VLANHTR, VLHT, vlan_hash_table);

	return 0;
}

static int dwc_eth_set_promiscuous_mode(struct dwc_eth_pdata *pdata,
					unsigned int enable)
{
	unsigned int val = enable ? 1 : 0;

	if (DWC_ETH_IOREAD_BITS(pdata, MAC_PFR, PR) == val)
		return 0;

	netif_dbg(pdata, drv, pdata->netdev, "%s promiscuous mode\n",
		  enable ? "entering" : "leaving");
	DWC_ETH_IOWRITE_BITS(pdata, MAC_PFR, PR, val);

	/* Hardware will still perform VLAN filtering in promiscuous mode */
	if (enable) {
		dwc_eth_disable_rx_vlan_filtering(pdata);
	} else {
		if (pdata->netdev->features & NETIF_F_HW_VLAN_CTAG_FILTER)
			dwc_eth_enable_rx_vlan_filtering(pdata);
	}

	return 0;
}

static int dwc_eth_set_all_multicast_mode(struct dwc_eth_pdata *pdata,
					  unsigned int enable)
{
	unsigned int val = enable ? 1 : 0;

	if (DWC_ETH_IOREAD_BITS(pdata, MAC_PFR, PM) == val)
		return 0;

	netif_dbg(pdata, drv, pdata->netdev, "%s allmulti mode\n",
		  enable ? "entering" : "leaving");
	DWC_ETH_IOWRITE_BITS(pdata, MAC_PFR, PM, val);

	return 0;
}

static void dwc_eth_set_mac_addn_addrs(struct dwc_eth_pdata *pdata)
{
	struct net_device *netdev = pdata->netdev;
	struct netdev_hw_addr *ha;
	unsigned int mac_reg;
	unsigned int addn_macs;

	mac_reg = MAC_MACA1HR;
	addn_macs = pdata->hw_feat.addn_mac;

	if (netdev_uc_count(netdev) > addn_macs) {
		dwc_eth_set_promiscuous_mode(pdata, 1);
	} else {
		netdev_for_each_uc_addr(ha, netdev) {
			dwc_eth_set_mac_reg(pdata, ha, &mac_reg);
			addn_macs--;
		}

		if (netdev_mc_count(netdev) > addn_macs) {
			dwc_eth_set_all_multicast_mode(pdata, 1);
		} else {
			netdev_for_each_mc_addr(ha, netdev) {
				dwc_eth_set_mac_reg(pdata, ha, &mac_reg);
				addn_macs--;
			}
		}
	}

	/* Clear remaining additional MAC address entries */
	while (addn_macs--)
		dwc_eth_set_mac_reg(pdata, NULL, &mac_reg);
}

static void dwc_eth_set_mac_hash_table(struct dwc_eth_pdata *pdata)
{
	struct net_device *netdev = pdata->netdev;
	struct netdev_hw_addr *ha;
	unsigned int hash_reg;
	unsigned int hash_table_shift, hash_table_count;
	u32 hash_table[DWC_ETH_MAC_HASH_TABLE_SIZE];
	u32 crc;
	unsigned int i;

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
	hash_reg = MAC_HTR0;
	for (i = 0; i < hash_table_count; i++) {
		DWC_ETH_IOWRITE(pdata, hash_reg, hash_table[i]);
		hash_reg += MAC_HTR_INC;
	}
}

static int dwc_eth_add_mac_addresses(struct dwc_eth_pdata *pdata)
{
	if (pdata->hw_feat.hash_table_size)
		dwc_eth_set_mac_hash_table(pdata);
	else
		dwc_eth_set_mac_addn_addrs(pdata);

	return 0;
}

static void dwc_eth_config_mac_address(struct dwc_eth_pdata *pdata)
{
	dwc_eth_set_mac_address(pdata, pdata->netdev->dev_addr);

	/* Filtering is done using perfect filtering and hash filtering */
	if (pdata->hw_feat.hash_table_size) {
		DWC_ETH_IOWRITE_BITS(pdata, MAC_PFR, HPF, 1);
		DWC_ETH_IOWRITE_BITS(pdata, MAC_PFR, HUC, 1);
		DWC_ETH_IOWRITE_BITS(pdata, MAC_PFR, HMC, 1);
	}
}

static void dwc_eth_config_jumbo_enable(struct dwc_eth_pdata *pdata)
{
	unsigned int val;

	val = (pdata->netdev->mtu > DWC_ETH_STD_PACKET_MTU) ? 1 : 0;

	DWC_ETH_IOWRITE_BITS(pdata, MAC_RCR, JE, val);
}

static void dwc_eth_config_checksum_offload(struct dwc_eth_pdata *pdata)
{
	if (pdata->netdev->features & NETIF_F_RXCSUM)
		dwc_eth_enable_rx_csum(pdata);
	else
		dwc_eth_disable_rx_csum(pdata);
}

static void dwc_eth_config_vlan_support(struct dwc_eth_pdata *pdata)
{
	/* Indicate that VLAN Tx CTAGs come from context descriptors */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_VLANIR, CSVL, 0);
	DWC_ETH_IOWRITE_BITS(pdata, MAC_VLANIR, VLTI, 1);

	/* Set the current VLAN Hash Table register value */
	dwc_eth_update_vlan_hash_table(pdata);

	if (pdata->netdev->features & NETIF_F_HW_VLAN_CTAG_FILTER)
		dwc_eth_enable_rx_vlan_filtering(pdata);
	else
		dwc_eth_disable_rx_vlan_filtering(pdata);

	if (pdata->netdev->features & NETIF_F_HW_VLAN_CTAG_RX)
		dwc_eth_enable_rx_vlan_stripping(pdata);
	else
		dwc_eth_disable_rx_vlan_stripping(pdata);
}

static int dwc_eth_config_rx_mode(struct dwc_eth_pdata *pdata)
{
	struct net_device *netdev = pdata->netdev;
	unsigned int pr_mode, am_mode;

	pr_mode = ((netdev->flags & IFF_PROMISC) != 0);
	am_mode = ((netdev->flags & IFF_ALLMULTI) != 0);

	dwc_eth_set_promiscuous_mode(pdata, pr_mode);
	dwc_eth_set_all_multicast_mode(pdata, am_mode);

	dwc_eth_add_mac_addresses(pdata);

	return 0;
}

static void dwc_eth_prepare_tx_stop(struct dwc_eth_pdata *pdata,
				    struct dwc_eth_channel *channel)
{
	unsigned int tx_dsr, tx_pos, tx_qidx;
	unsigned int tx_status;
	unsigned long tx_timeout;

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
	tx_timeout = jiffies + (pdata->dma_stop_timeout * HZ);
	while (time_before(jiffies, tx_timeout)) {
		tx_status = DWC_ETH_IOREAD(pdata, tx_dsr);
		tx_status = GET_BITS(tx_status, tx_pos, DMA_DSR_TPS_LEN);
		if ((tx_status == DMA_TPS_STOPPED) ||
		    (tx_status == DMA_TPS_SUSPENDED))
			break;

		usleep_range(500, 1000);
	}

	if (!time_before(jiffies, tx_timeout))
		netdev_info(pdata->netdev,
			    "timed out waiting for Tx DMA channel %u to stop\n",
			    channel->queue_index);
}

static void dwc_eth_enable_tx(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	/* Enable each Tx DMA channel */
	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			break;

		DWC_ETH_DMA_IOWRITE_BITS(channel, DMA_CH_TCR, ST, 1);
	}

	/* Enable each Tx queue */
	for (i = 0; i < pdata->tx_q_count; i++)
		DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_Q_TQOMR, TXQEN,
					 MTL_Q_ENABLED);

	/* Enable MAC Tx */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_TCR, TE, 1);
}

static void dwc_eth_disable_tx(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	/* Prepare for Tx DMA channel stop */
	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			break;

		dwc_eth_prepare_tx_stop(pdata, channel);
	}

	/* Disable MAC Tx */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_TCR, TE, 0);

	/* Disable each Tx queue */
	for (i = 0; i < pdata->tx_q_count; i++)
		DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_Q_TQOMR, TXQEN, 0);

	/* Disable each Tx DMA channel */
	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			break;

		DWC_ETH_DMA_IOWRITE_BITS(channel, DMA_CH_TCR, ST, 0);
	}
}

static void dwc_eth_prepare_rx_stop(struct dwc_eth_pdata *pdata,
				    unsigned int queue)
{
	unsigned int rx_status;
	unsigned long rx_timeout;

	/* The Rx engine cannot be stopped if it is actively processing
	 * packets. Wait for the Rx queue to empty the Rx fifo.  Don't
	 * wait forever though...
	 */
	rx_timeout = jiffies + (pdata->dma_stop_timeout * HZ);
	while (time_before(jiffies, rx_timeout)) {
		rx_status = DWC_ETH_MTL_IOREAD(pdata, queue, MTL_Q_RQDR);
		if ((DWC_ETH_GET_BITS(rx_status, MTL_Q_RQDR, PRXQ) == 0) &&
		    (DWC_ETH_GET_BITS(rx_status, MTL_Q_RQDR, RXQSTS) == 0))
			break;

		usleep_range(500, 1000);
	}

	if (!time_before(jiffies, rx_timeout))
		netdev_info(pdata->netdev,
			    "timed out waiting for Rx queue %u to empty\n",
			    queue);
}

static void dwc_eth_enable_rx(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int reg_val, i;

	/* Enable each Rx DMA channel */
	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->rx_ring)
			break;

		DWC_ETH_DMA_IOWRITE_BITS(channel, DMA_CH_RCR, SR, 1);
	}

	/* Enable each Rx queue */
	reg_val = 0;
	for (i = 0; i < pdata->rx_q_count; i++)
		reg_val |= (0x02 << (i << 1));
	DWC_ETH_IOWRITE(pdata, MAC_RQC0R, reg_val);

	/* Enable MAC Rx */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_RCR, DCRCC, 1);
	DWC_ETH_IOWRITE_BITS(pdata, MAC_RCR, CST, 1);
	DWC_ETH_IOWRITE_BITS(pdata, MAC_RCR, ACS, 1);
	DWC_ETH_IOWRITE_BITS(pdata, MAC_RCR, RE, 1);
}

static void dwc_eth_disable_rx(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	/* Disable MAC Rx */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_RCR, DCRCC, 0);
	DWC_ETH_IOWRITE_BITS(pdata, MAC_RCR, CST, 0);
	DWC_ETH_IOWRITE_BITS(pdata, MAC_RCR, ACS, 0);
	DWC_ETH_IOWRITE_BITS(pdata, MAC_RCR, RE, 0);

	/* Prepare for Rx DMA channel stop */
	for (i = 0; i < pdata->rx_q_count; i++)
		dwc_eth_prepare_rx_stop(pdata, i);

	/* Disable each Rx queue */
	DWC_ETH_IOWRITE(pdata, MAC_RQC0R, 0);

	/* Disable each Rx DMA channel */
	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->rx_ring)
			break;

		DWC_ETH_DMA_IOWRITE_BITS(channel, DMA_CH_RCR, SR, 0);
	}
}

static void dwc_eth_powerup_tx(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	/* Enable each Tx DMA channel */
	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			break;

		DWC_ETH_DMA_IOWRITE_BITS(channel, DMA_CH_TCR, ST, 1);
	}

	/* Enable MAC Tx */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_TCR, TE, 1);
}

static void dwc_eth_powerdown_tx(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	/* Prepare for Tx DMA channel stop */
	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			break;

		dwc_eth_prepare_tx_stop(pdata, channel);
	}

	/* Disable MAC Tx */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_TCR, TE, 0);

	/* Disable each Tx DMA channel */
	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			break;

		DWC_ETH_DMA_IOWRITE_BITS(channel, DMA_CH_TCR, ST, 0);
	}
}

static void dwc_eth_powerup_rx(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	/* Enable each Rx DMA channel */
	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->rx_ring)
			break;

		DWC_ETH_DMA_IOWRITE_BITS(channel, DMA_CH_RCR, SR, 1);
	}
}

static void dwc_eth_powerdown_rx(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	/* Disable each Rx DMA channel */
	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->rx_ring)
			break;

		DWC_ETH_DMA_IOWRITE_BITS(channel, DMA_CH_RCR, SR, 0);
	}
}

static void dwc_eth_tx_start_xmit(struct dwc_eth_channel *channel,
				  struct dwc_eth_ring *ring)
{
	struct dwc_eth_pdata *pdata = channel->pdata;
	struct dwc_eth_desc_data *desc_data;

	/* Make sure everything is written before the register write */
	wmb();

	/* Issue a poll command to Tx DMA by writing address
	 * of next immediate free descriptor
	 */
	desc_data = DWC_ETH_GET_DESC_DATA(ring, ring->cur);
	DWC_ETH_DMA_IOWRITE(channel, DMA_CH_TDTR_LO,
			    lower_32_bits(desc_data->dma_desc_addr));

	/* Start the Tx timer */
	if (pdata->tx_usecs && !channel->tx_timer_active) {
		channel->tx_timer_active = 1;
		mod_timer(&channel->tx_timer,
			  jiffies + usecs_to_jiffies(pdata->tx_usecs));
	}

	ring->tx.xmit_more = 0;
}

static void dwc_eth_dev_xmit(struct dwc_eth_channel *channel)
{
	struct dwc_eth_pdata *pdata = channel->pdata;
	struct dwc_eth_ring *ring = channel->tx_ring;
	struct dwc_eth_desc_data *desc_data;
	struct dwc_eth_dma_desc *dma_desc;
	struct dwc_eth_pkt_info *pkt_info = &ring->pkt_info;
	unsigned int csum, tso, vlan;
	unsigned int tso_context, vlan_context;
	unsigned int tx_set_ic;
	int start_index = ring->cur;
	int cur_index = ring->cur;
	int i;

	TRACE("-->");

	csum = DWC_ETH_GET_BITS(pkt_info->attributes, TX_PACKET_ATTRIBUTES,
				CSUM_ENABLE);
	tso = DWC_ETH_GET_BITS(pkt_info->attributes, TX_PACKET_ATTRIBUTES,
			       TSO_ENABLE);
	vlan = DWC_ETH_GET_BITS(pkt_info->attributes, TX_PACKET_ATTRIBUTES,
				VLAN_CTAG);

	if (tso && (pkt_info->mss != ring->tx.cur_mss))
		tso_context = 1;
	else
		tso_context = 0;

	if (vlan && (pkt_info->vlan_ctag != ring->tx.cur_vlan_ctag))
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

	desc_data = DWC_ETH_GET_DESC_DATA(ring, cur_index);
	dma_desc = desc_data->dma_desc;

	/* Create a context descriptor if this is a TSO pkt_info */
	if (tso_context || vlan_context) {
		if (tso_context) {
			netif_dbg(pdata, tx_queued, pdata->netdev,
				  "TSO context descriptor, mss=%u\n",
				  pkt_info->mss);

			/* Set the MSS size */
			DWC_ETH_SET_BITS_LE(dma_desc->desc2, TX_CONTEXT_DESC2,
					    MSS, pkt_info->mss);

			/* Mark it as a CONTEXT descriptor */
			DWC_ETH_SET_BITS_LE(dma_desc->desc3, TX_CONTEXT_DESC3,
					    CTXT, 1);

			/* Indicate this descriptor contains the MSS */
			DWC_ETH_SET_BITS_LE(dma_desc->desc3, TX_CONTEXT_DESC3,
					    TCMSSV, 1);

			ring->tx.cur_mss = pkt_info->mss;
		}

		if (vlan_context) {
			netif_dbg(pdata, tx_queued, pdata->netdev,
				  "VLAN context descriptor, ctag=%u\n",
				  pkt_info->vlan_ctag);

			/* Mark it as a CONTEXT descriptor */
			DWC_ETH_SET_BITS_LE(dma_desc->desc3, TX_CONTEXT_DESC3,
					    CTXT, 1);

			/* Set the VLAN tag */
			DWC_ETH_SET_BITS_LE(dma_desc->desc3, TX_CONTEXT_DESC3,
					    VT, pkt_info->vlan_ctag);

			/* Indicate this descriptor contains the VLAN tag */
			DWC_ETH_SET_BITS_LE(dma_desc->desc3, TX_CONTEXT_DESC3,
					    VLTV, 1);

			ring->tx.cur_vlan_ctag = pkt_info->vlan_ctag;
		}

		cur_index++;
		desc_data = DWC_ETH_GET_DESC_DATA(ring, cur_index);
		dma_desc = desc_data->dma_desc;
	}

	/* Update buffer address (for TSO this is the header) */
	dma_desc->desc0 =  cpu_to_le32(lower_32_bits(desc_data->skb_dma));
	dma_desc->desc1 =  cpu_to_le32(upper_32_bits(desc_data->skb_dma));

	/* Update the buffer length */
	DWC_ETH_SET_BITS_LE(dma_desc->desc2, TX_NORMAL_DESC2, HL_B1L,
			    desc_data->skb_dma_len);

	/* VLAN tag insertion check */
	if (vlan)
		DWC_ETH_SET_BITS_LE(dma_desc->desc2, TX_NORMAL_DESC2, VTIR,
				    TX_NORMAL_DESC2_VLAN_INSERT);

	/* Timestamp enablement check */
	if (DWC_ETH_GET_BITS(pkt_info->attributes, TX_PACKET_ATTRIBUTES, PTP))
		DWC_ETH_SET_BITS_LE(dma_desc->desc2, TX_NORMAL_DESC2, TTSE, 1);

	/* Mark it as First Descriptor */
	DWC_ETH_SET_BITS_LE(dma_desc->desc3, TX_NORMAL_DESC3, FD, 1);

	/* Mark it as a NORMAL descriptor */
	DWC_ETH_SET_BITS_LE(dma_desc->desc3, TX_NORMAL_DESC3, CTXT, 0);

	/* Set OWN bit if not the first descriptor */
	if (cur_index != start_index)
		DWC_ETH_SET_BITS_LE(dma_desc->desc3, TX_NORMAL_DESC3, OWN, 1);

	if (tso) {
		/* Enable TSO */
		DWC_ETH_SET_BITS_LE(dma_desc->desc3, TX_NORMAL_DESC3, TSE, 1);
		DWC_ETH_SET_BITS_LE(dma_desc->desc3, TX_NORMAL_DESC3, TCPPL,
				    pkt_info->tcp_payload_len);
		DWC_ETH_SET_BITS_LE(dma_desc->desc3, TX_NORMAL_DESC3, TCPHDRLEN,
				    pkt_info->tcp_header_len / 4);

		pdata->stats.tx_tso_packets++;
	} else {
		/* Enable CRC and Pad Insertion */
		DWC_ETH_SET_BITS_LE(dma_desc->desc3, TX_NORMAL_DESC3, CPC, 0);

		/* Enable HW CSUM */
		if (csum)
			DWC_ETH_SET_BITS_LE(dma_desc->desc3, TX_NORMAL_DESC3,
					    CIC, 0x3);

		/* Set the total length to be transmitted */
		DWC_ETH_SET_BITS_LE(dma_desc->desc3, TX_NORMAL_DESC3, FL,
				    pkt_info->length);
	}

	for (i = cur_index - start_index + 1; i < pkt_info->desc_count; i++) {
		cur_index++;
		desc_data = DWC_ETH_GET_DESC_DATA(ring, cur_index);
		dma_desc = desc_data->dma_desc;

		/* Update buffer address */
		dma_desc->desc0 =
			cpu_to_le32(lower_32_bits(desc_data->skb_dma));
		dma_desc->desc1 =
			cpu_to_le32(upper_32_bits(desc_data->skb_dma));

		/* Update the buffer length */
		DWC_ETH_SET_BITS_LE(dma_desc->desc2, TX_NORMAL_DESC2, HL_B1L,
				    desc_data->skb_dma_len);

		/* Set OWN bit */
		DWC_ETH_SET_BITS_LE(dma_desc->desc3, TX_NORMAL_DESC3, OWN, 1);

		/* Mark it as NORMAL descriptor */
		DWC_ETH_SET_BITS_LE(dma_desc->desc3, TX_NORMAL_DESC3, CTXT, 0);

		/* Enable HW CSUM */
		if (csum)
			DWC_ETH_SET_BITS_LE(dma_desc->desc3, TX_NORMAL_DESC3,
					    CIC, 0x3);
	}

	/* Set LAST bit for the last descriptor */
	DWC_ETH_SET_BITS_LE(dma_desc->desc3, TX_NORMAL_DESC3, LD, 1);

	/* Set IC bit based on Tx coalescing settings */
	if (tx_set_ic)
		DWC_ETH_SET_BITS_LE(dma_desc->desc2, TX_NORMAL_DESC2, IC, 1);

	/* Save the Tx info to report back during cleanup */
	desc_data->tx.packets = pkt_info->tx_packets;
	desc_data->tx.bytes = pkt_info->tx_bytes;

	/* In case the Tx DMA engine is running, make sure everything
	 * is written to the descriptor(s) before setting the OWN bit
	 * for the first descriptor
	 */
	dma_wmb();

	/* Set OWN bit for the first descriptor */
	desc_data = DWC_ETH_GET_DESC_DATA(ring, start_index);
	dma_desc = desc_data->dma_desc;
	DWC_ETH_SET_BITS_LE(dma_desc->desc3, TX_NORMAL_DESC3, OWN, 1);

	if (netif_msg_tx_queued(pdata))
		dwc_eth_dump_tx_desc(pdata, ring, start_index,
				     pkt_info->desc_count, 1);

	/* Make sure ownership is written to the descriptor */
	smp_wmb();

	ring->cur = cur_index + 1;
	if (!pkt_info->skb->xmit_more ||
	    netif_xmit_stopped(netdev_get_tx_queue(pdata->netdev,
						   channel->queue_index)))
		dwc_eth_tx_start_xmit(channel, ring);
	else
		ring->tx.xmit_more = 1;

	DBGPR("  %s: descriptors %u to %u written\n",
	      channel->name, start_index & (ring->dma_desc_count - 1),
	      (ring->cur - 1) & (ring->dma_desc_count - 1));
	TRACE("<--");
}

static void dwc_eth_update_tstamp_addend(struct dwc_eth_pdata *pdata,
					 unsigned int addend)
{
	/* Set the addend register value and tell the device */
	DWC_ETH_IOWRITE(pdata, MAC_TSAR, addend);
	DWC_ETH_IOWRITE_BITS(pdata, MAC_TSCR, TSADDREG, 1);

	/* Wait for addend update to complete */
	while (DWC_ETH_IOREAD_BITS(pdata, MAC_TSCR, TSADDREG))
		udelay(5);
}

static void dwc_eth_set_tstamp_time(struct dwc_eth_pdata *pdata,
				    unsigned int sec,
				    unsigned int nsec)
{
	/* Set the time values and tell the device */
	DWC_ETH_IOWRITE(pdata, MAC_STSUR, sec);
	DWC_ETH_IOWRITE(pdata, MAC_STNUR, nsec);
	DWC_ETH_IOWRITE_BITS(pdata, MAC_TSCR, TSINIT, 1);

	/* Wait for time update to complete */
	while (DWC_ETH_IOREAD_BITS(pdata, MAC_TSCR, TSINIT))
		udelay(5);
}

static u64 dwc_eth_get_tstamp_time(struct dwc_eth_pdata *pdata)
{
	u64 nsec;

	nsec = DWC_ETH_IOREAD(pdata, MAC_STSR);
	nsec *= NSEC_PER_SEC;
	nsec += DWC_ETH_IOREAD(pdata, MAC_STNR);

	return nsec;
}

static u64 dwc_eth_get_tx_tstamp(struct dwc_eth_pdata *pdata)
{
	unsigned int tx_snr;
	u64 nsec;

	tx_snr = DWC_ETH_IOREAD(pdata, MAC_TXSNR);
	if (DWC_ETH_GET_BITS(tx_snr, MAC_TXSNR, TXTSSTSMIS))
		return 0;

	nsec = DWC_ETH_IOREAD(pdata, MAC_TXSSR);
	nsec *= NSEC_PER_SEC;
	nsec += tx_snr;

	return nsec;
}

static void dwc_eth_get_rx_tstamp(struct dwc_eth_pkt_info *pkt_info,
				  struct dwc_eth_dma_desc *dma_desc)
{
	u64 nsec;

	if (DWC_ETH_GET_BITS_LE(dma_desc->desc3, RX_CONTEXT_DESC3, TSA) &&
	    !DWC_ETH_GET_BITS_LE(dma_desc->desc3, RX_CONTEXT_DESC3, TSD)) {
		nsec = le32_to_cpu(dma_desc->desc1);
		nsec <<= 32;
		nsec |= le32_to_cpu(dma_desc->desc0);
		if (nsec != 0xffffffffffffffffULL) {
			pkt_info->rx_tstamp = nsec;
			DWC_ETH_SET_BITS(pkt_info->attributes,
					 RX_PACKET_ATTRIBUTES,
					RX_TSTAMP, 1);
		}
	}
}

static int dwc_eth_config_tstamp(struct dwc_eth_pdata *pdata,
				 unsigned int mac_tscr)
{
	/* Set one nano-second accuracy */
	DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSCTRLSSR, 1);

	/* Set fine timestamp update */
	DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TSCFUPDT, 1);

	/* Overwrite earlier timestamps */
	DWC_ETH_SET_BITS(mac_tscr, MAC_TSCR, TXTSSTSM, 1);

	DWC_ETH_IOWRITE(pdata, MAC_TSCR, mac_tscr);

	/* Exit if timestamping is not enabled */
	if (!DWC_ETH_GET_BITS(mac_tscr, MAC_TSCR, TSENA))
		return 0;

	/* Initialize time registers */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_SSIR, SSINC, pdata->tstamp_ssinc);
	DWC_ETH_IOWRITE_BITS(pdata, MAC_SSIR, SNSINC, pdata->tstamp_snsinc);
	dwc_eth_update_tstamp_addend(pdata, pdata->tstamp_addend);
	dwc_eth_set_tstamp_time(pdata, 0, 0);

	/* Initialize the timecounter */
	timecounter_init(&pdata->tstamp_tc, &pdata->tstamp_cc,
			 ktime_to_ns(ktime_get_real()));

	return 0;
}

static void dwc_eth_tx_desc_reset(struct dwc_eth_desc_data *desc_data)
{
	struct dwc_eth_dma_desc *dma_desc = desc_data->dma_desc;

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

static void dwc_eth_tx_desc_init(struct dwc_eth_channel *channel)
{
	struct dwc_eth_ring *ring = channel->tx_ring;
	struct dwc_eth_desc_data *desc_data;
	int i;
	int start_index = ring->cur;

	TRACE("-->");

	/* Initialze all descriptors */
	for (i = 0; i < ring->dma_desc_count; i++) {
		desc_data = DWC_ETH_GET_DESC_DATA(ring, i);

		/* Initialize Tx descriptor */
		dwc_eth_tx_desc_reset(desc_data);
	}

	/* Update the total number of Tx descriptors */
	DWC_ETH_DMA_IOWRITE(channel, DMA_CH_TDRLR, ring->dma_desc_count - 1);

	/* Update the starting address of descriptor ring */
	desc_data = DWC_ETH_GET_DESC_DATA(ring, start_index);
	DWC_ETH_DMA_IOWRITE(channel, DMA_CH_TDLR_HI,
			    upper_32_bits(desc_data->dma_desc_addr));
	DWC_ETH_DMA_IOWRITE(channel, DMA_CH_TDLR_LO,
			    lower_32_bits(desc_data->dma_desc_addr));

	TRACE("<--");
}

static void dwc_eth_rx_desc_reset(struct dwc_eth_pdata *pdata,
				  struct dwc_eth_desc_data *desc_data,
				  unsigned int index)
{
	struct dwc_eth_dma_desc *dma_desc = desc_data->dma_desc;
	unsigned int rx_usecs = pdata->rx_usecs;
	unsigned int rx_frames = pdata->rx_frames;
	unsigned int inte;
	dma_addr_t hdr_dma, buf_dma;

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
	 *   Set buffer 1 (lo) address to header dma address (lo)
	 *   Set buffer 1 (hi) address to header dma address (hi)
	 *   Set buffer 2 (lo) address to buffer dma address (lo)
	 *   Set buffer 2 (hi) address to buffer dma address (hi) and
	 *     set control bits OWN and INTE
	 */
	hdr_dma = desc_data->rx.hdr.dma_base + desc_data->rx.hdr.dma_off;
	buf_dma = desc_data->rx.buf.dma_base + desc_data->rx.buf.dma_off;
	dma_desc->desc0 = cpu_to_le32(lower_32_bits(hdr_dma));
	dma_desc->desc1 = cpu_to_le32(upper_32_bits(hdr_dma));
	dma_desc->desc2 = cpu_to_le32(lower_32_bits(buf_dma));
	dma_desc->desc3 = cpu_to_le32(upper_32_bits(buf_dma));

	DWC_ETH_SET_BITS_LE(dma_desc->desc3, RX_NORMAL_DESC3, INTE, inte);

	/* Since the Rx DMA engine is likely running, make sure everything
	 * is written to the descriptor(s) before setting the OWN bit
	 * for the descriptor
	 */
	dma_wmb();

	DWC_ETH_SET_BITS_LE(dma_desc->desc3, RX_NORMAL_DESC3, OWN, 1);

	/* Make sure ownership is written to the descriptor */
	dma_wmb();
}

static void dwc_eth_rx_desc_init(struct dwc_eth_channel *channel)
{
	struct dwc_eth_pdata *pdata = channel->pdata;
	struct dwc_eth_ring *ring = channel->rx_ring;
	struct dwc_eth_desc_data *desc_data;
	unsigned int start_index = ring->cur;
	unsigned int i;

	TRACE("-->");

	/* Initialize all descriptors */
	for (i = 0; i < ring->dma_desc_count; i++) {
		desc_data = DWC_ETH_GET_DESC_DATA(ring, i);

		/* Initialize Rx descriptor */
		dwc_eth_rx_desc_reset(pdata, desc_data, i);
	}

	/* Update the total number of Rx descriptors */
	DWC_ETH_DMA_IOWRITE(channel, DMA_CH_RDRLR, ring->dma_desc_count - 1);

	/* Update the starting address of descriptor ring */
	desc_data = DWC_ETH_GET_DESC_DATA(ring, start_index);
	DWC_ETH_DMA_IOWRITE(channel, DMA_CH_RDLR_HI,
			    upper_32_bits(desc_data->dma_desc_addr));
	DWC_ETH_DMA_IOWRITE(channel, DMA_CH_RDLR_LO,
			    lower_32_bits(desc_data->dma_desc_addr));

	/* Update the Rx Descriptor Tail Pointer */
	desc_data = DWC_ETH_GET_DESC_DATA(ring, start_index +
					  ring->dma_desc_count - 1);
	DWC_ETH_DMA_IOWRITE(channel, DMA_CH_RDTR_LO,
			    lower_32_bits(desc_data->dma_desc_addr));

	TRACE("<--");
}

static int dwc_eth_is_context_desc(struct dwc_eth_dma_desc *dma_desc)
{
	/* Rx and Tx share CTXT bit, so check TDES3.CTXT bit */
	return DWC_ETH_GET_BITS_LE(dma_desc->desc3, TX_NORMAL_DESC3, CTXT);
}

static int dwc_eth_is_last_desc(struct dwc_eth_dma_desc *dma_desc)
{
	/* Rx and Tx share LD bit, so check TDES3.LD bit */
	return DWC_ETH_GET_BITS_LE(dma_desc->desc3, TX_NORMAL_DESC3, LD);
}

static int dwc_eth_disable_tx_flow_control(struct dwc_eth_pdata *pdata)
{
	unsigned int max_q_count, q_count;
	unsigned int reg, reg_val;
	unsigned int i;

	/* Clear MTL flow control */
	for (i = 0; i < pdata->rx_q_count; i++)
		DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_Q_RQOMR, EHFC, 0);

	/* Clear MAC flow control */
	max_q_count = pdata->max_flow_control_queues;
	q_count = min_t(unsigned int, pdata->tx_q_count, max_q_count);
	reg = MAC_Q0TFCR;
	for (i = 0; i < q_count; i++) {
		reg_val = DWC_ETH_IOREAD(pdata, reg);
		DWC_ETH_SET_BITS(reg_val, MAC_Q0TFCR, TFE, 0);
		DWC_ETH_IOWRITE(pdata, reg, reg_val);

		reg += MAC_QTFCR_INC;
	}

	return 0;
}

static int dwc_eth_enable_tx_flow_control(struct dwc_eth_pdata *pdata)
{
	struct ieee_pfc *pfc = pdata->pfc;
	struct ieee_ets *ets = pdata->ets;
	unsigned int max_q_count, q_count;
	unsigned int reg, reg_val;
	unsigned int i;

	/* Set MTL flow control */
	for (i = 0; i < pdata->rx_q_count; i++) {
		unsigned int ehfc = 0;

		if (pfc && ets) {
			unsigned int prio;

			for (prio = 0; prio < IEEE_8021QAZ_MAX_TCS; prio++) {
				unsigned int tc;

				/* Does this queue handle the priority? */
				if (pdata->prio2q_map[prio] != i)
					continue;

				/* Get the Traffic Class for this priority */
				tc = ets->prio_tc[prio];

				/* Check if flow control should be enabled */
				if (pfc->pfc_en & (1 << tc)) {
					ehfc = 1;
					break;
				}
			}
		} else {
			ehfc = 1;
		}

		DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_Q_RQOMR, EHFC, ehfc);

		netif_dbg(pdata, drv, pdata->netdev,
			  "flow control %s for RXq%u\n",
			  ehfc ? "enabled" : "disabled", i);
	}

	/* Set MAC flow control */
	max_q_count = pdata->max_flow_control_queues;
	q_count = min_t(unsigned int, pdata->tx_q_count, max_q_count);
	reg = MAC_Q0TFCR;
	for (i = 0; i < q_count; i++) {
		reg_val = DWC_ETH_IOREAD(pdata, reg);

		/* Enable transmit flow control */
		DWC_ETH_SET_BITS(reg_val, MAC_Q0TFCR, TFE, 1);
		/* Set pause time */
		DWC_ETH_SET_BITS(reg_val, MAC_Q0TFCR, PT, 0xffff);

		DWC_ETH_IOWRITE(pdata, reg, reg_val);

		reg += MAC_QTFCR_INC;
	}

	return 0;
}

static int dwc_eth_disable_rx_flow_control(struct dwc_eth_pdata *pdata)
{
	DWC_ETH_IOWRITE_BITS(pdata, MAC_RFCR, RFE, 0);

	return 0;
}

static int dwc_eth_enable_rx_flow_control(struct dwc_eth_pdata *pdata)
{
	DWC_ETH_IOWRITE_BITS(pdata, MAC_RFCR, RFE, 1);

	return 0;
}

static int dwc_eth_config_tx_flow_control(struct dwc_eth_pdata *pdata)
{
	struct ieee_pfc *pfc = pdata->pfc;

	if (pdata->tx_pause || (pfc && pfc->pfc_en))
		dwc_eth_enable_tx_flow_control(pdata);
	else
		dwc_eth_disable_tx_flow_control(pdata);

	return 0;
}

static int dwc_eth_config_rx_flow_control(struct dwc_eth_pdata *pdata)
{
	struct ieee_pfc *pfc = pdata->pfc;

	if (pdata->rx_pause || (pfc && pfc->pfc_en))
		dwc_eth_enable_rx_flow_control(pdata);
	else
		dwc_eth_disable_rx_flow_control(pdata);

	return 0;
}

static int dwc_eth_config_rx_coalesce(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->rx_ring)
			break;

		DWC_ETH_DMA_IOWRITE_BITS(channel, DMA_CH_RIWT, RWT,
					 pdata->rx_riwt);
	}

	return 0;
}

static void dwc_eth_config_flow_control(struct dwc_eth_pdata *pdata)
{
	struct ieee_pfc *pfc = pdata->pfc;

	dwc_eth_config_tx_flow_control(pdata);
	dwc_eth_config_rx_flow_control(pdata);

	DWC_ETH_IOWRITE_BITS(pdata, MAC_RFCR, PFCE,
			     (pfc && pfc->pfc_en) ? 1 : 0);
}

static void dwc_eth_config_tc(struct dwc_eth_pdata *pdata)
{
	unsigned int offset, queue, prio;
	u8 i;

	netdev_reset_tc(pdata->netdev);
	if (!pdata->num_tcs)
		return;

	netdev_set_num_tc(pdata->netdev, pdata->num_tcs);

	for (i = 0, queue = 0, offset = 0; i < pdata->num_tcs; i++) {
		while ((queue < pdata->tx_q_count) &&
		       (pdata->q2tc_map[queue] == i))
			queue++;

		netif_dbg(pdata, drv, pdata->netdev, "TC%u using TXq%u-%u\n",
			  i, offset, queue - 1);
		netdev_set_tc_queue(pdata->netdev, i, queue - offset, offset);
		offset = queue;
	}

	if (!pdata->ets)
		return;

	for (prio = 0; prio < IEEE_8021QAZ_MAX_TCS; prio++)
		netdev_set_prio_tc_map(pdata->netdev, prio,
				       pdata->ets->prio_tc[prio]);
}

static void dwc_eth_config_rx_fep_enable(struct dwc_eth_pdata *pdata)
{
	unsigned int i;

	for (i = 0; i < pdata->rx_q_count; i++)
		DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_Q_RQOMR, FEP, 1);
}

static void dwc_eth_config_rx_fup_enable(struct dwc_eth_pdata *pdata)
{
	unsigned int i;

	for (i = 0; i < pdata->rx_q_count; i++)
		DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_Q_RQOMR, FUP, 1);
}

static void dwc_eth_config_dcb_tc(struct dwc_eth_pdata *pdata)
{
	struct ieee_ets *ets = pdata->ets;
	unsigned int total_weight, min_weight, weight;
	unsigned int mask, reg, reg_val;
	unsigned int i, prio;

	if (!ets)
		return;

	/* Set Tx to deficit weighted round robin scheduling algorithm (when
	 * traffic class is using ETS algorithm)
	 */
	DWC_ETH_IOWRITE_BITS(pdata, MTL_OMR, ETSALG, MTL_ETSALG_DWRR);

	/* Set Traffic Class algorithms */
	total_weight = pdata->netdev->mtu * pdata->hw_feat.tc_cnt;
	min_weight = total_weight / 100;
	if (!min_weight)
		min_weight = 1;

	for (i = 0; i < pdata->hw_feat.tc_cnt; i++) {
		/* Map the priorities to the traffic class */
		mask = 0;
		for (prio = 0; prio < IEEE_8021QAZ_MAX_TCS; prio++) {
			if (ets->prio_tc[prio] == i)
				mask |= (1 << prio);
		}
		mask &= 0xff;

		netif_dbg(pdata, drv, pdata->netdev, "TC%u PRIO mask=%#x\n",
			  i, mask);
		reg = MTL_TCPM0R + (MTL_TCPM_INC * (i / MTL_TCPM_TC_PER_REG));
		reg_val = DWC_ETH_IOREAD(pdata, reg);

		reg_val &= ~(0xff << ((i % MTL_TCPM_TC_PER_REG) << 3));
		reg_val |= (mask << ((i % MTL_TCPM_TC_PER_REG) << 3));

		DWC_ETH_IOWRITE(pdata, reg, reg_val);

		/* Set the traffic class algorithm */
		switch (ets->tc_tsa[i]) {
		case IEEE_8021QAZ_TSA_STRICT:
			netif_dbg(pdata, drv, pdata->netdev,
				  "TC%u using SP\n", i);
			DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_TC_ETSCR, TSA,
						 MTL_TSA_SP);
			break;
		case IEEE_8021QAZ_TSA_ETS:
			weight = total_weight * ets->tc_tx_bw[i] / 100;
			weight = clamp(weight, min_weight, total_weight);

			netif_dbg(pdata, drv, pdata->netdev,
				  "TC%u using DWRR (weight %u)\n", i, weight);
			DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_TC_ETSCR, TSA,
						 MTL_TSA_ETS);
			DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_TC_QWR, QW,
						 weight);
			break;
		}
	}

	dwc_eth_config_tc(pdata);
}

static void dwc_eth_config_dcb_pfc(struct dwc_eth_pdata *pdata)
{
	dwc_eth_config_flow_control(pdata);
}

static int dwc_eth_config_tx_coalesce(struct dwc_eth_pdata *pdata)
{
	return 0;
}

static void dwc_eth_config_rx_buffer_size(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->rx_ring)
			break;

		DWC_ETH_DMA_IOWRITE_BITS(channel, DMA_CH_RCR, RBSZ,
					 pdata->rx_buf_size);
	}
}

static void dwc_eth_config_tso_mode(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			break;

		if (pdata->hw_feat.tso)
			DWC_ETH_DMA_IOWRITE_BITS(channel, DMA_CH_TCR, TSE, 1);
	}
}

static void dwc_eth_config_sph_mode(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->rx_ring)
			break;

		DWC_ETH_DMA_IOWRITE_BITS(channel, DMA_CH_CR, SPH, 1);
	}

	DWC_ETH_IOWRITE_BITS(pdata, MAC_RCR, HDSMS, pdata->sph_hdsms_size);
}

static unsigned int dwc_eth_usec_to_riwt(struct dwc_eth_pdata *pdata,
					 unsigned int usec)
{
	unsigned long rate;
	unsigned int ret;

	TRACE("-->");

	rate = pdata->sysclk_rate;

	/* Convert the input usec value to the watchdog timer value. Each
	 * watchdog timer value is equivalent to 256 clock cycles.
	 * Calculate the required value as:
	 *   ( usec * ( system_clock_mhz / 10^6 ) / 256
	 */
	ret = (usec * (rate / 1000000)) / 256;

	TRACE("<--");

	return ret;
}

static unsigned int dwc_eth_riwt_to_usec(struct dwc_eth_pdata *pdata,
					 unsigned int riwt)
{
	unsigned long rate;
	unsigned int ret;

	TRACE("-->");

	rate = pdata->sysclk_rate;

	/* Convert the input watchdog timer value to the usec value. Each
	 * watchdog timer value is equivalent to 256 clock cycles.
	 * Calculate the required value as:
	 *   ( riwt * 256 ) / ( system_clock_mhz / 10^6 )
	 */
	ret = (riwt * 256) / (rate / 1000000);

	TRACE("<--");

	return ret;
}

static int dwc_eth_config_rx_threshold(struct dwc_eth_pdata *pdata,
				       unsigned int val)
{
	unsigned int i;

	for (i = 0; i < pdata->rx_q_count; i++)
		DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_Q_RQOMR, RTC, val);

	return 0;
}

static void dwc_eth_config_mtl_mode(struct dwc_eth_pdata *pdata)
{
	unsigned int i;

	/* Set Tx to weighted round robin scheduling algorithm */
	DWC_ETH_IOWRITE_BITS(pdata, MTL_OMR, ETSALG, MTL_ETSALG_WRR);

	/* Set Tx traffic classes to use WRR algorithm with equal weights */
	for (i = 0; i < pdata->hw_feat.tc_cnt; i++) {
		DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_TC_ETSCR, TSA,
					 MTL_TSA_ETS);
		DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_TC_QWR, QW, 1);
	}

	/* Set Rx to strict priority algorithm */
	DWC_ETH_IOWRITE_BITS(pdata, MTL_OMR, RAA, MTL_RAA_SP);
}

static void dwc_eth_config_queue_mapping(struct dwc_eth_pdata *pdata)
{
	unsigned int qptc, qptc_extra, queue;
	unsigned int prio_queues;
	unsigned int ppq, ppq_extra, prio;
	unsigned int mask;
	unsigned int i, j, reg, reg_val;

	/* Map the MTL Tx Queues to Traffic Classes
	 *   Note: Tx Queues >= Traffic Classes
	 */
	qptc = pdata->tx_q_count / pdata->hw_feat.tc_cnt;
	qptc_extra = pdata->tx_q_count % pdata->hw_feat.tc_cnt;

	for (i = 0, queue = 0; i < pdata->hw_feat.tc_cnt; i++) {
		for (j = 0; j < qptc; j++) {
			netif_dbg(pdata, drv, pdata->netdev,
				  "TXq%u mapped to TC%u\n", queue, i);
			DWC_ETH_MTL_IOWRITE_BITS(pdata, queue, MTL_Q_TQOMR,
						 Q2TCMAP, i);
			pdata->q2tc_map[queue++] = i;
		}

		if (i < qptc_extra) {
			netif_dbg(pdata, drv, pdata->netdev,
				  "TXq%u mapped to TC%u\n", queue, i);
			DWC_ETH_MTL_IOWRITE_BITS(pdata, queue, MTL_Q_TQOMR,
						 Q2TCMAP, i);
			pdata->q2tc_map[queue++] = i;
		}
	}

	/* Map the 8 VLAN priority values to available MTL Rx queues */
	prio_queues = min_t(unsigned int, IEEE_8021QAZ_MAX_TCS,
			    pdata->rx_q_count);
	ppq = IEEE_8021QAZ_MAX_TCS / prio_queues;
	ppq_extra = IEEE_8021QAZ_MAX_TCS % prio_queues;

	reg = MAC_RQC2R;
	reg_val = 0;
	for (i = 0, prio = 0; i < prio_queues;) {
		mask = 0;
		for (j = 0; j < ppq; j++) {
			netif_dbg(pdata, drv, pdata->netdev,
				  "PRIO%u mapped to RXq%u\n", prio, i);
			mask |= (1 << prio);
			pdata->prio2q_map[prio++] = i;
		}

		if (i < ppq_extra) {
			netif_dbg(pdata, drv, pdata->netdev,
				  "PRIO%u mapped to RXq%u\n", prio, i);
			mask |= (1 << prio);
			pdata->prio2q_map[prio++] = i;
		}

		reg_val |= (mask << ((i++ % MAC_RQC2_Q_PER_REG) << 3));

		if ((i % MAC_RQC2_Q_PER_REG) && (i != prio_queues))
			continue;

		DWC_ETH_IOWRITE(pdata, reg, reg_val);
		reg += MAC_RQC2_INC;
		reg_val = 0;
	}

	/* Select dynamic mapping of MTL Rx queue to DMA Rx channel */
	reg = MTL_RQDCM0R;
	reg_val = 0;
	for (i = 0; i < pdata->rx_q_count;) {
		reg_val |= (0x80 << ((i++ % MTL_RQDCM_Q_PER_REG) << 3));

		if ((i % MTL_RQDCM_Q_PER_REG) && (i != pdata->rx_q_count))
			continue;

		DWC_ETH_IOWRITE(pdata, reg, reg_val);

		reg += MTL_RQDCM_INC;
		reg_val = 0;
	}
}

static unsigned int dwc_eth_calculate_per_queue_fifo(
					unsigned int fifo_size,
					unsigned int queue_count)
{
	unsigned int q_fifo_size;
	unsigned int p_fifo;

	/* Calculate the configured fifo size */
	q_fifo_size = 1 << (fifo_size + 7);

	/* The configured value may not be the actual amount of fifo RAM */
	q_fifo_size = min_t(unsigned int, DWC_ETH_MAX_FIFO, q_fifo_size);

	q_fifo_size = q_fifo_size / queue_count;

	/* Each increment in the queue fifo size represents 256 bytes of
	 * fifo, with 0 representing 256 bytes. Distribute the fifo equally
	 * between the queues.
	 */
	p_fifo = q_fifo_size / 256;
	if (p_fifo)
		p_fifo--;

	return p_fifo;
}

static void dwc_eth_config_tx_fifo_size(struct dwc_eth_pdata *pdata)
{
	unsigned int fifo_size;
	unsigned int i;

	fifo_size = dwc_eth_calculate_per_queue_fifo(
				pdata->hw_feat.tx_fifo_size,
				pdata->tx_q_count);

	for (i = 0; i < pdata->tx_q_count; i++)
		DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_Q_TQOMR, TQS, fifo_size);

	netif_info(pdata, drv, pdata->netdev,
		   "%d Tx hardware queues, %d byte fifo per queue\n",
		   pdata->tx_q_count, ((fifo_size + 1) * 256));
}

static void dwc_eth_config_rx_fifo_size(struct dwc_eth_pdata *pdata)
{
	unsigned int fifo_size;
	unsigned int i;

	fifo_size = dwc_eth_calculate_per_queue_fifo(
					pdata->hw_feat.rx_fifo_size,
					pdata->rx_q_count);

	for (i = 0; i < pdata->rx_q_count; i++)
		DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_Q_RQOMR, RQS, fifo_size);

	netif_info(pdata, drv, pdata->netdev,
		   "%d Rx hardware queues, %d byte fifo per queue\n",
		   pdata->rx_q_count, ((fifo_size + 1) * 256));
}

static void dwc_eth_config_flow_control_threshold(struct dwc_eth_pdata *pdata)
{
	unsigned int i;

	for (i = 0; i < pdata->rx_q_count; i++) {
		/* Activate flow control when less than 4k left in fifo */
		DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_Q_RQFCR, RFA, 2);

		/* De-activate flow control when more than 6k left in fifo */
		DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_Q_RQFCR, RFD, 4);
	}
}

static int dwc_eth_config_tx_threshold(struct dwc_eth_pdata *pdata,
				       unsigned int val)
{
	unsigned int i;

	for (i = 0; i < pdata->tx_q_count; i++)
		DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_Q_TQOMR, TTC, val);

	return 0;
}

static int dwc_eth_config_rsf_mode(struct dwc_eth_pdata *pdata,
				   unsigned int val)
{
	unsigned int i;

	for (i = 0; i < pdata->rx_q_count; i++)
		DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_Q_RQOMR, RSF, val);

	return 0;
}

static int dwc_eth_config_tsf_mode(struct dwc_eth_pdata *pdata,
				   unsigned int val)
{
	unsigned int i;

	for (i = 0; i < pdata->tx_q_count; i++)
		DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_Q_TQOMR, TSF, val);

	return 0;
}

static int dwc_eth_config_osp_mode(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			break;

		DWC_ETH_DMA_IOWRITE_BITS(channel, DMA_CH_TCR, OSP,
					 pdata->tx_osp_mode);
	}

	return 0;
}

static int dwc_eth_config_pblx8(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++)
		DWC_ETH_DMA_IOWRITE_BITS(channel, DMA_CH_CR, PBLX8,
					 pdata->pblx8);

	return 0;
}

static int dwc_eth_get_tx_pbl_val(struct dwc_eth_pdata *pdata)
{
	return DWC_ETH_DMA_IOREAD_BITS(pdata->channel_head, DMA_CH_TCR, PBL);
}

static int dwc_eth_config_tx_pbl_val(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->tx_ring)
			break;

		DWC_ETH_DMA_IOWRITE_BITS(channel, DMA_CH_TCR, PBL,
					 pdata->tx_pbl);
	}

	return 0;
}

static int dwc_eth_get_rx_pbl_val(struct dwc_eth_pdata *pdata)
{
	return DWC_ETH_DMA_IOREAD_BITS(pdata->channel_head, DMA_CH_RCR, PBL);
}

static int dwc_eth_config_rx_pbl_val(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!channel->rx_ring)
			break;

		DWC_ETH_DMA_IOWRITE_BITS(channel, DMA_CH_RCR, PBL,
					 pdata->rx_pbl);
	}

	return 0;
}

static u64 dwc_eth_mmc_read(struct dwc_eth_pdata *pdata, unsigned int reg_lo)
{
	bool read_hi;
	u64 val;

	switch (reg_lo) {
	/* These registers are always 64 bit */
	case MMC_TXOCTETCOUNT_GB_LO:
	case MMC_TXOCTETCOUNT_G_LO:
	case MMC_RXOCTETCOUNT_GB_LO:
	case MMC_RXOCTETCOUNT_G_LO:
		read_hi = true;
		break;

	default:
		read_hi = false;
	}

	val = DWC_ETH_IOREAD(pdata, reg_lo);

	if (read_hi)
		val |= ((u64)DWC_ETH_IOREAD(pdata, reg_lo + 4) << 32);

	return val;
}

static void dwc_eth_tx_mmc_int(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_stats *stats = &pdata->stats;
	unsigned int mmc_isr = DWC_ETH_IOREAD(pdata, MMC_TISR);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_TISR, TXOCTETCOUNT_GB))
		stats->txoctetcount_gb +=
			dwc_eth_mmc_read(pdata, MMC_TXOCTETCOUNT_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_TISR, TXFRAMECOUNT_GB))
		stats->txframecount_gb +=
			dwc_eth_mmc_read(pdata, MMC_TXFRAMECOUNT_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_TISR, TXBROADCASTFRAMES_G))
		stats->txbroadcastframes_g +=
			dwc_eth_mmc_read(pdata, MMC_TXBROADCASTFRAMES_G_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_TISR, TXMULTICASTFRAMES_G))
		stats->txmulticastframes_g +=
			dwc_eth_mmc_read(pdata, MMC_TXMULTICASTFRAMES_G_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_TISR, TX64OCTETS_GB))
		stats->tx64octets_gb +=
			dwc_eth_mmc_read(pdata, MMC_TX64OCTETS_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_TISR, TX65TO127OCTETS_GB))
		stats->tx65to127octets_gb +=
			dwc_eth_mmc_read(pdata, MMC_TX65TO127OCTETS_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_TISR, TX128TO255OCTETS_GB))
		stats->tx128to255octets_gb +=
			dwc_eth_mmc_read(pdata, MMC_TX128TO255OCTETS_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_TISR, TX256TO511OCTETS_GB))
		stats->tx256to511octets_gb +=
			dwc_eth_mmc_read(pdata, MMC_TX256TO511OCTETS_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_TISR, TX512TO1023OCTETS_GB))
		stats->tx512to1023octets_gb +=
			dwc_eth_mmc_read(pdata, MMC_TX512TO1023OCTETS_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_TISR, TX1024TOMAXOCTETS_GB))
		stats->tx1024tomaxoctets_gb +=
			dwc_eth_mmc_read(pdata, MMC_TX1024TOMAXOCTETS_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_TISR, TXUNICASTFRAMES_GB))
		stats->txunicastframes_gb +=
			dwc_eth_mmc_read(pdata, MMC_TXUNICASTFRAMES_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_TISR, TXMULTICASTFRAMES_GB))
		stats->txmulticastframes_gb +=
			dwc_eth_mmc_read(pdata, MMC_TXMULTICASTFRAMES_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_TISR, TXBROADCASTFRAMES_GB))
		stats->txbroadcastframes_g +=
			dwc_eth_mmc_read(pdata, MMC_TXBROADCASTFRAMES_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_TISR, TXUNDERFLOWERROR))
		stats->txunderflowerror +=
			dwc_eth_mmc_read(pdata, MMC_TXUNDERFLOWERROR_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_TISR, TXOCTETCOUNT_G))
		stats->txoctetcount_g +=
			dwc_eth_mmc_read(pdata, MMC_TXOCTETCOUNT_G_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_TISR, TXFRAMECOUNT_G))
		stats->txframecount_g +=
			dwc_eth_mmc_read(pdata, MMC_TXFRAMECOUNT_G_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_TISR, TXPAUSEFRAMES))
		stats->txpauseframes +=
			dwc_eth_mmc_read(pdata, MMC_TXPAUSEFRAMES_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_TISR, TXVLANFRAMES_G))
		stats->txvlanframes_g +=
			dwc_eth_mmc_read(pdata, MMC_TXVLANFRAMES_G_LO);
}

static void dwc_eth_rx_mmc_int(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_stats *stats = &pdata->stats;
	unsigned int mmc_isr = DWC_ETH_IOREAD(pdata, MMC_RISR);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RXFRAMECOUNT_GB))
		stats->rxframecount_gb +=
			dwc_eth_mmc_read(pdata, MMC_RXFRAMECOUNT_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RXOCTETCOUNT_GB))
		stats->rxoctetcount_gb +=
			dwc_eth_mmc_read(pdata, MMC_RXOCTETCOUNT_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RXOCTETCOUNT_G))
		stats->rxoctetcount_g +=
			dwc_eth_mmc_read(pdata, MMC_RXOCTETCOUNT_G_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RXBROADCASTFRAMES_G))
		stats->rxbroadcastframes_g +=
			dwc_eth_mmc_read(pdata, MMC_RXBROADCASTFRAMES_G_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RXMULTICASTFRAMES_G))
		stats->rxmulticastframes_g +=
			dwc_eth_mmc_read(pdata, MMC_RXMULTICASTFRAMES_G_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RXCRCERROR))
		stats->rxcrcerror +=
			dwc_eth_mmc_read(pdata, MMC_RXCRCERROR_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RXRUNTERROR))
		stats->rxrunterror +=
			dwc_eth_mmc_read(pdata, MMC_RXRUNTERROR);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RXJABBERERROR))
		stats->rxjabbererror +=
			dwc_eth_mmc_read(pdata, MMC_RXJABBERERROR);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RXUNDERSIZE_G))
		stats->rxundersize_g +=
			dwc_eth_mmc_read(pdata, MMC_RXUNDERSIZE_G);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RXOVERSIZE_G))
		stats->rxoversize_g +=
			dwc_eth_mmc_read(pdata, MMC_RXOVERSIZE_G);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RX64OCTETS_GB))
		stats->rx64octets_gb +=
			dwc_eth_mmc_read(pdata, MMC_RX64OCTETS_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RX65TO127OCTETS_GB))
		stats->rx65to127octets_gb +=
			dwc_eth_mmc_read(pdata, MMC_RX65TO127OCTETS_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RX128TO255OCTETS_GB))
		stats->rx128to255octets_gb +=
			dwc_eth_mmc_read(pdata, MMC_RX128TO255OCTETS_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RX256TO511OCTETS_GB))
		stats->rx256to511octets_gb +=
			dwc_eth_mmc_read(pdata, MMC_RX256TO511OCTETS_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RX512TO1023OCTETS_GB))
		stats->rx512to1023octets_gb +=
			dwc_eth_mmc_read(pdata, MMC_RX512TO1023OCTETS_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RX1024TOMAXOCTETS_GB))
		stats->rx1024tomaxoctets_gb +=
			dwc_eth_mmc_read(pdata, MMC_RX1024TOMAXOCTETS_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RXUNICASTFRAMES_G))
		stats->rxunicastframes_g +=
			dwc_eth_mmc_read(pdata, MMC_RXUNICASTFRAMES_G_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RXLENGTHERROR))
		stats->rxlengtherror +=
			dwc_eth_mmc_read(pdata, MMC_RXLENGTHERROR_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RXOUTOFRANGETYPE))
		stats->rxoutofrangetype +=
			dwc_eth_mmc_read(pdata, MMC_RXOUTOFRANGETYPE_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RXPAUSEFRAMES))
		stats->rxpauseframes +=
			dwc_eth_mmc_read(pdata, MMC_RXPAUSEFRAMES_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RXFIFOOVERFLOW))
		stats->rxfifooverflow +=
			dwc_eth_mmc_read(pdata, MMC_RXFIFOOVERFLOW_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RXVLANFRAMES_GB))
		stats->rxvlanframes_gb +=
			dwc_eth_mmc_read(pdata, MMC_RXVLANFRAMES_GB_LO);

	if (DWC_ETH_GET_BITS(mmc_isr, MMC_RISR, RXWATCHDOGERROR))
		stats->rxwatchdogerror +=
			dwc_eth_mmc_read(pdata, MMC_RXWATCHDOGERROR);
}

static void dwc_eth_read_mmc_stats(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_stats *stats = &pdata->stats;

	/* Freeze counters */
	DWC_ETH_IOWRITE_BITS(pdata, MMC_CR, MCF, 1);

	stats->txoctetcount_gb +=
		dwc_eth_mmc_read(pdata, MMC_TXOCTETCOUNT_GB_LO);

	stats->txframecount_gb +=
		dwc_eth_mmc_read(pdata, MMC_TXFRAMECOUNT_GB_LO);

	stats->txbroadcastframes_g +=
		dwc_eth_mmc_read(pdata, MMC_TXBROADCASTFRAMES_G_LO);

	stats->txmulticastframes_g +=
		dwc_eth_mmc_read(pdata, MMC_TXMULTICASTFRAMES_G_LO);

	stats->tx64octets_gb +=
		dwc_eth_mmc_read(pdata, MMC_TX64OCTETS_GB_LO);

	stats->tx65to127octets_gb +=
		dwc_eth_mmc_read(pdata, MMC_TX65TO127OCTETS_GB_LO);

	stats->tx128to255octets_gb +=
		dwc_eth_mmc_read(pdata, MMC_TX128TO255OCTETS_GB_LO);

	stats->tx256to511octets_gb +=
		dwc_eth_mmc_read(pdata, MMC_TX256TO511OCTETS_GB_LO);

	stats->tx512to1023octets_gb +=
		dwc_eth_mmc_read(pdata, MMC_TX512TO1023OCTETS_GB_LO);

	stats->tx1024tomaxoctets_gb +=
		dwc_eth_mmc_read(pdata, MMC_TX1024TOMAXOCTETS_GB_LO);

	stats->txunicastframes_gb +=
		dwc_eth_mmc_read(pdata, MMC_TXUNICASTFRAMES_GB_LO);

	stats->txmulticastframes_gb +=
		dwc_eth_mmc_read(pdata, MMC_TXMULTICASTFRAMES_GB_LO);

	stats->txbroadcastframes_g +=
		dwc_eth_mmc_read(pdata, MMC_TXBROADCASTFRAMES_GB_LO);

	stats->txunderflowerror +=
		dwc_eth_mmc_read(pdata, MMC_TXUNDERFLOWERROR_LO);

	stats->txoctetcount_g +=
		dwc_eth_mmc_read(pdata, MMC_TXOCTETCOUNT_G_LO);

	stats->txframecount_g +=
		dwc_eth_mmc_read(pdata, MMC_TXFRAMECOUNT_G_LO);

	stats->txpauseframes +=
		dwc_eth_mmc_read(pdata, MMC_TXPAUSEFRAMES_LO);

	stats->txvlanframes_g +=
		dwc_eth_mmc_read(pdata, MMC_TXVLANFRAMES_G_LO);

	stats->rxframecount_gb +=
		dwc_eth_mmc_read(pdata, MMC_RXFRAMECOUNT_GB_LO);

	stats->rxoctetcount_gb +=
		dwc_eth_mmc_read(pdata, MMC_RXOCTETCOUNT_GB_LO);

	stats->rxoctetcount_g +=
		dwc_eth_mmc_read(pdata, MMC_RXOCTETCOUNT_G_LO);

	stats->rxbroadcastframes_g +=
		dwc_eth_mmc_read(pdata, MMC_RXBROADCASTFRAMES_G_LO);

	stats->rxmulticastframes_g +=
		dwc_eth_mmc_read(pdata, MMC_RXMULTICASTFRAMES_G_LO);

	stats->rxcrcerror +=
		dwc_eth_mmc_read(pdata, MMC_RXCRCERROR_LO);

	stats->rxrunterror +=
		dwc_eth_mmc_read(pdata, MMC_RXRUNTERROR);

	stats->rxjabbererror +=
		dwc_eth_mmc_read(pdata, MMC_RXJABBERERROR);

	stats->rxundersize_g +=
		dwc_eth_mmc_read(pdata, MMC_RXUNDERSIZE_G);

	stats->rxoversize_g +=
		dwc_eth_mmc_read(pdata, MMC_RXOVERSIZE_G);

	stats->rx64octets_gb +=
		dwc_eth_mmc_read(pdata, MMC_RX64OCTETS_GB_LO);

	stats->rx65to127octets_gb +=
		dwc_eth_mmc_read(pdata, MMC_RX65TO127OCTETS_GB_LO);

	stats->rx128to255octets_gb +=
		dwc_eth_mmc_read(pdata, MMC_RX128TO255OCTETS_GB_LO);

	stats->rx256to511octets_gb +=
		dwc_eth_mmc_read(pdata, MMC_RX256TO511OCTETS_GB_LO);

	stats->rx512to1023octets_gb +=
		dwc_eth_mmc_read(pdata, MMC_RX512TO1023OCTETS_GB_LO);

	stats->rx1024tomaxoctets_gb +=
		dwc_eth_mmc_read(pdata, MMC_RX1024TOMAXOCTETS_GB_LO);

	stats->rxunicastframes_g +=
		dwc_eth_mmc_read(pdata, MMC_RXUNICASTFRAMES_G_LO);

	stats->rxlengtherror +=
		dwc_eth_mmc_read(pdata, MMC_RXLENGTHERROR_LO);

	stats->rxoutofrangetype +=
		dwc_eth_mmc_read(pdata, MMC_RXOUTOFRANGETYPE_LO);

	stats->rxpauseframes +=
		dwc_eth_mmc_read(pdata, MMC_RXPAUSEFRAMES_LO);

	stats->rxfifooverflow +=
		dwc_eth_mmc_read(pdata, MMC_RXFIFOOVERFLOW_LO);

	stats->rxvlanframes_gb +=
		dwc_eth_mmc_read(pdata, MMC_RXVLANFRAMES_GB_LO);

	stats->rxwatchdogerror +=
		dwc_eth_mmc_read(pdata, MMC_RXWATCHDOGERROR);

	/* Un-freeze counters */
	DWC_ETH_IOWRITE_BITS(pdata, MMC_CR, MCF, 0);
}

static void dwc_eth_config_mmc(struct dwc_eth_pdata *pdata)
{
	/* Set counters to reset on read */
	DWC_ETH_IOWRITE_BITS(pdata, MMC_CR, ROR, 1);

	/* Reset the counters */
	DWC_ETH_IOWRITE_BITS(pdata, MMC_CR, CR, 1);
}

static int dwc_eth_write_rss_reg(struct dwc_eth_pdata *pdata, unsigned int type,
				 unsigned int index, unsigned int val)
{
	unsigned int wait;
	int ret = 0;

	mutex_lock(&pdata->rss_mutex);

	if (DWC_ETH_IOREAD_BITS(pdata, MAC_RSSAR, OB)) {
		ret = -EBUSY;
		goto unlock;
	}

	DWC_ETH_IOWRITE(pdata, MAC_RSSDR, val);

	DWC_ETH_IOWRITE_BITS(pdata, MAC_RSSAR, RSSIA, index);
	DWC_ETH_IOWRITE_BITS(pdata, MAC_RSSAR, ADDRT, type);
	DWC_ETH_IOWRITE_BITS(pdata, MAC_RSSAR, CT, 0);
	DWC_ETH_IOWRITE_BITS(pdata, MAC_RSSAR, OB, 1);

	wait = 1000;
	while (wait--) {
		if (!DWC_ETH_IOREAD_BITS(pdata, MAC_RSSAR, OB))
			goto unlock;

		usleep_range(1000, 1500);
	}

	ret = -EBUSY;

unlock:
	mutex_unlock(&pdata->rss_mutex);

	return ret;
}

static int dwc_eth_write_rss_hash_key(struct dwc_eth_pdata *pdata)
{
	unsigned int key_regs = sizeof(pdata->rss_key) / sizeof(u32);
	unsigned int *key = (unsigned int *)&pdata->rss_key;
	int ret;

	while (key_regs--) {
		ret = dwc_eth_write_rss_reg(pdata, DWC_ETH_RSS_HASH_KEY_TYPE,
					    key_regs, *key++);
		if (ret)
			return ret;
	}

	return 0;
}

static int dwc_eth_write_rss_lookup_table(struct dwc_eth_pdata *pdata)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(pdata->rss_table); i++) {
		ret = dwc_eth_write_rss_reg(pdata,
					    DWC_ETH_RSS_LOOKUP_TABLE_TYPE, i,
					 pdata->rss_table[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int dwc_eth_set_rss_hash_key(struct dwc_eth_pdata *pdata, const u8 *key)
{
	memcpy(pdata->rss_key, key, sizeof(pdata->rss_key));

	return dwc_eth_write_rss_hash_key(pdata);
}

static int dwc_eth_set_rss_lookup_table(struct dwc_eth_pdata *pdata,
					const u32 *table)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pdata->rss_table); i++)
		DWC_ETH_SET_BITS(pdata->rss_table[i], MAC_RSSDR,
				 DMCH, table[i]);

	return dwc_eth_write_rss_lookup_table(pdata);
}

static int dwc_eth_enable_rss(struct dwc_eth_pdata *pdata)
{
	int ret;

	if (!pdata->hw_feat.rss)
		return -EOPNOTSUPP;

	/* Program the hash key */
	ret = dwc_eth_write_rss_hash_key(pdata);
	if (ret)
		return ret;

	/* Program the lookup table */
	ret = dwc_eth_write_rss_lookup_table(pdata);
	if (ret)
		return ret;

	/* Set the RSS options */
	DWC_ETH_IOWRITE(pdata, MAC_RSSCR, pdata->rss_options);

	/* Enable RSS */
	DWC_ETH_IOWRITE_BITS(pdata, MAC_RSSCR, RSSE, 1);

	return 0;
}

static int dwc_eth_disable_rss(struct dwc_eth_pdata *pdata)
{
	if (!pdata->hw_feat.rss)
		return -EOPNOTSUPP;

	DWC_ETH_IOWRITE_BITS(pdata, MAC_RSSCR, RSSE, 0);

	return 0;
}

static void dwc_eth_config_rss(struct dwc_eth_pdata *pdata)
{
	int ret;

	if (!pdata->hw_feat.rss)
		return;

	if (pdata->netdev->features & NETIF_F_RXHASH)
		ret = dwc_eth_enable_rss(pdata);
	else
		ret = dwc_eth_disable_rss(pdata);

	if (ret)
		netdev_err(pdata->netdev,
			   "error configuring RSS, RSS disabled\n");
}

static void dwc_eth_enable_dma_interrupts(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int dma_ch_isr, dma_ch_ier;
	unsigned int i;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		/* Clear all the interrupts which are set */
		dma_ch_isr = DWC_ETH_DMA_IOREAD(channel, DMA_CH_SR);
		DWC_ETH_DMA_IOWRITE(channel, DMA_CH_SR, dma_ch_isr);

		/* Clear all interrupt enable bits */
		dma_ch_ier = 0;

		/* Enable following interrupts
		 *   NIE  - Normal Interrupt Summary Enable
		 *   AIE  - Abnormal Interrupt Summary Enable
		 *   FBEE - Fatal Bus Error Enable
		 */
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, NIE, 1);
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, AIE, 1);
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, FBEE, 1);

		if (channel->tx_ring) {
			/* Enable the following Tx interrupts
			 *   TIE  - Transmit Interrupt Enable (unless using
			 *          per channel interrupts)
			 */
			if (!pdata->per_channel_irq)
				DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER,
						 TIE, 1);
		}
		if (channel->rx_ring) {
			/* Enable following Rx interrupts
			 *   RBUE - Receive Buffer Unavailable Enable
			 *   RIE  - Receive Interrupt Enable (unless using
			 *          per channel interrupts)
			 */
			DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, RBUE, 1);
			if (!pdata->per_channel_irq)
				DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER,
						 RIE, 1);
		}

		DWC_ETH_DMA_IOWRITE(channel, DMA_CH_IER, dma_ch_ier);
	}
}

static void dwc_eth_enable_mtl_interrupts(struct dwc_eth_pdata *pdata)
{
	unsigned int mtl_q_isr;
	unsigned int q_count, i;

	q_count = max(pdata->hw_feat.tx_q_cnt, pdata->hw_feat.rx_q_cnt);
	for (i = 0; i < q_count; i++) {
		/* Clear all the interrupts which are set */
		mtl_q_isr = DWC_ETH_MTL_IOREAD(pdata, i, MTL_Q_ISR);
		DWC_ETH_MTL_IOWRITE(pdata, i, MTL_Q_ISR, mtl_q_isr);

		/* No MTL interrupts to be enabled */
		DWC_ETH_MTL_IOWRITE(pdata, i, MTL_Q_IER, 0);
	}
}

static void dwc_eth_enable_mac_interrupts(struct dwc_eth_pdata *pdata)
{
	unsigned int mac_ier = 0;

	/* Enable Timestamp interrupt */
	DWC_ETH_SET_BITS(mac_ier, MAC_IER, TSIE, 1);

	DWC_ETH_IOWRITE(pdata, MAC_IER, mac_ier);

	/* Enable all counter interrupts */
	DWC_ETH_IOWRITE_BITS(pdata, MMC_RIER, ALL_INTERRUPTS, 0xffffffff);
	DWC_ETH_IOWRITE_BITS(pdata, MMC_TIER, ALL_INTERRUPTS, 0xffffffff);
}

static int dwc_eth_set_gmii_1000_speed(struct dwc_eth_pdata *pdata)
{
	if (pdata->hw2_ops->set_gmii_1000_speed)
		return pdata->hw2_ops->set_gmii_1000_speed(pdata);

	TRACE("-->");

	if (DWC_ETH_IOREAD_BITS(pdata, MAC_TCR, SS) == 0x7)
		return 0;

	DWC_ETH_IOWRITE_BITS(pdata, MAC_TCR, SS, 0x7);

	TRACE("<--");

	return 0;
}

static int dwc_eth_set_gmii_2500_speed(struct dwc_eth_pdata *pdata)
{
	if (pdata->hw2_ops->set_gmii_2500_speed)
		return pdata->hw2_ops->set_gmii_2500_speed(pdata);

	TRACE("-->");

	if (DWC_ETH_IOREAD_BITS(pdata, MAC_TCR, SS) == 0x6)
		return 0;

	DWC_ETH_IOWRITE_BITS(pdata, MAC_TCR, SS, 0x6);

	TRACE("<--");

	return 0;
}

static int dwc_eth_set_xgmii_10000_speed(struct dwc_eth_pdata *pdata)
{
	if (pdata->hw2_ops->set_xgmii_10000_speed)
		return pdata->hw2_ops->set_xgmii_10000_speed(pdata);

	TRACE("-->");

	if (DWC_ETH_IOREAD_BITS(pdata, MAC_TCR, SS) == 0x4)
		return 0;

	DWC_ETH_IOWRITE_BITS(pdata, MAC_TCR, SS, 0x4);

	TRACE("<--");

	return 0;
}

static int dwc_eth_set_xlgmii_25000_speed(struct dwc_eth_pdata *pdata)
{
	if (DWC_ETH_IOREAD_BITS(pdata, MAC_TCR, SS) == 0x1)
		return 0;

	DWC_ETH_IOWRITE_BITS(pdata, MAC_TCR, SS, 0x1);

	return 0;
}

static int dwc_eth_set_xlgmii_40000_speed(struct dwc_eth_pdata *pdata)
{
	if (DWC_ETH_IOREAD_BITS(pdata, MAC_TCR, SS) == 0)
		return 0;

	DWC_ETH_IOWRITE_BITS(pdata, MAC_TCR, SS, 0);

	return 0;
}

static int dwc_eth_set_xlgmii_50000_speed(struct dwc_eth_pdata *pdata)
{
	if (DWC_ETH_IOREAD_BITS(pdata, MAC_TCR, SS) == 0x2)
		return 0;

	DWC_ETH_IOWRITE_BITS(pdata, MAC_TCR, SS, 0x2);

	return 0;
}

static int dwc_eth_set_xlgmii_100000_speed(struct dwc_eth_pdata *pdata)
{
	if (DWC_ETH_IOREAD_BITS(pdata, MAC_TCR, SS) == 0x3)
		return 0;

	DWC_ETH_IOWRITE_BITS(pdata, MAC_TCR, SS, 0x3);

	return 0;
}

static void dwc_eth_config_mac_speed(struct dwc_eth_pdata *pdata)
{
	TRACE("-->");

	switch (pdata->phy_speed) {
	case SPEED_100000:
		dwc_eth_set_xlgmii_100000_speed(pdata);
		break;

	case SPEED_50000:
		dwc_eth_set_xlgmii_50000_speed(pdata);
		break;

	case SPEED_40000:
		dwc_eth_set_xlgmii_40000_speed(pdata);
		break;

	case SPEED_25000:
		dwc_eth_set_xlgmii_25000_speed(pdata);
		break;

	case SPEED_10000:
		dwc_eth_set_xgmii_10000_speed(pdata);
		break;

	case SPEED_2500:
		dwc_eth_set_gmii_2500_speed(pdata);
		break;

	case SPEED_1000:
		dwc_eth_set_gmii_1000_speed(pdata);
		break;
	}

	TRACE("<--");
}

static int dwc_eth_mdio_wait_until_free(struct dwc_eth_pdata *pdata)
{
	unsigned int timeout;

	/* Wait till the bus is free */
	timeout = DWC_ETH_MDIO_RD_TIMEOUT;
	while (DWC_ETH_IOREAD_BITS(pdata, MAC_MDIOSCCDR, BUSY) && timeout) {
		cpu_relax();
		timeout--;
	}

	if (!timeout) {
		dev_err(pdata->dev, "timeout waiting for bus to be free\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static int dwc_eth_read_mmd_regs(struct dwc_eth_pdata *pdata,
				 int prtad, int mmd_reg)
{
	int mmd_data;
	int ret;
	unsigned int scar;
	unsigned int sccdr = 0;

	if (pdata->hw2_ops->read_mmd_regs)
		return pdata->hw2_ops->read_mmd_regs(pdata,
						prtad, mmd_reg);

	TRACE("-->");

	mutex_lock(&pdata->pcs_mutex);

	ret = dwc_eth_mdio_wait_until_free(pdata);
	if (ret)
		return ret;

	/* Updating desired bits for read operation */
	scar = DWC_ETH_IOREAD(pdata, MAC_MDIOSCAR);
	scar = scar & (0x3e00000UL);
	scar = scar | ((prtad) << MAC_MDIOSCAR_PA_POS) |
		((mmd_reg) << MAC_MDIOSCAR_RA_POS);
	DWC_ETH_IOWRITE(pdata, MAC_MDIOSCAR, scar);

	/* Initiate the read */
	sccdr = sccdr | ((0x1) << MAC_MDIOSCCDR_BUSY_POS) |
		((0x5) << MAC_MDIOSCCDR_CR_POS) |
		((0x1) << MAC_MDIOSCCDR_SADDR_POS) |
		((0x3) << MAC_MDIOSCCDR_CMD_POS);
	DWC_ETH_IOWRITE(pdata, MAC_MDIOSCCDR, sccdr);

	ret = dwc_eth_mdio_wait_until_free(pdata);
	if (ret)
		return ret;

	/* Read the data */
	mmd_data = DWC_ETH_IOREAD_BITS(pdata, MAC_MDIOSCCDR, SDATA);

	mutex_unlock(&pdata->pcs_mutex);

	TRACE("<--");

	return mmd_data;
}

static int dwc_eth_write_mmd_regs(struct dwc_eth_pdata *pdata,
				  int prtad, int mmd_reg, int mmd_data)
{
	int ret;
	unsigned int scar;
	unsigned int sccdr = 0;

	if (pdata->hw2_ops->write_mmd_regs)
		return pdata->hw2_ops->write_mmd_regs(pdata,
						      prtad,
						      mmd_reg,
						      mmd_data);

	TRACE("-->");

	mutex_lock(&pdata->pcs_mutex);

	ret = dwc_eth_mdio_wait_until_free(pdata);
	if (ret)
		return ret;

	/* Updating desired bits for write operation */
	scar = DWC_ETH_IOREAD(pdata, MAC_MDIOSCAR);
	scar = scar & (0x3e00000UL);
	scar = scar | ((prtad) << MAC_MDIOSCAR_PA_POS) |
		((mmd_reg) << MAC_MDIOSCAR_RA_POS);
	DWC_ETH_IOWRITE(pdata, MAC_MDIOSCAR, scar);

	/* Initiate Write */
	sccdr = sccdr | ((0x1) << MAC_MDIOSCCDR_BUSY_POS) |
		((0x5) << MAC_MDIOSCCDR_CR_POS) |
		((0x1) << MAC_MDIOSCCDR_SADDR_POS) |
		((0x1) << MAC_MDIOSCCDR_CMD_POS) |
		((mmd_data) << MAC_MDIOSCCDR_SDATA_POS);
	DWC_ETH_IOWRITE(pdata, MAC_MDIOSCCDR, sccdr);

	ret = dwc_eth_mdio_wait_until_free(pdata);
	if (ret)
		return ret;

	mutex_unlock(&pdata->pcs_mutex);

	TRACE("<--");

	return 0;
}

static int dwc_eth_dev_read(struct dwc_eth_channel *channel)
{
	struct dwc_eth_pdata *pdata = channel->pdata;
	struct dwc_eth_ring *ring = channel->rx_ring;
	struct dwc_eth_desc_data *desc_data;
	struct dwc_eth_dma_desc *dma_desc;
	struct dwc_eth_pkt_info *pkt_info = &ring->pkt_info;
	struct net_device *netdev = pdata->netdev;
	unsigned int err, etlt, l34t;

	TRACE("-->");
	DBGPR("  cur = %d\n", ring->cur);

	desc_data = DWC_ETH_GET_DESC_DATA(ring, ring->cur);
	dma_desc = desc_data->dma_desc;

	/* Check for data availability */
	if (DWC_ETH_GET_BITS_LE(dma_desc->desc3, RX_NORMAL_DESC3, OWN))
		return 1;

	/* Make sure descriptor fields are read after reading the OWN bit */
	dma_rmb();

	if (netif_msg_rx_status(pdata))
		dwc_eth_dump_rx_desc(pdata, ring, ring->cur);

	if (DWC_ETH_GET_BITS_LE(dma_desc->desc3, RX_NORMAL_DESC3, CTXT)) {
		/* Timestamp Context Descriptor */
		dwc_eth_get_rx_tstamp(pkt_info, dma_desc);

		DWC_ETH_SET_BITS(pkt_info->attributes, RX_PACKET_ATTRIBUTES,
				 CONTEXT, 1);
		DWC_ETH_SET_BITS(pkt_info->attributes, RX_PACKET_ATTRIBUTES,
				 CONTEXT_NEXT, 0);
		return 0;
	}

	/* Normal Descriptor, be sure Context Descriptor bit is off */
	DWC_ETH_SET_BITS(pkt_info->attributes, RX_PACKET_ATTRIBUTES,
			 CONTEXT, 0);

	/* Indicate if a Context Descriptor is next */
	if (DWC_ETH_GET_BITS_LE(dma_desc->desc3, RX_NORMAL_DESC3, CDA))
		DWC_ETH_SET_BITS(pkt_info->attributes, RX_PACKET_ATTRIBUTES,
				 CONTEXT_NEXT, 1);

	/* Get the header length */
	if (DWC_ETH_GET_BITS_LE(dma_desc->desc3, RX_NORMAL_DESC3, FD)) {
		desc_data->rx.hdr_len = DWC_ETH_GET_BITS_LE(dma_desc->desc2,
							RX_NORMAL_DESC2, HL);
		if (desc_data->rx.hdr_len)
			pdata->stats.rx_split_header_packets++;
	}

	/* Get the RSS hash */
	if (DWC_ETH_GET_BITS_LE(dma_desc->desc3, RX_NORMAL_DESC3, RSV)) {
		DWC_ETH_SET_BITS(pkt_info->attributes, RX_PACKET_ATTRIBUTES,
				 RSS_HASH, 1);

		pkt_info->rss_hash = le32_to_cpu(dma_desc->desc1);

		l34t = DWC_ETH_GET_BITS_LE(dma_desc->desc3,
					   RX_NORMAL_DESC3, L34T);
		switch (l34t) {
		case RX_DESC3_L34T_IPV4_TCP:
		case RX_DESC3_L34T_IPV4_UDP:
		case RX_DESC3_L34T_IPV6_TCP:
		case RX_DESC3_L34T_IPV6_UDP:
			pkt_info->rss_hash_type = PKT_HASH_TYPE_L4;
			break;
		default:
			pkt_info->rss_hash_type = PKT_HASH_TYPE_L3;
		}
	}

	/* Get the pkt_info length */
	desc_data->rx.len = DWC_ETH_GET_BITS_LE(dma_desc->desc3,
					RX_NORMAL_DESC3, PL);

	if (!DWC_ETH_GET_BITS_LE(dma_desc->desc3, RX_NORMAL_DESC3, LD)) {
		/* Not all the data has been transferred for this pkt_info */
		DWC_ETH_SET_BITS(pkt_info->attributes, RX_PACKET_ATTRIBUTES,
				 INCOMPLETE, 1);
		return 0;
	}

	/* This is the last of the data for this pkt_info */
	DWC_ETH_SET_BITS(pkt_info->attributes, RX_PACKET_ATTRIBUTES,
			 INCOMPLETE, 0);

	/* Set checksum done indicator as appropriate */
	if (netdev->features & NETIF_F_RXCSUM)
		DWC_ETH_SET_BITS(pkt_info->attributes, RX_PACKET_ATTRIBUTES,
				 CSUM_DONE, 1);

	/* Check for errors (only valid in last descriptor) */
	err = DWC_ETH_GET_BITS_LE(dma_desc->desc3, RX_NORMAL_DESC3, ES);
	etlt = DWC_ETH_GET_BITS_LE(dma_desc->desc3, RX_NORMAL_DESC3, ETLT);
	netif_dbg(pdata, rx_status, netdev, "err=%u, etlt=%#x\n", err, etlt);

	if (!err || !etlt) {
		/* No error if err is 0 or etlt is 0 */
		if ((etlt == 0x09) &&
		    (netdev->features & NETIF_F_HW_VLAN_CTAG_RX)) {
			DWC_ETH_SET_BITS(pkt_info->attributes,
					 RX_PACKET_ATTRIBUTES,
					VLAN_CTAG, 1);
			pkt_info->vlan_ctag =
				DWC_ETH_GET_BITS_LE(dma_desc->desc0,
						    RX_NORMAL_DESC0, OVT);
			netif_dbg(pdata, rx_status, netdev, "vlan-ctag=%#06x\n",
				  pkt_info->vlan_ctag);
		}
	} else {
		if ((etlt == 0x05) || (etlt == 0x06))
			DWC_ETH_SET_BITS(pkt_info->attributes,
					 RX_PACKET_ATTRIBUTES,
					CSUM_DONE, 0);
		else
			DWC_ETH_SET_BITS(pkt_info->errors, RX_PACKET_ERRORS,
					 FRAME, 1);
	}

	DBGPR("  %s - descriptor=%u (cur=%d)\n", channel->name,
	      ring->cur & (ring->dma_desc_count - 1), ring->cur);
	TRACE("<--");

	return 0;
}

static int dwc_eth_enable_int(struct dwc_eth_channel *channel,
			      enum dwc_eth_int int_id)
{
	unsigned int dma_ch_ier;

	dma_ch_ier = DWC_ETH_DMA_IOREAD(channel, DMA_CH_IER);

	switch (int_id) {
	case DWC_ETH_INT_DMA_CH_SR_TI:
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, TIE, 1);
		break;
	case DWC_ETH_INT_DMA_CH_SR_TPS:
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, TXSE, 1);
		break;
	case DWC_ETH_INT_DMA_CH_SR_TBU:
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, TBUE, 1);
		break;
	case DWC_ETH_INT_DMA_CH_SR_RI:
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, RIE, 1);
		break;
	case DWC_ETH_INT_DMA_CH_SR_RBU:
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, RBUE, 1);
		break;
	case DWC_ETH_INT_DMA_CH_SR_RPS:
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, RSE, 1);
		break;
	case DWC_ETH_INT_DMA_CH_SR_TI_RI:
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, TIE, 1);
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, RIE, 1);
		break;
	case DWC_ETH_INT_DMA_CH_SR_FBE:
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, FBEE, 1);
		break;
	case DWC_ETH_INT_DMA_ALL:
		dma_ch_ier |= channel->saved_ier;
		break;
	default:
		return -1;
	}

	DWC_ETH_DMA_IOWRITE(channel, DMA_CH_IER, dma_ch_ier);

	return 0;
}

static int dwc_eth_disable_int(struct dwc_eth_channel *channel,
			       enum dwc_eth_int int_id)
{
	unsigned int dma_ch_ier;

	dma_ch_ier = DWC_ETH_DMA_IOREAD(channel, DMA_CH_IER);

	switch (int_id) {
	case DWC_ETH_INT_DMA_CH_SR_TI:
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, TIE, 0);
		break;
	case DWC_ETH_INT_DMA_CH_SR_TPS:
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, TXSE, 0);
		break;
	case DWC_ETH_INT_DMA_CH_SR_TBU:
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, TBUE, 0);
		break;
	case DWC_ETH_INT_DMA_CH_SR_RI:
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, RIE, 0);
		break;
	case DWC_ETH_INT_DMA_CH_SR_RBU:
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, RBUE, 0);
		break;
	case DWC_ETH_INT_DMA_CH_SR_RPS:
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, RSE, 0);
		break;
	case DWC_ETH_INT_DMA_CH_SR_TI_RI:
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, TIE, 0);
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, RIE, 0);
		break;
	case DWC_ETH_INT_DMA_CH_SR_FBE:
		DWC_ETH_SET_BITS(dma_ch_ier, DMA_CH_IER, FBEE, 0);
		break;
	case DWC_ETH_INT_DMA_ALL:
		channel->saved_ier = dma_ch_ier & DWC_ETH_DMA_INTERRUPT_MASK;
		dma_ch_ier &= ~DWC_ETH_DMA_INTERRUPT_MASK;
		break;
	default:
		return -1;
	}

	DWC_ETH_DMA_IOWRITE(channel, DMA_CH_IER, dma_ch_ier);

	return 0;
}

static int dwc_eth_flush_tx_queues(struct dwc_eth_pdata *pdata)
{
	unsigned int i, count;

	if (DWC_ETH_GET_BITS(pdata->hw_feat.version, MAC_VR, SNPSVER) < 0x21)
		return 0;

	for (i = 0; i < pdata->tx_q_count; i++)
		DWC_ETH_MTL_IOWRITE_BITS(pdata, i, MTL_Q_TQOMR, FTQ, 1);

	/* Poll Until Poll Condition */
	for (i = 0; i < pdata->tx_q_count; i++) {
		count = 2000;
		while (--count && DWC_ETH_MTL_IOREAD_BITS(pdata, i,
							  MTL_Q_TQOMR, FTQ))
			usleep_range(500, 600);

		if (!count)
			return -EBUSY;
	}

	return 0;
}

static void dwc_eth_config_dma_bus(struct dwc_eth_pdata *pdata)
{
	/* Set enhanced addressing mode */
	DWC_ETH_IOWRITE_BITS(pdata, DMA_SBMR, EAME, 1);

	/* Set the System Bus mode */
	DWC_ETH_IOWRITE_BITS(pdata, DMA_SBMR, UNDEF, 1);
	DWC_ETH_IOWRITE_BITS(pdata, DMA_SBMR, BLEN_256, 1);
}

static void dwc_eth_config_dma_cache(struct dwc_eth_pdata *pdata)
{
	unsigned int arcache, awcache;

	arcache = 0;
	DWC_ETH_SET_BITS(arcache, DMA_AXIARCR, DRC, pdata->arcache);
	DWC_ETH_SET_BITS(arcache, DMA_AXIARCR, DRD, pdata->axdomain);
	DWC_ETH_SET_BITS(arcache, DMA_AXIARCR, TEC, pdata->arcache);
	DWC_ETH_SET_BITS(arcache, DMA_AXIARCR, TED, pdata->axdomain);
	DWC_ETH_SET_BITS(arcache, DMA_AXIARCR, THC, pdata->arcache);
	DWC_ETH_SET_BITS(arcache, DMA_AXIARCR, THD, pdata->axdomain);
	DWC_ETH_IOWRITE(pdata, DMA_AXIARCR, arcache);

	awcache = 0;
	DWC_ETH_SET_BITS(awcache, DMA_AXIAWCR, DWC, pdata->awcache);
	DWC_ETH_SET_BITS(awcache, DMA_AXIAWCR, DWD, pdata->axdomain);
	DWC_ETH_SET_BITS(awcache, DMA_AXIAWCR, RPC, pdata->awcache);
	DWC_ETH_SET_BITS(awcache, DMA_AXIAWCR, RPD, pdata->axdomain);
	DWC_ETH_SET_BITS(awcache, DMA_AXIAWCR, RHC, pdata->awcache);
	DWC_ETH_SET_BITS(awcache, DMA_AXIAWCR, RHD, pdata->axdomain);
	DWC_ETH_SET_BITS(awcache, DMA_AXIAWCR, TDC, pdata->awcache);
	DWC_ETH_SET_BITS(awcache, DMA_AXIAWCR, TDD, pdata->axdomain);
	DWC_ETH_IOWRITE(pdata, DMA_AXIAWCR, awcache);
}

static int dwc_eth_init(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_desc_ops *desc_ops = &pdata->desc_ops;
	int ret;

	TRACE("-->");

	/* Flush Tx queues */
	ret = dwc_eth_flush_tx_queues(pdata);
	if (ret)
		return ret;

	/* Initialize DMA related features */
	dwc_eth_config_dma_bus(pdata);
	dwc_eth_config_dma_cache(pdata);
	dwc_eth_config_osp_mode(pdata);
	dwc_eth_config_pblx8(pdata);
	dwc_eth_config_tx_pbl_val(pdata);
	dwc_eth_config_rx_pbl_val(pdata);
	dwc_eth_config_rx_coalesce(pdata);
	dwc_eth_config_tx_coalesce(pdata);
	dwc_eth_config_rx_buffer_size(pdata);
	dwc_eth_config_tso_mode(pdata);
	dwc_eth_config_sph_mode(pdata);
	dwc_eth_config_rss(pdata);
	desc_ops->tx_desc_init(pdata);
	desc_ops->rx_desc_init(pdata);
	dwc_eth_enable_dma_interrupts(pdata);

	/* Initialize MTL related features */
	dwc_eth_config_mtl_mode(pdata);
	dwc_eth_config_queue_mapping(pdata);
	dwc_eth_config_tsf_mode(pdata, pdata->tx_sf_mode);
	dwc_eth_config_rsf_mode(pdata, pdata->rx_sf_mode);
	dwc_eth_config_tx_threshold(pdata, pdata->tx_threshold);
	dwc_eth_config_rx_threshold(pdata, pdata->rx_threshold);
	dwc_eth_config_tx_fifo_size(pdata);
	dwc_eth_config_rx_fifo_size(pdata);
	dwc_eth_config_flow_control_threshold(pdata);
	dwc_eth_config_rx_fep_enable(pdata);
	dwc_eth_config_rx_fup_enable(pdata);
	dwc_eth_config_dcb_tc(pdata);
	dwc_eth_config_dcb_pfc(pdata);
	dwc_eth_enable_mtl_interrupts(pdata);

	/* Initialize MAC related features */
	dwc_eth_config_mac_address(pdata);
	dwc_eth_config_rx_mode(pdata);
	dwc_eth_config_jumbo_enable(pdata);
	dwc_eth_config_flow_control(pdata);
	dwc_eth_config_mac_speed(pdata);
	dwc_eth_config_checksum_offload(pdata);
	dwc_eth_config_vlan_support(pdata);
	dwc_eth_config_mmc(pdata);
	dwc_eth_enable_mac_interrupts(pdata);

	TRACE("<--");

	return 0;
}

static int dwc_eth_exit(struct dwc_eth_pdata *pdata)
{
	unsigned int count = 2000;

	TRACE("-->");

	/* Issue a software reset */
	DWC_ETH_IOWRITE_BITS(pdata, DMA_MR, SWR, 1);
	usleep_range(10, 15);

	/* Poll Until Poll Condition */
	while (--count && DWC_ETH_IOREAD_BITS(pdata, DMA_MR, SWR))
		usleep_range(500, 600);

	if (!count)
		return -EBUSY;

	TRACE("<--");

	return 0;
}

void dwc_eth_init_hw_ops(struct dwc_eth_hw_ops *hw_ops)
{
	TRACE("-->");

	hw_ops->tx_complete = dwc_eth_tx_complete;

	hw_ops->set_mac_address = dwc_eth_set_mac_address;
	hw_ops->config_rx_mode = dwc_eth_config_rx_mode;

	hw_ops->enable_rx_csum = dwc_eth_enable_rx_csum;
	hw_ops->disable_rx_csum = dwc_eth_disable_rx_csum;

	hw_ops->enable_rx_vlan_stripping = dwc_eth_enable_rx_vlan_stripping;
	hw_ops->disable_rx_vlan_stripping = dwc_eth_disable_rx_vlan_stripping;
	hw_ops->enable_rx_vlan_filtering = dwc_eth_enable_rx_vlan_filtering;
	hw_ops->disable_rx_vlan_filtering = dwc_eth_disable_rx_vlan_filtering;
	hw_ops->update_vlan_hash_table = dwc_eth_update_vlan_hash_table;

	hw_ops->read_mmd_regs = dwc_eth_read_mmd_regs;
	hw_ops->write_mmd_regs = dwc_eth_write_mmd_regs;

	hw_ops->set_gmii_1000_speed = dwc_eth_set_gmii_1000_speed;
	hw_ops->set_gmii_2500_speed = dwc_eth_set_gmii_2500_speed;
	hw_ops->set_xgmii_10000_speed = dwc_eth_set_xgmii_10000_speed;
	hw_ops->set_xlgmii_25000_speed = dwc_eth_set_xlgmii_25000_speed;
	hw_ops->set_xlgmii_40000_speed = dwc_eth_set_xlgmii_40000_speed;
	hw_ops->set_xlgmii_50000_speed = dwc_eth_set_xlgmii_50000_speed;
	hw_ops->set_xlgmii_100000_speed = dwc_eth_set_xlgmii_100000_speed;

	hw_ops->enable_tx = dwc_eth_enable_tx;
	hw_ops->disable_tx = dwc_eth_disable_tx;
	hw_ops->enable_rx = dwc_eth_enable_rx;
	hw_ops->disable_rx = dwc_eth_disable_rx;

	hw_ops->powerup_tx = dwc_eth_powerup_tx;
	hw_ops->powerdown_tx = dwc_eth_powerdown_tx;
	hw_ops->powerup_rx = dwc_eth_powerup_rx;
	hw_ops->powerdown_rx = dwc_eth_powerdown_rx;

	hw_ops->dev_xmit = dwc_eth_dev_xmit;
	hw_ops->dev_read = dwc_eth_dev_read;
	hw_ops->enable_int = dwc_eth_enable_int;
	hw_ops->disable_int = dwc_eth_disable_int;

	hw_ops->init = dwc_eth_init;
	hw_ops->exit = dwc_eth_exit;

	/* Descriptor related Sequences have to be initialized here */
	hw_ops->tx_desc_init = dwc_eth_tx_desc_init;
	hw_ops->rx_desc_init = dwc_eth_rx_desc_init;
	hw_ops->tx_desc_reset = dwc_eth_tx_desc_reset;
	hw_ops->rx_desc_reset = dwc_eth_rx_desc_reset;
	hw_ops->is_last_desc = dwc_eth_is_last_desc;
	hw_ops->is_context_desc = dwc_eth_is_context_desc;
	hw_ops->tx_start_xmit = dwc_eth_tx_start_xmit;

	/* For FLOW ctrl */
	hw_ops->config_tx_flow_control = dwc_eth_config_tx_flow_control;
	hw_ops->config_rx_flow_control = dwc_eth_config_rx_flow_control;

	/* For RX coalescing */
	hw_ops->config_rx_coalesce = dwc_eth_config_rx_coalesce;
	hw_ops->config_tx_coalesce = dwc_eth_config_tx_coalesce;
	hw_ops->usec_to_riwt = dwc_eth_usec_to_riwt;
	hw_ops->riwt_to_usec = dwc_eth_riwt_to_usec;

	/* For RX and TX threshold config */
	hw_ops->config_rx_threshold = dwc_eth_config_rx_threshold;
	hw_ops->config_tx_threshold = dwc_eth_config_tx_threshold;

	/* For RX and TX Store and Forward Mode config */
	hw_ops->config_rsf_mode = dwc_eth_config_rsf_mode;
	hw_ops->config_tsf_mode = dwc_eth_config_tsf_mode;

	/* For TX DMA Operating on Second Frame config */
	hw_ops->config_osp_mode = dwc_eth_config_osp_mode;

	/* For RX and TX PBL config */
	hw_ops->config_rx_pbl_val = dwc_eth_config_rx_pbl_val;
	hw_ops->get_rx_pbl_val = dwc_eth_get_rx_pbl_val;
	hw_ops->config_tx_pbl_val = dwc_eth_config_tx_pbl_val;
	hw_ops->get_tx_pbl_val = dwc_eth_get_tx_pbl_val;
	hw_ops->config_pblx8 = dwc_eth_config_pblx8;

	/* For MMC statistics support */
	hw_ops->tx_mmc_int = dwc_eth_tx_mmc_int;
	hw_ops->rx_mmc_int = dwc_eth_rx_mmc_int;
	hw_ops->read_mmc_stats = dwc_eth_read_mmc_stats;

	/* For PTP config */
	hw_ops->config_tstamp = dwc_eth_config_tstamp;
	hw_ops->update_tstamp_addend = dwc_eth_update_tstamp_addend;
	hw_ops->set_tstamp_time = dwc_eth_set_tstamp_time;
	hw_ops->get_tstamp_time = dwc_eth_get_tstamp_time;
	hw_ops->get_tx_tstamp = dwc_eth_get_tx_tstamp;

	/* For Data Center Bridging config */
	hw_ops->config_tc = dwc_eth_config_tc;
	hw_ops->config_dcb_tc = dwc_eth_config_dcb_tc;
	hw_ops->config_dcb_pfc = dwc_eth_config_dcb_pfc;

	/* For Receive Side Scaling */
	hw_ops->enable_rss = dwc_eth_enable_rss;
	hw_ops->disable_rss = dwc_eth_disable_rss;
	hw_ops->set_rss_hash_key = dwc_eth_set_rss_hash_key;
	hw_ops->set_rss_lookup_table = dwc_eth_set_rss_lookup_table;

	TRACE("<--");
}
