/*
 * Copyright (c) 2016, Mellanox Technologies inc.  All rights reserved.
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

#include <rdma/uverbs_ioctl_cmd.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/ib_verbs.h>
#include <linux/bug.h>
#include <linux/file.h>
#include "rdma_core.h"
#include "uverbs.h"

#define IB_UVERBS_VENDOR_FLAG	0x8000

int ib_uverbs_std_dist(__u16 *id, void *priv)
{
	if (*id & IB_UVERBS_VENDOR_FLAG) {
		*id &= ~IB_UVERBS_VENDOR_FLAG;
		return 1;
	}
	return 0;
}
EXPORT_SYMBOL(ib_uverbs_std_dist);

int uverbs_action_std_handle(struct ib_device *ib_dev,
			     struct ib_uverbs_file *ufile,
			     struct uverbs_attr_array *ctx, size_t num,
			     void *_priv)
{
	struct uverbs_action_std_handler *priv = _priv;

	if (!ufile->ucontext)
		return -EINVAL;

	WARN_ON((num != 1) && (num != 2));
	return priv->handler(ib_dev, ufile->ucontext, &ctx[0],
			     (num == 2 ? &ctx[1] : NULL),
			     priv->priv);
}
EXPORT_SYMBOL(uverbs_action_std_handle);

int uverbs_action_std_ctx_handle(struct ib_device *ib_dev,
				 struct ib_uverbs_file *ufile,
				 struct uverbs_attr_array *ctx, size_t num,
				 void *_priv)
{
	struct uverbs_action_std_ctx_handler *priv = _priv;

	WARN_ON((num != 1) && (num != 2));
	return priv->handler(ib_dev, ufile, &ctx[0], (num == 2 ? &ctx[1] : NULL), priv->priv);
}
EXPORT_SYMBOL(uverbs_action_std_ctx_handle);

void uverbs_free_ah(const struct uverbs_type_alloc_action *uobject_type,
		    struct ib_uobject *uobject)
{
	ib_destroy_ah((struct ib_ah *)uobject->object);
}
EXPORT_SYMBOL(uverbs_free_ah);

void uverbs_free_flow(const struct uverbs_type_alloc_action *type_alloc_action,
		      struct ib_uobject *uobject)
{
	ib_destroy_flow((struct ib_flow *)uobject->object);
}
EXPORT_SYMBOL(uverbs_free_flow);

void uverbs_free_mw(const struct uverbs_type_alloc_action *type_alloc_action,
		    struct ib_uobject *uobject)
{
	uverbs_dealloc_mw((struct ib_mw *)uobject->object);
}
EXPORT_SYMBOL(uverbs_free_mw);

void uverbs_free_qp(const struct uverbs_type_alloc_action *type_alloc_action,
		    struct ib_uobject *uobject)
{
	struct ib_qp *qp = uobject->object;
	struct ib_uqp_object *uqp =
		container_of(uobject, struct ib_uqp_object, uevent.uobject);

	if (qp != qp->real_qp) {
		ib_close_qp(qp);
	} else {
		ib_uverbs_detach_umcast(qp, uqp);
		ib_destroy_qp(qp);
	}
	ib_uverbs_release_uevent(uobject->context->ufile, &uqp->uevent);
}
EXPORT_SYMBOL(uverbs_free_qp);

void uverbs_free_srq(const struct uverbs_type_alloc_action *type_alloc_action,
		     struct ib_uobject *uobject)
{
	struct ib_srq *srq = uobject->object;
	struct ib_uevent_object *uevent =
		container_of(uobject, struct ib_uevent_object, uobject);

	ib_destroy_srq(srq);
	ib_uverbs_release_uevent(uobject->context->ufile, uevent);
}
EXPORT_SYMBOL(uverbs_free_srq);

void uverbs_free_cq(const struct uverbs_type_alloc_action *type_alloc_action,
		    struct ib_uobject *uobject)
{
	struct ib_cq *cq = uobject->object;
	struct ib_uverbs_event_file *ev_file = cq->cq_context;
	struct ib_ucq_object *ucq =
		container_of(uobject, struct ib_ucq_object, uobject);

	ib_destroy_cq(cq);
	ib_uverbs_release_ucq(uobject->context->ufile, ev_file, ucq);
}
EXPORT_SYMBOL(uverbs_free_cq);

void uverbs_free_mr(const struct uverbs_type_alloc_action *type_alloc_action,
		    struct ib_uobject *uobject)
{
	ib_dereg_mr((struct ib_mr *)uobject->object);
}
EXPORT_SYMBOL(uverbs_free_mr);

void uverbs_free_xrcd(const struct uverbs_type_alloc_action *type_alloc_action,
		      struct ib_uobject *uobject)
{
	struct ib_xrcd *xrcd = uobject->object;

	mutex_lock(&uobject->context->ufile->device->xrcd_tree_mutex);
	ib_uverbs_dealloc_xrcd(uobject->context->ufile->device, xrcd);
	mutex_unlock(&uobject->context->ufile->device->xrcd_tree_mutex);
}
EXPORT_SYMBOL(uverbs_free_xrcd);

void uverbs_free_pd(const struct uverbs_type_alloc_action *type_alloc_action,
		    struct ib_uobject *uobject)
{
	ib_dealloc_pd((struct ib_pd *)uobject->object);
}
EXPORT_SYMBOL(uverbs_free_pd);

enum {
	UVERBS_UHW_IN,
	UVERBS_UHW_OUT,
};

DECLARE_UVERBS_ATTR_SPEC(
	uverbs_uhw_compat_spec,
	UVERBS_ATTR_PTR_IN(UVERBS_UHW_IN, 0),
	UVERBS_ATTR_PTR_OUT(UVERBS_UHW_OUT, 0));
EXPORT_SYMBOL(uverbs_uhw_compat_spec);

static void create_udata(struct uverbs_attr_array *vendor,
			 struct ib_udata *udata)
{
	/*
	 * This is for ease of conversion. The purpose is to convert all vendors
	 * to use uverbs_attr_array instead of ib_udata.
	 * Assume attr == 0 is input and attr == 1 is output.
	 */
	void * __user inbuf;
	size_t inbuf_len = 0;
	void * __user outbuf;
	size_t outbuf_len = 0;

	if (vendor) {
		WARN_ON(vendor->num_attrs > 2);

		if (vendor->attrs[0].valid) {
			inbuf = vendor->attrs[0].cmd_attr.ptr;
			inbuf_len = vendor->attrs[0].cmd_attr.len;
		}

		if (vendor->num_attrs == 2 && vendor->attrs[1].valid) {
			outbuf = vendor->attrs[1].cmd_attr.ptr;
			outbuf_len = vendor->attrs[1].cmd_attr.len;
		}
	}
	INIT_UDATA_BUF_OR_NULL(udata, inbuf, outbuf, inbuf_len, outbuf_len);
}

DECLARE_UVERBS_ATTR_SPEC(
	uverbs_get_context_spec,
	UVERBS_ATTR_PTR_OUT(GET_CONTEXT_RESP,
			    sizeof(struct ib_uverbs_get_context_resp)));
EXPORT_SYMBOL(uverbs_get_context_spec);

int uverbs_get_context(struct ib_device *ib_dev,
		       struct ib_uverbs_file *file,
		       struct uverbs_attr_array *common,
		       struct uverbs_attr_array *vendor,
		       void *priv)
{
	struct ib_udata uhw;
	struct ib_uverbs_get_context_resp resp;
	struct ib_ucontext		 *ucontext;
	struct file			 *filp;
	int ret;

	if (!common->attrs[GET_CONTEXT_RESP].valid)
		return -EINVAL;

	/* Temporary, only until vendors get the new uverbs_attr_array */
	create_udata(vendor, &uhw);

	mutex_lock(&file->mutex);

	if (file->ucontext) {
		ret = -EINVAL;
		goto err;
	}

	ucontext = ib_dev->alloc_ucontext(ib_dev, &uhw);
	if (IS_ERR(ucontext)) {
		ret = PTR_ERR(ucontext);
		goto err;
	}

	ucontext->device = ib_dev;
	ib_uverbs_uobject_type_initialize_ucontext(ucontext);

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

	if (copy_to_user(common->attrs[GET_CONTEXT_RESP].cmd_attr.ptr,
			 &resp, sizeof(resp))) {
		ret = -EFAULT;
		goto err_file;
	}

	file->ucontext = ucontext;
	ucontext->ufile = file;

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

err:
	mutex_unlock(&file->mutex);
	return ret;
}
EXPORT_SYMBOL(uverbs_get_context);
DECLARE_UVERBS_CTX_ACTION(uverbs_action_get_context, uverbs_get_context, NULL,
			  &uverbs_get_context_spec, &uverbs_uhw_compat_spec);
EXPORT_SYMBOL(uverbs_action_get_context);

DECLARE_UVERBS_ATTR_SPEC(
	uverbs_query_device_spec,
	UVERBS_ATTR_PTR_OUT(QUERY_DEVICE_RESP, sizeof(struct ib_uverbs_query_device_resp)),
	UVERBS_ATTR_PTR_OUT(QUERY_DEVICE_ODP, sizeof(struct ib_uverbs_odp_caps)),
	UVERBS_ATTR_PTR_OUT(QUERY_DEVICE_TIMESTAMP_MASK, sizeof(__u64)),
	UVERBS_ATTR_PTR_OUT(QUERY_DEVICE_HCA_CORE_CLOCK, sizeof(__u64)),
	UVERBS_ATTR_PTR_OUT(QUERY_DEVICE_CAP_FLAGS, sizeof(__u64)));
EXPORT_SYMBOL(uverbs_query_device_spec);

int uverbs_query_device_handler(struct ib_device *ib_dev,
				struct ib_ucontext *ucontext,
				struct uverbs_attr_array *common,
				struct uverbs_attr_array *vendor,
				void *priv)
{
	struct ib_device_attr attr = {};
	struct ib_udata uhw;
	int err;

	/* Temporary, only until vendors get the new uverbs_attr_array */
	create_udata(vendor, &uhw);

	err = ib_dev->query_device(ib_dev, &attr, &uhw);
	if (err)
		return err;

	if (common->attrs[QUERY_DEVICE_RESP].valid) {
		struct ib_uverbs_query_device_resp resp = {};

		uverbs_copy_query_dev_fields(ib_dev, &resp, &attr);
		if (copy_to_user(common->attrs[QUERY_DEVICE_RESP].cmd_attr.ptr,
				 &resp, sizeof(resp)))
			return -EFAULT;
	}

#ifdef CONFIG_INFINIBAND_ON_DEMAND_PAGING
	if (common->attrs[QUERY_DEVICE_ODP].valid) {
		struct ib_uverbs_odp_caps odp_caps;

		odp_caps.general_caps = attr.odp_caps.general_caps;
		odp_caps.per_transport_caps.rc_odp_caps =
			attr.odp_caps.per_transport_caps.rc_odp_caps;
		odp_caps.per_transport_caps.uc_odp_caps =
			attr.odp_caps.per_transport_caps.uc_odp_caps;
		odp_caps.per_transport_caps.ud_odp_caps =
			attr.odp_caps.per_transport_caps.ud_odp_caps;

		if (copy_to_user(common->attrs[QUERY_DEVICE_ODP].cmd_attr.ptr,
				 &odp_caps, sizeof(odp_caps)))
			return -EFAULT;
	}
#endif
	if (UVERBS_COPY_TO(common, QUERY_DEVICE_TIMESTAMP_MASK,
			   &attr.timestamp_mask) == -EFAULT)
		return -EFAULT;

	if (UVERBS_COPY_TO(common, QUERY_DEVICE_HCA_CORE_CLOCK,
			   &attr.hca_core_clock) == -EFAULT)
		return -EFAULT;

	if (UVERBS_COPY_TO(common, QUERY_DEVICE_CAP_FLAGS,
			   &attr.device_cap_flags) == -EFAULT)
		return -EFAULT;

	return 0;
}
EXPORT_SYMBOL(uverbs_query_device_handler);
DECLARE_UVERBS_ACTION(uverbs_action_query_device, uverbs_query_device_handler, NULL,
		      &uverbs_query_device_spec, &uverbs_uhw_compat_spec);
EXPORT_SYMBOL(uverbs_action_query_device);

DECLARE_UVERBS_ATTR_SPEC(
	uverbs_alloc_pd_spec,
	UVERBS_ATTR_IDR(ALLOC_PD_HANDLE, UVERBS_TYPE_PD,
			UVERBS_IDR_ACCESS_NEW));
EXPORT_SYMBOL(uverbs_alloc_pd_spec);

int uverbs_alloc_pd_handler(struct ib_device *ib_dev,
			    struct ib_ucontext *ucontext,
			    struct uverbs_attr_array *common,
			    struct uverbs_attr_array *vendor,
			    void *priv)
{
	struct ib_udata uhw;
	struct ib_uobject *uobject;
	struct ib_pd *pd;

	if (!common->attrs[ALLOC_PD_HANDLE].valid)
		return -EINVAL;

	/* Temporary, only until vendors get the new uverbs_attr_array */
	create_udata(vendor, &uhw);

	pd = ib_dev->alloc_pd(ib_dev, ucontext, &uhw);
	if (IS_ERR(pd))
		return PTR_ERR(pd);

	uobject = common->attrs[ALLOC_PD_HANDLE].obj_attr.uobject;
	pd->device  = ib_dev;
	pd->uobject = uobject;
	uobject->object = pd;
	pd->local_mr = NULL;
	atomic_set(&pd->usecnt, 0);

	return 0;
}
EXPORT_SYMBOL(uverbs_alloc_pd_handler);
DECLARE_UVERBS_ACTION(uverbs_action_alloc_pd, uverbs_alloc_pd_handler, NULL,
		      &uverbs_alloc_pd_spec, &uverbs_uhw_compat_spec);
EXPORT_SYMBOL(uverbs_action_alloc_pd);

DECLARE_UVERBS_ATTR_SPEC(
	uverbs_reg_mr_spec,
	UVERBS_ATTR_IDR(REG_MR_HANDLE, UVERBS_TYPE_MR, UVERBS_IDR_ACCESS_NEW),
	UVERBS_ATTR_IDR(REG_MR_PD_HANDLE, UVERBS_TYPE_PD, UVERBS_IDR_ACCESS_READ),
	UVERBS_ATTR_PTR_IN(REG_MR_CMD, sizeof(struct ib_uverbs_ioctl_reg_mr)),
	UVERBS_ATTR_PTR_OUT(REG_MR_RESP, sizeof(struct ib_uverbs_ioctl_reg_mr_resp)));
EXPORT_SYMBOL(uverbs_reg_mr_spec);

int uverbs_reg_mr_handler(struct ib_device *ib_dev,
			  struct ib_ucontext *ucontext,
			  struct uverbs_attr_array *common,
			  struct uverbs_attr_array *vendor,
			  void *priv)
{
	struct ib_uverbs_ioctl_reg_mr		cmd;
	struct ib_uverbs_ioctl_reg_mr_resp	resp;
	struct ib_udata uhw;
	struct ib_uobject *uobject;
	struct ib_pd                *pd;
	struct ib_mr                *mr;
	int                          ret;

	if (!common->attrs[REG_MR_HANDLE].valid ||
	    !common->attrs[REG_MR_PD_HANDLE].valid ||
	    !common->attrs[REG_MR_CMD].valid ||
	    !common->attrs[REG_MR_RESP].valid)
		return -EINVAL;

	if (copy_from_user(&cmd, common->attrs[REG_MR_CMD].cmd_attr.ptr, sizeof(cmd)))
		return -EFAULT;

	if ((cmd.start & ~PAGE_MASK) != (cmd.hca_va & ~PAGE_MASK))
		return -EINVAL;

	ret = ib_check_mr_access(cmd.access_flags);
	if (ret)
		return ret;

	/* Temporary, only until vendors get the new uverbs_attr_array */
	create_udata(vendor, &uhw);

	uobject = common->attrs[REG_MR_HANDLE].obj_attr.uobject;
	pd = common->attrs[REG_MR_PD_HANDLE].obj_attr.uobject->object;

	if (cmd.access_flags & IB_ACCESS_ON_DEMAND) {
		if (!(pd->device->attrs.device_cap_flags &
		      IB_DEVICE_ON_DEMAND_PAGING)) {
			pr_debug("ODP support not available\n");
			return -EINVAL;
		}
	}

	mr = pd->device->reg_user_mr(pd, cmd.start, cmd.length, cmd.hca_va,
				     cmd.access_flags, &uhw);
	if (IS_ERR(mr))
		return PTR_ERR(mr);

	mr->device  = pd->device;
	mr->pd      = pd;
	mr->uobject = uobject;
	atomic_inc(&pd->usecnt);
	uobject->object = mr;

	resp.lkey      = mr->lkey;
	resp.rkey      = mr->rkey;

	if (copy_to_user(common->attrs[REG_MR_RESP].cmd_attr.ptr,
			 &resp, sizeof(resp))) {
		ret = -EFAULT;
		goto err;
	}

	return 0;

err:
	ib_dereg_mr(mr);
	return ret;
}
EXPORT_SYMBOL(uverbs_reg_mr_handler);

DECLARE_UVERBS_ACTION(uverbs_action_reg_mr, uverbs_reg_mr_handler, NULL,
		      &uverbs_reg_mr_spec, &uverbs_uhw_compat_spec);
EXPORT_SYMBOL(uverbs_action_reg_mr);

DECLARE_UVERBS_ACTIONS(
	uverbs_actions_mr,
	ADD_UVERBS_ACTION_PTR(UVERBS_MR_REG, &uverbs_action_reg_mr),
);
EXPORT_SYMBOL(uverbs_actions_mr);

DECLARE_UVERBS_ACTIONS(
	uverbs_actions_pd,
	ADD_UVERBS_ACTION_PTR(UVERBS_PD_ALLOC, &uverbs_action_alloc_pd),
);
EXPORT_SYMBOL(uverbs_actions_pd);

DECLARE_UVERBS_ACTIONS(
	uverbs_actions_device,
	ADD_UVERBS_ACTION_PTR(UVERBS_DEVICE_QUERY, &uverbs_action_query_device),
	ADD_UVERBS_ACTION_PTR(UVERBS_DEVICE_ALLOC_CONTEXT, &uverbs_action_get_context),
);
EXPORT_SYMBOL(uverbs_actions_device);

DECLARE_UVERBS_TYPE(uverbs_type_mr,
		    /* 1 is used in order to free the MR after all the MWs */
		    &UVERBS_TYPE_ALLOC(1, uverbs_free_mr),
		    &uverbs_actions_mr);
EXPORT_SYMBOL(uverbs_type_mr);

DECLARE_UVERBS_TYPE(uverbs_type_pd,
		    /* 2 is used in order to free the PD after all objects */
		    &UVERBS_TYPE_ALLOC(2, uverbs_free_pd),
		    &uverbs_actions_pd);
EXPORT_SYMBOL(uverbs_type_pd);

DECLARE_UVERBS_TYPE(uverbs_type_device, NULL, &uverbs_actions_device);
EXPORT_SYMBOL(uverbs_type_device);

DECLARE_UVERBS_TYPES(uverbs_types,
		     ADD_UVERBS_TYPE(UVERBS_TYPE_DEVICE, uverbs_type_device),
		     ADD_UVERBS_TYPE(UVERBS_TYPE_PD, uverbs_type_pd),
		     ADD_UVERBS_TYPE(UVERBS_TYPE_MR, uverbs_type_mr),
);
EXPORT_SYMBOL(uverbs_types);

DECLARE_UVERBS_TYPES_GROUP(uverbs_types_group, &uverbs_types);
EXPORT_SYMBOL(uverbs_types_group);

