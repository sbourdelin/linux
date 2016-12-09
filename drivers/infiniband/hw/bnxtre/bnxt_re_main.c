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
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/rculist.h>
#include "bnxt_re.h"
static char version[] =
		BNXT_RE_DESC " v" ROCE_DRV_MODULE_VERSION "\n";


MODULE_AUTHOR("Eddie Wai <eddie.wai@broadcom.com>");
MODULE_DESCRIPTION(BNXT_RE_DESC " Driver");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION(ROCE_DRV_MODULE_VERSION);

/* globals */
static struct list_head bnxt_re_dev_list = LIST_HEAD_INIT(bnxt_re_dev_list);
static DEFINE_MUTEX(bnxt_re_dev_lock);
static struct workqueue_struct *bnxt_re_wq;
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
