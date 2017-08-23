/* Copyright (c) 2013-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 *
 * RMNET Data virtual network driver
 *
 */

#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <net/pkt_sched.h>
#include "rmnet_config.h"
#include "rmnet_handlers.h"
#include "rmnet_private.h"
#include "rmnet_map.h"
#include "rmnet_vnd.h"

/* RX/TX Fixup */

void rmnet_vnd_rx_fixup(struct sk_buff *skb, struct net_device *dev)
{
	dev->stats.rx_packets++;
	dev->stats.rx_bytes += skb->len;
}

void rmnet_vnd_tx_fixup(struct sk_buff *skb, struct net_device *dev)
{
	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;
}

/* Network Device Operations */

static netdev_tx_t rmnet_vnd_start_xmit(struct sk_buff *skb,
					struct net_device *dev)
{
	struct rmnet_priv *priv;

	priv = netdev_priv(dev);
	if (priv->local_ep.egress_dev) {
		rmnet_egress_handler(skb, &priv->local_ep);
	} else {
		dev->stats.tx_dropped++;
		kfree_skb(skb);
	}
	return NETDEV_TX_OK;
}

static int rmnet_vnd_change_mtu(struct net_device *rmnet_dev, int new_mtu)
{
	if (new_mtu < 0 || new_mtu > RMNET_MAX_PACKET_SIZE)
		return -EINVAL;

	rmnet_dev->mtu = new_mtu;
	return 0;
}

static const struct net_device_ops rmnet_vnd_ops = {
	.ndo_start_xmit = rmnet_vnd_start_xmit,
	.ndo_change_mtu = rmnet_vnd_change_mtu,
};

/* Called by kernel whenever a new rmnet<n> device is created. Sets MTU,
 * flags, ARP type, needed headroom, etc...
 */
void rmnet_vnd_setup(struct net_device *rmnet_dev)
{
	struct rmnet_priv *priv;

	priv = netdev_priv(rmnet_dev);
	netdev_dbg(rmnet_dev, "Setting up device %s\n", rmnet_dev->name);

	rmnet_dev->netdev_ops = &rmnet_vnd_ops;
	rmnet_dev->mtu = RMNET_DFLT_PACKET_SIZE;
	rmnet_dev->needed_headroom = RMNET_NEEDED_HEADROOM;
	random_ether_addr(rmnet_dev->dev_addr);
	rmnet_dev->tx_queue_len = RMNET_TX_QUEUE_LEN;

	/* Raw IP mode */
	rmnet_dev->header_ops = NULL;  /* No header */
	rmnet_dev->type = ARPHRD_RAWIP;
	rmnet_dev->hard_header_len = 0;
	rmnet_dev->flags &= ~(IFF_BROADCAST | IFF_MULTICAST);

	rmnet_dev->needs_free_netdev = true;
}

/* Exposed API */

int rmnet_vnd_newlink(struct net_device *real_dev, int id,
		      struct net_device *rmnet_dev)
{
	struct rmnet_real_dev_info *rdinfo;
	int rc;

	rdinfo = rmnet_get_real_dev_info(real_dev);

	if (rdinfo->rmnet_devices[id])
		return -EINVAL;

	rc = register_netdevice(rmnet_dev);
	if (!rc) {
		rdinfo->rmnet_devices[id] = rmnet_dev;
		rmnet_dev->rtnl_link_ops = &rmnet_link_ops;
	}
	return rc;
}

/* Unregisters the virtual network device node and frees it.
 * unregister_netdev locks the rtnl mutex, so the mutex must not be locked
 * by the caller of the function. unregister_netdev enqueues the request to
 * unregister the device into a TODO queue. The requests in the TODO queue
 * are only done after rtnl mutex is unlocked, therefore free_netdev has to
 * called after unlocking rtnl mutex.
 */
int rmnet_vnd_free_dev(struct net_device *real_dev, int id)
{
	struct rmnet_real_dev_info *rdinfo;
	struct net_device *rmnet_dev;
	struct rmnet_endpoint *ep;

	rdinfo = rmnet_get_real_dev_info(real_dev);

	rtnl_lock();
	if (id < 0 || id >= RMNET_MAX_VND || !rdinfo->rmnet_devices[id]) {
		rtnl_unlock();
		return -EINVAL;
	}

	ep = rmnet_vnd_get_endpoint(rdinfo->rmnet_devices[id]);
	if (ep) {
		rtnl_unlock();
		return -EINVAL;
	}

	rmnet_dev = rdinfo->rmnet_devices[id];
	rdinfo->rmnet_devices[id] = NULL;
	rtnl_unlock();

	if (rmnet_dev) {
		unregister_netdev(rmnet_dev);
		free_netdev(rmnet_dev);
		return 0;
	} else {
		return -EINVAL;
	}
}

int rmnet_vnd_remove_ref_dev(struct net_device *real_dev, int id)
{
	struct rmnet_real_dev_info *rdinfo;
	struct rmnet_endpoint *ep;

	rdinfo = rmnet_get_real_dev_info(real_dev);
	if (id < 0 || id >= RMNET_MAX_VND || !rdinfo->rmnet_devices[id])
		return -EINVAL;

	ep = rmnet_vnd_get_endpoint(rdinfo->rmnet_devices[id]);
	rdinfo->rmnet_devices[id] = NULL;
	return 0;
}

/* Searches through list of known RmNet virtual devices. This function is O(n)
 * and should not be used in the data path.
 *
 * To get the read id, subtract this result by 1.
 */
int rmnet_vnd_get_mux(struct net_device *real_dev,
		      struct net_device *rmnet_dev)
{
	/* This is not an efficient search, but, this will only be called in
	 * a configuration context, and the list is small.
	 */
	struct rmnet_real_dev_info *rdinfo;
	int i;

	rdinfo = rmnet_get_real_dev_info(real_dev);

	if (!rmnet_dev)
		return 0;

	for (i = 0; i < RMNET_MAX_VND; i++)
		if (rmnet_dev == rdinfo->rmnet_devices[i])
			return i + 1;

	return 0;
}

/* Gets the logical endpoint configuration for a RmNet virtual network device
 * node. Caller should confirm that devices is a RmNet VND before calling.
 */
struct rmnet_endpoint *rmnet_vnd_get_endpoint(struct net_device *rmnet_dev)
{
	struct rmnet_priv *priv;

	if (!rmnet_dev)
		return NULL;

	priv = netdev_priv(rmnet_dev);

	return &priv->local_ep;
}

int rmnet_vnd_do_flow_control(struct net_device *rmnet_dev, int enable)
{
	netdev_dbg(rmnet_dev, "Setting VND TX queue state to %d\n", enable);
	/* Although we expect similar number of enable/disable
	 * commands, optimize for the disable. That is more
	 * latency sensitive than enable
	 */
	if (unlikely(enable))
		netif_wake_queue(rmnet_dev);
	else
		netif_stop_queue(rmnet_dev);

	return 0;
}

struct net_device *rmnet_vnd_get_by_id(struct net_device *real_dev, int id)
{
	struct rmnet_real_dev_info *rdinfo;

	rdinfo = rmnet_get_real_dev_info(real_dev);

	if (id < 0 || id >= RMNET_MAX_VND)
		return NULL;

	return rdinfo->rmnet_devices[id];
}
