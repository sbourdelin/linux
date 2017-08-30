/**
 * drivers/net/ethernet/socionext/netsec/netsec_ethtool.c
 *
 *  Copyright (C) 2013-2014 Fujitsu Semiconductor Limited.
 *  Copyright (C) 2014 Linaro Ltd  Andy Green <andy.green@linaro.org>
 *  All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */

#include "netsec.h"

static void netsec_et_get_drvinfo(struct net_device *net_device,
				  struct ethtool_drvinfo *info)
{
	strlcpy(info->driver, "netsec", sizeof(info->driver));
	strlcpy(info->bus_info, dev_name(net_device->dev.parent),
		sizeof(info->bus_info));
}

static int netsec_et_get_coalesce(struct net_device *net_device,
				  struct ethtool_coalesce *et_coalesce)
{
	struct netsec_priv *priv = netdev_priv(net_device);

	*et_coalesce = priv->et_coalesce;

	return 0;
}

static int netsec_et_set_coalesce(struct net_device *net_device,
				  struct ethtool_coalesce *et_coalesce)
{
	struct netsec_priv *priv = netdev_priv(net_device);

	if (et_coalesce->rx_max_coalesced_frames > NETSEC_INT_PKTCNT_MAX)
		return -EINVAL;
	if (et_coalesce->tx_max_coalesced_frames > NETSEC_INT_PKTCNT_MAX)
		return -EINVAL;
	if (!et_coalesce->rx_max_coalesced_frames)
		return -EINVAL;
	if (!et_coalesce->tx_max_coalesced_frames)
		return -EINVAL;

	priv->et_coalesce = *et_coalesce;

	return 0;
}

static u32 netsec_et_get_msglevel(struct net_device *dev)
{
	struct netsec_priv *priv = netdev_priv(dev);

	return priv->msg_enable;
}

static void netsec_et_set_msglevel(struct net_device *dev, u32 datum)
{
	struct netsec_priv *priv = netdev_priv(dev);

	priv->msg_enable = datum;
}

const struct ethtool_ops netsec_ethtool_ops = {
	.get_drvinfo		= netsec_et_get_drvinfo,
	.get_link_ksettings	= phy_ethtool_get_link_ksettings,
	.set_link_ksettings	= phy_ethtool_set_link_ksettings,
	.get_link		= ethtool_op_get_link,
	.get_coalesce		= netsec_et_get_coalesce,
	.set_coalesce		= netsec_et_set_coalesce,
	.get_msglevel		= netsec_et_get_msglevel,
	.set_msglevel		= netsec_et_set_msglevel,
};
