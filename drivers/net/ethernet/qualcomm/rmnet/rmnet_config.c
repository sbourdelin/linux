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
 * RMNET configuration engine
 *
 */

#include <net/sock.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/netdevice.h>
#include "rmnet_config.h"
#include "rmnet_handlers.h"
#include "rmnet_vnd.h"
#include "rmnet_private.h"

/* Local Definitions and Declarations */
#define RMNET_LOCAL_LOGICAL_ENDPOINT -1

struct rmnet_free_work {
	struct work_struct work;
	struct net_device *rmnet_dev;
};

static int rmnet_is_real_dev_registered(const struct net_device *real_dev)
{
	rx_handler_func_t *rx_handler;

	rx_handler = rcu_dereference(real_dev->rx_handler);
	return (rx_handler == rmnet_rx_handler);
}

static struct rmnet_real_dev_info*
__rmnet_get_real_dev_info(const struct net_device *real_dev)
{
	if (rmnet_is_real_dev_registered(real_dev))
		return (struct rmnet_real_dev_info *)
			rcu_dereference(real_dev->rx_handler_data);
	else
		return 0;
}

static struct rmnet_endpoint*
rmnet_get_endpoint(struct net_device *dev, int config_id)
{
	struct rmnet_real_dev_info *rdinfo;
	struct rmnet_endpoint *ep;

	if (!rmnet_is_real_dev_registered(dev)) {
		ep = rmnet_vnd_get_endpoint(dev);
	} else {
		rdinfo = __rmnet_get_real_dev_info(dev);

		if (!rdinfo)
			return NULL;

		if (config_id == RMNET_LOCAL_LOGICAL_ENDPOINT)
			ep = &rdinfo->local_ep;
		else
			ep = &rdinfo->muxed_ep[config_id];
	}

	return ep;
}

static int rmnet_unregister_real_device(struct net_device *real_dev)
{
	struct rmnet_real_dev_info *rdinfo;
	struct list_head *iter;

	ASSERT_RTNL();

	if (!rmnet_is_real_dev_registered(real_dev) ||
	    netdev_lower_get_next(real_dev, &iter))
		return -EINVAL;

	rdinfo = __rmnet_get_real_dev_info(real_dev);
	kfree(rdinfo);

	netdev_rx_handler_unregister(real_dev);

	/* release reference on real_dev */
	dev_put(real_dev);

	netdev_info(real_dev, "Removed from rmnet\n");
	return 0;
}

static int rmnet_register_real_device(struct net_device *real_dev)
{
	struct rmnet_real_dev_info *rdinfo;
	int rc;

	ASSERT_RTNL();

	if (rmnet_is_real_dev_registered(real_dev))
		return -EINVAL;

	rdinfo = kzalloc(sizeof(*rdinfo), GFP_ATOMIC);
	if (!rdinfo)
		return -ENOMEM;

	rdinfo->dev = real_dev;
	rc = netdev_rx_handler_register(real_dev, rmnet_rx_handler, rdinfo);

	if (rc) {
		kfree(rdinfo);
		return -EBUSY;
	}

	/* hold on to real dev for MAP data */
	dev_hold(real_dev);

	netdev_info(real_dev, "registered with rmnet\n");
	return 0;
}

static int rmnet_set_ingress_data_format(struct net_device *dev, u32 idf)
{
	struct rmnet_real_dev_info *rdinfo;

	ASSERT_RTNL();

	netdev_info(dev, "Ingress format 0x%08X\n", idf);

	rdinfo = __rmnet_get_real_dev_info(dev);
	if (!rdinfo)
		return -EINVAL;

	rdinfo->ingress_data_format = idf;

	return 0;
}

static int rmnet_set_egress_data_format(struct net_device *dev, u32 edf,
					u16 agg_size, u16 agg_count)
{
	struct rmnet_real_dev_info *rdinfo;

	ASSERT_RTNL();

	netdev_info(dev, "Egress format 0x%08X agg size %d cnt %d\n",
		    edf, agg_size, agg_count);

	rdinfo = __rmnet_get_real_dev_info(dev);
	if (!rdinfo)
		return -EINVAL;

	rdinfo->egress_data_format = edf;

	return 0;
}

static int __rmnet_set_endpoint_config(struct net_device *dev, int config_id,
				       struct rmnet_endpoint *ep)
{
	struct rmnet_endpoint *dev_ep;

	ASSERT_RTNL();

	dev_ep = rmnet_get_endpoint(dev, config_id);

	if (!dev_ep)
		return -EINVAL;

	memcpy(dev_ep, ep, sizeof(struct rmnet_endpoint));
	if (config_id == RMNET_LOCAL_LOGICAL_ENDPOINT)
		dev_ep->mux_id = 0;
	else
		dev_ep->mux_id = config_id;

	return 0;
}

static int __rmnet_unset_endpoint_config(struct net_device *dev, int config_id)
{
	struct rmnet_endpoint *ep = 0;

	ASSERT_RTNL();

	ep = rmnet_get_endpoint(dev, config_id);
	if (!ep)
		return -EINVAL;

	memset(ep, 0, sizeof(struct rmnet_endpoint));

	return 0;
}

static int rmnet_set_endpoint_config(struct net_device *dev,
				     int config_id, u8 rmnet_mode,
				     struct net_device *egress_dev)
{
	struct rmnet_endpoint ep;

	netdev_info(dev, "id %d mode %d dev %s\n",
		    config_id, rmnet_mode, egress_dev->name);

	if (config_id < RMNET_LOCAL_LOGICAL_ENDPOINT ||
	    config_id >= RMNET_MAX_LOGICAL_EP)
		return -EINVAL;

	memset(&ep, 0, sizeof(struct rmnet_endpoint));
	ep.rmnet_mode = rmnet_mode;
	ep.egress_dev = egress_dev;

	return __rmnet_set_endpoint_config(dev, config_id, &ep);
}

static int rmnet_unset_endpoint_config(struct net_device *dev, int config_id)
{
	netdev_info(dev, "id %d\n", config_id);

	if (config_id < RMNET_LOCAL_LOGICAL_ENDPOINT ||
	    config_id >= RMNET_MAX_LOGICAL_EP)
		return -EINVAL;

	return __rmnet_unset_endpoint_config(dev, config_id);
}

static int rmnet_newlink(struct net *src_net, struct net_device *dev,
			 struct nlattr *tb[], struct nlattr *data[],
			 struct netlink_ext_ack *extack)
{
	int ingress_format = RMNET_INGRESS_FORMAT_DEMUXING |
			     RMNET_INGRESS_FORMAT_DEAGGREGATION |
			     RMNET_INGRESS_FORMAT_MAP;
	int egress_format = RMNET_EGRESS_FORMAT_MUXING |
			    RMNET_EGRESS_FORMAT_MAP;
	struct net_device *real_dev;
	int mode = RMNET_EPMODE_VND;
	u16 mux_id;

	real_dev = __dev_get_by_index(src_net, nla_get_u32(tb[IFLA_LINK]));
	if (!real_dev || !dev)
		return -ENODEV;

	if (!data[IFLA_VLAN_ID])
		return -EINVAL;

	mux_id = nla_get_u16(data[IFLA_VLAN_ID]);

	rmnet_register_real_device(real_dev);

	if (rmnet_vnd_newlink(real_dev, mux_id, dev))
		return -EINVAL;

	rmnet_set_egress_data_format(real_dev, egress_format, 0, 0);
	rmnet_set_ingress_data_format(real_dev, ingress_format);
	rmnet_set_endpoint_config(real_dev, mux_id, mode, dev);
	rmnet_set_endpoint_config(dev, mux_id, mode, real_dev);
	netdev_master_upper_dev_link(dev, real_dev, NULL, NULL);
	return 0;
}

static void rmnet_delink(struct net_device *dev, struct list_head *head)
{
	struct net_device *real_dev;
	int mux_id;

	real_dev = netdev_master_upper_dev_get_rcu(dev);
	if (real_dev) {
		mux_id = rmnet_vnd_get_mux(real_dev, dev);

		/* rmnet_vnd_get_mux() gives mux_id + 1,
		 * so subtract 1 to get the correct mux_id
		 */
		mux_id--;
		rmnet_unset_endpoint_config(real_dev, mux_id);
		rmnet_unset_endpoint_config(dev, mux_id);
		rmnet_vnd_remove_ref_dev(real_dev, mux_id);
		netdev_upper_dev_unlink(dev, real_dev);
		rmnet_unregister_real_device(real_dev);
	}

	unregister_netdevice_queue(dev, head);
}

static void rmnet_free_later(struct work_struct *work)
{
	struct rmnet_free_work *fwork;

	fwork = container_of(work, struct rmnet_free_work, work);

	rtnl_lock();
	rmnet_delink(fwork->rmnet_dev, NULL);
	rtnl_unlock();

	kfree(fwork);
}

static int rmnet_dev_walk(struct net_device *lower_dev, void *data)
{
	struct net_device *real_dev = data;
	struct rmnet_free_work *vnd_work;
	int rc = 0;

	netdev_upper_dev_unlink(lower_dev, real_dev);

	vnd_work = kzalloc(sizeof(*vnd_work), GFP_KERNEL);
	if (!vnd_work)
		return -ENOMEM;

	INIT_WORK(&vnd_work->work, rmnet_free_later);
	vnd_work->rmnet_dev = lower_dev;
	schedule_work(&vnd_work->work);

	return rc;
}

static void rmnet_force_unassociate_device(struct net_device *dev)
{
	struct net_device *real_dev = dev;

	if (!rmnet_is_real_dev_registered(real_dev))
		return;

	netdev_walk_all_lower_dev(real_dev, rmnet_dev_walk, real_dev);
	rmnet_unregister_real_device(real_dev);
}

static int rmnet_config_notify_cb(struct notifier_block *nb,
				  unsigned long event, void *data)
{
	struct net_device *dev = netdev_notifier_info_to_dev(data);

	if (!dev)
		return NOTIFY_DONE;

	switch (event) {
	case NETDEV_UNREGISTER_FINAL:
	case NETDEV_UNREGISTER:
		netdev_info(dev, "Kernel unregister\n");
		rmnet_force_unassociate_device(dev);
		break;

	default:
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block rmnet_dev_notifier __read_mostly = {
	.notifier_call = rmnet_config_notify_cb,
};

static int rmnet_rtnl_validate(struct nlattr *tb[], struct nlattr *data[],
			       struct netlink_ext_ack *extack)
{
	u16 mux_id;

	if (!data || !data[IFLA_VLAN_ID])
		return -EINVAL;

	mux_id = nla_get_u16(data[IFLA_VLAN_ID]);
	if (!mux_id || mux_id > (RMNET_MAX_LOGICAL_EP - 1))
		return -ERANGE;

	return 0;
}

static size_t rmnet_get_size(const struct net_device *dev)
{
	return nla_total_size(2); /* IFLA_VLAN_ID */
}

struct rtnl_link_ops rmnet_link_ops __read_mostly = {
	.kind		= "rmnet",
	.maxtype	= __IFLA_VLAN_MAX,
	.priv_size	= sizeof(struct rmnet_priv),
	.setup		= rmnet_vnd_setup,
	.validate	= rmnet_rtnl_validate,
	.newlink	= rmnet_newlink,
	.dellink	= rmnet_delink,
	.get_size	= rmnet_get_size,
};

struct rmnet_real_dev_info*
rmnet_get_real_dev_info(struct net_device *real_dev)
{
	return __rmnet_get_real_dev_info(real_dev);
}

/* Startup/Shutdown */

static int __init rmnet_init(void)
{
	int rc;

	rc = register_netdevice_notifier(&rmnet_dev_notifier);
	if (rc != 0)
		return rc;

	rc = rtnl_link_register(&rmnet_link_ops);
	if (rc != 0) {
		unregister_netdevice_notifier(&rmnet_dev_notifier);
		return rc;
	}
	return rc;
}

static void __exit rmnet_exit(void)
{
	unregister_netdevice_notifier(&rmnet_dev_notifier);
	rtnl_link_unregister(&rmnet_link_ops);
}

module_init(rmnet_init)
module_exit(rmnet_exit)
MODULE_LICENSE("GPL v2");
