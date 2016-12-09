/*
 * Broadcom NetXtreme-E RoCE driver.
 *
 * Copyright (c) 2016, Broadcom. All rights reserved.  The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Description: IB Verbs interpreter
 */

#include <linux/interrupt.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/netdevice.h>

#include <rdma/ib_verbs.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_addr.h>
#include <rdma/ib_mad.h>
#include <rdma/ib_cache.h>

#include "bnxt_ulp.h"

#include "bnxt_re_hsi.h"
#include "bnxt_qplib_res.h"
#include "bnxt_qplib_sp.h"
#include "bnxt_qplib_fp.h"
#include "bnxt_qplib_rcfw.h"

#include "bnxt_re.h"
#include "bnxt_re_ib_verbs.h"
#include <rdma/bnxt_re_uverbs_abi.h>

static int bnxt_re_copy_to_udata(struct bnxt_re_dev *rdev, void *data, int len,
				 struct ib_udata *udata)
{
	int rc;

	rc = ib_copy_to_udata(udata, data, len);

	return rc ? -EFAULT : 0;
}

/* Device */
struct net_device *bnxt_re_get_netdev(struct ib_device *ibdev, u8 port_num)
{
	struct bnxt_re_dev *rdev = to_bnxt_re_dev(ibdev, ibdev);
	struct net_device *netdev = NULL;

	rcu_read_lock();
	if (rdev)
		netdev = rdev->netdev;
	if (netdev)
		dev_hold(netdev);

	rcu_read_unlock();
	return netdev;
}

int bnxt_re_query_device(struct ib_device *ibdev,
			 struct ib_device_attr *ib_attr,
			 struct ib_udata *udata)
{
	struct bnxt_re_dev *rdev = to_bnxt_re_dev(ibdev, ibdev);
	struct bnxt_qplib_dev_attr *dev_attr = &rdev->dev_attr;

	memset(ib_attr, 0, sizeof(*ib_attr));

	ib_attr->fw_ver = (u64)dev_attr->fw_ver;
	bnxt_qplib_get_guid(rdev->netdev->dev_addr,
			    (u8 *)&ib_attr->sys_image_guid);
	ib_attr->max_mr_size = ~0ull;
	ib_attr->page_size_cap = BNXT_RE_PAGE_SIZE_4K | BNXT_RE_PAGE_SIZE_8K |
				 BNXT_RE_PAGE_SIZE_64K | BNXT_RE_PAGE_SIZE_2M |
				 BNXT_RE_PAGE_SIZE_8M | BNXT_RE_PAGE_SIZE_1G;

	ib_attr->vendor_id = rdev->en_dev->pdev->vendor;
	ib_attr->vendor_part_id = rdev->en_dev->pdev->device;
	ib_attr->hw_ver = rdev->en_dev->pdev->subsystem_device;
	ib_attr->max_qp = dev_attr->max_qp;
	ib_attr->max_qp_wr = dev_attr->max_qp_wqes;
	ib_attr->device_cap_flags =
				    IB_DEVICE_CURR_QP_STATE_MOD
				    | IB_DEVICE_RC_RNR_NAK_GEN
				    | IB_DEVICE_SHUTDOWN_PORT
				    | IB_DEVICE_SYS_IMAGE_GUID
				    | IB_DEVICE_LOCAL_DMA_LKEY
				    | IB_DEVICE_RESIZE_MAX_WR
				    | IB_DEVICE_PORT_ACTIVE_EVENT
				    | IB_DEVICE_N_NOTIFY_CQ
				    | IB_DEVICE_MEM_WINDOW
				    | IB_DEVICE_MEM_WINDOW_TYPE_2B
				    | IB_DEVICE_MEM_MGT_EXTENSIONS;
	ib_attr->max_sge = dev_attr->max_qp_sges;
	ib_attr->max_sge_rd = dev_attr->max_qp_sges;
	ib_attr->max_cq = dev_attr->max_cq;
	ib_attr->max_cqe = dev_attr->max_cq_wqes;
	ib_attr->max_mr = dev_attr->max_mr;
	ib_attr->max_pd = dev_attr->max_pd;
	ib_attr->max_qp_rd_atom = dev_attr->max_qp_rd_atom;
	ib_attr->max_qp_init_rd_atom = dev_attr->max_qp_rd_atom;
	ib_attr->atomic_cap = IB_ATOMIC_HCA;
	ib_attr->masked_atomic_cap = IB_ATOMIC_HCA;

	ib_attr->max_ee_rd_atom = 0;
	ib_attr->max_res_rd_atom = 0;
	ib_attr->max_ee_init_rd_atom = 0;
	ib_attr->max_ee = 0;
	ib_attr->max_rdd = 0;
	ib_attr->max_mw = dev_attr->max_mw;
	ib_attr->max_raw_ipv6_qp = 0;
	ib_attr->max_raw_ethy_qp = dev_attr->max_raw_ethy_qp;
	ib_attr->max_mcast_grp = 0;
	ib_attr->max_mcast_qp_attach = 0;
	ib_attr->max_total_mcast_qp_attach = 0;
	ib_attr->max_ah = dev_attr->max_ah;

	ib_attr->max_fmr = dev_attr->max_fmr;
	ib_attr->max_map_per_fmr = 1;	/* ? */

	ib_attr->max_srq = dev_attr->max_srq;
	ib_attr->max_srq_wr = dev_attr->max_srq_wqes;
	ib_attr->max_srq_sge = dev_attr->max_srq_sges;

	ib_attr->max_fast_reg_page_list_len = MAX_PBL_LVL_1_PGS;

	ib_attr->max_pkeys = 1;
	ib_attr->local_ca_ack_delay = 0;
	return 0;
}

int bnxt_re_modify_device(struct ib_device *ibdev,
			  int device_modify_mask,
			  struct ib_device_modify *device_modify)
{
	switch (device_modify_mask) {
	case IB_DEVICE_MODIFY_SYS_IMAGE_GUID:
		/* Modify the GUID requires the modification of the GID table */
		/* GUID should be made as READ-ONLY */
		break;
	case IB_DEVICE_MODIFY_NODE_DESC:
		/* Node Desc should be made as READ-ONLY */
		break;
	default:
		break;
	}
	return 0;
}

static void __to_ib_speed_width(struct net_device *netdev, u8 *speed, u8 *width)
{
	struct ethtool_link_ksettings lksettings;
	u32 espeed;

	if (netdev->ethtool_ops && netdev->ethtool_ops->get_link_ksettings) {
		memset(&lksettings, 0, sizeof(lksettings));
		rtnl_lock();
		netdev->ethtool_ops->get_link_ksettings(netdev, &lksettings);
		rtnl_unlock();
		espeed = lksettings.base.speed;
	} else {
		espeed = SPEED_UNKNOWN;
	}
	switch (espeed) {
	case SPEED_1000:
		*speed = IB_SPEED_SDR;
		*width = IB_WIDTH_1X;
		break;
	case SPEED_10000:
		*speed = IB_SPEED_QDR;
		*width = IB_WIDTH_1X;
		break;
	case SPEED_20000:
		*speed = IB_SPEED_DDR;
		*width = IB_WIDTH_4X;
		break;
	case SPEED_25000:
		*speed = IB_SPEED_EDR;
		*width = IB_WIDTH_1X;
		break;
	case SPEED_40000:
		*speed = IB_SPEED_QDR;
		*width = IB_WIDTH_4X;
		break;
	case SPEED_50000:
		break;
	default:
		*speed = IB_SPEED_SDR;
		*width = IB_WIDTH_1X;
		break;
	}
}

/* Port */
int bnxt_re_query_port(struct ib_device *ibdev, u8 port_num,
		       struct ib_port_attr *port_attr)
{
	struct bnxt_re_dev *rdev = to_bnxt_re_dev(ibdev, ibdev);
	struct bnxt_qplib_dev_attr *dev_attr = &rdev->dev_attr;

	memset(port_attr, 0, sizeof(*port_attr));

	if (netif_running(rdev->netdev) && netif_carrier_ok(rdev->netdev)) {
		port_attr->state = IB_PORT_ACTIVE;
		port_attr->phys_state = 5;
	} else {
		port_attr->state = IB_PORT_DOWN;
		port_attr->phys_state = 3;
	}
	port_attr->max_mtu = IB_MTU_4096;
	port_attr->active_mtu = iboe_get_mtu(rdev->netdev->mtu);
	port_attr->gid_tbl_len = dev_attr->max_sgid;
	port_attr->port_cap_flags = IB_PORT_CM_SUP | IB_PORT_REINIT_SUP |
				    IB_PORT_DEVICE_MGMT_SUP |
				    IB_PORT_VENDOR_CLASS_SUP |
				    IB_PORT_IP_BASED_GIDS;

	/* Max MSG size set to 2G for now */
	port_attr->max_msg_sz = 0x80000000;
	port_attr->bad_pkey_cntr = 0;
	port_attr->qkey_viol_cntr = 0;
	port_attr->pkey_tbl_len = dev_attr->max_pkey;
	port_attr->lid = 0;
	port_attr->sm_lid = 0;
	port_attr->lmc = 0;
	port_attr->max_vl_num = 4;
	port_attr->sm_sl = 0;
	port_attr->subnet_timeout = 0;
	port_attr->init_type_reply = 0;
	/* call the underlying netdev's ethtool hooks to query speed settings
	 * for which we acquire rtnl_lock _only_ if it's registered with
	 * IB stack to avoid race in the NETDEV_UNREG path
	 */
	if (test_bit(BNXT_RE_FLAG_IBDEV_REGISTERED, &rdev->flags))
		__to_ib_speed_width(rdev->netdev, &port_attr->active_speed,
				    &port_attr->active_width);
	return 0;
}

int bnxt_re_modify_port(struct ib_device *ibdev, u8 port_num,
			int port_modify_mask,
			struct ib_port_modify *port_modify)
{
	switch (port_modify_mask) {
	case IB_PORT_SHUTDOWN:
		break;
	case IB_PORT_INIT_TYPE:
		break;
	case IB_PORT_RESET_QKEY_CNTR:
		break;
	default:
		break;
	}
	return 0;
}

int bnxt_re_get_port_immutable(struct ib_device *ibdev, u8 port_num,
			       struct ib_port_immutable *immutable)
{
	struct ib_port_attr port_attr;

	if (bnxt_re_query_port(ibdev, port_num, &port_attr))
		return -EINVAL;

	immutable->pkey_tbl_len = port_attr.pkey_tbl_len;
	immutable->gid_tbl_len = port_attr.gid_tbl_len;
	immutable->core_cap_flags = RDMA_CORE_PORT_IBA_ROCE;
	immutable->core_cap_flags |= RDMA_CORE_CAP_PROT_ROCE_UDP_ENCAP;
	immutable->max_mad_size = IB_MGMT_MAD_SIZE;
	return 0;
}

int bnxt_re_query_pkey(struct ib_device *ibdev, u8 port_num,
		       u16 index, u16 *pkey)
{
	struct bnxt_re_dev *rdev = to_bnxt_re_dev(ibdev, ibdev);

	/* Ignore port_num */

	memset(pkey, 0, sizeof(*pkey));
	return bnxt_qplib_get_pkey(&rdev->qplib_res,
				   &rdev->qplib_res.pkey_tbl, index, pkey);
}

int bnxt_re_query_gid(struct ib_device *ibdev, u8 port_num,
		      int index, union ib_gid *gid)
{
	struct bnxt_re_dev *rdev = to_bnxt_re_dev(ibdev, ibdev);
	int rc = 0;

	/* Ignore port_num */
	memset(gid, 0, sizeof(*gid));
	rc = bnxt_qplib_get_sgid(&rdev->qplib_res,
				 &rdev->qplib_res.sgid_tbl, index,
				 (struct bnxt_qplib_gid *)gid);
	return rc;
}

int bnxt_re_del_gid(struct ib_device *ibdev, u8 port_num,
		    unsigned int index, void **context)
{
	int rc = 0;
	struct bnxt_re_gid_ctx *ctx, **ctx_tbl;
	struct bnxt_re_dev *rdev = to_bnxt_re_dev(ibdev, ibdev);
	struct bnxt_qplib_sgid_tbl *sgid_tbl = &rdev->qplib_res.sgid_tbl;

	/* Delete the entry from the hardware */
	ctx = *context;
	if (!ctx)
		return -EINVAL;

	if (sgid_tbl && sgid_tbl->active) {
		if (ctx->idx >= sgid_tbl->max)
			return -EINVAL;
		ctx->refcnt--;
		if (!ctx->refcnt) {
			rc = bnxt_qplib_del_sgid
					(sgid_tbl,
					 &sgid_tbl->tbl[ctx->idx], true);
			if (rc)
				dev_err(rdev_to_dev(rdev),
					"Failed to remove GID: %#x", rc);
			ctx_tbl = sgid_tbl->ctx;
			ctx_tbl[ctx->idx] = NULL;
			kfree(ctx);
		}
	} else {
		return -EINVAL;
	}
	return rc;
}

int bnxt_re_add_gid(struct ib_device *ibdev, u8 port_num,
		    unsigned int index, const union ib_gid *gid,
		    const struct ib_gid_attr *attr, void **context)
{
	int rc;
	u32 tbl_idx = 0;
	u16 vlan_id = 0xFFFF;
	struct bnxt_re_gid_ctx *ctx, **ctx_tbl;
	struct bnxt_re_dev *rdev = to_bnxt_re_dev(ibdev, ibdev);
	struct bnxt_qplib_sgid_tbl *sgid_tbl = &rdev->qplib_res.sgid_tbl;

	if ((attr->ndev) && is_vlan_dev(attr->ndev))
		vlan_id = vlan_dev_vlan_id(attr->ndev);

	rc = bnxt_qplib_add_sgid(sgid_tbl, (struct bnxt_qplib_gid *)gid,
				 rdev->qplib_res.netdev->dev_addr,
				 vlan_id, true, &tbl_idx);
	if (rc == -EALREADY) {
		ctx_tbl = sgid_tbl->ctx;
		ctx_tbl[tbl_idx]->refcnt++;
		*context = ctx_tbl[tbl_idx];
		return 0;
	}

	if (rc < 0) {
		dev_err(rdev_to_dev(rdev), "Failed to add GID: %#x", rc);
		return rc;
	}

	ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx_tbl = sgid_tbl->ctx;
	ctx->idx = tbl_idx;
	ctx->refcnt = 1;
	ctx_tbl[tbl_idx] = ctx;

	return rc;
}

enum rdma_link_layer bnxt_re_get_link_layer(struct ib_device *ibdev,
					    u8 port_num)
{
	return IB_LINK_LAYER_ETHERNET;
}

/* Protection Domains */
int bnxt_re_dealloc_pd(struct ib_pd *ib_pd)
{
	struct bnxt_re_pd *pd = to_bnxt_re(ib_pd, struct bnxt_re_pd, ib_pd);
	struct bnxt_re_dev *rdev = pd->rdev;
	int rc;

	if (ib_pd->uobject && pd->dpi.dbr) {
		struct ib_ucontext *ib_uctx = ib_pd->uobject->context;
		struct bnxt_re_ucontext *ucntx;

		/* Free DPI only if this is the first PD allocated by the
		 * application and mark the context dpi as NULL
		 */
		ucntx = to_bnxt_re(ib_uctx, struct bnxt_re_ucontext, ib_uctx);

		rc = bnxt_qplib_dealloc_dpi(&rdev->qplib_res,
					    &rdev->qplib_res.dpi_tbl,
					    &pd->dpi);
		if (rc)
			dev_err(rdev_to_dev(rdev), "Failed to deallocate HW DPI");
			/* Don't fail, continue*/
		ucntx->dpi = NULL;
	}

	rc = bnxt_qplib_dealloc_pd(&rdev->qplib_res,
				   &rdev->qplib_res.pd_tbl,
				   &pd->qplib_pd);
	if (rc) {
		dev_err(rdev_to_dev(rdev), "Failed to deallocate HW PD");
		return rc;
	}

	kfree(pd);
	return 0;
}

struct ib_pd *bnxt_re_alloc_pd(struct ib_device *ibdev,
			       struct ib_ucontext *ucontext,
			       struct ib_udata *udata)
{
	struct bnxt_re_dev *rdev = to_bnxt_re_dev(ibdev, ibdev);
	struct bnxt_re_ucontext *ucntx = to_bnxt_re(ucontext,
						    struct bnxt_re_ucontext,
						    ib_uctx);
	struct bnxt_re_pd *pd;
	int rc;

	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return ERR_PTR(-ENOMEM);

	pd->rdev = rdev;
	if (bnxt_qplib_alloc_pd(&rdev->qplib_res.pd_tbl, &pd->qplib_pd)) {
		dev_err(rdev_to_dev(rdev), "Failed to allocate HW PD");
		rc = -ENOMEM;
		goto fail;
	}

	if (udata) {
		struct bnxt_re_pd_resp resp;

		if (!ucntx->dpi) {
			/* Allocate DPI in alloc_pd to avoid failing of
			 * ibv_devinfo and family of application when DPIs
			 * are depleted.
			 */
			if (bnxt_qplib_alloc_dpi(&rdev->qplib_res.dpi_tbl,
						 &pd->dpi, ucntx)) {
				rc = -ENOMEM;
				goto dbfail;
			}
			ucntx->dpi = &pd->dpi;
		}

		resp.pdid = pd->qplib_pd.id;
		/* Still allow mapping this DBR to the new user PD. */
		resp.dpi = ucntx->dpi->dpi;
		resp.dbr = (u64)ucntx->dpi->umdbr;

		rc = ib_copy_to_udata(udata, &resp, sizeof(resp));
		if (rc) {
			dev_err(rdev_to_dev(rdev),
				"Failed to copy user response\n");
			goto dbfail;
		}
	}

	return &pd->ib_pd;
dbfail:
	(void)bnxt_qplib_dealloc_pd(&rdev->qplib_res, &rdev->qplib_res.pd_tbl,
				    &pd->qplib_pd);
fail:
	kfree(pd);
	return ERR_PTR(rc);
}

/* Address Handles */
int bnxt_re_destroy_ah(struct ib_ah *ib_ah)
{
	struct bnxt_re_ah *ah = to_bnxt_re(ib_ah, struct bnxt_re_ah, ib_ah);
	struct bnxt_re_dev *rdev = ah->rdev;
	int rc;

	rc = bnxt_qplib_destroy_ah(&rdev->qplib_res, &ah->qplib_ah);
	if (rc) {
		dev_err(rdev_to_dev(rdev), "Failed to destroy HW AH");
		return rc;
	}
	kfree(ah);
	return 0;
}

struct ib_ah *bnxt_re_create_ah(struct ib_pd *ib_pd,
				struct ib_ah_attr *ah_attr)
{
	struct bnxt_re_pd *pd = to_bnxt_re(ib_pd, struct bnxt_re_pd, ib_pd);
	struct bnxt_re_dev *rdev = pd->rdev;
	struct bnxt_re_ah *ah;
	int rc;
	u16 vlan_tag;
	u8 nw_type;

	struct ib_gid_attr sgid_attr;

	if (!(ah_attr->ah_flags & IB_AH_GRH)) {
		dev_err(rdev_to_dev(rdev), "Failed to alloc AH: GRH not set");
		return ERR_PTR(-EINVAL);
	}
	ah = kzalloc(sizeof(*ah), GFP_ATOMIC);
	if (!ah)
		return ERR_PTR(-ENOMEM);

	ah->rdev = rdev;
	ah->qplib_ah.pd = &pd->qplib_pd;

	/* Supply the configuration for the HW */
	memcpy(ah->qplib_ah.dgid.data, ah_attr->grh.dgid.raw,
	       sizeof(union ib_gid));
	/*
	 * If RoCE V2 is enabled, stack will have two entries for
	 * each GID entry. Avoiding this duplicte entry in HW. Dividing
	 * the GID index by 2 for RoCE V2
	 */
	ah->qplib_ah.sgid_index = ah_attr->grh.sgid_index / 2;
	ah->qplib_ah.host_sgid_index = ah_attr->grh.sgid_index;
	ah->qplib_ah.traffic_class = ah_attr->grh.traffic_class;
	ah->qplib_ah.flow_label = ah_attr->grh.flow_label;
	ah->qplib_ah.hop_limit = ah_attr->grh.hop_limit;
	ah->qplib_ah.sl = ah_attr->sl;
	if (ib_pd->uobject &&
	    !rdma_is_multicast_addr((struct in6_addr *)
				    ah_attr->grh.dgid.raw) &&
	    !rdma_link_local_addr((struct in6_addr *)
				  ah_attr->grh.dgid.raw)) {
		union ib_gid sgid;

		rc = ib_get_cached_gid(&rdev->ibdev, 1,
				       ah_attr->grh.sgid_index, &sgid,
				       &sgid_attr);
		if (rc) {
			dev_err(rdev_to_dev(rdev),
				"Failed to query gid at index %d",
				ah_attr->grh.sgid_index);
			goto fail;
		}
		if (sgid_attr.ndev) {
			if (is_vlan_dev(sgid_attr.ndev))
				vlan_tag = vlan_dev_vlan_id(sgid_attr.ndev);
			dev_put(sgid_attr.ndev);
		}
		/* Get network header type for this GID */
		nw_type = ib_gid_to_network_type(sgid_attr.gid_type, &sgid);
		switch (nw_type) {
		case RDMA_NETWORK_IPV4:
			ah->qplib_ah.nw_type = CMDQ_CREATE_AH_TYPE_V2IPV4;
			break;
		case RDMA_NETWORK_IPV6:
			ah->qplib_ah.nw_type = CMDQ_CREATE_AH_TYPE_V2IPV6;
			break;
		default:
			ah->qplib_ah.nw_type = CMDQ_CREATE_AH_TYPE_V1;
			break;
		}
		rc = rdma_addr_find_l2_eth_by_grh(&sgid, &ah_attr->grh.dgid,
						  ah_attr->dmac, &vlan_tag,
						  &sgid_attr.ndev->ifindex,
						  NULL);
		if (rc) {
			dev_err(rdev_to_dev(rdev), "Failed to get dmac\n");
			goto fail;
		}
	}

	memcpy(ah->qplib_ah.dmac, ah_attr->dmac, ETH_ALEN);
	rc = bnxt_qplib_create_ah(&rdev->qplib_res, &ah->qplib_ah);
	if (rc) {
		dev_err(rdev_to_dev(rdev), "Failed to allocate HW AH");
		goto fail;
	}

	/* Write AVID to shared page. */
	if (ib_pd->uobject) {
		struct ib_ucontext *ib_uctx = ib_pd->uobject->context;
		struct bnxt_re_ucontext *uctx;
		unsigned long flag;
		u32 *wrptr;

		uctx = to_bnxt_re(ib_uctx, struct bnxt_re_ucontext, ib_uctx);
		spin_lock_irqsave(&uctx->sh_lock, flag);
		wrptr = (u32 *)(uctx->shpg + BNXT_RE_AVID_OFFT);
		*wrptr = ah->qplib_ah.id;
		wmb(); /* make sure cache is updated. */
		spin_unlock_irqrestore(&uctx->sh_lock, flag);
	}

	return &ah->ib_ah;

fail:
	kfree(ah);
	return ERR_PTR(rc);
}

int bnxt_re_modify_ah(struct ib_ah *ib_ah, struct ib_ah_attr *ah_attr)
{
	return 0;
}

int bnxt_re_query_ah(struct ib_ah *ib_ah, struct ib_ah_attr *ah_attr)
{
	struct bnxt_re_ah *ah = to_bnxt_re(ib_ah, struct bnxt_re_ah, ib_ah);

	memcpy(ah_attr->grh.dgid.raw, ah->qplib_ah.dgid.data,
	       sizeof(union ib_gid));
	ah_attr->grh.sgid_index = ah->qplib_ah.host_sgid_index;
	ah_attr->grh.traffic_class = ah->qplib_ah.traffic_class;
	ah_attr->sl = ah->qplib_ah.sl;
	memcpy(ah_attr->dmac, ah->qplib_ah.dmac, ETH_ALEN);
	ah_attr->ah_flags = IB_AH_GRH;
	ah_attr->port_num = 1;
	ah_attr->static_rate = 0;
	return 0;
}

static int __from_ib_access_flags(int iflags)
{
	int qflags = 0;

	if (iflags & IB_ACCESS_LOCAL_WRITE)
		qflags |= BNXT_QPLIB_ACCESS_LOCAL_WRITE;
	if (iflags & IB_ACCESS_REMOTE_READ)
		qflags |= BNXT_QPLIB_ACCESS_REMOTE_READ;
	if (iflags & IB_ACCESS_REMOTE_WRITE)
		qflags |= BNXT_QPLIB_ACCESS_REMOTE_WRITE;
	if (iflags & IB_ACCESS_REMOTE_ATOMIC)
		qflags |= BNXT_QPLIB_ACCESS_REMOTE_ATOMIC;
	if (iflags & IB_ACCESS_MW_BIND)
		qflags |= BNXT_QPLIB_ACCESS_MW_BIND;
	if (iflags & IB_ZERO_BASED)
		qflags |= BNXT_QPLIB_ACCESS_ZERO_BASED;
	if (iflags & IB_ACCESS_ON_DEMAND)
		qflags |= BNXT_QPLIB_ACCESS_ON_DEMAND;
	return qflags;
};

static enum ib_access_flags __to_ib_access_flags(int qflags)
{
	enum ib_access_flags iflags = 0;

	if (qflags & BNXT_QPLIB_ACCESS_LOCAL_WRITE)
		iflags |= IB_ACCESS_LOCAL_WRITE;
	if (qflags & BNXT_QPLIB_ACCESS_REMOTE_WRITE)
		iflags |= IB_ACCESS_REMOTE_WRITE;
	if (qflags & BNXT_QPLIB_ACCESS_REMOTE_READ)
		iflags |= IB_ACCESS_REMOTE_READ;
	if (qflags & BNXT_QPLIB_ACCESS_REMOTE_ATOMIC)
		iflags |= IB_ACCESS_REMOTE_ATOMIC;
	if (qflags & BNXT_QPLIB_ACCESS_MW_BIND)
		iflags |= IB_ACCESS_MW_BIND;
	if (qflags & BNXT_QPLIB_ACCESS_ZERO_BASED)
		iflags |= IB_ZERO_BASED;
	if (qflags & BNXT_QPLIB_ACCESS_ON_DEMAND)
		iflags |= IB_ACCESS_ON_DEMAND;
	return iflags;
};
/* Completion Queues */
int bnxt_re_destroy_cq(struct ib_cq *ib_cq)
{
	struct bnxt_re_cq *cq = to_bnxt_re(ib_cq, struct bnxt_re_cq, ib_cq);
	struct bnxt_re_dev *rdev = cq->rdev;
	int rc;

	rc = bnxt_qplib_destroy_cq(&rdev->qplib_res, &cq->qplib_cq);
	if (rc) {
		dev_err(rdev_to_dev(rdev), "Failed to destroy HW CQ");
		return rc;
	}
	if (cq->umem && !IS_ERR(cq->umem))
		ib_umem_release(cq->umem);

	if (cq) {
		kfree(cq->cql);
		kfree(cq);
	}
	atomic_dec(&rdev->cq_count);
	rdev->nq.budget--;
	return 0;
}

struct ib_cq *bnxt_re_create_cq(struct ib_device *ibdev,
				const struct ib_cq_init_attr *attr,
				struct ib_ucontext *context,
				struct ib_udata *udata)
{
	struct bnxt_re_dev *rdev = to_bnxt_re_dev(ibdev, ibdev);
	struct bnxt_qplib_dev_attr *dev_attr = &rdev->dev_attr;
	struct bnxt_re_cq *cq = NULL;
	int rc, entries;
	int cqe = attr->cqe;

	/* Validate CQ fields */
	if (cqe < 1 || cqe > dev_attr->max_cq_wqes) {
		dev_err(rdev_to_dev(rdev), "Failed to create CQ -max exceeded");
		return ERR_PTR(-EINVAL);
	}
	cq = kzalloc(sizeof(*cq), GFP_KERNEL);
	if (!cq)
		return ERR_PTR(-ENOMEM);

	cq->rdev = rdev;
	cq->qplib_cq.cq_handle = (u64)&cq->qplib_cq;

	entries = roundup_pow_of_two(cqe + 1);
	if (entries > dev_attr->max_cq_wqes + 1)
		entries = dev_attr->max_cq_wqes + 1;

	if (context) {
		struct bnxt_re_cq_req req;
		struct bnxt_re_ucontext *uctx = to_bnxt_re(context,
						   struct bnxt_re_ucontext,
						   ib_uctx);
		if (ib_copy_from_udata(&req, udata, sizeof(req))) {
			rc = -EFAULT;
			goto fail;
		}

		cq->umem = ib_umem_get(context, req.cq_va,
				       entries * sizeof(struct cq_base),
				       IB_ACCESS_LOCAL_WRITE, 1);
		if (IS_ERR(cq->umem)) {
			rc = PTR_ERR(cq->umem);
			goto fail;
		}
		cq->qplib_cq.sghead = cq->umem->sg_head.sgl;
		cq->qplib_cq.nmap = cq->umem->nmap;
		cq->qplib_cq.dpi = uctx->dpi;
	} else {
		cq->max_cql = entries > MAX_CQL_PER_POLL ? MAX_CQL_PER_POLL :
					entries;
		cq->max_cql = min_t(u32, entries, MAX_CQL_PER_POLL);
		cq->cql = kcalloc(cq->max_cql, sizeof(struct bnxt_qplib_cqe),
				  GFP_KERNEL);
		if (!cq->cql) {
			rc = -ENOMEM;
			goto fail;
		}

		cq->qplib_cq.dpi = &rdev->dpi_privileged;
		cq->qplib_cq.sghead = NULL;
		cq->qplib_cq.nmap = 0;
	}
	cq->qplib_cq.max_wqe = entries;
	cq->qplib_cq.cnq_hw_ring_id = rdev->nq.ring_id;

	rc = bnxt_qplib_create_cq(&rdev->qplib_res, &cq->qplib_cq);
	if (rc) {
		dev_err(rdev_to_dev(rdev), "Failed to create HW CQ");
		goto fail;
	}

	cq->ib_cq.cqe = entries;
	cq->cq_period = cq->qplib_cq.period;
	rdev->nq.budget++;

	atomic_inc(&rdev->cq_count);

	if (context) {
		struct bnxt_re_cq_resp resp;

		resp.cqid = cq->qplib_cq.id;
		resp.tail = cq->qplib_cq.hwq.cons;
		resp.phase = cq->qplib_cq.period;
		rc = bnxt_re_copy_to_udata(rdev, &resp, sizeof(resp), udata);
		if (rc) {
			dev_err(rdev_to_dev(rdev), "Failed to copy CQ udata");
			bnxt_qplib_destroy_cq(&rdev->qplib_res, &cq->qplib_cq);
			goto c2fail;
		}
	}

	return &cq->ib_cq;

c2fail:
	if (context && cq->umem && !IS_ERR(cq->umem))
		ib_umem_release(cq->umem);
fail:
	kfree(cq->cql);
	kfree(cq);
	return ERR_PTR(rc);
}

int bnxt_re_req_notify_cq(struct ib_cq *ib_cq,
			  enum ib_cq_notify_flags ib_cqn_flags)
{
	struct bnxt_re_cq *cq = to_bnxt_re(ib_cq, struct bnxt_re_cq, ib_cq);
	int type = 0;

	/* Trigger on the very next completion */
	if (ib_cqn_flags & IB_CQ_NEXT_COMP)
		type = DBR_DBR_TYPE_CQ_ARMALL;
	/* Trigger on the next solicited completion */
	else if (ib_cqn_flags & IB_CQ_SOLICITED)
		type = DBR_DBR_TYPE_CQ_ARMSE;

	bnxt_qplib_req_notify_cq(&cq->qplib_cq, type);

	return 0;
}

/* Memory Regions */
struct ib_mr *bnxt_re_get_dma_mr(struct ib_pd *ib_pd, int mr_access_flags)
{
	struct bnxt_re_pd *pd = to_bnxt_re(ib_pd, struct bnxt_re_pd, ib_pd);
	struct bnxt_re_dev *rdev = pd->rdev;
	struct bnxt_re_mr *mr;
	u64 pbl = 0;
	int rc;

	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr)
		return ERR_PTR(-ENOMEM);

	mr->rdev = rdev;
	mr->qplib_mr.pd = &pd->qplib_pd;
	mr->qplib_mr.flags = __from_ib_access_flags(mr_access_flags);
	mr->qplib_mr.type = CMDQ_ALLOCATE_MRW_MRW_FLAGS_PMR;

	/* Allocate and register 0 as the address */
	rc = bnxt_qplib_alloc_mrw(&rdev->qplib_res, &mr->qplib_mr);
	if (rc)
		goto fail;

	mr->qplib_mr.hwq.level = PBL_LVL_MAX;
	mr->qplib_mr.total_size = -1; /* Infinte length */
	rc = bnxt_qplib_reg_mr(&rdev->qplib_res, &mr->qplib_mr, &pbl, 0, false);
	if (rc)
		goto fail_mr;

	mr->ib_mr.lkey = mr->qplib_mr.lkey;
	if (mr_access_flags & (IB_ACCESS_REMOTE_WRITE | IB_ACCESS_REMOTE_READ |
			       IB_ACCESS_REMOTE_ATOMIC))
		mr->ib_mr.rkey = mr->ib_mr.lkey;
	atomic_inc(&rdev->mr_count);

	return &mr->ib_mr;

fail_mr:
	bnxt_qplib_free_mrw(&rdev->qplib_res, &mr->qplib_mr);
fail:
	kfree(mr);
	return ERR_PTR(rc);
}

int bnxt_re_dereg_mr(struct ib_mr *ib_mr)
{
	struct bnxt_re_mr *mr = to_bnxt_re(ib_mr, struct bnxt_re_mr, ib_mr);
	struct bnxt_re_dev *rdev = mr->rdev;
	int rc = 0;

	if (mr->npages && mr->pages) {
		rc = bnxt_qplib_free_fast_reg_page_list(&rdev->qplib_res,
							&mr->qplib_frpl);
		kfree(mr->pages);
		mr->npages = 0;
		mr->pages = NULL;
	}
	rc = bnxt_qplib_free_mrw(&rdev->qplib_res, &mr->qplib_mr);

	if (!IS_ERR(mr->ib_umem) && mr->ib_umem)
		ib_umem_release(mr->ib_umem);

	kfree(mr);
	atomic_dec(&rdev->mr_count);
	return rc;
}

static int bnxt_re_set_page(struct ib_mr *ib_mr, u64 addr)
{
	struct bnxt_re_mr *mr = to_bnxt_re(ib_mr, struct bnxt_re_mr, ib_mr);

	if (unlikely(mr->npages == mr->qplib_frpl.max_pg_ptrs))
		return -ENOMEM;

	mr->pages[mr->npages++] = addr;
	return 0;
}

int bnxt_re_map_mr_sg(struct ib_mr *ib_mr, struct scatterlist *sg, int sg_nents,
		      unsigned int *sg_offset)
{
	struct bnxt_re_mr *mr = to_bnxt_re(ib_mr, struct bnxt_re_mr, ib_mr);

	mr->npages = 0;
	return ib_sg_to_pages(ib_mr, sg, sg_nents, sg_offset, bnxt_re_set_page);
}

struct ib_mr *bnxt_re_alloc_mr(struct ib_pd *ib_pd, enum ib_mr_type type,
			       u32 max_num_sg)
{
	struct bnxt_re_pd *pd = to_bnxt_re(ib_pd, struct bnxt_re_pd, ib_pd);
	struct bnxt_re_dev *rdev = pd->rdev;
	struct bnxt_re_mr *mr = NULL;
	int rc;

	if (type != IB_MR_TYPE_MEM_REG) {
		dev_dbg(rdev_to_dev(rdev), "MR type 0x%x not supported", type);
		return ERR_PTR(-EINVAL);
	}
	if (max_num_sg > MAX_PBL_LVL_1_PGS)
		return ERR_PTR(-EINVAL);

	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr)
		return ERR_PTR(-ENOMEM);

	mr->rdev = rdev;
	mr->qplib_mr.pd = &pd->qplib_pd;
	mr->qplib_mr.flags = BNXT_QPLIB_FR_PMR;
	mr->qplib_mr.type = CMDQ_ALLOCATE_MRW_MRW_FLAGS_PMR;

	rc = bnxt_qplib_alloc_mrw(&rdev->qplib_res, &mr->qplib_mr);
	if (rc)
		goto fail;

	mr->ib_mr.lkey = mr->qplib_mr.lkey;
	mr->ib_mr.rkey = mr->ib_mr.lkey;

	mr->pages = kcalloc(max_num_sg, sizeof(u64), GFP_KERNEL);
	if (!mr->pages) {
		rc = -ENOMEM;
		goto fail;
	}
	rc = bnxt_qplib_alloc_fast_reg_page_list(&rdev->qplib_res,
						 &mr->qplib_frpl, max_num_sg);
	if (rc) {
		dev_err(rdev_to_dev(rdev),
			"Failed to allocate HW FR page list");
		goto fail_mr;
	}

	atomic_inc(&rdev->mr_count);
	return &mr->ib_mr;

fail_mr:
	bnxt_qplib_free_mrw(&rdev->qplib_res, &mr->qplib_mr);
fail:
	kfree(mr->pages);
	kfree(mr);
	return ERR_PTR(rc);
}

/* Fast Memory Regions */
struct ib_fmr *bnxt_re_alloc_fmr(struct ib_pd *ib_pd, int mr_access_flags,
				 struct ib_fmr_attr *fmr_attr)
{
	struct bnxt_re_pd *pd = to_bnxt_re(ib_pd, struct bnxt_re_pd, ib_pd);
	struct bnxt_re_dev *rdev = pd->rdev;
	struct bnxt_re_fmr *fmr;
	int rc;

	if (fmr_attr->max_pages > MAX_PBL_LVL_2_PGS ||
	    fmr_attr->max_maps > rdev->dev_attr.max_map_per_fmr) {
		dev_err(rdev_to_dev(rdev), "Allocate FMR exceeded Max limit");
		return ERR_PTR(-ENOMEM);
	}
	fmr = kzalloc(sizeof(*fmr), GFP_KERNEL);
	if (!fmr)
		return ERR_PTR(-ENOMEM);

	fmr->rdev = rdev;
	fmr->qplib_fmr.pd = &pd->qplib_pd;
	fmr->qplib_fmr.type = CMDQ_ALLOCATE_MRW_MRW_FLAGS_PMR;

	rc = bnxt_qplib_alloc_mrw(&rdev->qplib_res, &fmr->qplib_fmr);
	if (rc)
		goto fail;

	fmr->qplib_fmr.flags = __from_ib_access_flags(mr_access_flags);
	fmr->ib_fmr.lkey = fmr->qplib_fmr.lkey;
	fmr->ib_fmr.rkey = fmr->ib_fmr.lkey;

	atomic_inc(&rdev->mr_count);
	return &fmr->ib_fmr;
fail:
	kfree(fmr);
	return ERR_PTR(rc);
}

int bnxt_re_map_phys_fmr(struct ib_fmr *ib_fmr, u64 *page_list, int list_len,
			 u64 iova)
{
	struct bnxt_re_fmr *fmr = to_bnxt_re(ib_fmr, struct bnxt_re_fmr,
					     ib_fmr);
	struct bnxt_re_dev *rdev = fmr->rdev;
	int rc;

	fmr->qplib_fmr.va = iova;
	fmr->qplib_fmr.total_size = list_len * PAGE_SIZE;

	rc = bnxt_qplib_reg_mr(&rdev->qplib_res, &fmr->qplib_fmr, page_list,
			       list_len, true);
	if (rc)
		dev_err(rdev_to_dev(rdev), "Failed to map FMR for lkey = 0x%x!",
			fmr->ib_fmr.lkey);
	return rc;
}

int bnxt_re_unmap_fmr(struct list_head *fmr_list)
{
	struct bnxt_re_dev *rdev;
	struct bnxt_re_fmr *fmr;
	struct ib_fmr *ib_fmr;
	int rc = 0;

	/* Validate each FMRs inside the fmr_list */
	list_for_each_entry(ib_fmr, fmr_list, list) {
		fmr = to_bnxt_re(ib_fmr, struct bnxt_re_fmr, ib_fmr);
		rdev = fmr->rdev;

		if (rdev) {
			rc = bnxt_qplib_dereg_mrw(&rdev->qplib_res,
						  &fmr->qplib_fmr, true);
			if (rc)
				break;
		}
	}
	return rc;
}

int bnxt_re_dealloc_fmr(struct ib_fmr *ib_fmr)
{
	struct bnxt_re_fmr *fmr = to_bnxt_re(ib_fmr, struct bnxt_re_fmr,
					     ib_fmr);
	struct bnxt_re_dev *rdev = fmr->rdev;
	int rc;

	rc = bnxt_qplib_free_mrw(&rdev->qplib_res, &fmr->qplib_fmr);
	if (rc)
		dev_err(rdev_to_dev(rdev), "Failed to free FMR");

	kfree(fmr);
	atomic_dec(&rdev->mr_count);
	return rc;
}

/* uverbs */
struct ib_mr *bnxt_re_reg_user_mr(struct ib_pd *ib_pd, u64 start, u64 length,
				  u64 virt_addr, int mr_access_flags,
				  struct ib_udata *udata)
{
	struct bnxt_re_pd *pd = to_bnxt_re(ib_pd, struct bnxt_re_pd, ib_pd);
	struct bnxt_re_dev *rdev = pd->rdev;
	struct bnxt_re_mr *mr;
	struct ib_umem *umem;
	u64 *pbl_tbl, *pbl_tbl_orig;
	int i, umem_pgs, pages, page_shift, rc;
	struct scatterlist *sg;
	int entry;

	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr)
		return ERR_PTR(-ENOMEM);

	mr->rdev = rdev;
	mr->qplib_mr.pd = &pd->qplib_pd;
	mr->qplib_mr.flags = __from_ib_access_flags(mr_access_flags);
	mr->qplib_mr.type = CMDQ_ALLOCATE_MRW_MRW_FLAGS_MR;

	umem = ib_umem_get(ib_pd->uobject->context, start, length,
			   mr_access_flags, 0);
	if (IS_ERR(umem)) {
		dev_err(rdev_to_dev(rdev), "Failed to get umem");
		rc = -EFAULT;
		goto free_mr;
	}
	mr->ib_umem = umem;

	rc = bnxt_qplib_alloc_mrw(&rdev->qplib_res, &mr->qplib_mr);
	if (rc) {
		dev_err(rdev_to_dev(rdev), "Failed to allocate MR");
		goto release_umem;
	}
	/* The fixed portion of the rkey is the same as the lkey */
	mr->ib_mr.rkey = mr->qplib_mr.rkey;

	mr->qplib_mr.va = virt_addr;
	umem_pgs = ib_umem_page_count(umem);
	if (!umem_pgs) {
		dev_err(rdev_to_dev(rdev), "umem is invalid!");
		rc = -EINVAL;
		goto free_mrw;
	}
	mr->qplib_mr.total_size = length;

	pbl_tbl = kcalloc(umem_pgs, sizeof(u64 *), GFP_KERNEL);
	if (!pbl_tbl) {
		rc = -EINVAL;
		goto free_mrw;
	}
	pbl_tbl_orig = pbl_tbl;

	page_shift = ilog2(umem->page_size);
	if (umem->hugetlb) {
		dev_err(rdev_to_dev(rdev), "umem hugetlb not supported!");
		rc = -EFAULT;
		goto fail;
	}
	if (umem->page_size != PAGE_SIZE) {
		dev_err(rdev_to_dev(rdev), "umem page size unsupported!");
		rc = -EFAULT;
		goto fail;
	}
	/* Map umem buf ptrs to the PBL */
	for_each_sg(umem->sg_head.sgl, sg, umem->nmap, entry) {
		pages = sg_dma_len(sg) >> page_shift;
		for (i = 0; i < pages; i++, pbl_tbl++)
			*pbl_tbl = sg_dma_address(sg) + (i << page_shift);
	}
	rc = bnxt_qplib_reg_mr(&rdev->qplib_res, &mr->qplib_mr, pbl_tbl_orig,
			       umem_pgs, false);
	if (rc) {
		dev_err(rdev_to_dev(rdev), "Failed to register user MR");
		goto fail;
	}

	kfree(pbl_tbl_orig);

	mr->ib_mr.lkey = mr->qplib_mr.lkey;
	mr->ib_mr.rkey = mr->qplib_mr.lkey;
	atomic_inc(&rdev->mr_count);

	return &mr->ib_mr;
fail:
	kfree(pbl_tbl_orig);
free_mrw:
	bnxt_qplib_free_mrw(&rdev->qplib_res, &mr->qplib_mr);
release_umem:
	ib_umem_release(umem);
free_mr:
	kfree(mr);
	return ERR_PTR(rc);
}

struct ib_ucontext *bnxt_re_alloc_ucontext(struct ib_device *ibdev,
					   struct ib_udata *udata)
{
	struct bnxt_re_dev *rdev = to_bnxt_re_dev(ibdev, ibdev);
	struct bnxt_re_uctx_resp resp;
	struct bnxt_re_ucontext *uctx;
	struct bnxt_qplib_dev_attr *dev_attr = &rdev->dev_attr;
	int rc;

	dev_dbg(rdev_to_dev(rdev), "ABI version requested %d",
		ibdev->uverbs_abi_ver);

	if (ibdev->uverbs_abi_ver != BNXT_RE_ABI_VERSION) {
		dev_dbg(rdev_to_dev(rdev), " is different from the device %d ",
			BNXT_RE_ABI_VERSION);
		return ERR_PTR(-EPERM);
	}

	uctx = kzalloc(sizeof(*uctx), GFP_KERNEL);
	if (!uctx)
		return ERR_PTR(-ENOMEM);

	uctx->rdev = rdev;

	uctx->shpg = (void *)__get_free_page(GFP_KERNEL);
	if (!uctx->shpg) {
		rc = -ENOMEM;
		goto fail;
	}
	spin_lock_init(&uctx->sh_lock);

	resp.dev_id = rdev->en_dev->pdev->devfn; /*Temp, Use idr_alloc instead*/
	resp.max_qp = rdev->qplib_ctx.qpc_count;
	resp.pg_size = PAGE_SIZE;
	resp.cqe_sz = sizeof(struct cq_base);
	resp.max_cqd = dev_attr->max_cq_wqes;

	rc = ib_copy_to_udata(udata, &resp, sizeof(resp));
	if (rc) {
		dev_err(rdev_to_dev(rdev), "Failed to copy user context");
		rc = -EFAULT;
		goto cfail;
	}

	return &uctx->ib_uctx;
cfail:
	free_page((u64)uctx->shpg);
	uctx->shpg = NULL;
fail:
	kfree(uctx);
	return ERR_PTR(rc);
}

int bnxt_re_dealloc_ucontext(struct ib_ucontext *ib_uctx)
{
	struct bnxt_re_ucontext *uctx = to_bnxt_re(ib_uctx,
						   struct bnxt_re_ucontext,
						   ib_uctx);
	if (uctx->shpg)
		free_page((u64)uctx->shpg);
	kfree(uctx);
	return 0;
}

/* Helper function to mmap the virtual memory from user app */
int bnxt_re_mmap(struct ib_ucontext *ib_uctx, struct vm_area_struct *vma)
{
	struct bnxt_re_ucontext *uctx = to_bnxt_re(ib_uctx,
						   struct bnxt_re_ucontext,
						   ib_uctx);
	struct bnxt_re_dev *rdev = uctx->rdev;
	u64 pfn;

	if (vma->vm_end - vma->vm_start != PAGE_SIZE)
		return -EINVAL;

	if (vma->vm_pgoff) {
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		if (io_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
				       PAGE_SIZE, vma->vm_page_prot)) {
			dev_err(rdev_to_dev(rdev), "Failed to map DPI");
			return -EAGAIN;
		}
	} else {
		pfn = virt_to_phys(uctx->shpg) >> PAGE_SHIFT;
		if (remap_pfn_range(vma, vma->vm_start,
				    pfn, PAGE_SIZE, vma->vm_page_prot)) {
			dev_err(rdev_to_dev(rdev),
				"Failed to map shared page");
			return -EAGAIN;
		}
	}

	return 0;
}
