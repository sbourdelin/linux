/*
 * Copyright (c) 2016 Intel Corporation, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
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

#include <linux/types.h>
#include <linux/bitops.h>

#include <rdma/ib_user_verbs.h>
#include <rdma/ib_verbs.h>
#include <uapi/rdma/rdma_ioctl.h>
#include <rdma/rdma_uapi.h>

#include "uverbs.h"


static long urdma_query_device(struct urdma_device *dev, void *data,
			       void *file_data)
{
	return -ENOSYS;
}

/* shared ioctl function dispatch table, usable by all verbs devices */
const struct urdma_ioctl_desc verbs_ioctl[URDMA_MAX_BASE] = {
	URDMA_DESC(DEVICE, QUERY, urdma_query_device, 0),
//	URDMA_DESC(DEVICE, OPEN, urdma_open_device, URDMA_EVENT),
	/* we could also assume exclusive access for modify/close operations */
//	URDMA_DESC(DEVICE, CLOSE, urdma_close_device, URDMA_EXCL),
//	URDMA_DESC(DEVICE, MODIFY, urdma_modify_device, URDMA_EXCL),
//	URDMA_DESC(CQ, QUERY, urdma_query_cq, 0),
//	URDMA_DESC(CQ, OPEN, urdma_open_cq, URDMA_EVENT),
//	URDMA_DESC(CQ, CLOSE, urdma_close_cq, URDMA_EXCL),
//	URDMA_DESC(CQ, MODIFY, urdma_modify_cq, URDMA_EXCL),
	/* ... yadda yadda yadda */
};
EXPORT_SYMBOL(verbs_ioctl);


/* Map instance id's to object structures.
 * We can define per object/device/driver maps if needed for better
 * parallelism, but use one for now.
 */
struct urdma_map map = { IDR_INIT(map.idr),
			 __MUTEX_INITIALIZER(map.lock) };


static struct urdma_obj * urdma_get_obj(struct idr *idr, struct urdma_device *dev,
					struct urdma_obj_id *id, bool excl)
{
	struct urdma_obj *obj;

	if (id->resv)
		return ERR_PTR(-EINVAL);

	obj = idr_find(idr, id->instance_id);
	if (!obj || obj->dev != dev || obj->obj_type != id->obj_type)
		return ERR_PTR(-ENOENT);
	else if (obj->flags & URDMA_EXCL || (excl && atomic_read(&obj->use_cnt)))
		return ERR_PTR(-EBUSY);

	if (excl)
		obj->flags |= URDMA_EXCL;
	atomic_inc(&obj->use_cnt);
	return obj;
}

static void urdma_put_obj(struct urdma_obj *obj)
{
	if (obj->flags & URDMA_EXCL)
		obj->flags &= ~URDMA_EXCL;
	atomic_dec(&obj->use_cnt);
}

static void urdma_unmap_obj(struct urdma_ioctl *ioctl, int index)
{
	struct urdma_obj *obj;

	obj = ioctl->obj[index];
	ioctl->obj_id[index].instance_id = obj->instance_id;
	ioctl->obj_id[index].obj_type = obj->obj_type;
	ioctl->obj_id[index].resv = 0;
	urdma_put_obj(obj);
}

static void urdma_unmap_objs(struct urdma_device *dev, struct urdma_ioctl *ioctl)
{
	int i;

	for (i = 0; i < ioctl->count; i++)
		urdma_unmap_obj(ioctl, i);
}

static long urdma_map_objs(struct urdma_device *dev,
			   struct urdma_ioctl *ioctl, bool excl)
{
	struct urdma_obj *obj;
	int i;

	mutex_lock(&map.lock);
	for (i = 0; i < ioctl->count; i++) {
		obj = urdma_get_obj(&map.idr, dev, &ioctl->obj_id[i],
				    excl && i == 0);
		if (IS_ERR(obj))
			goto err;

		ioctl->obj[i] = obj;
	}
	mutex_unlock(&map.lock);
	return 0;
err:
	while (i--)
		urdma_unmap_obj(ioctl, i);
	return PTR_ERR(obj);
}

/* process driver specific ioctl
 * driver ioctl's follow more conventional ioctl format
 */
long urdma_driver_ioctl(struct ib_uverbs_file *file_data, unsigned int cmd,
			unsigned long arg)
{
	struct urdma_device *dev /*= file_data->dev*/;
	struct urdma_driver *drv = dev->drv;
	struct urdma_ioctl_desc *desc;
	char stack_data[128], *data;
	u16 size;
	int offset;
	long ret;

	offset = URDMA_OP(cmd);
	if (offset >= drv->num_ioctls || !drv->ioctl[offset].func)
		return -EINVAL;

	desc = &drv->ioctl[offset];
	size = _IOC_SIZE(desc->cmd);
	if (size > sizeof(stack_data)) {
		data = kmalloc(size, GFP_KERNEL);
		if (!data)
			return -ENOMEM;
	} else {
		data = stack_data;
	}

	if (desc->cmd & IOC_IN) {
		if (copy_from_user(data, (void __user *) arg, size)) {
			ret = -EFAULT;
			goto out;
		}
	} else if (desc->cmd & IOC_OUT) {
		memset(data, 0, size);
	}

	/* data is in/out parameter */
	ret = desc->func(dev, data, file_data);

	if (desc->cmd & IOC_OUT) {
		if (copy_to_user((void __user *) arg, data, size))
			ret = -EFAULT;
	}
out:
	if (data != stack_data)
		kfree(data);
	return ret;
}

static long urdma_pre_common(struct urdma_device *dev, struct urdma_ioctl *ioctl,
			     struct urdma_ioctl_desc *desc, void *file_data)
{
	down_read(&dev->rw_lock);
	if (dev->flags & URDMA_CLOSED) {
		up_read(&dev->rw_lock);
		return -ENODEV;
	}

	return urdma_map_objs(dev, ioctl, desc->flags & URDMA_EXCL);
}

static long urdma_post_common(struct urdma_device *dev, struct urdma_ioctl *ioctl,
			      struct urdma_ioctl_desc *desc, void *file_data)
{
	urdma_unmap_objs(dev, ioctl);
	up_read(&dev->rw_lock);
	return 0;
}

static long urdma_pre_open(struct urdma_device *dev, struct urdma_ioctl *ioctl,
			   struct urdma_ioctl_desc *desc, void *file_data)
{
	struct urdma_obj *obj;

	obj = kzalloc(sizeof *obj, GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	obj->flags = URDMA_EXCL;
	obj->obj_type = ioctl->domain;
	atomic_set(&obj->use_cnt, 1);

	mutex_lock(&map.lock);
	obj->instance_id = idr_alloc(&map.idr, obj, 0, 0, GFP_KERNEL);
	/* TODO: handle driver objects */
	if (obj->instance_id >= 0)
		list_add_tail(&obj->entry, &dev->obj_lists[obj->obj_type]);
	mutex_unlock(&map.lock);

	if (obj->instance_id < 0) {
		kfree(obj);
		return -ENOMEM;
	}

	/* new object added after input object array */
	ioctl->obj[ioctl->count++] = obj;
	return 0;
}

static long urdma_pre_close(struct urdma_device *dev, struct urdma_ioctl *ioctl,
			    struct urdma_ioctl_desc *desc, void *file_data)
{
	if (ioctl->count != 1)
		return -EINVAL;
	return urdma_map_objs(dev, ioctl, desc->flags & URDMA_EXCL);
}

static long urdma_post_close(struct urdma_device *dev, struct urdma_ioctl *ioctl,
			     struct urdma_ioctl_desc *desc, void *file_data)
{
	struct urdma_obj *obj;

	obj = ioctl->obj[0];
	ioctl->obj[0] = NULL;

	mutex_lock(&map.lock);
	idr_remove(&map.idr, obj->instance_id);
	list_del(&obj->entry);
	mutex_unlock(&map.lock);
	kfree(obj);
	return 0;
}

const static urdma_ioctl_hook_t urdma_pre_op[URDMA_MAX_OP] = {
	[URDMA_QUERY] = urdma_pre_common,
	[URDMA_OPEN] = urdma_pre_open,
	[URDMA_CLOSE] = urdma_pre_close,
	[URDMA_MODIFY] = urdma_pre_common,
	[URDMA_READ] = urdma_pre_common,
	[URDMA_WRITE]= urdma_pre_common,
};

const static urdma_ioctl_hook_t urdma_post_op[URDMA_MAX_OP] = {
	[URDMA_QUERY] = urdma_post_common,
	[URDMA_OPEN] = urdma_post_common,
	[URDMA_CLOSE] = urdma_post_close,
	[URDMA_MODIFY] = urdma_post_common,
	[URDMA_READ] = urdma_post_common,
	[URDMA_WRITE]= urdma_post_common,
};

long urdma_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct urdma_device *dev;
	struct ib_uverbs_file *file_data;
	struct urdma_ioctl_desc *desc;
	struct urdma_ioctl hdr, *data;
	char stack_data[128];
	u8 op;
	int offset;
	long ret;

	file_data = filp->private_data;
	/* dev = file_data->dev; */

	if (_IOC_NR(cmd) & URDMA_DRIVER_OP)
		return urdma_driver_ioctl(file_data, cmd, arg);

	op = URDMA_OP(cmd);
	if (op > URDMA_MAX_OP || _IOC_SIZE(cmd) < sizeof(hdr))
		return -EINVAL;

	if (copy_from_user(&hdr, (void __user *) arg, sizeof(hdr)))
		return -EFAULT;

	offset = URDMA_OFFSET(hdr.domain, op);
	if (offset >= dev->num_ioctls || !dev->ioctl[offset].func)
		return -EINVAL;

	desc = &dev->ioctl[offset];
	if ((sizeof(hdr) + hdr.count * sizeof(hdr.obj_id) > hdr.length) ||
	    (hdr.length > desc->length))
		return -EINVAL;

	if (desc->length > sizeof(stack_data)) {
		data = kmalloc(desc->length, GFP_KERNEL);
		if (!data)
			return -ENOMEM;
	} else {
		data = (struct urdma_ioctl *) stack_data;
	}

	if (copy_from_user(data, (void __user *) arg, hdr.length)) {
		ret = -EFAULT;
		goto out;
	}

	if (urdma_pre_op[op]) {
		ret = urdma_pre_op[op](dev, data, desc, file_data);
		if (ret)
			goto out;
	}

	ret = desc->func(dev, data, file_data);

	if (urdma_post_op[op]) {
		ret = urdma_post_op[op](dev, data, desc, file_data);
		if (ret)
			goto out;
	}

	if (copy_to_user((void __user *) arg, data, data->length))
		ret = -EFAULT;
out:
	if (data != (struct urdma_ioctl *) stack_data)
		kfree(data);
	return ret;
}

static void urdma_close_obj(struct urdma_device *dev, struct urdma_obj *obj)
{
	/* kernel initiated close, releaes device resources */
}

static void urdma_close_dev(struct urdma_device *dev)
{
	struct urdma_obj *obj;
	int i;

	down_write(&dev->rw_lock);
	dev->flags |= URDMA_CLOSED;

	for (i = 0; i < dev->num_objs; i++) {
		list_for_each_entry(obj, &dev->obj_lists[dev->close_map[i]], entry) {
			urdma_close_obj(dev, obj);
		}
	}
	up_write(&dev->rw_lock);
}

