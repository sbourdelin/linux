/*
 * Copyright (c) 2016 Mellanox Technologies Ltd.  All rights reserved.
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
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
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

#ifdef CONFIG_SECURITY_INFINIBAND

#include <linux/security.h>

#include <rdma/ib_verbs.h>
#include <rdma/ib_cache.h>
#include "core_priv.h"

static int get_pkey_info(struct ib_device *dev,
			 u8 port_num,
			 u16 pkey_index,
			 u64 *subnet_prefix,
			 u16 *pkey)
{
	int err;

	err = ib_get_cached_pkey(dev, port_num, pkey_index, pkey);
	if (err)
		return err;

	err = ib_get_cached_subnet_prefix(dev, port_num, subnet_prefix);

	return err;
}

static int enforce_qp_pkey_security(struct ib_device *dev,
				    u8 port_num,
				    u16 pkey_index,
				    struct ib_qp_security *sec)
{
	struct ib_qp_security *shared_qp_sec;
	u64 subnet_prefix;
	int err = 0;
	u16 pkey;

	err = get_pkey_info(dev, port_num, pkey_index, &subnet_prefix, &pkey);
	if (err)
		return err;

	err = security_qp_pkey_access(subnet_prefix, pkey, sec);
	if (err)
		return err;

	if (sec->qp == sec->qp->real_qp) {
		/* The caller of this function holds the QP security
		 * mutex so this list traversal is safe
		*/
		list_for_each_entry(shared_qp_sec,
				    &sec->shared_qp_list,
				    shared_qp_list) {
			err = security_qp_pkey_access(subnet_prefix,
						      pkey,
						      shared_qp_sec);
			if (err)
				break;
		}
	}
	return err;
}

static int check_pkey(const struct ib_qp *qp,
		      const struct ib_qp_attr *qp_attr,
		      int qp_attr_mask)
{
	bool check_pkey = !!(qp_attr_mask & (IB_QP_PKEY_INDEX | IB_QP_PORT));

	return check_pkey && (qp->qp_num != IB_QPT_SMI &&
			      qp->qp_num != IB_QPT_GSI);
}

static int check_alt_pkey(const struct ib_qp *qp,
			  const struct ib_qp_attr *qp_attr,
			  int qp_attr_mask)
{
	bool check_alt_pkey = !!(qp_attr_mask & IB_QP_ALT_PATH);

	return check_alt_pkey && (qp->qp_num != IB_QPT_SMI &&
				  qp->qp_num != IB_QPT_GSI);
}

static int affects_security_settings(const struct ib_qp *qp,
				     const struct ib_qp_attr *qp_attr,
				     int qp_attr_mask)
{
	return check_pkey(qp, qp_attr, qp_attr_mask) ||
	       check_alt_pkey(qp, qp_attr, qp_attr_mask);
}

static void begin_port_pkey_change(struct ib_qp *qp,
				   struct ib_port_pkey *pp,
				   struct ib_port_pkey *old_pp,
				   u8 port_num,
				   u16 pkey_index)
{
	if (pp->state == IB_PORT_PKEY_NOT_VALID ||
	    (pkey_index != pp->pkey_index ||
	     port_num != pp->port_num)) {
		old_pp->pkey_index = pp->pkey_index;
		old_pp->port_num = pp->port_num;
		old_pp->state = pp->state;

		pp->port_num = port_num;
		pp->pkey_index = pkey_index;
		pp->state = IB_PORT_PKEY_CHANGING;
	}
}

static int qp_modify_enforce_security(struct ib_qp *qp,
				      const struct ib_qp_attr *qp_attr,
				      int qp_attr_mask)
{
	struct ib_qp_security *sec = qp->qp_sec;
	int err = 0;

	if (check_pkey(qp, qp_attr, qp_attr_mask)) {
		u8 port_num = (qp_attr_mask & IB_QP_PORT) ?
			       qp_attr->port_num :
			       sec->ports_pkeys.main.port_num;

		u16 pkey_index = (qp_attr_mask & IB_QP_PKEY_INDEX) ?
				  qp_attr->pkey_index :
				  sec->ports_pkeys.main.pkey_index;

		err = enforce_qp_pkey_security(qp->device,
					       port_num,
					       pkey_index,
					       sec);

		if (err)
			return err;

		begin_port_pkey_change(qp,
				       &sec->ports_pkeys.main,
				       &sec->old_ports_pkeys.main,
				       port_num,
				       pkey_index);
	}

	if (check_alt_pkey(qp, qp_attr, qp_attr_mask)) {
		err = enforce_qp_pkey_security(qp->device,
					       qp_attr->alt_port_num,
					       qp_attr->alt_pkey_index,
					       sec);

		if (err)
			return err;

		begin_port_pkey_change(qp,
				       &sec->ports_pkeys.alt,
				       &sec->old_ports_pkeys.alt,
				       qp_attr->alt_port_num,
				       qp_attr->alt_pkey_index);
	}
	return err;
}

static void abort_port_pkey_change(struct ib_qp *qp,
				   struct ib_port_pkey *pp,
				   struct ib_port_pkey *old_pp)
{
	if (pp->state == IB_PORT_PKEY_CHANGING) {
		pp->pkey_index = old_pp->pkey_index;
		pp->port_num = old_pp->port_num;
		pp->state = old_pp->state;
	}
}

static int cleanup_qp_pkey_associations(struct ib_qp *qp,
					bool revert_to_old)
{
	struct ib_qp_security *sec = qp->qp_sec;

	if (revert_to_old) {
		abort_port_pkey_change(qp,
				       &qp->qp_sec->ports_pkeys.main,
				       &qp->qp_sec->old_ports_pkeys.main);

		abort_port_pkey_change(qp,
				       &qp->qp_sec->ports_pkeys.alt,
				       &qp->qp_sec->old_ports_pkeys.alt);
	} else {
		if (sec->ports_pkeys.main.state == IB_PORT_PKEY_CHANGING)
			sec->ports_pkeys.main.state = IB_PORT_PKEY_VALID;

		if (sec->ports_pkeys.alt.state == IB_PORT_PKEY_CHANGING)
			sec->ports_pkeys.alt.state = IB_PORT_PKEY_VALID;
	}

	memset(&sec->old_ports_pkeys, 0, sizeof(sec->old_ports_pkeys));

	return 0;
}

int ib_security_open_shared_qp(struct ib_qp *qp)
{
	struct ib_qp *real_qp = qp->real_qp;
	int err;

	err = ib_security_create_qp_security(qp);
	if (err)
		goto out;

	mutex_lock(&real_qp->qp_sec->mutex);

	if (real_qp->qp_sec->ports_pkeys.main.state != IB_PORT_PKEY_NOT_VALID)
		err = enforce_qp_pkey_security(real_qp->device,
					       real_qp->qp_sec->ports_pkeys.main.port_num,
					       real_qp->qp_sec->ports_pkeys.main.pkey_index,
					       qp->qp_sec);
	if (err)
		goto err;

	if (real_qp->qp_sec->ports_pkeys.alt.state != IB_PORT_PKEY_NOT_VALID)
		err = enforce_qp_pkey_security(real_qp->device,
					       real_qp->qp_sec->ports_pkeys.alt.port_num,
					       real_qp->qp_sec->ports_pkeys.alt.pkey_index,
					       qp->qp_sec);

	if (err)
		goto err;

	if (qp != real_qp)
		list_add(&qp->qp_sec->shared_qp_list,
			 &real_qp->qp_sec->shared_qp_list);
err:
	mutex_unlock(&real_qp->qp_sec->mutex);
	if (err)
		ib_security_destroy_qp(qp->qp_sec);

out:
	return err;
}

void ib_security_close_shared_qp(struct ib_qp_security *sec)
{
	struct ib_qp *real_qp = sec->qp->real_qp;

	mutex_lock(&real_qp->qp_sec->mutex);
	list_del(&sec->shared_qp_list);
	mutex_unlock(&real_qp->qp_sec->mutex);

	ib_security_destroy_qp(sec);
}

int ib_security_create_qp_security(struct ib_qp *qp)
{
	int err;

	qp->qp_sec = kzalloc(sizeof(*qp->qp_sec), GFP_KERNEL);
	if (!qp->qp_sec)
		return -ENOMEM;

	qp->qp_sec->qp = qp;
	mutex_init(&qp->qp_sec->mutex);
	INIT_LIST_HEAD(&qp->qp_sec->shared_qp_list);
	err = security_ib_qp_alloc_security(qp->qp_sec);
	if (err)
		kfree(qp->qp_sec);

	return err;
}
EXPORT_SYMBOL(ib_security_create_qp_security);

void ib_security_destroy_qp(struct ib_qp_security *sec)
{
	security_ib_qp_free_security(sec);
	kfree(sec);
}

int ib_security_modify_qp(struct ib_qp *qp,
			  struct ib_qp_attr *qp_attr,
			  int qp_attr_mask,
			  struct ib_udata *udata)
{
	int err = 0;
	bool enforce_security = affects_security_settings(qp,
							  qp_attr,
							  qp_attr_mask);

	if (enforce_security) {
		mutex_lock(&qp->qp_sec->mutex);

		err = qp_modify_enforce_security(qp, qp_attr, qp_attr_mask);
	}

	if (!err)
		err = qp->device->modify_qp(qp->real_qp,
					    qp_attr,
					    qp_attr_mask,
					    udata);
	if (enforce_security) {
		cleanup_qp_pkey_associations(qp, !!err);
		mutex_unlock(&qp->qp_sec->mutex);
	}
	return err;
}
EXPORT_SYMBOL(ib_security_modify_qp);

#endif /* CONFIG_SECURITY_INFINIBAND */
