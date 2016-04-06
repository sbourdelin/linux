/* NEW COMMIT TO INSERT INTO REBASE
 *
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
#include <linux/list.h>

#include <rdma/ib_verbs.h>
#include <rdma/ib_cache.h>
#include "core_priv.h"

static struct pkey_index_qp_list *get_pkey_index_qp_list(struct ib_device *dev,
							 u8 port_num,
							 u16 index)
{
	struct pkey_index_qp_list *tmp_pkey;

	list_for_each_entry(tmp_pkey,
			    &dev->port_pkey_list[port_num].pkey_list,
			    pkey_index_list) {
		if (tmp_pkey->pkey_index == index)
			return tmp_pkey;
	}
	return NULL;
}

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

static int check_qp_port_pkey_settings(struct ib_qp_security *sec)
{
	struct ib_qp *real_qp = sec->qp->real_qp;
	int err = 0;

	if (real_qp->qp_sec->ports_pkeys.main.state != IB_PORT_PKEY_NOT_VALID)
		err = enforce_qp_pkey_security(real_qp->device,
					       real_qp->qp_sec->ports_pkeys.main.port_num,
					       real_qp->qp_sec->ports_pkeys.main.pkey_index,
					       sec);
	if (err)
		goto out;

	if (real_qp->qp_sec->ports_pkeys.alt.state != IB_PORT_PKEY_NOT_VALID)
		err = enforce_qp_pkey_security(real_qp->device,
					       real_qp->qp_sec->ports_pkeys.alt.port_num,
					       real_qp->qp_sec->ports_pkeys.alt.pkey_index,
					       sec);

	if (err)
		goto out;

out:
	return err;
}

static void reset_qp(struct ib_qp_security *sec)
{
	struct ib_qp_security *shared_qp_sec;
	struct ib_qp_attr attr = {
		.qp_state = IB_QPS_ERR
	};
	struct ib_event event = {
		.event = IB_EVENT_QP_FATAL
	};

	mutex_lock(&sec->mutex);
	if (sec->destroying)
		goto unlock;

	ib_modify_qp(sec->qp,
		     &attr,
		     IB_QP_STATE);

	if (sec->qp->event_handler && sec->qp->qp_context) {
		event.element.qp = sec->qp;
		sec->qp->event_handler(&event,
				       sec->qp->qp_context);
	}

	list_for_each_entry(shared_qp_sec,
			    &sec->shared_qp_list,
			    shared_qp_list) {
		struct ib_qp *qp = shared_qp_sec->qp;

		if (qp->event_handler && qp->qp_context) {
			event.element.qp = qp;
			event.device = qp->device;
			qp->event_handler(&event,
					  qp->qp_context);
		}
	}
unlock:
	mutex_unlock(&sec->mutex);
}

static inline void check_pkey_qps(struct pkey_index_qp_list *pkey,
				  struct ib_device *device,
				  u8 port_num,
				  u64 subnet_prefix)
{
	struct ib_qp_security *shared_qp_sec;
	struct ib_port_pkey *pp, *tmp_pp;
	LIST_HEAD(reset_list);
	u16 pkey_val;

	if (!ib_get_cached_pkey(device,
				port_num,
				pkey->pkey_index,
				&pkey_val)) {
		spin_lock(&pkey->qp_list_lock);
		list_for_each_entry(pp, &pkey->qp_list, qp_list) {
			if (pp->sec->destroying)
				continue;

			if (security_qp_pkey_access(subnet_prefix,
						    pkey_val,
						    pp->sec)) {
				list_add(&pp->reset_list,
					 &reset_list);
			} else {
				list_for_each_entry(shared_qp_sec,
						    &pp->sec->shared_qp_list,
						    shared_qp_list) {
					if (security_qp_pkey_access(subnet_prefix,
								    pkey_val,
								    shared_qp_sec)) {
						list_add(&pp->reset_list,
							 &reset_list);
						break;
					}
				}
			}
		}
		spin_unlock(&pkey->qp_list_lock);
	}

	list_for_each_entry_safe(pp,
				 tmp_pp,
				 &reset_list,
				 reset_list) {
		reset_qp(pp->sec);
		list_del(&pp->reset_list);
	}
}

static int port_pkey_list_insert(struct ib_port_pkey *pp,
				 u8  port_num,
				 u16 index)
{
	struct pkey_index_qp_list *pkey;
	struct ib_device *device = pp->sec->dev;
	int err = 0;

	spin_lock(&device->port_pkey_list[port_num].list_lock);
	pkey = get_pkey_index_qp_list(pp->sec->dev, port_num, index);
	if (pkey)
		goto list_qp;

	pkey = kzalloc(sizeof(*pkey), GFP_ATOMIC);
	if (!pkey) {
		spin_unlock(&device->port_pkey_list[port_num].list_lock);
		return -ENOMEM;
	}

	pkey->pkey_index = index;
	spin_lock_init(&pkey->qp_list_lock);
	INIT_LIST_HEAD(&pkey->qp_list);
	list_add(&pkey->pkey_index_list,
		 &device->port_pkey_list[port_num].pkey_list);

list_qp:
	spin_unlock(&device->port_pkey_list[port_num].list_lock);

	spin_lock(&pkey->qp_list_lock);
	list_add(&pp->qp_list, &pkey->qp_list);
	spin_unlock(&pkey->qp_list_lock);

	return err;
}

static int port_pkey_list_remove(struct ib_port_pkey *pp)
{
	struct pkey_index_qp_list *pkey;
	int err = 0;

	pkey = get_pkey_index_qp_list(pp->sec->dev,
				      pp->port_num,
				      pp->pkey_index);
	if (!pkey)
		return -ENOENT;

	spin_lock(&pkey->qp_list_lock);
	list_del(&pp->qp_list);
	pp->state = IB_PORT_PKEY_NOT_VALID;
	spin_unlock(&pkey->qp_list_lock);
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

static int begin_port_pkey_change(struct ib_port_pkey *pp,
				  struct ib_port_pkey *old_pp,
				  u8 port_num,
				  u16 pkey_index)
{
	int err;

	if (pp->state == IB_PORT_PKEY_NOT_VALID ||
	    (pkey_index != pp->pkey_index ||
	     port_num != pp->port_num)) {
		err = port_pkey_list_insert(pp,
					    port_num,
					    pkey_index);

		if (err)
			return err;

		old_pp->pkey_index = pp->pkey_index;
		old_pp->port_num = pp->port_num;
		old_pp->state = pp->state;

		pp->port_num = port_num;
		pp->pkey_index = pkey_index;
		pp->state = IB_PORT_PKEY_CHANGING;
	}
	return 0;
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

		err = begin_port_pkey_change(&sec->ports_pkeys.main,
					     &sec->old_ports_pkeys.main,
					     port_num,
					     pkey_index);
		if (err)
			return err;
	}

	if (check_alt_pkey(qp, qp_attr, qp_attr_mask)) {
		err = enforce_qp_pkey_security(qp->device,
					       qp_attr->alt_port_num,
					       qp_attr->alt_pkey_index,
					       sec);

		if (err)
			return err;

		err = begin_port_pkey_change(&sec->ports_pkeys.alt,
					     &sec->old_ports_pkeys.alt,
					     qp_attr->alt_port_num,
					     qp_attr->alt_pkey_index);
	}
	return err;
}

static void abort_port_pkey_change(struct ib_port_pkey *pp,
				   struct ib_port_pkey *old_pp)
{
	if (pp->state == IB_PORT_PKEY_CHANGING) {
		port_pkey_list_remove(pp);

		pp->pkey_index = old_pp->pkey_index;
		pp->port_num = old_pp->port_num;
		pp->state = old_pp->state;
	}
}

static void end_port_pkey_change(struct ib_port_pkey *pp,
				 struct ib_port_pkey *old_pp)
{
	if (pp->state == IB_PORT_PKEY_CHANGING)
		pp->state = IB_PORT_PKEY_VALID;

	if (old_pp->state == IB_PORT_PKEY_VALID)
		port_pkey_list_remove(old_pp);
}

static int cleanup_qp_pkey_associations(struct ib_qp *qp,
					bool revert_to_old)
{
	if (revert_to_old) {
		abort_port_pkey_change(&qp->qp_sec->ports_pkeys.main,
				       &qp->qp_sec->old_ports_pkeys.main);

		abort_port_pkey_change(&qp->qp_sec->ports_pkeys.alt,
				       &qp->qp_sec->old_ports_pkeys.alt);
	} else {
		end_port_pkey_change(&qp->qp_sec->ports_pkeys.main,
				     &qp->qp_sec->old_ports_pkeys.main);

		end_port_pkey_change(&qp->qp_sec->ports_pkeys.alt,
				     &qp->qp_sec->old_ports_pkeys.alt);
	}

	return 0;
}

static void destroy_qp_security(struct ib_qp_security *sec)
{
	security_ib_qp_free_security(sec);
	kfree(sec);
}

static void qp_lists_lock_unlock(struct ib_qp_security *sec,
				 bool lock)
{
	struct ib_port_pkey *prim = NULL;
	struct ib_port_pkey *alt = NULL;
	struct ib_port_pkey *first = NULL;
	struct ib_port_pkey *second = NULL;
	struct pkey_index_qp_list *pkey;

	if (sec->ports_pkeys.main.state != IB_PORT_PKEY_NOT_VALID)
		prim = &sec->ports_pkeys.main;

	if (sec->ports_pkeys.alt.state != IB_PORT_PKEY_NOT_VALID)
		alt = &sec->ports_pkeys.alt;

	if (prim && alt) {
		if (prim->port_num != alt->port_num) {
			first = prim->port_num < alt->port_num ? prim : alt;
			second = prim->port_num >= alt->port_num ? prim : alt;
		} else {
			first = prim->pkey_index < alt->pkey_index ?
				prim : alt;
			second = prim->pkey_index >= alt->pkey_index ?
				alt : prim;
		}
	} else {
		first = !prim ? alt : prim;
	}

	if (first) {
		pkey = get_pkey_index_qp_list(sec->dev,
					      first->port_num,
					      first->pkey_index);
		if (lock)
			spin_lock(&pkey->qp_list_lock);
		else
			spin_unlock(&pkey->qp_list_lock);
	}

	if (second) {
		pkey = get_pkey_index_qp_list(sec->dev,
					      second->port_num,
					      second->pkey_index);
		if (lock)
			spin_lock(&pkey->qp_list_lock);
		else
			spin_unlock(&pkey->qp_list_lock);
	}
}

int ib_security_open_shared_qp(struct ib_qp *qp, struct ib_device *dev)
{
	struct ib_qp *real_qp = qp->real_qp;
	int err;

	err = ib_security_create_qp_security(qp, dev);
	if (err)
		goto out;

	mutex_lock(&real_qp->qp_sec->mutex);
	err = check_qp_port_pkey_settings(qp->qp_sec);

	if (err)
		goto err;

	if (qp != real_qp)
		list_add(&qp->qp_sec->shared_qp_list,
			 &real_qp->qp_sec->shared_qp_list);
err:
	mutex_unlock(&real_qp->qp_sec->mutex);
	if (err)
		destroy_qp_security(qp->qp_sec);

out:
	return err;
}

void ib_security_close_shared_qp(struct ib_qp_security *sec)
{
	struct ib_qp *real_qp = sec->qp->real_qp;

	mutex_lock(&real_qp->qp_sec->mutex);
	list_del(&sec->shared_qp_list);
	mutex_unlock(&real_qp->qp_sec->mutex);

	destroy_qp_security(sec);
}

int ib_security_create_qp_security(struct ib_qp *qp, struct ib_device *dev)
{
	int err;

	qp->qp_sec = kzalloc(sizeof(*qp->qp_sec), GFP_KERNEL);
	if (!qp->qp_sec)
		return -ENOMEM;

	qp->qp_sec->qp = qp;
	qp->qp_sec->dev = dev;
	qp->qp_sec->ports_pkeys.main.sec = qp->qp_sec;
	qp->qp_sec->ports_pkeys.alt.sec = qp->qp_sec;
	mutex_init(&qp->qp_sec->mutex);
	INIT_LIST_HEAD(&qp->qp_sec->shared_qp_list);
	err = security_ib_qp_alloc_security(qp->qp_sec);
	if (err)
		kfree(qp->qp_sec);

	return err;
}
EXPORT_SYMBOL(ib_security_create_qp_security);

void ib_security_destroy_qp_end(struct ib_qp_security *sec)
{
	mutex_lock(&sec->mutex);
	if (sec->ports_pkeys.main.state != IB_PORT_PKEY_NOT_VALID)
		port_pkey_list_remove(&sec->ports_pkeys.main);

	if (sec->ports_pkeys.alt.state != IB_PORT_PKEY_NOT_VALID)
		port_pkey_list_remove(&sec->ports_pkeys.alt);

	memset(&sec->ports_pkeys, 0, sizeof(sec->ports_pkeys));
	mutex_unlock(&sec->mutex);
	destroy_qp_security(sec);
}

void ib_security_destroy_qp_abort(struct ib_qp_security *sec)
{
	int err;

	mutex_lock(&sec->mutex);
	qp_lists_lock_unlock(sec, true);
	err = check_qp_port_pkey_settings(sec);
	if (err)
		reset_qp(sec);
	sec->destroying = false;
	qp_lists_lock_unlock(sec, false);
	mutex_unlock(&sec->mutex);
}

void ib_security_destroy_qp_begin(struct ib_qp_security *sec)
{
	mutex_lock(&sec->mutex);
	qp_lists_lock_unlock(sec, true);
	sec->destroying = true;
	qp_lists_lock_unlock(sec, false);
	mutex_unlock(&sec->mutex);
}

void ib_security_cache_change(struct ib_device *device,
			      u8 port_num,
			      u64 subnet_prefix)
{
	struct pkey_index_qp_list *pkey;

	list_for_each_entry(pkey,
			    &device->port_pkey_list[port_num].pkey_list,
			    pkey_index_list) {
		check_pkey_qps(pkey,
			       device,
			       port_num,
			       subnet_prefix);
	}
}

void ib_security_destroy_port_pkey_list(struct ib_device *device)
{
	struct pkey_index_qp_list *pkey, *tmp_pkey;
	struct ib_port_pkey *pp, *tmp_pp;
	int i;

	for (i = rdma_start_port(device); i <= rdma_end_port(device); i++) {
		spin_lock(&device->port_pkey_list[i].list_lock);
		list_for_each_entry_safe(pkey,
					 tmp_pkey,
					 &device->port_pkey_list[i].pkey_list,
					 pkey_index_list) {
			spin_lock(&pkey->qp_list_lock);
			list_for_each_entry_safe(pp,
						 tmp_pp,
						 &pkey->qp_list,
						 qp_list) {
				if (pp->state != IB_PORT_PKEY_NOT_VALID)
					list_del(&pp->qp_list);
			}
			spin_unlock(&pkey->qp_list_lock);

			list_del(&pkey->pkey_index_list);
			kfree(pkey);
		}
		spin_unlock(&device->port_pkey_list[i].list_lock);
	}
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

int ib_security_enforce_mad_agent_pkey_access(struct ib_device *dev,
					      u8 port_num,
					      u16 pkey_index,
					      struct ib_mad_agent *mad_agent)
{
	u64 subnet_prefix;
	u16 pkey;
	int err;

	err = get_pkey_info(dev, port_num, pkey_index, &subnet_prefix, &pkey);
	if (err)
		return err;

	return security_mad_agent_pkey_access(subnet_prefix, pkey, mad_agent);
}
EXPORT_SYMBOL(ib_security_enforce_mad_agent_pkey_access);

#endif /* CONFIG_SECURITY_INFINIBAND */
