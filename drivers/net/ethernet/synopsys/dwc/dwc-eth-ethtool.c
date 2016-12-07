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

#include <linux/spinlock.h>
#include <linux/phy.h>
#include <linux/net_tstamp.h>

#include "dwc-eth.h"
#include "dwc-eth-regacc.h"

struct dwc_eth_stats_desc {
	char stat_string[ETH_GSTRING_LEN];
	int stat_size;
	int stat_offset;
};

#define DWC_ETH_STAT(_name, _var) \
	{\
		_name,						\
		FIELD_SIZEOF(struct dwc_eth_stats, _var),	\
		offsetof(struct dwc_eth_pdata, stats._var),	\
	}

static const struct dwc_eth_stats_desc dwc_eth_gstring_stats[] = {
	DWC_ETH_STAT("tx_bytes", txoctetcount_gb),
	DWC_ETH_STAT("tx_packets", txframecount_gb),
	DWC_ETH_STAT("tx_unicast_packets", txunicastframes_gb),
	DWC_ETH_STAT("tx_broadcast_packets", txbroadcastframes_gb),
	DWC_ETH_STAT("tx_multicast_packets", txmulticastframes_gb),
	DWC_ETH_STAT("tx_vlan_packets", txvlanframes_g),
	DWC_ETH_STAT("tx_tso_packets", tx_tso_packets),
	DWC_ETH_STAT("tx_64_byte_packets", tx64octets_gb),
	DWC_ETH_STAT("tx_65_to_127_byte_packets", tx65to127octets_gb),
	DWC_ETH_STAT("tx_128_to_255_byte_packets", tx128to255octets_gb),
	DWC_ETH_STAT("tx_256_to_511_byte_packets", tx256to511octets_gb),
	DWC_ETH_STAT("tx_512_to_1023_byte_packets", tx512to1023octets_gb),
	DWC_ETH_STAT("tx_1024_to_max_byte_packets", tx1024tomaxoctets_gb),
	DWC_ETH_STAT("tx_underflow_errors", txunderflowerror),
	DWC_ETH_STAT("tx_pause_frames", txpauseframes),

	DWC_ETH_STAT("rx_bytes", rxoctetcount_gb),
	DWC_ETH_STAT("rx_packets", rxframecount_gb),
	DWC_ETH_STAT("rx_unicast_packets", rxunicastframes_g),
	DWC_ETH_STAT("rx_broadcast_packets", rxbroadcastframes_g),
	DWC_ETH_STAT("rx_multicast_packets", rxmulticastframes_g),
	DWC_ETH_STAT("rx_vlan_packets", rxvlanframes_gb),
	DWC_ETH_STAT("rx_64_byte_packets", rx64octets_gb),
	DWC_ETH_STAT("rx_65_to_127_byte_packets", rx65to127octets_gb),
	DWC_ETH_STAT("rx_128_to_255_byte_packets", rx128to255octets_gb),
	DWC_ETH_STAT("rx_256_to_511_byte_packets", rx256to511octets_gb),
	DWC_ETH_STAT("rx_512_to_1023_byte_packets", rx512to1023octets_gb),
	DWC_ETH_STAT("rx_1024_to_max_byte_packets", rx1024tomaxoctets_gb),
	DWC_ETH_STAT("rx_undersize_packets", rxundersize_g),
	DWC_ETH_STAT("rx_oversize_packets", rxoversize_g),
	DWC_ETH_STAT("rx_crc_errors", rxcrcerror),
	DWC_ETH_STAT("rx_crc_errors_small_packets", rxrunterror),
	DWC_ETH_STAT("rx_crc_errors_giant_packets", rxjabbererror),
	DWC_ETH_STAT("rx_length_errors", rxlengtherror),
	DWC_ETH_STAT("rx_out_of_range_errors", rxoutofrangetype),
	DWC_ETH_STAT("rx_fifo_overflow_errors", rxfifooverflow),
	DWC_ETH_STAT("rx_watchdog_errors", rxwatchdogerror),
	DWC_ETH_STAT("rx_pause_frames", rxpauseframes),
	DWC_ETH_STAT("rx_split_header_packets", rx_split_header_packets),
	DWC_ETH_STAT("rx_buffer_unavailable", rx_buffer_unavailable),
};

#define DWC_ETH_STATS_COUNT	ARRAY_SIZE(dwc_eth_gstring_stats)

static void dwc_eth_get_strings(struct net_device *netdev,
				u32 stringset, u8 *data)
{
	int i;

	TRACE("-->");

	switch (stringset) {
	case ETH_SS_STATS:
		for (i = 0; i < DWC_ETH_STATS_COUNT; i++) {
			memcpy(data, dwc_eth_gstring_stats[i].stat_string,
			       ETH_GSTRING_LEN);
			data += ETH_GSTRING_LEN;
		}
		break;
	}

	TRACE("<--");
}

static void dwc_eth_get_ethtool_stats(struct net_device *netdev,
				      struct ethtool_stats *stats,
				      u64 *data)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	u8 *stat;
	int i;

	TRACE("-->");

	pdata->hw_ops.read_mmc_stats(pdata);
	for (i = 0; i < DWC_ETH_STATS_COUNT; i++) {
		stat = (u8 *)pdata + dwc_eth_gstring_stats[i].stat_offset;
		*data++ = *(u64 *)stat;
	}

	TRACE("<--");
}

static int dwc_eth_get_sset_count(struct net_device *netdev, int stringset)
{
	int ret;

	TRACE("-->");

	switch (stringset) {
	case ETH_SS_STATS:
		ret = DWC_ETH_STATS_COUNT;
		break;

	default:
		ret = -EOPNOTSUPP;
	}

	TRACE("<--");

	return ret;
}

static void dwc_eth_get_pauseparam(struct net_device *netdev,
				   struct ethtool_pauseparam *pause)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);

	TRACE("-->");

	pause->autoneg = pdata->pause_autoneg;
	pause->tx_pause = pdata->tx_pause;
	pause->rx_pause = pdata->rx_pause;

	TRACE("<--");
}

static int dwc_eth_set_pauseparam(struct net_device *netdev,
				  struct ethtool_pauseparam *pause)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	struct phy_device *phydev = pdata->phydev;
	int ret = 0;

	TRACE("-->");
	DBGPR("  autoneg = %d, tx_pause = %d, rx_pause = %d\n",
	      pause->autoneg, pause->tx_pause, pause->rx_pause);

	pdata->pause_autoneg = pause->autoneg;
	if (pause->autoneg) {
		phydev->advertising |= ADVERTISED_Pause;
		phydev->advertising |= ADVERTISED_Asym_Pause;

	} else {
		phydev->advertising &= ~ADVERTISED_Pause;
		phydev->advertising &= ~ADVERTISED_Asym_Pause;

		pdata->tx_pause = pause->tx_pause;
		pdata->rx_pause = pause->rx_pause;
	}

	if (netif_running(netdev))
		ret = phy_start_aneg(phydev);

	TRACE("<--");

	return ret;
}

static int dwc_eth_get_settings(struct net_device *netdev,
				struct ethtool_cmd *cmd)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	int ret;

	TRACE("-->");

	if (!pdata->phydev)
		return -ENODEV;

	ret = phy_ethtool_gset(pdata->phydev, cmd);

	TRACE("<--");

	return ret;
}

static int dwc_eth_set_settings(struct net_device *netdev,
				struct ethtool_cmd *cmd)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	struct phy_device *phydev = pdata->phydev;
	u32 speed;
	int ret;

	TRACE("-->");

	if (!pdata->phydev)
		return -ENODEV;

	speed = ethtool_cmd_speed(cmd);

	if (cmd->phy_address != phydev->mdio.addr)
		return -EINVAL;

	if ((cmd->autoneg != AUTONEG_ENABLE) &&
	    (cmd->autoneg != AUTONEG_DISABLE))
		return -EINVAL;

	if (cmd->autoneg == AUTONEG_DISABLE) {
		switch (speed) {
		case SPEED_100000:
		case SPEED_50000:
		case SPEED_40000:
		case SPEED_25000:
		case SPEED_10000:
		case SPEED_2500:
		case SPEED_1000:
			break;
		default:
			return -EINVAL;
		}

		if (cmd->duplex != DUPLEX_FULL)
			return -EINVAL;
	}

	cmd->advertising &= phydev->supported;
	if ((cmd->autoneg == AUTONEG_ENABLE) && !cmd->advertising)
		return -EINVAL;

	ret = 0;
	phydev->autoneg = cmd->autoneg;
	phydev->speed = speed;
	phydev->duplex = cmd->duplex;
	phydev->advertising = cmd->advertising;

	if (cmd->autoneg == AUTONEG_ENABLE)
		phydev->advertising |= ADVERTISED_Autoneg;
	else
		phydev->advertising &= ~ADVERTISED_Autoneg;

	if (netif_running(netdev))
		ret = phy_start_aneg(phydev);

	TRACE("<--");

	return ret;
}

static u32 dwc_eth_get_msglevel(struct net_device *netdev)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);

	return pdata->msg_enable;
}

static void dwc_eth_set_msglevel(struct net_device *netdev, u32 msglevel)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);

	pdata->msg_enable = msglevel;
}

static void dwc_eth_get_drvinfo(struct net_device *netdev,
				struct ethtool_drvinfo *drvinfo)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	struct dwc_eth_hw_features *hw_feat = &pdata->hw_feat;

	TRACE("-->");

	strlcpy(drvinfo->driver, pdata->drv_name,
		sizeof(drvinfo->driver));
	strlcpy(drvinfo->version, pdata->drv_ver,
		sizeof(drvinfo->version));
	strlcpy(drvinfo->bus_info, dev_name(pdata->dev),
		sizeof(drvinfo->bus_info));
	snprintf(drvinfo->fw_version, sizeof(drvinfo->fw_version), "%d.%d.%d",
		 DWC_ETH_GET_BITS(hw_feat->version, MAC_VR, USERVER),
		 DWC_ETH_GET_BITS(hw_feat->version, MAC_VR, DEVID),
		 DWC_ETH_GET_BITS(hw_feat->version, MAC_VR, SNPSVER));
	drvinfo->n_stats = DWC_ETH_STATS_COUNT;

	TRACE("<--");
}

static int dwc_eth_get_coalesce(struct net_device *netdev,
				struct ethtool_coalesce *ec)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);

	TRACE("-->");

	memset(ec, 0, sizeof(struct ethtool_coalesce));

	ec->rx_coalesce_usecs = pdata->rx_usecs;
	ec->rx_max_coalesced_frames = pdata->rx_frames;

	ec->tx_max_coalesced_frames = pdata->tx_frames;

	TRACE("<--");

	return 0;
}

static int dwc_eth_set_coalesce(struct net_device *netdev,
				struct ethtool_coalesce *ec)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;
	unsigned int rx_frames, rx_riwt, rx_usecs;
	unsigned int tx_frames;

	TRACE("-->");

	/* Check for not supported parameters  */
	if ((ec->rx_coalesce_usecs_irq) ||
	    (ec->rx_max_coalesced_frames_irq) ||
	    (ec->tx_coalesce_usecs) ||
	    (ec->tx_coalesce_usecs_irq) ||
	    (ec->tx_max_coalesced_frames_irq) ||
	    (ec->stats_block_coalesce_usecs) ||
	    (ec->use_adaptive_rx_coalesce) ||
	    (ec->use_adaptive_tx_coalesce) ||
	    (ec->pkt_rate_low) ||
	    (ec->rx_coalesce_usecs_low) ||
	    (ec->rx_max_coalesced_frames_low) ||
	    (ec->tx_coalesce_usecs_low) ||
	    (ec->tx_max_coalesced_frames_low) ||
	    (ec->pkt_rate_high) ||
	    (ec->rx_coalesce_usecs_high) ||
	    (ec->rx_max_coalesced_frames_high) ||
	    (ec->tx_coalesce_usecs_high) ||
	    (ec->tx_max_coalesced_frames_high) ||
	    (ec->rate_sample_interval))
		return -EOPNOTSUPP;

	rx_riwt = hw_ops->usec_to_riwt(pdata, ec->rx_coalesce_usecs);
	rx_usecs = ec->rx_coalesce_usecs;
	rx_frames = ec->rx_max_coalesced_frames;

	/* Use smallest possible value if conversion resulted in zero */
	if (rx_usecs && !rx_riwt)
		rx_riwt = 1;

	/* Check the bounds of values for Rx */
	if (rx_riwt > pdata->max_dma_riwt) {
		netdev_alert(netdev, "rx-usec is limited to %d usecs\n",
			     hw_ops->riwt_to_usec(pdata, pdata->max_dma_riwt));
		return -EINVAL;
	}
	if (rx_frames > pdata->rx_desc_count) {
		netdev_alert(netdev, "rx-frames is limited to %d frames\n",
			     pdata->rx_desc_count);
		return -EINVAL;
	}

	tx_frames = ec->tx_max_coalesced_frames;

	/* Check the bounds of values for Tx */
	if (tx_frames > pdata->tx_desc_count) {
		netdev_alert(netdev, "tx-frames is limited to %d frames\n",
			     pdata->tx_desc_count);
		return -EINVAL;
	}

	pdata->rx_riwt = rx_riwt;
	pdata->rx_usecs = rx_usecs;
	pdata->rx_frames = rx_frames;
	hw_ops->config_rx_coalesce(pdata);

	pdata->tx_frames = tx_frames;
	hw_ops->config_tx_coalesce(pdata);

	TRACE("<--");

	return 0;
}

static int dwc_eth_get_rxnfc(struct net_device *netdev,
			     struct ethtool_rxnfc *rxnfc, u32 *rule_locs)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);

	switch (rxnfc->cmd) {
	case ETHTOOL_GRXRINGS:
		rxnfc->data = pdata->rx_ring_count;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static u32 dwc_eth_get_rxfh_key_size(struct net_device *netdev)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);

	return sizeof(pdata->rss_key);
}

static u32 dwc_eth_get_rxfh_indir_size(struct net_device *netdev)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);

	return ARRAY_SIZE(pdata->rss_table);
}

static int dwc_eth_get_rxfh(struct net_device *netdev, u32 *indir,
			    u8 *key, u8 *hfunc)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	unsigned int i;

	if (indir) {
		for (i = 0; i < ARRAY_SIZE(pdata->rss_table); i++)
			indir[i] = DWC_ETH_GET_BITS(pdata->rss_table[i],
						   MAC_RSSDR, DMCH);
	}

	if (key)
		memcpy(key, pdata->rss_key, sizeof(pdata->rss_key));

	if (hfunc)
		*hfunc = ETH_RSS_HASH_TOP;

	return 0;
}

static int dwc_eth_set_rxfh(struct net_device *netdev, const u32 *indir,
			    const u8 *key, const u8 hfunc)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;
	unsigned int ret;

	if (hfunc != ETH_RSS_HASH_NO_CHANGE && hfunc != ETH_RSS_HASH_TOP)
		return -EOPNOTSUPP;

	if (indir) {
		ret = hw_ops->set_rss_lookup_table(pdata, indir);
		if (ret)
			return ret;
	}

	if (key) {
		ret = hw_ops->set_rss_hash_key(pdata, key);
		if (ret)
			return ret;
	}

	return 0;
}

static int dwc_eth_get_ts_info(struct net_device *netdev,
			       struct ethtool_ts_info *ts_info)
{
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);

	ts_info->so_timestamping = SOF_TIMESTAMPING_TX_SOFTWARE |
				   SOF_TIMESTAMPING_RX_SOFTWARE |
				   SOF_TIMESTAMPING_SOFTWARE |
				   SOF_TIMESTAMPING_TX_HARDWARE |
				   SOF_TIMESTAMPING_RX_HARDWARE |
				   SOF_TIMESTAMPING_RAW_HARDWARE;

	if (pdata->ptp_clock)
		ts_info->phc_index = ptp_clock_index(pdata->ptp_clock);
	else
		ts_info->phc_index = -1;

	ts_info->tx_types = (1 << HWTSTAMP_TX_OFF) | (1 << HWTSTAMP_TX_ON);
	ts_info->rx_filters = (1 << HWTSTAMP_FILTER_NONE) |
			      (1 << HWTSTAMP_FILTER_PTP_V1_L4_EVENT) |
			      (1 << HWTSTAMP_FILTER_PTP_V1_L4_SYNC) |
			      (1 << HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ) |
			      (1 << HWTSTAMP_FILTER_PTP_V2_L4_EVENT) |
			      (1 << HWTSTAMP_FILTER_PTP_V2_L4_SYNC) |
			      (1 << HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ) |
			      (1 << HWTSTAMP_FILTER_PTP_V2_EVENT) |
			      (1 << HWTSTAMP_FILTER_PTP_V2_SYNC) |
			      (1 << HWTSTAMP_FILTER_PTP_V2_DELAY_REQ) |
			      (1 << HWTSTAMP_FILTER_ALL);

	return 0;
}

static const struct ethtool_ops dwc_eth_ethtool_ops = {
	.get_settings = dwc_eth_get_settings,
	.set_settings = dwc_eth_set_settings,
	.get_drvinfo = dwc_eth_get_drvinfo,
	.get_msglevel = dwc_eth_get_msglevel,
	.set_msglevel = dwc_eth_set_msglevel,
	.get_link = ethtool_op_get_link,
	.get_coalesce = dwc_eth_get_coalesce,
	.set_coalesce = dwc_eth_set_coalesce,
	.get_pauseparam = dwc_eth_get_pauseparam,
	.set_pauseparam = dwc_eth_set_pauseparam,
	.get_strings = dwc_eth_get_strings,
	.get_ethtool_stats = dwc_eth_get_ethtool_stats,
	.get_sset_count = dwc_eth_get_sset_count,
	.get_rxnfc = dwc_eth_get_rxnfc,
	.get_rxfh_key_size = dwc_eth_get_rxfh_key_size,
	.get_rxfh_indir_size = dwc_eth_get_rxfh_indir_size,
	.get_rxfh = dwc_eth_get_rxfh,
	.set_rxfh = dwc_eth_set_rxfh,
	.get_ts_info = dwc_eth_get_ts_info,
};

const struct ethtool_ops *dwc_eth_get_ethtool_ops(void)
{
	return &dwc_eth_ethtool_ops;
}
