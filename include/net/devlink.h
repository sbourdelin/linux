/*
 * include/net/devlink.h - Network physical device Netlink interface
 * Copyright (c) 2016 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2016 Jiri Pirko <jiri@mellanox.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef _NET_DEVLINK_H_
#define _NET_DEVLINK_H_

#include <linux/device.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/list.h>
#include <linux/netdevice.h>
#include <net/net_namespace.h>
#include <uapi/linux/devlink.h>

struct devlink_ops;

struct devlink {
	struct list_head list;
	struct list_head port_list;
	int index;
	const struct devlink_ops *ops;
	struct device dev;
	possible_net_t _net;
	char priv[0] __aligned(NETDEV_ALIGN);
};

struct devlink_port {
	struct list_head list;
	struct devlink *devlink;
	unsigned index;
	enum devlink_port_type type;
	enum devlink_port_type desired_type;
	void *type_dev;
};

struct devlink_ops {
	size_t priv_size;
	int (*port_type_set)(struct devlink_port *devlink_port,
			     enum devlink_port_type port_type);
};

static inline void *devlink_priv(struct devlink *devlink)
{
	BUG_ON(!devlink);
	return &devlink->priv;
}

static inline struct devlink *priv_to_devlink(void *priv)
{
	BUG_ON(!priv);
	return container_of(priv, struct devlink, priv);
}

static inline struct device *devlink_dev(struct devlink *devlink)
{
	return &devlink->dev;
}

static inline void set_devlink_dev(struct devlink *devlink, struct device *dev)
{
	devlink->dev.parent = dev;
}

static inline const char *devlink_name(const struct devlink *devlink)
{
	return dev_name(&devlink->dev);
}

struct ib_device;

#if IS_ENABLED(CONFIG_NET_DEVLINK)

struct devlink *devlink_alloc(const struct devlink_ops *ops, size_t priv_size);
int devlink_register(struct devlink *devlink);
void devlink_unregister(struct devlink *devlink);
void devlink_free(struct devlink *devlink);
void devlink_hwmsg_notify(struct devlink *devlink,
			  const char *buf, size_t buf_len,
			  enum devlink_hwmsg_type type,
			  enum devlink_hwmsg_dir dir,
			  gfp_t gfp_mask);
int devlink_port_register(struct devlink *devlink,
			  struct devlink_port *devlink_port,
			  unsigned int port_index);
void devlink_port_unregister(struct devlink_port *devlink_port);
void devlink_port_type_eth_set(struct devlink_port *devlink_port,
			       struct net_device *netdev);
void devlink_port_type_ib_set(struct devlink_port *devlink_port,
			      struct ib_device *ibdev);
void devlink_port_type_clear(struct devlink_port *devlink_port);

#else

static inline struct devlink *devlink_alloc(const struct devlink_ops *ops,
					    size_t priv_size)
{
	return kzalloc(sizeof(*devlink) + priv_size, GFP_KERNEL);
}

static inline int devlink_register(struct devlink *devlink)
{
	return 0;
}

static inline void devlink_unregister(struct devlink *devlink)
{
}

static inline void devlink_free(struct devlink *devlink)
{
	kfree(devlink);
}

static inline void devlink_hwmsg_notify(struct devlink *devlink,
					const char *buf, size_t buf_len,
					enum devlink_hwmsg_type type,
					enum devlink_hwmsg_dir dir,
					gfp_t gfp_mask)
{
}

static inline int devlink_port_register(struct devlink *devlink,
					struct devlink_port *devlink_port,
					unsigned int port_index)
{
	return 0;
}

static inline void devlink_port_type_eth_set(struct devlink_port *devlink_port,
					     struct net_device *netdev)
{
}

static inline void devlink_port_type_ib_set(struct devlink_port *devlink_port,
					    struct ib_device *ibdev)
{
}

static inline void devlink_port_type_clear(struct devlink_port *devlink_port)
{
}

#endif

#endif /* _NET_DEVLINK_H_ */
