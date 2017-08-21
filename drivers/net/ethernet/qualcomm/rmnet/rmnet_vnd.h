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
 * RMNET Data Virtual Network Device APIs
 *
 */

#ifndef _RMNET_VND_H_
#define _RMNET_VND_H_

int rmnet_vnd_do_flow_control(struct rmnet_real_dev_info *rdinfo,
			      struct net_device *dev, int enable);
struct rmnet_endpoint *rmnet_vnd_get_endpoint(struct net_device *dev);
int rmnet_vnd_free_dev(struct net_device *real_dev, int id);
int rmnet_vnd_remove_ref_dev(struct net_device *real_dev, int id);
int rmnet_vnd_rx_fixup(struct sk_buff *skb, struct net_device *dev);
int rmnet_vnd_tx_fixup(struct sk_buff *skb, struct net_device *dev);
int rmnet_vnd_get_mux(struct net_device *real_dev,
		      struct net_device *rmnet_dev);
struct net_device *rmnet_vnd_get_by_id(struct net_device *real_dev, int id);
void rmnet_vnd_setup(struct net_device *dev);
int rmnet_vnd_newlink(struct net_device *real_dev, int id,
		      struct net_device *new_device);
#endif /* _RMNET_VND_H_ */
