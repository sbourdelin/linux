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

#include <rdma/rdma_ioctl.h>
#include <rdma/uverbs_ioctl.h>
#include "rdma_core.h"
#include "uverbs.h"

static int uverbs_validate_attr(struct ib_device *ibdev,
				struct ib_ucontext *ucontext,
				const struct ib_uverbs_attr *uattr,
				u16 attr_id,
				const struct uverbs_attr_chain_spec *chain_spec,
				struct uverbs_attr *elements)
{
	const struct uverbs_attr_spec *spec;
	struct uverbs_attr *e;

	if (uattr->reserved)
		return -EINVAL;

	if (attr_id >= chain_spec->num_attrs)
		return -EINVAL;

	spec = &chain_spec->attrs[attr_id];
	e = &elements[attr_id];

	if (e->valid)
		return -EINVAL;

	switch (spec->type) {
	case UVERBS_ATTR_TYPE_PTR_IN:
	case UVERBS_ATTR_TYPE_PTR_OUT:
		/* If spec->len is zero, don't validate (flexible) */
		if (spec->len && uattr->len != spec->len)
			return -EINVAL;
		e->cmd_attr.ptr = (void * __user)uattr->ptr_idr;
		e->cmd_attr.len = uattr->len;
		break;

	case UVERBS_ATTR_TYPE_IDR:
		if (uattr->len != 0 || (uattr->ptr_idr >> 32) || (!ucontext))
			return -EINVAL;

		e->obj_attr.idr = (uint32_t)uattr->ptr_idr;
		e->obj_attr.val = spec;
		e->obj_attr.type = uverbs_get_type(ibdev,
						   spec->idr.idr_type);
		if (!e->obj_attr.type)
			return -EINVAL;

		e->obj_attr.uobject = uverbs_get_type_from_idr(e->obj_attr.type,
							       ucontext,
							       spec->idr.access,
							       e->obj_attr.idr);
		if (!e->obj_attr.uobject)
			return -EINVAL;

		break;
	};

	e->valid = 1;
	return 0;
}

static int uverbs_validate(struct ib_device *ibdev,
			   struct ib_ucontext *ucontext,
			   const struct ib_uverbs_attr *uattrs,
			   size_t num_attrs,
			   const struct action_spec *action_spec,
			   struct uverbs_attr_array *attr_array)
{
	size_t i;
	int ret;
	int n_val = -1;

	for (i = 0; i < num_attrs; i++) {
		const struct ib_uverbs_attr *uattr = &uattrs[i];
		__u16 attr_id = uattr->attr_id;
		const struct uverbs_attr_chain_spec *chain_spec;

		ret = action_spec->dist(&attr_id, action_spec->priv);
		if (ret < 0)
			return ret;

		if (ret > n_val)
			n_val = ret;

		chain_spec = action_spec->validator_chains[ret];
		ret = uverbs_validate_attr(ibdev, ucontext, uattr, attr_id,
					   chain_spec, attr_array[ret].attrs);
		if (ret)
			return ret;

	}

	return n_val >= 0 ? n_val + 1 : n_val;
}

static int uverbs_handle_action(const struct ib_uverbs_attr *uattrs,
				size_t num_attrs,
				struct ib_device *ibdev,
				struct ib_uverbs_file *ufile,
				const struct uverbs_action *handler,
				struct uverbs_attr_array *attr_array)
{
	int ret;
	int n_val;

	n_val = uverbs_validate(ibdev, ufile->ucontext, uattrs, num_attrs,
				&handler->chain, attr_array);
	if (n_val <= 0)
		return n_val;

	ret = handler->handler(ibdev, ufile, attr_array, n_val,
			       handler->priv);
	uverbs_unlock_objects(attr_array, n_val, &handler->chain, !ret);

	return ret;
}

static long ib_uverbs_cmd_verbs(struct ib_device *ib_dev,
				struct ib_uverbs_file *file,
				struct ib_uverbs_ioctl_hdr *hdr,
				void __user *buf)
{
	const struct uverbs_type_actions *type;
	const struct uverbs_action *action;
	const struct action_spec *action_spec;
	long err = 0;
	unsigned int num_specs = 0;
	unsigned int i;
	struct {
		struct ib_uverbs_attr		*uattrs;
		struct uverbs_attr_array	*uverbs_attr_array;
	} *ctx = NULL;
	struct uverbs_attr *curr_attr;
	size_t ctx_size;

	if (!ib_dev)
		return -EIO;

	if (ib_dev->types->num_types < hdr->object_type ||
	    ib_dev->driver_id != hdr->driver_id)
		return -EINVAL;

	type = ib_dev->types->types[hdr->object_type];
	if (!type || type->num_actions < hdr->action)
		return -EOPNOTSUPP;

	action = &type->actions[hdr->action];
	if (!action)
		return -EOPNOTSUPP;

	action_spec = &action->chain;
	for (i = 0; i < action_spec->num_chains;
	     num_specs += action_spec->validator_chains[i]->num_attrs, i++)
		;

	ctx_size = sizeof(*ctx->uattrs) * hdr->num_attrs +
		   sizeof(*ctx->uverbs_attr_array->attrs) * num_specs +
		   sizeof(struct uverbs_attr_array) * action_spec->num_chains +
		   sizeof(*ctx);

	ctx = kzalloc(ctx_size, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->uverbs_attr_array = (void *)ctx + sizeof(*ctx);
	ctx->uattrs = (void *)(ctx->uverbs_attr_array +
			       action_spec->num_chains);
	curr_attr = (void *)(ctx->uattrs + hdr->num_attrs);
	for (i = 0; i < action_spec->num_chains; i++) {
		ctx->uverbs_attr_array[i].attrs = curr_attr;
		ctx->uverbs_attr_array[i].num_attrs =
			action_spec->validator_chains[i]->num_attrs;
		curr_attr += action_spec->validator_chains[i]->num_attrs;
	}

	err = copy_from_user(ctx->uattrs, buf,
			     sizeof(*ctx->uattrs) * hdr->num_attrs);
	if (err) {
		err = -EFAULT;
		goto out;
	}

	err = uverbs_handle_action(ctx->uattrs, hdr->num_attrs, ib_dev,
				   file, action, ctx->uverbs_attr_array);
out:
	kfree(ctx);
	return err;
}

#define IB_UVERBS_MAX_CMD_SZ 4096

long ib_uverbs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct ib_uverbs_file *file = filp->private_data;
	struct ib_uverbs_ioctl_hdr __user *user_hdr =
		(struct ib_uverbs_ioctl_hdr __user *)arg;
	struct ib_uverbs_ioctl_hdr hdr;
	struct ib_device *ib_dev;
	int srcu_key;
	long err;

	srcu_key = srcu_read_lock(&file->device->disassociate_srcu);
	ib_dev = srcu_dereference(file->device->ib_dev,
				  &file->device->disassociate_srcu);
	if (!ib_dev) {
		err = -EIO;
		goto out;
	}

	if (cmd == RDMA_DIRECT_IOCTL) {
		/* TODO? */
		err = -ENOSYS;
		goto out;
	} else {
		if (cmd != RDMA_VERBS_IOCTL) {
			err = -ENOIOCTLCMD;
			goto out;
		}

		err = copy_from_user(&hdr, user_hdr, sizeof(hdr));

		if (err || hdr.length > IB_UVERBS_MAX_CMD_SZ ||
		    hdr.length <= sizeof(hdr) ||
		    hdr.length != sizeof(hdr) + hdr.num_attrs * sizeof(struct ib_uverbs_attr)) {
			err = -EINVAL;
			goto out;
		}

		/* currently there are no flags supported */
		if (hdr.flags) {
			err = -EOPNOTSUPP;
			goto out;
		}

		/* We're closing, fail all commands */
		if (!down_read_trylock(&file->close_sem))
			return -EIO;
		err = ib_uverbs_cmd_verbs(ib_dev, file, &hdr,
					  (__user void *)arg + sizeof(hdr));
		up_read(&file->close_sem);
	}
out:
	srcu_read_unlock(&file->device->disassociate_srcu, srcu_key);

	return err;
}
