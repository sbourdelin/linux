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

static int cpsw_port_attr_set(struct net_device *dev,
			      const struct switchdev_attr *attr,
			      struct switchdev_trans *trans)
{
	return -EOPNOTSUPP;
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
			      u16 vid)
{
	struct cpsw_common *cpsw = priv->cpsw;
	int port_mask = BIT(priv->emac_port);
	int unreg_mcast_mask = 0;
	int reg_mcast_mask = 0;
	int untag_mask = 0;
	int ret = 0;

	if (priv->ndev->flags & IFF_ALLMULTI)
		unreg_mcast_mask = port_mask;

	if (priv->ndev->flags & IFF_MULTICAST)
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

	dev_dbg(priv->dev, "VID: %u dev: %s port: %u\n", vid,
		priv->ndev->name, priv->emac_port);

	return ret;
}

static int cpsw_port_vlan_del(struct cpsw_priv *priv, u16 vid)
{
	struct cpsw_common *cpsw = priv->cpsw;
	int port_mask = BIT(priv->emac_port);
	int ret = 0;

	ret = cpsw_ale_vlan_del_modify(cpsw->ale, vid, port_mask);
	if (ret != 0)
		return ret;

	ret = cpsw_ale_del_ucast(cpsw->ale, priv->mac_addr,
				 HOST_PORT_NUM, ALE_VLAN, vid);

	if (vid == cpsw_get_pvid(priv))
		cpsw_set_pvid(priv, 0, 0, 0);

	if (ret != 0) {
		dev_dbg(priv->dev, "Failed to delete unicast entry\n");
		ret = 0;
	}

	ret = cpsw_ale_del_mcast(cpsw->ale, priv->ndev->broadcast,
				 0, ALE_VLAN, vid);
	if (ret != 0) {
		dev_dbg(priv->dev, "Failed to delete multicast entry\n");
		ret = 0;
	}

	return ret;
}

static int cpsw_port_vlans_add(struct cpsw_priv *priv,
			       const struct switchdev_obj_port_vlan *vlan,
			       struct switchdev_trans *trans)
{
	bool untagged = vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED;
	bool pvid = vlan->flags & BRIDGE_VLAN_INFO_PVID;
	u16 vid;

	if (switchdev_trans_ph_prepare(trans))
		return 0;

	for (vid = vlan->vid_begin; vid <= vlan->vid_end; vid++) {
		int err;

		err = cpsw_port_vlan_add(priv, untagged, pvid, vid);
		if (err)
			return err;
	}

	return 0;
}

static int cpsw_port_vlans_del(struct cpsw_priv *priv,
			       const struct switchdev_obj_port_vlan *vlan)

{
	u16 vid;

	for (vid = vlan->vid_begin; vid <= vlan->vid_end; vid++) {
		int err;

		err = cpsw_port_vlan_del(priv, vid);
		if (err)
			return err;
	}

	return 0;
}

static int cpsw_port_mdb_add(struct cpsw_priv *priv,
			     struct switchdev_obj_port_mdb *mdb,
			     struct switchdev_trans *trans)
{
	struct cpsw_common *cpsw = priv->cpsw;
	int port_mask;
	int err;

	if (switchdev_trans_ph_prepare(trans))
		return 0;

	port_mask = BIT(priv->emac_port);
	err = cpsw_ale_mcast_add_modify(cpsw->ale, mdb->addr, port_mask,
					ALE_VLAN, mdb->vid, 0);

	return err;
}

static int cpsw_port_mdb_del(struct cpsw_priv *priv,
			     struct switchdev_obj_port_mdb *mdb)

{
	struct cpsw_common *cpsw = priv->cpsw;
	int del_mask;
	int err;

	del_mask = BIT(priv->emac_port);
	err = cpsw_ale_mcast_del_modify(cpsw->ale, mdb->addr, del_mask,
					ALE_VLAN, mdb->vid);

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
	struct cpsw_priv *priv = netdev_priv(ndev);
	int err = 0;

	switch (obj->id) {
	case SWITCHDEV_OBJ_ID_PORT_VLAN:
		err = cpsw_port_vlans_del(priv, vlan);
		break;
	case SWITCHDEV_OBJ_ID_PORT_MDB:
		err = cpsw_port_mdb_del(priv, SWITCHDEV_OBJ_PORT_MDB(obj));
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
