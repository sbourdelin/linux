// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2018 MediaTek Inc.
#include <linux/ethtool.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>

#include "mtk-gmac.h"

struct gmac_stats_desc {
	char stat_string[ETH_GSTRING_LEN];
	int stat_offset;
};

#define GMAC_STAT(str, var)				\
	{						\
		str,					\
		offsetof(struct gmac_pdata, stats.var),	\
	}

static const struct gmac_stats_desc gmac_gstring_stats[] = {
	/* MMC TX counters */
	GMAC_STAT("tx_bytes", txoctetcount_gb),
	GMAC_STAT("tx_bytes_good", txoctetcount_g),
	GMAC_STAT("tx_packets", txframecount_gb),
	GMAC_STAT("tx_packets_good", txframecount_g),
	GMAC_STAT("tx_unicast_packets", txunicastframes_gb),
	GMAC_STAT("tx_broadcast_packets", txbroadcastframes_gb),
	GMAC_STAT("tx_broadcast_packets_good", txbroadcastframes_g),
	GMAC_STAT("tx_multicast_packets", txmulticastframes_gb),
	GMAC_STAT("tx_multicast_packets_good", txmulticastframes_g),
	GMAC_STAT("tx_vlan_packets_good", txvlanframes_g),
	GMAC_STAT("tx_over_size_packets_good", txosizeframe_g),
	GMAC_STAT("tx_64_byte_packets", tx64octets_gb),
	GMAC_STAT("tx_65_to_127_byte_packets", tx65to127octets_gb),
	GMAC_STAT("tx_128_to_255_byte_packets", tx128to255octets_gb),
	GMAC_STAT("tx_256_to_511_byte_packets", tx256to511octets_gb),
	GMAC_STAT("tx_512_to_1023_byte_packets", tx512to1023octets_gb),
	GMAC_STAT("tx_1024_to_max_byte_packets", tx1024tomaxoctets_gb),
	GMAC_STAT("tx_underflow_errors", txunderflowerror),
	GMAC_STAT("tx_single_collision_good", txsinglecol_g),
	GMAC_STAT("tx_multiple_collision_good", txmulticol_g),
	GMAC_STAT("tx_deferred_packets", txdeferred),
	GMAC_STAT("tx_late_collision_packets", txlatecol),
	GMAC_STAT("tx_excessive-collision_packets", txexesscol),
	GMAC_STAT("tx_carrier_error_packets", txcarriererror),
	GMAC_STAT("tx_excessive_deferral_error", txexcessdef),
	GMAC_STAT("tx_pause_frames", txpauseframes),
	GMAC_STAT("tx_timestamp_packets", tx_timestamp_packets),
	GMAC_STAT("tx_lpi_microseconds", txlpiusec),
	GMAC_STAT("tx_lpi_transition", txlpitran),

	/* MMC RX counters */
	GMAC_STAT("rx_bytes", rxoctetcount_gb),
	GMAC_STAT("rx_bytes_good", rxoctetcount_g),
	GMAC_STAT("rx_packets", rxframecount_gb),
	GMAC_STAT("rx_unicast_packets_good", rxunicastframes_g),
	GMAC_STAT("rx_broadcast_packets_good", rxbroadcastframes_g),
	GMAC_STAT("rx_multicast_packets_good", rxmulticastframes_g),
	GMAC_STAT("rx_vlan_packets", rxvlanframes_gb),
	GMAC_STAT("rx_64_byte_packets", rx64octets_gb),
	GMAC_STAT("rx_65_to_127_byte_packets", rx65to127octets_gb),
	GMAC_STAT("rx_128_to_255_byte_packets", rx128to255octets_gb),
	GMAC_STAT("rx_256_to_511_byte_packets", rx256to511octets_gb),
	GMAC_STAT("rx_512_to_1023_byte_packets", rx512to1023octets_gb),
	GMAC_STAT("rx_1024_to_max_byte_packets", rx1024tomaxoctets_gb),
	GMAC_STAT("rx_undersize_packets_good", rxundersize_g),
	GMAC_STAT("rx_oversize_packets_good", rxoversize_g),
	GMAC_STAT("rx_crc_errors", rxcrcerror),
	GMAC_STAT("rx_alignment_error_packets", rxalignerror),
	GMAC_STAT("rx_crc_errors_small_packets", rxrunterror),
	GMAC_STAT("rx_crc_errors_giant_packets", rxjabbererror),
	GMAC_STAT("rx_length_errors", rxlengtherror),
	GMAC_STAT("rx_out_of_range_errors", rxoutofrangetype),
	GMAC_STAT("rx_fifo_overflow_errors", rxfifooverflow),
	GMAC_STAT("rx_watchdog_errors", rxwatchdogerror),
	GMAC_STAT("rx_receive_errors", rxreceiveerror),
	GMAC_STAT("rx_control_packets_good", rxctrlframes_g),
	GMAC_STAT("rx_pause_frames", rxpauseframes),
	GMAC_STAT("rx_timestamp_packets", rx_timestamp_packets),
	GMAC_STAT("rx_lpi_microseconds", rxlpiusec),
	GMAC_STAT("rx_lpi_transition", rxlpitran),

	/* MMC RXIPC counters */
	GMAC_STAT("rx_ipv4_good_packets", rxipv4_g),
	GMAC_STAT("rx_ipv4_header_error_packets", rxipv4hderr),
	GMAC_STAT("rx_ipv4_no_payload_packets", rxipv4nopay),
	GMAC_STAT("rx_ipv4_fragmented_packets", rxipv4frag),
	GMAC_STAT("rx_ipv4_udp_csum_dis_packets", rxipv4udsbl),
	GMAC_STAT("rx_ipv6_good_packets", rxipv6octets_g),
	GMAC_STAT("rx_ipv6_header_error_packets", rxipv6hderroctets),
	GMAC_STAT("rx_ipv6_no_payload_packets", rxipv6nopayoctets),
	GMAC_STAT("rx_udp_good_packets", rxudp_g),
	GMAC_STAT("rx_udp_error_packets", rxudperr),
	GMAC_STAT("rx_tcp_good_packets", rxtcp_g),
	GMAC_STAT("rx_tcp_error_packets", rxtcperr),
	GMAC_STAT("rx_icmp_good_packets", rxicmp_g),
	GMAC_STAT("rx_icmp_error_packets", rxicmperr),
	GMAC_STAT("rx_ipv4_good_bytes", rxipv4octets_g),
	GMAC_STAT("rx_ipv4_header_error_bytes", rxipv4hderroctets),
	GMAC_STAT("rx_ipv4_no_payload_bytes", rxipv4nopayoctets),
	GMAC_STAT("rx_ipv4_fragmented_bytes", rxipv4fragoctets),
	GMAC_STAT("rx_ipv4_udp_csum_dis_bytes", rxipv4udsbloctets),
	GMAC_STAT("rx_ipv6_good_bytes", rxipv6_g),
	GMAC_STAT("rx_ipv6_header_error_bytes", rxipv6hderr),
	GMAC_STAT("rx_ipv6_no_payload_bytes", rxipv6nopay),
	GMAC_STAT("rx_udp_good_bytes", rxudpoctets_g),
	GMAC_STAT("rx_udp_error_bytes", rxudperroctets),
	GMAC_STAT("rx_tcp_good_bytes", rxtcpoctets_g),
	GMAC_STAT("rx_tcp_error_bytes", rxtcperroctets),
	GMAC_STAT("rx_icmp_good_bytes", rxicmpoctets_g),
	GMAC_STAT("rx_icmp_error_bytes", rxicmperroctets),

	/* Extra counters */
	GMAC_STAT("tx_tso_packets", tx_tso_packets),
	GMAC_STAT("rx_split_header_packets", rx_split_header_packets),
	GMAC_STAT("tx_process_stopped", tx_process_stopped),
	GMAC_STAT("rx_process_stopped", rx_process_stopped),
	GMAC_STAT("tx_buffer_unavailable", tx_buffer_unavailable),
	GMAC_STAT("rx_buffer_unavailable", rx_buffer_unavailable),
	GMAC_STAT("fatal_bus_error", fatal_bus_error),
	GMAC_STAT("tx_vlan_packets", tx_vlan_packets),
	GMAC_STAT("rx_vlan_packets", rx_vlan_packets),
	GMAC_STAT("napi_poll_isr", napi_poll_isr),
	GMAC_STAT("napi_poll_txtimer", napi_poll_txtimer),
};

#define GMAC_STATS_COUNT	ARRAY_SIZE(gmac_gstring_stats)

static void gmac_ethtool_get_drvinfo(struct net_device *netdev,
				     struct ethtool_drvinfo *drvinfo)
{
	struct gmac_pdata *pdata = netdev_priv(netdev);
	u32 ver = pdata->hw_feat.version;
	u32 snpsver, userver;

	strlcpy(drvinfo->driver, pdata->drv_name, sizeof(drvinfo->driver));
	strlcpy(drvinfo->version, pdata->drv_ver, sizeof(drvinfo->version));
	strlcpy(drvinfo->bus_info, dev_name(pdata->dev),
		sizeof(drvinfo->bus_info));
	/* S|SNPSVER: Synopsys-defined Version
	 * U|USERVER: User-defined Version
	 */
	snpsver = GMAC_GET_REG_BITS(ver,
				    MAC_VR_SNPSVER_POS,
				    MAC_VR_SNPSVER_LEN);
	userver =  GMAC_GET_REG_BITS(ver,
				     MAC_VR_USERVER_POS,
				     MAC_VR_USERVER_LEN);
	snprintf(drvinfo->fw_version, sizeof(drvinfo->fw_version),
		 "S.U: %x.%x", snpsver, userver);
}

static u32 gmac_ethtool_get_msglevel(struct net_device *netdev)
{
	struct gmac_pdata *pdata = netdev_priv(netdev);

	return pdata->msg_enable;
}

static void gmac_ethtool_set_msglevel(struct net_device *netdev,
				      u32 msglevel)
{
	struct gmac_pdata *pdata = netdev_priv(netdev);

	pdata->msg_enable = msglevel;
}

static void gmac_ethtool_get_channels(struct net_device *netdev,
				      struct ethtool_channels *channel)
{
	struct gmac_pdata *pdata = netdev_priv(netdev);

	channel->max_rx = GMAC_MAX_DMA_CHANNELS;
	channel->max_tx = GMAC_MAX_DMA_CHANNELS;
	channel->rx_count = pdata->rx_q_count;
	channel->tx_count = pdata->tx_q_count;
}

static int gmac_ethtool_get_coalesce(struct net_device *netdev,
				     struct ethtool_coalesce *ec)
{
	struct gmac_pdata *pdata = netdev_priv(netdev);

	memset(ec, 0, sizeof(struct ethtool_coalesce));
	ec->rx_coalesce_usecs = pdata->rx_usecs;
	ec->rx_max_coalesced_frames = pdata->rx_frames;
	ec->tx_max_coalesced_frames = pdata->tx_frames;

	return 0;
}

static int gmac_ethtool_set_coalesce(struct net_device *netdev,
				     struct ethtool_coalesce *ec)
{
	struct gmac_pdata *pdata = netdev_priv(netdev);
	struct gmac_hw_ops *hw_ops = &pdata->hw_ops;
	unsigned int rx_frames, rx_riwt, rx_usecs;
	unsigned int tx_frames;

	/* Check for not supported parameters */
	if (ec->rx_coalesce_usecs_irq || ec->rx_max_coalesced_frames_irq ||
	    ec->tx_coalesce_usecs || ec->tx_coalesce_usecs_high ||
	    ec->tx_max_coalesced_frames_irq || ec->tx_coalesce_usecs_irq ||
	    ec->stats_block_coalesce_usecs ||  ec->pkt_rate_low ||
	    ec->use_adaptive_rx_coalesce || ec->use_adaptive_tx_coalesce ||
	    ec->rx_max_coalesced_frames_low || ec->rx_coalesce_usecs_low ||
	    ec->tx_coalesce_usecs_low || ec->tx_max_coalesced_frames_low ||
	    ec->pkt_rate_high || ec->rx_coalesce_usecs_high ||
	    ec->rx_max_coalesced_frames_high ||
	    ec->tx_max_coalesced_frames_high ||
	    ec->rate_sample_interval)
		return -EOPNOTSUPP;

	rx_usecs = ec->rx_coalesce_usecs;
	rx_riwt = hw_ops->usec_to_riwt(pdata, rx_usecs);
	rx_frames = ec->rx_max_coalesced_frames;
	tx_frames = ec->tx_max_coalesced_frames;

	if (rx_riwt > GMAC_MAX_DMA_RIWT ||
	    rx_riwt < GMAC_MIN_DMA_RIWT ||
	    rx_frames > pdata->rx_desc_count)
		return -EINVAL;

	if (tx_frames > pdata->tx_desc_count)
		return -EINVAL;

	pdata->rx_riwt = rx_riwt;
	pdata->rx_usecs = rx_usecs;
	pdata->rx_frames = rx_frames;
	hw_ops->config_rx_coalesce(pdata);

	pdata->tx_frames = tx_frames;
	hw_ops->config_tx_coalesce(pdata);

	return 0;
}

static void gmac_ethtool_get_strings(struct net_device *netdev,
				     u32 stringset, u8 *data)
{
	int i;

	switch (stringset) {
	case ETH_SS_STATS:
		for (i = 0; i < GMAC_STATS_COUNT; i++) {
			memcpy(data, gmac_gstring_stats[i].stat_string,
			       ETH_GSTRING_LEN);
			data += ETH_GSTRING_LEN;
		}
		break;
	default:
		WARN_ON(1);
		break;
	}
}

static int gmac_ethtool_get_sset_count(struct net_device *netdev,
				       int stringset)
{
	int ret;

	switch (stringset) {
	case ETH_SS_STATS:
		ret = GMAC_STATS_COUNT;
		break;

	default:
		ret = -EOPNOTSUPP;
	}

	return ret;
}

static void gmac_ethtool_get_ethtool_stats(struct net_device *netdev,
					   struct ethtool_stats *stats,
					   u64 *data)
{
	struct gmac_pdata *pdata = netdev_priv(netdev);
	u8 *stat;
	int i;

	pdata->hw_ops.read_mmc_stats(pdata);
	for (i = 0; i < GMAC_STATS_COUNT; i++) {
		stat = (u8 *)pdata + gmac_gstring_stats[i].stat_offset;
		*data++ = *(u64 *)stat;
	}
}

static int gmac_get_ts_info(struct net_device *dev,
			    struct ethtool_ts_info *info)
{
	struct gmac_pdata *pdata = netdev_priv(dev);

	if (pdata->hw_feat.ts_src) {
		info->so_timestamping = SOF_TIMESTAMPING_TX_SOFTWARE |
					SOF_TIMESTAMPING_TX_HARDWARE |
					SOF_TIMESTAMPING_RX_SOFTWARE |
					SOF_TIMESTAMPING_RX_HARDWARE |
					SOF_TIMESTAMPING_SOFTWARE |
					SOF_TIMESTAMPING_RAW_HARDWARE;

		if (pdata->ptp_clock)
			info->phc_index = ptp_clock_index(pdata->ptp_clock);

		info->tx_types = (1 << HWTSTAMP_TX_OFF) | (1 << HWTSTAMP_TX_ON);

		info->rx_filters = ((1 << HWTSTAMP_FILTER_NONE) |
				    (1 << HWTSTAMP_FILTER_PTP_V1_L4_EVENT) |
				    (1 << HWTSTAMP_FILTER_PTP_V1_L4_SYNC) |
				    (1 << HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ) |
				    (1 << HWTSTAMP_FILTER_PTP_V2_L4_EVENT) |
				    (1 << HWTSTAMP_FILTER_PTP_V2_L4_SYNC) |
				    (1 << HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ) |
				    (1 << HWTSTAMP_FILTER_PTP_V2_EVENT) |
				    (1 << HWTSTAMP_FILTER_PTP_V2_SYNC) |
				    (1 << HWTSTAMP_FILTER_PTP_V2_DELAY_REQ) |
				    (1 << HWTSTAMP_FILTER_ALL));
		return 0;
	} else {
		return ethtool_op_get_ts_info(dev, info);
	}
}

static const struct ethtool_ops gmac_ethtool_ops = {
	.get_drvinfo = gmac_ethtool_get_drvinfo,
	.get_link = ethtool_op_get_link,
	.get_msglevel = gmac_ethtool_get_msglevel,
	.set_msglevel = gmac_ethtool_set_msglevel,
	.get_channels = gmac_ethtool_get_channels,
	.get_coalesce = gmac_ethtool_get_coalesce,
	.set_coalesce = gmac_ethtool_set_coalesce,
	.get_strings = gmac_ethtool_get_strings,
	.get_sset_count = gmac_ethtool_get_sset_count,
	.get_ethtool_stats = gmac_ethtool_get_ethtool_stats,
	.get_ts_info = gmac_get_ts_info,
};

const struct ethtool_ops *gmac_get_ethtool_ops(void)
{
	return &gmac_ethtool_ops;
}
