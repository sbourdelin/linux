/*
 * Copyright (c) 2015 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *	- Redistributions of source code must retain the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer.
 *
 *	- Redistributions in binary form must reproduce the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer in the documentation and/or other materials
 *	  provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "rvt_loc.h"

MODULE_AUTHOR("Bob Pearson, Frank Zago, John Groves, Kamal Heib");
MODULE_DESCRIPTION("Soft RDMA transport");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("0.2");

/* free resources for all ports on a device */
void rvt_cleanup_ports(struct rvt_dev *rvt)
{
	unsigned int port_num;
	struct rvt_port *port;

	for (port_num = 1; port_num <= rvt->num_ports; port_num++) {
		port = &rvt->port[port_num - 1];

		kfree(port->pkey_tbl);
		port->pkey_tbl = NULL;
	}

	kfree(rvt->port);
	rvt->port = NULL;
}

/* free resources for a rvt device all objects created for this device must
 * have been destroyed
 */
static void rvt_cleanup(struct rvt_dev *rvt)
{
	rvt_pool_cleanup(&rvt->uc_pool);
	rvt_pool_cleanup(&rvt->pd_pool);
	rvt_pool_cleanup(&rvt->ah_pool);
	rvt_pool_cleanup(&rvt->srq_pool);
	rvt_pool_cleanup(&rvt->qp_pool);
	rvt_pool_cleanup(&rvt->cq_pool);
	rvt_pool_cleanup(&rvt->mr_pool);
	rvt_pool_cleanup(&rvt->fmr_pool);
	rvt_pool_cleanup(&rvt->mw_pool);
	rvt_pool_cleanup(&rvt->mc_grp_pool);
	rvt_pool_cleanup(&rvt->mc_elem_pool);

	rvt_cleanup_ports(rvt);
}

/* called when all references have been dropped */
void rvt_release(struct kref *kref)
{
	struct rvt_dev *rvt = container_of(kref, struct rvt_dev, ref_cnt);

	rvt_cleanup(rvt);
	ib_dealloc_device(&rvt->ib_dev);
}

void rvt_dev_put(struct rvt_dev *rvt)
{
	kref_put(&rvt->ref_cnt, rvt_release);
}
EXPORT_SYMBOL_GPL(rvt_dev_put);

int rvt_set_mtu(struct rvt_dev *rvt, unsigned int ndev_mtu,
		unsigned int port_num)
{
	struct rvt_port *port = &rvt->port[port_num - 1];
	enum ib_mtu mtu;

	mtu = eth_mtu_int_to_enum(ndev_mtu);

	/* Make sure that new MTU in range */
	mtu = mtu ? min_t(enum ib_mtu, mtu, RVT_PORT_MAX_MTU) : IB_MTU_256;

	port->attr.active_mtu = mtu;
	port->mtu_cap = ib_mtu_enum_to_int(mtu);

	return 0;
}
EXPORT_SYMBOL(rvt_set_mtu);

void rvt_send_done(void *rvt_ctx)
{
	struct rvt_qp *qp = (struct rvt_qp *)rvt_ctx;
	int skb_out = atomic_dec_return(&qp->skb_out);

	if (unlikely(qp->need_req_skb &&
		     skb_out < RVT_INFLIGHT_SKBS_PER_QP_LOW))
		rvt_run_task(&qp->req.task, 1);
}
EXPORT_SYMBOL(rvt_send_done);

static int __init rvt_module_init(void)
{
	int err;

	/* initialize slab caches for managed objects */
	err = rvt_cache_init();
	if (err) {
		pr_err("rvt: unable to init object pools\n");
		return err;
	}

	pr_info("rvt: loaded\n");

	return 0;
}

static void __exit rvt_module_exit(void)
{
	rvt_cache_exit();

	pr_info("rvt: unloaded\n");
}

module_init(rvt_module_init);
module_exit(rvt_module_exit);
