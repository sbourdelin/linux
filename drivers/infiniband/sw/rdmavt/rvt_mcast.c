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
 *	   Redistribution and use in source and binary forms, with or
 *	   without modification, are permitted provided that the following
 *	   conditions are met:
 *
 *		- Redistributions of source code must retain the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer.
 *
 *		- Redistributions in binary form must reproduce the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer in the documentation and/or other materials
 *		  provided with the distribution.
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

int rvt_mcast_get_grp(struct rvt_dev *rvt, union ib_gid *mgid,
		      struct rvt_mc_grp **grp_p)
{
	int err;
	struct rvt_mc_grp *grp;

	if (rvt->attr.max_mcast_qp_attach == 0) {
		err = -EINVAL;
		goto err1;
	}

	grp = rvt_pool_get_key(&rvt->mc_grp_pool, mgid);
	if (grp)
		goto done;

	grp = rvt_alloc(&rvt->mc_grp_pool);
	if (!grp) {
		err = -ENOMEM;
		goto err1;
	}

	INIT_LIST_HEAD(&grp->qp_list);
	spin_lock_init(&grp->mcg_lock);
	grp->rvt = rvt;

	rvt_add_key(grp, mgid);

	err = rvt->ifc_ops->mcast_add(rvt, mgid);
	if (err)
		goto err2;

done:
	*grp_p = grp;
	return 0;

err2:
	rvt_drop_ref(grp);
err1:
	return err;
}

int rvt_mcast_add_grp_elem(struct rvt_dev *rvt, struct rvt_qp *qp,
			   struct rvt_mc_grp *grp)
{
	int err;
	struct rvt_mc_elem *elem;

	/* check to see of the qp is already a member of the group */
	spin_lock_bh(&qp->grp_lock);
	spin_lock_bh(&grp->mcg_lock);
	list_for_each_entry(elem, &grp->qp_list, qp_list) {
		if (elem->qp == qp) {
			err = 0;
			goto out;
		}
	}

	if (grp->num_qp >= rvt->attr.max_mcast_qp_attach) {
		err = -ENOMEM;
		goto out;
	}

	elem = rvt_alloc(&rvt->mc_elem_pool);
	if (!elem) {
		err = -ENOMEM;
		goto out;
	}

	/* each qp holds a ref on the grp */
	rvt_add_ref(grp);

	grp->num_qp++;
	elem->qp = qp;
	elem->grp = grp;

	list_add(&elem->qp_list, &grp->qp_list);
	list_add(&elem->grp_list, &qp->grp_list);

	err = 0;
out:
	spin_unlock_bh(&grp->mcg_lock);
	spin_unlock_bh(&qp->grp_lock);
	return err;
}

int rvt_mcast_drop_grp_elem(struct rvt_dev *rvt, struct rvt_qp *qp,
			    union ib_gid *mgid)
{
	struct rvt_mc_grp *grp;
	struct rvt_mc_elem *elem, *tmp;

	grp = rvt_pool_get_key(&rvt->mc_grp_pool, mgid);
	if (!grp)
		goto err1;

	spin_lock_bh(&qp->grp_lock);
	spin_lock_bh(&grp->mcg_lock);

	list_for_each_entry_safe(elem, tmp, &grp->qp_list, qp_list) {
		if (elem->qp == qp) {
			list_del(&elem->qp_list);
			list_del(&elem->grp_list);
			grp->num_qp--;

			spin_unlock_bh(&grp->mcg_lock);
			spin_unlock_bh(&qp->grp_lock);
			rvt_drop_ref(elem);
			rvt_drop_ref(grp);	/* ref held by QP */
			rvt_drop_ref(grp);	/* ref from get_key */
			return 0;
		}
	}

	spin_unlock_bh(&grp->mcg_lock);
	spin_unlock_bh(&qp->grp_lock);
	rvt_drop_ref(grp);			/* ref from get_key */
err1:
	return -EINVAL;
}

void rvt_drop_all_mcast_groups(struct rvt_qp *qp)
{
	struct rvt_mc_grp *grp;
	struct rvt_mc_elem *elem;

	while (1) {
		spin_lock_bh(&qp->grp_lock);
		if (list_empty(&qp->grp_list)) {
			spin_unlock_bh(&qp->grp_lock);
			break;
		}
		elem = list_first_entry(&qp->grp_list, struct rvt_mc_elem,
					grp_list);
		list_del(&elem->grp_list);
		spin_unlock_bh(&qp->grp_lock);

		grp = elem->grp;
		spin_lock_bh(&grp->mcg_lock);
		list_del(&elem->qp_list);
		grp->num_qp--;
		spin_unlock_bh(&grp->mcg_lock);
		rvt_drop_ref(grp);
		rvt_drop_ref(elem);
	}
}

void rvt_mc_cleanup(void *arg)
{
	struct rvt_mc_grp *grp = arg;
	struct rvt_dev *rvt = grp->rvt;

	rvt_drop_key(grp);
	rvt->ifc_ops->mcast_delete(rvt, &grp->mgid);
}
