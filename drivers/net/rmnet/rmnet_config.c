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
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/rmnet.h>
#include "rmnet_config.h"
#include "rmnet_handlers.h"
#include "rmnet_vnd.h"
#include "rmnet_private.h"

RMNET_LOG_MODULE(RMNET_LOGMASK_CONFIG);

/* Local Definitions and Declarations */
#define RMNET_LOCAL_LOGICAL_ENDPOINT -1

/* _rmnet_is_physical_endpoint_associated() - Determines if device is associated
 * @dev:      Device to get check
 *
 * Compares device rx_handler callback pointer against known function
 */
static inline int _rmnet_is_physical_endpoint_associated(struct net_device *dev)
{
	rx_handler_func_t *rx_handler;

	rx_handler = rcu_dereference(dev->rx_handler);

	if (rx_handler == rmnet_rx_handler)
		return 1;
	else
		return 0;
}

/* _rmnet_get_phys_ep_config() - Get physical ep config for an associated device
 * @dev:      Device to get endpoint configuration from
 */
static inline struct rmnet_phys_ep_conf_s *_rmnet_get_phys_ep_config
						(struct net_device *dev)
{
	if (_rmnet_is_physical_endpoint_associated(dev))
		return (struct rmnet_phys_ep_conf_s *)
			rcu_dereference(dev->rx_handler_data);
	else
		return 0;
}

struct rmnet_free_vnd_work {
	struct work_struct work;
	int vnd_id[RMNET_MAX_VND];
	int count;
};

/* _rmnet_get_logical_ep() - Gets the logical end point configuration
 * structure for a network device
 * @dev:             Device to get endpoint configuration from
 * @config_id:       Logical endpoint id on device
 * Retrieves the logical_endpoint_config structure.
 */
static struct rmnet_logical_ep_conf_s *_rmnet_get_logical_ep
	(struct net_device *dev, int config_id)
{
	struct rmnet_phys_ep_conf_s *config;
	struct rmnet_logical_ep_conf_s *epconfig_l;

	if (rmnet_vnd_is_vnd(dev)) {
		epconfig_l = rmnet_vnd_get_le_config(dev);
	} else {
		config = _rmnet_get_phys_ep_config(dev);

		if (!config)
			return NULL;

		if (config_id == RMNET_LOCAL_LOGICAL_ENDPOINT)
			epconfig_l = &config->local_ep;
		else
			epconfig_l = &config->muxed_ep[config_id];
	}

	return epconfig_l;
}

/* rmnet_unassociate_network_device() - Unassociate network device
 * @dev:      Device to unassociate
 *
 * Frees all structures generate for device. Unregisters rx_handler
 */
static int rmnet_unassociate_network_device(struct net_device *dev)
{
	struct rmnet_phys_ep_conf_s *config;
	int config_id = RMNET_LOCAL_LOGICAL_ENDPOINT;
	struct rmnet_logical_ep_conf_s *epconfig_l;

	ASSERT_RTNL();

	LOGL("(%s);", dev->name);

	if (!dev || !_rmnet_is_physical_endpoint_associated(dev))
		return -EINVAL;

	for (; config_id < RMNET_MAX_LOGICAL_EP; config_id++) {
		epconfig_l = _rmnet_get_logical_ep(dev, config_id);
		if (epconfig_l && epconfig_l->refcount)
			return -EINVAL;
	}

	config = (struct rmnet_phys_ep_conf_s *)
		  rcu_dereference(dev->rx_handler_data);

	if (!config)
		return -EINVAL;

	kfree(config);

	netdev_rx_handler_unregister(dev);

	dev_put(dev);
	return 0;
}

/* rmnet_set_ingress_data_format() - Set ingress data format on network device
 * @dev:                 Device to ingress data format on
 * @egress_data_format:  32-bit unsigned bitmask of ingress format
 *
 * Network device must already have association with RmNet Data driver
 */
static int rmnet_set_ingress_data_format(struct net_device *dev,
					 u32 ingress_data_format)
{
	struct rmnet_phys_ep_conf_s *config;

	ASSERT_RTNL();

	LOGL("(%s,0x%08X);", dev->name, ingress_data_format);

	if (!dev)
		return -EINVAL;

	config = _rmnet_get_phys_ep_config(dev);
	if (!config)
		return -EINVAL;

	config->ingress_data_format = ingress_data_format;

	return 0;
}

/* rmnet_set_egress_data_format() - Set egress data format on network device
 * @dev:                 Device to egress data format on
 * @egress_data_format:  32-bit unsigned bitmask of egress format
 *
 * Network device must already have association with RmNet Data driver
 */
static int rmnet_set_egress_data_format(struct net_device *dev,
					u32 egress_data_format,
					u16 agg_size,
					u16 agg_count)
{
	struct rmnet_phys_ep_conf_s *config;

	ASSERT_RTNL();

	LOGL("(%s,0x%08X, %d, %d);",
	     dev->name, egress_data_format, agg_size, agg_count);

	if (!dev)
		return -EINVAL;

	config = _rmnet_get_phys_ep_config(dev);
	if (!config)
		return -EINVAL;

	config->egress_data_format = egress_data_format;

	return 0;
}

/* rmnet_associate_network_device() - Associate network device
 * @dev:      Device to register with RmNet data
 *
 * Typically used on physical network devices. Registers RX handler and private
 * metadata structures.
 */
static int rmnet_associate_network_device(struct net_device *dev)
{
	struct rmnet_phys_ep_conf_s *config;
	int rc;

	ASSERT_RTNL();

	LOGL("(%s);\n", dev->name);

	if (!dev || _rmnet_is_physical_endpoint_associated(dev) ||
	    rmnet_vnd_is_vnd(dev)) {
		LOGM("cannot register with this dev");
		return -EINVAL;
	}

	config = kmalloc(sizeof(*config), GFP_ATOMIC);
	if (!config)
		return -ENOMEM;

	memset(config, 0, sizeof(struct rmnet_phys_ep_conf_s));
	config->dev = dev;

	rc = netdev_rx_handler_register(dev, rmnet_rx_handler, config);

	if (rc) {
		LOGM("netdev_rx_handler_register returns %d", rc);
		kfree(config);
		return -EBUSY;
	}

	dev_hold(dev);
	return 0;
}

/* __rmnet_set_logical_endpoint_config() - Set logical endpoing config on device
 * @dev:         Device to set endpoint configuration on
 * @config_id:   logical endpoint id on device
 * @epconfig:    endpoint configuration structure to set
 */
static int __rmnet_set_logical_endpoint_config
	(struct net_device *dev,
	 int config_id,
	 struct rmnet_logical_ep_conf_s *epconfig)
{
	struct rmnet_logical_ep_conf_s *epconfig_l;

	ASSERT_RTNL();

	if (!dev || config_id < RMNET_LOCAL_LOGICAL_ENDPOINT ||
	    config_id >= RMNET_MAX_LOGICAL_EP)
		return -EINVAL;

	epconfig_l = _rmnet_get_logical_ep(dev, config_id);

	if (!epconfig_l || epconfig_l->refcount)
		return -EINVAL;

	memcpy(epconfig_l, epconfig, sizeof(struct rmnet_logical_ep_conf_s));
	if (config_id == RMNET_LOCAL_LOGICAL_ENDPOINT)
		epconfig_l->mux_id = 0;
	else
		epconfig_l->mux_id = config_id;

	/* Explicitly hold a reference to the egress device */
	dev_hold(epconfig_l->egress_dev);
	return 0;
}

/* _rmnet_unset_logical_endpoint_config() - Un-set the logical endpoing config
 * on device
 * @dev:         Device to set endpoint configuration on
 * @config_id:   logical endpoint id on device
 */
static int _rmnet_unset_logical_endpoint_config(struct net_device *dev,
						int config_id)
{
	struct rmnet_logical_ep_conf_s *epconfig_l = 0;

	ASSERT_RTNL();

	if (!dev || config_id < RMNET_LOCAL_LOGICAL_ENDPOINT ||
	    config_id >= RMNET_MAX_LOGICAL_EP)
		return -EINVAL;

	epconfig_l = _rmnet_get_logical_ep(dev, config_id);

	if (!epconfig_l || !epconfig_l->refcount)
		return -EINVAL;

	/* Explicitly release the reference from the egress device */
	dev_put(epconfig_l->egress_dev);
	memset(epconfig_l, 0, sizeof(struct rmnet_logical_ep_conf_s));

	return 0;
}

/* rmnet_set_logical_endpoint_config() - Set logical endpoint config on a device
 * @dev:            Device to set endpoint configuration on
 * @config_id:      logical endpoint id on device
 * @rmnet_mode:     endpoint mode. Values from: rmnet_config_endpoint_modes_e
 * @egress_device:  device node to forward packet to once done processing in
 *                  ingress/egress handlers
 *
 * Creates a logical_endpoint_config structure and fills in the information from
 * function arguments. Calls __rmnet_set_logical_endpoint_config() to finish
 * configuration. Network device must already have association with RmNet Data
 * driver
 */
static int rmnet_set_logical_endpoint_config(struct net_device *dev,
					     int config_id,
					     u8 rmnet_mode,
					     struct net_device *egress_dev)
{
	struct rmnet_logical_ep_conf_s epconfig;

	LOGL("(%s, %d, %d, %s);",
	     dev->name, config_id, rmnet_mode, egress_dev->name);

	if (!egress_dev ||
	    ((!_rmnet_is_physical_endpoint_associated(egress_dev)) &&
	    (!rmnet_vnd_is_vnd(egress_dev)))) {
		return -EINVAL;
	}

	memset(&epconfig, 0, sizeof(struct rmnet_logical_ep_conf_s));
	epconfig.refcount = 1;
	epconfig.rmnet_mode = rmnet_mode;
	epconfig.egress_dev = egress_dev;

	return __rmnet_set_logical_endpoint_config(dev, config_id, &epconfig);
}

/* rmnet_unset_logical_endpoint_config() - Un-set logical endpoing configuration
 * on a device
 * @dev:            Device to set endpoint configuration on
 * @config_id:      logical endpoint id on device
 *
 * Retrieves the logical_endpoint_config structure and frees the egress device.
 * Network device must already have association with RmNet Data driver
 */
static int rmnet_unset_logical_endpoint_config(struct net_device *dev,
					       int config_id)
{
	LOGL("(%s, %d);", dev->name, config_id);

	if (!dev || ((!_rmnet_is_physical_endpoint_associated(dev)) &&
		     (!rmnet_vnd_is_vnd(dev)))) {
		return -EINVAL;
	}

	return _rmnet_unset_logical_endpoint_config(dev, config_id);
}

/* rmnet_free_vnd() - Free virtual network device node
 * @id:       RmNet virtual device node id
 */
int rmnet_free_vnd(int id)
{
	LOGL("(%d);", id);
	return rmnet_vnd_free_dev(id);
}

static void _rmnet_free_vnd_later(struct work_struct *work)
{
	int i;
	struct rmnet_free_vnd_work *fwork;

	fwork = container_of(work, struct rmnet_free_vnd_work, work);

	for (i = 0; i < fwork->count; i++)
		rmnet_free_vnd(fwork->vnd_id[i]);
	kfree(fwork);
}

/* rmnet_force_unassociate_device() - Force a device to unassociate
 * @dev:       Device to unassociate
 */
static void rmnet_force_unassociate_device(struct net_device *dev)
{
	int i, j;
	struct net_device *vndev;
	struct rmnet_phys_ep_conf_s *config;
	struct rmnet_logical_ep_conf_s *cfg;
	struct rmnet_free_vnd_work *vnd_work;

	ASSERT_RTNL();
	if (!dev)
		return;

	if (!_rmnet_is_physical_endpoint_associated(dev)) {
		LOGM("%s", "Called on unassociated device, skipping");
		return;
	}

	vnd_work = kmalloc(sizeof(*vnd_work), GFP_KERNEL);
	if (!vnd_work)
		return;

	INIT_WORK(&vnd_work->work, _rmnet_free_vnd_later);
	vnd_work->count = 0;

	/* Check the VNDs for offending mappings */
	for (i = 0, j = 0; i < RMNET_MAX_VND &&
	     j < RMNET_MAX_VND; i++) {
		vndev = rmnet_vnd_get_by_id(i);
		if (!vndev) {
			LOGL("VND %d not in use; skipping", i);
			continue;
		}
		cfg = rmnet_vnd_get_le_config(vndev);
		if (!cfg) {
			LOGD("Got NULL config from VND %d", i);
			continue;
		}
		if (cfg->refcount && (cfg->egress_dev == dev)) {
			/* Make sure the device is down before clearing any of
			 * the mappings. Otherwise we could see a potential
			 * race condition if packets are actively being
			 * transmitted.
			 */
			dev_close(vndev);
			rmnet_unset_logical_endpoint_config
				(vndev, RMNET_LOCAL_LOGICAL_ENDPOINT);
			vnd_work->vnd_id[j] = i;
			j++;
		}
	}
	if (j > 0) {
		vnd_work->count = j;
		schedule_work(&vnd_work->work);
	} else {
		kfree(vnd_work);
	}

	config = _rmnet_get_phys_ep_config(dev);

	if (config) {
		cfg = &config->local_ep;

		if (cfg && cfg->refcount)
			rmnet_unset_logical_endpoint_config
			(cfg->egress_dev, RMNET_LOCAL_LOGICAL_ENDPOINT);
	}

	/* Clear the mappings on the phys ep */
	rmnet_unset_logical_endpoint_config(dev, RMNET_LOCAL_LOGICAL_ENDPOINT);
	for (i = 0; i < RMNET_MAX_LOGICAL_EP; i++)
		rmnet_unset_logical_endpoint_config(dev, i);
	rmnet_unassociate_network_device(dev);
}

/* rmnet_config_notify_cb() - Callback for netdevice notifier chain
 * @nb:       Notifier block data
 * @event:    Netdevice notifier event ID
 * @data:     Contains a net device for which we are getting notified
 */
static int rmnet_config_notify_cb(struct notifier_block *nb,
				  unsigned long event, void *data)
{
	struct net_device *dev = netdev_notifier_info_to_dev(data);

	if (!dev)
		return NOTIFY_DONE;

	switch (event) {
	case NETDEV_UNREGISTER_FINAL:
	case NETDEV_UNREGISTER:
		LOGM("Kernel is trying to unregister %s", dev->name);
		rmnet_force_unassociate_device(dev);
		break;

	default:
		LOGD("Unhandeled event [%lu]", event);
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block rmnet_dev_notifier = {
	.notifier_call = rmnet_config_notify_cb,
	.next = 0,
	.priority = 0
};

static int rmnet_newlink(struct net *src_net, struct net_device *dev,
			 struct nlattr *tb[], struct nlattr *data[])
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
	if (!real_dev)
		return -ENODEV;

	if (!data[IFLA_RMNET_MUX_ID])
		return -EINVAL;

	mux_id = nla_get_u16(data[IFLA_VLAN_ID]);
	if (rmnet_vnd_newlink(mux_id, dev))
		return -EINVAL;

	rmnet_associate_network_device(real_dev);
	rmnet_set_egress_data_format(real_dev, egress_format, 0, 0);
	rmnet_set_ingress_data_format(real_dev, ingress_format);
	rmnet_set_logical_endpoint_config(real_dev, mux_id, mode, dev);
	rmnet_set_logical_endpoint_config(dev, mux_id, mode, real_dev);
	return 0;
}

static void rmnet_delink(struct net_device *dev, struct list_head *head)
{
	struct rmnet_logical_ep_conf_s *cfg;
	int mux_id;

	mux_id = rmnet_vnd_is_vnd(dev);
	if (!mux_id)
		return;

/* rmnet_vnd_is_vnd() gives mux_id + 1, so subtract 1 to get the correct mux_id
 */
	mux_id--;
	cfg = rmnet_vnd_get_le_config(dev);

	if (cfg && cfg->refcount) {
		_rmnet_unset_logical_endpoint_config(cfg->egress_dev, mux_id);
		_rmnet_unset_logical_endpoint_config(dev, mux_id);
		rmnet_vnd_remove_ref_dev(mux_id);
		rmnet_unassociate_network_device(cfg->egress_dev);
	}

	unregister_netdevice_queue(dev, head);
}

static int rmnet_rtnl_validate(struct nlattr *tb[], struct nlattr *data[])
{
	u16 mux_id;

	if (!data || !data[IFLA_RMNET_MUX_ID])
		return -EINVAL;

	mux_id = nla_get_u16(data[IFLA_RMNET_MUX_ID]);
	if (!mux_id || mux_id > (RMNET_MAX_LOGICAL_EP - 1))
		return -ERANGE;

	return 0;
}

static size_t rmnet_get_size(const struct net_device *dev)
{
	return nla_total_size(2); /* IFLA_RMNET_MUX_ID */
}

struct rtnl_link_ops rmnet_link_ops __read_mostly = {
	.kind		= "rmnet",
	.maxtype	= __IFLA_RMNET_MAX,
	.priv_size	= sizeof(struct rmnet_vnd_private_s),
	.setup		= rmnet_vnd_setup,
	.validate	= rmnet_rtnl_validate,
	.newlink	= rmnet_newlink,
	.dellink	= rmnet_delink,
	.get_size	= rmnet_get_size,
};

int rmnet_config_init(void)
{
	int rc;

	rc = register_netdevice_notifier(&rmnet_dev_notifier);
	if (rc != 0) {
		LOGE("Failed to register device notifier; rc=%d", rc);
		return rc;
	}

	rc = rtnl_link_register(&rmnet_link_ops);
	if (rc != 0) {
		unregister_netdevice_notifier(&rmnet_dev_notifier);
		LOGE("Failed to register netlink handler; rc=%d", rc);
		return rc;
	}
	return rc;
}

void rmnet_config_exit(void)
{
	unregister_netdevice_notifier(&rmnet_dev_notifier);
	rtnl_link_unregister(&rmnet_link_ops);
}
