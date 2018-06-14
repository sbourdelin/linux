// SPDX-License-Identifier: GPL-2.0
/* Texas Instruments switchdev Driver
 *
 * Copyright (C) 2018 Texas Instruments
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <net/switchdev.h>
#include "cpsw.h"
#include "cpsw_priv.h"
#include "cpsw_ale.h"

static u32 cpsw_switchdev_get_ver(struct net_device *ndev)
{
	struct cpsw_priv *priv = netdev_priv(ndev);
	struct cpsw_common *cpsw = priv->cpsw;

	return cpsw->version;
}

static int cpsw_port_stp_state_set(struct cpsw_priv *priv,
				   struct switchdev_trans *trans, u8 state)
{
	struct cpsw_common *cpsw = priv->cpsw;
	u8 cpsw_state;
	int ret = 0;

	if (switchdev_trans_ph_prepare(trans))
		return 0;

	switch (state) {
	case BR_STATE_FORWARDING:
		cpsw_state = ALE_PORT_STATE_FORWARD;
		break;
	case BR_STATE_LEARNING:
		cpsw_state = ALE_PORT_STATE_LEARN;
		break;
	case BR_STATE_DISABLED:
		cpsw_state = ALE_PORT_STATE_DISABLE;
		break;
	case BR_STATE_LISTENING:
	case BR_STATE_BLOCKING:
		cpsw_state = ALE_PORT_STATE_BLOCK;
		break;
	default:
		return -EOPNOTSUPP;
	}

	ret = cpsw_ale_control_set(cpsw->ale, priv->emac_port,
				   ALE_PORT_STATE, cpsw_state);
	dev_dbg(priv->dev, "ale state: %u\n", cpsw_state);

	return ret;
}

static int cpsw_port_attr_br_flags_set(struct cpsw_priv *priv,
				       struct switchdev_trans *trans,
				       struct net_device *orig_dev,
				       unsigned long brport_flags)
{
	struct cpsw_common *cpsw = priv->cpsw;
	bool unreg_mcast_add = false;

	if (switchdev_trans_ph_prepare(trans))
		return 0;

	if (brport_flags & BR_MCAST_FLOOD)
		unreg_mcast_add = true;
	cpsw_ale_set_unreg_mcast(cpsw->ale, BIT(priv->emac_port),
				 unreg_mcast_add);

	return 0;
}

static int cpsw_port_attr_set(struct net_device *ndev,
			      const struct switchdev_attr *attr,
			      struct switchdev_trans *trans)
{
	struct cpsw_priv *priv = netdev_priv(ndev);
	u8 state;
	int ret;

	dev_dbg(priv->dev, "attr: id %u dev: %s port: %u\n", attr->id,
		priv->ndev->name, priv->emac_port);

	switch (attr->id) {
	case SWITCHDEV_ATTR_ID_PORT_STP_STATE:
		ret = cpsw_port_stp_state_set(priv, trans, attr->u.stp_state);
		dev_dbg(priv->dev, "stp state: %u\n", state);
		break;
	case SWITCHDEV_ATTR_ID_PORT_BRIDGE_FLAGS:
		ret = cpsw_port_attr_br_flags_set(priv, trans, attr->orig_dev,
						  attr->u.brport_flags);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

static int cpsw_port_attr_get(struct net_device *dev,
			      struct switchdev_attr *attr)
{
	u32 cpsw_ver;
	int err = 0;

	switch (attr->id) {
	case SWITCHDEV_ATTR_ID_PORT_PARENT_ID:
		cpsw_ver = cpsw_switchdev_get_ver(dev);
		attr->u.ppid.id_len = sizeof(cpsw_ver);
		memcpy(&attr->u.ppid.id, &cpsw_ver, attr->u.ppid.id_len);
		break;
	case SWITCHDEV_ATTR_ID_PORT_BRIDGE_FLAGS_SUPPORT:
		attr->u.brport_flags_support = BR_MCAST_FLOOD;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return err;
}

static u16 cpsw_get_pvid(struct cpsw_priv *priv)
{
	struct cpsw_common *cpsw = priv->cpsw;
	u32 __iomem *port_vlan_reg;
	u32 pvid;

	if (priv->emac_port) {
		int reg = CPSW2_PORT_VLAN;

		if (cpsw->version == CPSW_VERSION_1)
			reg = CPSW1_PORT_VLAN;
		pvid = slave_read(cpsw->slaves + (priv->emac_port - 1), reg);
	} else {
		port_vlan_reg = &cpsw->host_port_regs->port_vlan;
		pvid = readl(port_vlan_reg);
	}

	pvid = pvid & 0xfff;

	return pvid;
}

static void cpsw_set_pvid(struct cpsw_priv *priv, u16 vid, bool cfi, u32 cos)
{
	struct cpsw_common *cpsw = priv->cpsw;
	void __iomem *port_vlan_reg;
	u32 pvid;

	pvid = vid;
	pvid |= cfi ? BIT(12) : 0;
	pvid |= (cos & 0x7) << 13;

	if (priv->emac_port) {
		int reg = CPSW2_PORT_VLAN;

		if (cpsw->version == CPSW_VERSION_1)
			reg = CPSW1_PORT_VLAN;
		/* no barrier */
		slave_write(cpsw->slaves + (priv->emac_port - 1), pvid, reg);
	} else {
		/* CPU port */
		port_vlan_reg = &cpsw->host_port_regs->port_vlan;
		writel(pvid, port_vlan_reg);
	}
}

static int cpsw_port_vlan_add(struct cpsw_priv *priv, bool untag, bool pvid,
			      u16 vid, struct net_device *orig_dev)
{
	bool cpu_port = netif_is_bridge_master(orig_dev);
	struct cpsw_common *cpsw = priv->cpsw;
	int unreg_mcast_mask = 0;
	int reg_mcast_mask = 0;
	int untag_mask = 0;
	int port_mask;
	int ret = 0;
	u32 flags;

	if (cpu_port) {
		port_mask = BIT(HOST_PORT_NUM);
		flags = orig_dev->flags;
		unreg_mcast_mask = port_mask;
	} else {
		port_mask = BIT(priv->emac_port);
		flags = priv->ndev->flags;
	}

	if (flags & IFF_MULTICAST)
		reg_mcast_mask = port_mask;

	if (untag)
		untag_mask = port_mask;

	ret = cpsw_ale_vlan_add_modify(cpsw->ale, vid, port_mask, untag_mask,
				       reg_mcast_mask, unreg_mcast_mask);
	if (ret) {
		dev_err(priv->dev, "Unable to add vlan\n");
		return ret;
	}

	if (!pvid)
		return ret;

	cpsw_set_pvid(priv, vid, 0, 0);

	dev_dbg(priv->dev, "VID add: %u dev: %s port: %u\n", vid,
		priv->ndev->name, priv->emac_port);

	return ret;
}

static int cpsw_port_vlan_del(struct cpsw_priv *priv, u16 vid,
			      struct net_device *orig_dev)
{
	bool cpu_port = netif_is_bridge_master(orig_dev);
	struct cpsw_common *cpsw = priv->cpsw;
	int port_mask;
	int ret = 0;

	if (cpu_port)
		port_mask = BIT(HOST_PORT_NUM);
	else
		port_mask = BIT(priv->emac_port);

	ret = cpsw_ale_vlan_del_modify(cpsw->ale, vid, port_mask);
	if (ret != 0)
		return ret;

	/* We don't care for the return value here, error is returned only if
	 * the unicast entry is not present
	 */
	cpsw_ale_del_ucast(cpsw->ale, priv->mac_addr,
			   HOST_PORT_NUM, ALE_VLAN, vid);

	if (vid == cpsw_get_pvid(priv))
		cpsw_set_pvid(priv, 0, 0, 0);

	/* We don't care for the return value here, error is returned only if
	 * the multicast entry is not present
	 */
	cpsw_ale_del_mcast(cpsw->ale, priv->ndev->broadcast,
			   0, ALE_VLAN, vid);

	dev_dbg(priv->dev, "VID del: %u dev: %s port: %u\n", vid,
		priv->ndev->name, priv->emac_port);

	return ret;
}

static int cpsw_port_vlans_add(struct cpsw_priv *priv,
			       const struct switchdev_obj_port_vlan *vlan,
			       struct switchdev_trans *trans)
{
	bool untag = vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED;
	struct net_device *orig_dev = vlan->obj.orig_dev;
	bool cpu_port = netif_is_bridge_master(orig_dev);
	bool pvid = vlan->flags & BRIDGE_VLAN_INFO_PVID;
	u16 vid;

	if (cpu_port && !(vlan->flags & BRIDGE_VLAN_INFO_BRENTRY))
		return 0;

	if (switchdev_trans_ph_prepare(trans))
		return 0;

	for (vid = vlan->vid_begin; vid <= vlan->vid_end; vid++) {
		int err;

		err = cpsw_port_vlan_add(priv, untag, pvid, vid, orig_dev);
		if (err)
			return err;
	}

	return 0;
}

static int cpsw_port_vlans_del(struct cpsw_priv *priv,
			       const struct switchdev_obj_port_vlan *vlan)

{
	struct net_device *orig_dev = vlan->obj.orig_dev;
	u16 vid;

	for (vid = vlan->vid_begin; vid <= vlan->vid_end; vid++) {
		int err;

		err = cpsw_port_vlan_del(priv, vid, orig_dev);
		if (err)
			return err;
	}

	return 0;
}

static int cpsw_port_mdb_add(struct cpsw_priv *priv,
			     struct switchdev_obj_port_mdb *mdb,
			     struct switchdev_trans *trans)

{
	struct net_device *orig_dev = mdb->obj.orig_dev;
	bool cpu_port = netif_is_bridge_master(orig_dev);
	struct cpsw_common *cpsw = priv->cpsw;
	int port_mask;
	int err;

	if (switchdev_trans_ph_prepare(trans))
		return 0;

	if (cpu_port)
		port_mask = BIT(HOST_PORT_NUM);
	else
		port_mask = BIT(priv->emac_port);

	err = cpsw_ale_mcast_add_modify(cpsw->ale, mdb->addr, port_mask,
					ALE_VLAN, mdb->vid, 0);

	dev_dbg(priv->dev, "MDB add: %pM dev: %s vid %u port: %u\n", mdb->addr,
		priv->ndev->name, mdb->vid, priv->emac_port);

	return err;
}

static int cpsw_port_mdb_del(struct cpsw_priv *priv,
			     struct switchdev_obj_port_mdb *mdb)

{
	struct net_device *orig_dev = mdb->obj.orig_dev;
	bool cpu_port = netif_is_bridge_master(orig_dev);
	struct cpsw_common *cpsw = priv->cpsw;
	int del_mask;
	int err;

	if (cpu_port)
		del_mask = BIT(HOST_PORT_NUM);
	else
		del_mask = BIT(priv->emac_port);
	err = cpsw_ale_mcast_del_modify(cpsw->ale, mdb->addr, del_mask,
					ALE_VLAN, mdb->vid);
	dev_dbg(priv->dev, "MDB del: %pM dev: %s vid %u port: %u\n", mdb->addr,
		priv->ndev->name, mdb->vid, priv->emac_port);

	return err;
}

static int cpsw_port_obj_add(struct net_device *ndev,
			     const struct switchdev_obj *obj,
			     struct switchdev_trans *trans)
{
	struct switchdev_obj_port_vlan *vlan = SWITCHDEV_OBJ_PORT_VLAN(obj);
	struct switchdev_obj_port_mdb *mdb = SWITCHDEV_OBJ_PORT_MDB(obj);
	struct cpsw_priv *priv = netdev_priv(ndev);
	int err = 0;

	switch (obj->id) {
	case SWITCHDEV_OBJ_ID_PORT_VLAN:
		err = cpsw_port_vlans_add(priv, vlan, trans);
		break;
	case SWITCHDEV_OBJ_ID_PORT_MDB:
	case SWITCHDEV_OBJ_ID_HOST_MDB:
		err = cpsw_port_mdb_add(priv, mdb, trans);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}

	return err;
}

static int cpsw_port_obj_del(struct net_device *ndev,
			     const struct switchdev_obj *obj)
{
	struct switchdev_obj_port_vlan *vlan = SWITCHDEV_OBJ_PORT_VLAN(obj);
	struct switchdev_obj_port_mdb *mdb = SWITCHDEV_OBJ_PORT_MDB(obj);
	struct cpsw_priv *priv = netdev_priv(ndev);
	int err = 0;

	switch (obj->id) {
	case SWITCHDEV_OBJ_ID_PORT_VLAN:
		err = cpsw_port_vlans_del(priv, vlan);
		break;
	case SWITCHDEV_OBJ_ID_PORT_MDB:
	case SWITCHDEV_OBJ_ID_HOST_MDB:
		err = cpsw_port_mdb_del(priv, mdb);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}

	return err;
}

static const struct switchdev_ops cpsw_port_switchdev_ops = {
	.switchdev_port_attr_set	= cpsw_port_attr_set,
	.switchdev_port_attr_get	= cpsw_port_attr_get,
	.switchdev_port_obj_add		= cpsw_port_obj_add,
	.switchdev_port_obj_del		= cpsw_port_obj_del,
};

void cpsw_port_switchdev_init(struct net_device *ndev)
{
	ndev->switchdev_ops = &cpsw_port_switchdev_ops;
}
