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
 * Description: Main component of the bnxt_re driver
 */

#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/ethtool.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/spinlock.h>
#include <linux/pci.h>
#include <net/ipv6.h>
#include <net/addrconf.h>

#include <rdma/ib_verbs.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_addr.h>

#include "bnxt_ulp.h"
#include "roce_hsi.h"
#include "bnxt_re.h"
#include "bnxt.h"
static char version[] =
		BNXT_RE_DESC " v" ROCE_DRV_MODULE_VERSION "\n";

MODULE_AUTHOR("Eddie Wai <eddie.wai@broadcom.com>");
MODULE_DESCRIPTION(BNXT_RE_DESC " Driver");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION(ROCE_DRV_MODULE_VERSION);

/* globals */
static struct list_head bnxt_re_dev_list = LIST_HEAD_INIT(bnxt_re_dev_list);
/* Mutex to protect the list of bnxt_re devices added */
static DEFINE_MUTEX(bnxt_re_dev_lock);
static struct workqueue_struct *bnxt_re_wq;

/* for handling bnxt_en callbacks later */
static void bnxt_re_stop(void *p)
{
}

static void bnxt_re_start(void *p)
{
}

static void bnxt_re_sriov_config(void *p, int num_vfs)
{
}

static struct bnxt_ulp_ops bnxt_re_ulp_ops = {
	.ulp_async_notifier = NULL,
	.ulp_stop = bnxt_re_stop,
	.ulp_start = bnxt_re_start,
	.ulp_sriov_config = bnxt_re_sriov_config
};

/* The rdev ref_count is to protect immature removal of the device */
static inline void bnxt_re_hold(struct bnxt_re_dev *rdev)
{
	atomic_inc(&rdev->ref_count);
}

static inline void bnxt_re_put(struct bnxt_re_dev *rdev)
{
	atomic_dec(&rdev->ref_count);
}

/* RoCE -> Net driver */

/* Driver registration routines used to let the networking driver (bnxt_en)
 * to know that the RoCE driver is now installed
 */
static int bnxt_re_unregister_netdev(struct bnxt_re_dev *rdev, bool lock_wait)
{
	struct bnxt_en_dev *en_dev;
	int rc;

	if (!rdev)
		return -EINVAL;

	en_dev = rdev->en_dev;
	/* Acquire rtnl lock if it is not invokded from netdev event */
	if (lock_wait)
		rtnl_lock();

	rc = en_dev->en_ops->bnxt_unregister_device(rdev->en_dev,
						    BNXT_ROCE_ULP);
	if (lock_wait)
		rtnl_unlock();
	return rc;
}

static int bnxt_re_register_netdev(struct bnxt_re_dev *rdev)
{
	struct bnxt_en_dev *en_dev;
	int rc = 0;

	if (!rdev)
		return -EINVAL;

	en_dev = rdev->en_dev;

	rtnl_lock();
	rc = en_dev->en_ops->bnxt_register_device(en_dev, BNXT_ROCE_ULP,
						  &bnxt_re_ulp_ops, rdev);
	rtnl_unlock();
	return rc;
}

static int bnxt_re_free_msix(struct bnxt_re_dev *rdev, bool lock_wait)
{
	struct bnxt_en_dev *en_dev;
	int rc;

	if (!rdev)
		return -EINVAL;

	en_dev = rdev->en_dev;

	if (lock_wait)
		rtnl_lock();

	rc = en_dev->en_ops->bnxt_free_msix(rdev->en_dev, BNXT_ROCE_ULP);

	if (lock_wait)
		rtnl_unlock();
	return rc;
}

static int bnxt_re_request_msix(struct bnxt_re_dev *rdev)
{
	int rc = 0, num_msix_want = BNXT_RE_MIN_MSIX, num_msix_got;
	struct bnxt_en_dev *en_dev;

	if (!rdev)
		return -EINVAL;

	en_dev = rdev->en_dev;

	rtnl_lock();
	num_msix_got = en_dev->en_ops->bnxt_request_msix(en_dev, BNXT_ROCE_ULP,
							 rdev->msix_entries,
							 num_msix_want);
	if (num_msix_got < BNXT_RE_MIN_MSIX) {
		rc = -EINVAL;
		goto done;
	}
	if (num_msix_got != num_msix_want) {
		dev_warn(rdev_to_dev(rdev),
			 "Requested %d MSI-X vectors, got %d\n",
			 num_msix_want, num_msix_got);
	}
	rdev->num_msix = num_msix_got;
done:
	rtnl_unlock();
	return rc;
}

/* Device */

static bool is_bnxt_re_dev(struct net_device *netdev)
{
	struct ethtool_drvinfo drvinfo;

	if (netdev->ethtool_ops && netdev->ethtool_ops->get_drvinfo) {
		memset(&drvinfo, 0, sizeof(drvinfo));
		netdev->ethtool_ops->get_drvinfo(netdev, &drvinfo);

		if (strcmp(drvinfo.driver, "bnxt_en"))
			return false;
		return true;
	}
	return false;
}

static struct bnxt_re_dev *bnxt_re_from_netdev(struct net_device *netdev)
{
	struct bnxt_re_dev *rdev;

	rcu_read_lock();
	list_for_each_entry_rcu(rdev, &bnxt_re_dev_list, list) {
		if (rdev->netdev == netdev) {
			rcu_read_unlock();
			return rdev;
		}
	}
	rcu_read_unlock();
	return NULL;
}

static void bnxt_re_dev_unprobe(struct net_device *netdev,
				struct bnxt_en_dev *en_dev)
{
	dev_put(netdev);
	module_put(en_dev->pdev->driver->driver.owner);
}

static struct bnxt_en_dev *bnxt_re_dev_probe(struct net_device *netdev)
{
	struct bnxt *bp = netdev_priv(netdev);
	struct bnxt_en_dev *en_dev;
	struct pci_dev *pdev;

	/* Call bnxt_en's RoCE probe via indirect API */
	if (!bp->ulp_probe)
		return ERR_PTR(-EINVAL);

	en_dev = bp->ulp_probe(netdev);
	if (IS_ERR(en_dev))
		return en_dev;

	pdev = en_dev->pdev;
	if (!pdev)
		return ERR_PTR(-EINVAL);

	/* Bump net device reference count */
	if (!try_module_get(pdev->driver->driver.owner))
		return ERR_PTR(-ENODEV);

	dev_hold(netdev);

	return en_dev;
}

static void bnxt_re_dev_remove(struct bnxt_re_dev *rdev)
{
	int i = BNXT_RE_REF_WAIT_COUNT;

	/* Wait for rdev refcount to come down */
	while ((atomic_read(&rdev->ref_count) > 1) && i--)
		msleep(100);

	if (atomic_read(&rdev->ref_count) > 1)
		dev_err(rdev_to_dev(rdev),
			"Failed waiting for ref count to deplete %d",
			atomic_read(&rdev->ref_count));

	atomic_set(&rdev->ref_count, 0);
	dev_put(rdev->netdev);
	rdev->netdev = NULL;

	mutex_lock(&bnxt_re_dev_lock);
	list_del_rcu(&rdev->list);
	mutex_unlock(&bnxt_re_dev_lock);

	synchronize_rcu();
	flush_workqueue(bnxt_re_wq);

	ib_dealloc_device(&rdev->ibdev);
	/* rdev is gone */
}

static struct bnxt_re_dev *bnxt_re_dev_add(struct net_device *netdev,
					   struct bnxt_en_dev *en_dev)
{
	struct bnxt_re_dev *rdev;

	/* Allocate bnxt_re_dev instance here */
	rdev = (struct bnxt_re_dev *)ib_alloc_device(sizeof(*rdev));
	if (!rdev) {
		dev_err(NULL, "%s: bnxt_re_dev allocation failure!",
			ROCE_DRV_MODULE_NAME);
		return NULL;
	}
	/* Default values */
	atomic_set(&rdev->ref_count, 0);
	rdev->netdev = netdev;
	dev_hold(rdev->netdev);
	rdev->en_dev = en_dev;
	rdev->id = rdev->en_dev->pdev->devfn;
	INIT_LIST_HEAD(&rdev->qp_list);
	mutex_init(&rdev->qp_lock);
	atomic_set(&rdev->qp_count, 0);
	atomic_set(&rdev->cq_count, 0);
	atomic_set(&rdev->srq_count, 0);
	atomic_set(&rdev->mr_count, 0);
	atomic_set(&rdev->mw_count, 0);
	rdev->cosq[0] = 0xFFFF;
	rdev->cosq[1] = 0xFFFF;

	mutex_lock(&bnxt_re_dev_lock);
	list_add_tail_rcu(&rdev->list, &bnxt_re_dev_list);
	mutex_unlock(&bnxt_re_dev_lock);
	return rdev;
}

static void bnxt_re_ib_unreg(struct bnxt_re_dev *rdev, bool lock_wait)
{
	int rc;

	if (test_and_clear_bit(BNXT_RE_FLAG_GOT_MSIX, &rdev->flags)) {
		rc = bnxt_re_free_msix(rdev, lock_wait);
		if (rc)
			dev_warn(rdev_to_dev(rdev),
				 "Failed to free MSI-X vectors: %#x", rc);
	}
	if (test_and_clear_bit(BNXT_RE_FLAG_NETDEV_REGISTERED, &rdev->flags)) {
		rc = bnxt_re_unregister_netdev(rdev, lock_wait);
		if (rc)
			dev_warn(rdev_to_dev(rdev),
				 "Failed to unregister with netdev: %#x", rc);
	}
}

static int bnxt_re_ib_reg(struct bnxt_re_dev *rdev)
{
	int i, j, rc;

	/* Registered a new RoCE device instance to netdev */
	rc = bnxt_re_register_netdev(rdev);
	if (rc) {
		pr_err("Failed to register with netedev: %#x\n", rc);
		return -EINVAL;
	}
	set_bit(BNXT_RE_FLAG_NETDEV_REGISTERED, &rdev->flags);

	rc = bnxt_re_request_msix(rdev);
	if (rc) {
		pr_err("Failed to get MSI-X vectors: %#x\n", rc);
		rc = -EINVAL;
		goto fail;
	}
	set_bit(BNXT_RE_FLAG_GOT_MSIX, &rdev->flags);

	return 0;
fail:
	bnxt_re_ib_unreg(rdev, true);
	return rc;
}

static void bnxt_re_dev_unreg(struct bnxt_re_dev *rdev)
{
	struct bnxt_en_dev *en_dev = rdev->en_dev;
	struct net_device *netdev = rdev->netdev;

	bnxt_re_dev_remove(rdev);

	if (netdev)
		bnxt_re_dev_unprobe(netdev, en_dev);
}

static int bnxt_re_dev_reg(struct bnxt_re_dev **rdev, struct net_device *netdev)
{
	struct bnxt_en_dev *en_dev;
	int rc = 0;

	if (!is_bnxt_re_dev(netdev))
		return -ENODEV;

	en_dev = bnxt_re_dev_probe(netdev);
	if (IS_ERR(en_dev)) {
		pr_err("%s: Failed to probe\n", ROCE_DRV_MODULE_NAME);
		rc = PTR_ERR(en_dev);
		goto exit;
	}
	*rdev = bnxt_re_dev_add(netdev, en_dev);
	if (!*rdev) {
		rc = -ENOMEM;
		bnxt_re_dev_unprobe(netdev, en_dev);
		goto exit;
	}
	bnxt_re_hold(*rdev);
exit:
	return rc;
}

static void bnxt_re_remove_one(struct bnxt_re_dev *rdev)
{
	pci_dev_put(rdev->en_dev->pdev);
}

/* Handle all deferred netevents tasks */
static void bnxt_re_task(struct work_struct *work)
{
	struct bnxt_re_work *re_work;
	struct bnxt_re_dev *rdev;
	int rc = 0;

	re_work = container_of(work, struct bnxt_re_work, work);
	rdev = re_work->rdev;

	if (re_work->event != NETDEV_REGISTER &&
	    !test_bit(BNXT_RE_FLAG_IBDEV_REGISTERED, &rdev->flags))
		return;

	switch (re_work->event) {
	case NETDEV_REGISTER:
		rc = bnxt_re_ib_reg(rdev);
		if (rc)
			dev_err(rdev_to_dev(rdev),
				"Failed to register with IB: %#x", rc);
		break;
	case NETDEV_UP:

		break;
	case NETDEV_DOWN:

		break;

	case NETDEV_CHANGE:

		break;
	default:
		break;
	}
	kfree(re_work);
}

static void bnxt_re_init_one(struct bnxt_re_dev *rdev)
{
	pci_dev_get(rdev->en_dev->pdev);
}

/*
 * "Notifier chain callback can be invoked for the same chain from
 * different CPUs at the same time".
 *
 * For cases when the netdev is already present, our call to the
 * register_netdevice_notifier() will actually get the rtnl_lock()
 * before sending NETDEV_REGISTER and (if up) NETDEV_UP
 * events.
 *
 * But for cases when the netdev is not already present, the notifier
 * chain is subjected to be invoked from different CPUs simultaneously.
 *
 * This is protected by the netdev_mutex.
 */
static int bnxt_re_netdev_event(struct notifier_block *notifier,
				unsigned long event, void *ptr)
{
	struct net_device *real_dev, *netdev = netdev_notifier_info_to_dev(ptr);
	struct bnxt_re_work *re_work;
	struct bnxt_re_dev *rdev;
	int rc = 0;

	real_dev = rdma_vlan_dev_real_dev(netdev);
	if (!real_dev)
		real_dev = netdev;

	rdev = bnxt_re_from_netdev(real_dev);
	if (!rdev && event != NETDEV_REGISTER)
		goto exit;
	if (real_dev != netdev)
		goto exit;

	if (rdev)
		bnxt_re_hold(rdev);

	switch (event) {
	case NETDEV_REGISTER:
		if (rdev)
			break;
		rc = bnxt_re_dev_reg(&rdev, real_dev);
		if (rc == -ENODEV)
			break;
		if (rc) {
			pr_err("Failed to register with the device %s: %#x\n",
			       real_dev->name, rc);
			break;
		}
		bnxt_re_init_one(rdev);
		goto sch_work;

	case NETDEV_UNREGISTER:
		bnxt_re_ib_unreg(rdev, false);
		bnxt_re_remove_one(rdev);
		bnxt_re_dev_unreg(rdev);
		break;

	default:
sch_work:
		/* Allocate for the deferred task */
		re_work = kzalloc(sizeof(*re_work), GFP_ATOMIC);
		if (!re_work)
			break;

		re_work->rdev = rdev;
		re_work->event = event;
		re_work->vlan_dev = (real_dev == netdev ? NULL : netdev);
		INIT_WORK(&re_work->work, bnxt_re_task);
		queue_work(bnxt_re_wq, &re_work->work);
		break;
	}
	if (rdev)
		bnxt_re_put(rdev);
exit:
	return NOTIFY_DONE;
}

static struct notifier_block bnxt_re_netdev_notifier = {
	.notifier_call = bnxt_re_netdev_event
};

static int __init bnxt_re_mod_init(void)
{
	int rc = 0;

	pr_info("%s: %s", ROCE_DRV_MODULE_NAME, version);

	bnxt_re_wq = create_singlethread_workqueue("bnxt_re");
	if (!bnxt_re_wq)
		return -ENOMEM;

	INIT_LIST_HEAD(&bnxt_re_dev_list);

	rc = register_netdevice_notifier(&bnxt_re_netdev_notifier);
	if (rc) {
		pr_err("%s: Cannot register to netdevice_notifier",
		       ROCE_DRV_MODULE_NAME);
		goto err_netdev;
	}
	return 0;

err_netdev:
	destroy_workqueue(bnxt_re_wq);

	return rc;
}

static void __exit bnxt_re_mod_exit(void)
{
	unregister_netdevice_notifier(&bnxt_re_netdev_notifier);
	if (bnxt_re_wq)
		destroy_workqueue(bnxt_re_wq);
}

module_init(bnxt_re_mod_init);
module_exit(bnxt_re_mod_exit);
