/*
 * Copyright (c) 2016 Mellanox Technologies, LTD. All rights reserved.
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

#include <net/netlink.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/sched.h>

#include <linux/uaccess.h>

#include "uverbs.h"
#include "core_priv.h"

long ib_uverbs_nl_context_create(struct ib_uverbs_file *file,
				 struct ib_device *ib_dev,
				 struct ib_uverbs_ioctl_hdr *hdr,
				 struct nlattr **tb, struct ib_udata *uresp,
				 struct ib_udata *uhw)
{
	struct ib_uverbs_get_context_resp resp;
	struct ib_ucontext		 *ucontext;
	int ret;
	struct file			 *filp;
	struct nlattr __user *nla;

	mutex_lock(&file->mutex);

	if (file->ucontext) {
		pr_debug("uverbs context create with already existing context\n");
		ret = -EINVAL;
		goto err;
	}

	ucontext = ib_dev->alloc_ucontext(ib_dev, uhw);
	if (IS_ERR(ucontext)) {
		ret = PTR_ERR(ucontext);
		goto err;
	}
	if (uhw->outptr - uhw->outbuf) {
		__u32 vendor_len = uhw->outptr - uhw->outbuf;

		nla = ib_uverbs_nla_put(uresp, IBNL_RESPONSE_TYPE_VENDOR,
					sizeof(vendor_len), &vendor_len);
		if (IS_ERR(nla)) {
			ret = PTR_ERR(nla);
			goto err_ctx;
		}
	}

	ucontext->device = ib_dev;
	INIT_LIST_HEAD(&ucontext->pd_list);
	INIT_LIST_HEAD(&ucontext->mr_list);
	INIT_LIST_HEAD(&ucontext->mw_list);
	INIT_LIST_HEAD(&ucontext->cq_list);
	INIT_LIST_HEAD(&ucontext->qp_list);
	INIT_LIST_HEAD(&ucontext->srq_list);
	INIT_LIST_HEAD(&ucontext->ah_list);
	INIT_LIST_HEAD(&ucontext->xrcd_list);
	INIT_LIST_HEAD(&ucontext->rule_list);
	rcu_read_lock();
	ucontext->tgid = get_task_pid(current->group_leader, PIDTYPE_PID);
	rcu_read_unlock();
	ucontext->closing = 0;

#ifdef CONFIG_INFINIBAND_ON_DEMAND_PAGING
	ucontext->umem_tree = RB_ROOT;
	init_rwsem(&ucontext->umem_rwsem);
	ucontext->odp_mrs_count = 0;
	INIT_LIST_HEAD(&ucontext->no_private_counters);

	if (!(ib_dev->attrs.device_cap_flags & IB_DEVICE_ON_DEMAND_PAGING))
		ucontext->invalidate_range = NULL;

#endif

	resp.num_comp_vectors = file->device->num_comp_vectors;

	ret = get_unused_fd_flags(O_CLOEXEC);
	if (ret < 0)
		goto err_free;
	resp.async_fd = ret;

	filp = ib_uverbs_alloc_event_file(file, ib_dev, 1);
	if (IS_ERR(filp)) {
		ret = PTR_ERR(filp);
		goto err_fd;
	}

	nla = ib_uverbs_nla_put(uresp, IBNL_RESPONSE_TYPE_RESP,
				sizeof(resp), &resp);
	if (IS_ERR(nla)) {
		ret = PTR_ERR(nla);
		goto err_file;
	}

	file->ucontext = ucontext;

	fd_install(resp.async_fd, filp);

	mutex_unlock(&file->mutex);

	return 0;

err_file:
	ib_uverbs_free_async_event_file(file);
	fput(filp);

err_fd:
	put_unused_fd(resp.async_fd);

err_free:
	put_pid(ucontext->tgid);
err_ctx:
	ib_dev->dealloc_ucontext(ucontext);

err:
	mutex_unlock(&file->mutex);
	return ret;
	return 0;
};

