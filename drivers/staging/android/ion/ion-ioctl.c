/*
 *
 * Copyright (C) 2011 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#include "ion.h"
#include "ion_priv.h"
#include "compat_ion.h"

union ion_ioctl_arg {
	struct ion_fd_data fd;
	struct ion_allocation_data allocation;
	struct ion_handle_data handle;
	struct ion_custom_data custom;
	struct ion_abi_version abi_version;
	struct ion_new_alloc_data allocation2;
	struct ion_usage_id_map id_map;
	struct ion_usage_cnt usage_cnt;
	struct ion_heap_query query;
};

static int validate_ioctl_arg(unsigned int cmd, union ion_ioctl_arg *arg)
{
	int ret = 0;

	switch (cmd) {
	case ION_IOC_ABI_VERSION:
		ret =  arg->abi_version.reserved != 0;
		break;
	case ION_IOC_ALLOC2:
		ret = arg->allocation2.reserved0 != 0;
		ret |= arg->allocation2.reserved1 != 0;
		ret |= arg->allocation2.reserved2 != 0;
		break;
	case ION_IOC_ID_MAP:
		ret = arg->id_map.reserved0 != 0;
		ret |= arg->id_map.reserved1 != 0;
		break;
	case ION_IOC_USAGE_CNT:
		ret = arg->usage_cnt.reserved != 0;
		break;
	case ION_IOC_HEAP_QUERY:
		ret = arg->query.reserved0 != 0;
		ret |= arg->query.reserved1 != 0;
		ret |= arg->query.reserved2 != 0;
		break;
	default:
		break;
	}
	return ret ? -EINVAL : 0;
}

/* fix up the cases where the ioctl direction bits are incorrect */
static unsigned int ion_ioctl_dir(unsigned int cmd)
{
	switch (cmd) {
	case ION_IOC_SYNC:
	case ION_IOC_FREE:
	case ION_IOC_CUSTOM:
		return _IOC_WRITE;
	default:
		return _IOC_DIR(cmd);
	}
}

long ion_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct ion_client *client = filp->private_data;
	struct ion_device *dev = client->dev;
	struct ion_handle *cleanup_handle = NULL;
	int ret = 0;
	unsigned int dir;
	union ion_ioctl_arg data;

	dir = ion_ioctl_dir(cmd);

	if (_IOC_SIZE(cmd) > sizeof(data))
		return -EINVAL;

	if (dir & _IOC_WRITE)
		if (copy_from_user(&data, (void __user *)arg, _IOC_SIZE(cmd)))
			return -EFAULT;

	ret = validate_ioctl_arg(cmd, &data);
	if (ret)
		return ret;

	switch (cmd) {
	/* Old ioctl */
	case ION_IOC_ALLOC:
	{
		struct ion_handle *handle;

		handle = ion_alloc(client, data.allocation.len,
						data.allocation.align,
						data.allocation.heap_id_mask,
						data.allocation.flags);
		if (IS_ERR(handle))
			return PTR_ERR(handle);

		data.allocation.handle = handle->id;

		cleanup_handle = handle;
		break;
	}
	/* Old ioctl */
	case ION_IOC_FREE:
	{
		struct ion_handle *handle;

		mutex_lock(&client->lock);
		handle = ion_handle_get_by_id_nolock(client, data.handle.handle);
		if (IS_ERR(handle)) {
			mutex_unlock(&client->lock);
			return PTR_ERR(handle);
		}
		ion_free_nolock(client, handle);
		ion_handle_put_nolock(handle);
		mutex_unlock(&client->lock);
		break;
	}
	/* Old ioctl */
	case ION_IOC_SHARE:
	case ION_IOC_MAP:
	{
		struct ion_handle *handle;

		handle = ion_handle_get_by_id(client, data.handle.handle);
		if (IS_ERR(handle))
			return PTR_ERR(handle);
		data.fd.fd = ion_share_dma_buf_fd(client, handle);
		ion_handle_put(handle);
		if (data.fd.fd < 0)
			ret = data.fd.fd;
		break;
	}
	/* Old ioctl */
	case ION_IOC_IMPORT:
	{
		struct ion_handle *handle;

		handle = ion_import_dma_buf_fd(client, data.fd.fd);
		if (IS_ERR(handle))
			ret = PTR_ERR(handle);
		else
			data.handle.handle = handle->id;
		break;
	}
	/* Old ioctl */
	case ION_IOC_SYNC:
	{
		ret = ion_sync_for_device(client, data.fd.fd);
		break;
	}
	/* Old ioctl */
	case ION_IOC_CUSTOM:
	{
		if (!dev->custom_ioctl)
			return -ENOTTY;
		ret = dev->custom_ioctl(client, data.custom.cmd,
						data.custom.arg);
		break;
	}
	case ION_IOC_ABI_VERSION:
	{
		data.abi_version.abi_version = ION_ABI_VERSION;
		break;
	}
	case ION_IOC_ALLOC2:
	{
		struct ion_handle *handle;

		handle = ion_alloc2(client, data.allocation2.len,
					data.allocation2.align,
					data.allocation2.usage_id,
					data.allocation2.flags);
		if (IS_ERR(handle))
			return PTR_ERR(handle);

		if (data.allocation2.flags & ION_FLAG_NO_HANDLE) {
			data.allocation2.fd = ion_share_dma_buf_fd(client,
								handle);
			ion_handle_put(handle);
			if (data.allocation2.fd < 0)
				ret = data.allocation2.fd;
		} else {
			data.allocation2.handle = handle->id;

			cleanup_handle = handle;
		}
		break;
	}
	case ION_IOC_ID_MAP:
	{
		ret = ion_map_usage_ids(client,
				(unsigned int __user *)data.id_map.usage_ids,
				data.id_map.cnt);
		if (ret > 0)
			data.id_map.new_id = ret;
		break;
	}
	case ION_IOC_USAGE_CNT:
	{
		down_read(&client->dev->lock);
		data.usage_cnt.cnt = client->dev->heap_cnt;
		up_read(&client->dev->lock);
		break;
	}
	case ION_IOC_HEAP_QUERY:
	{
		ret = ion_query_heaps(client,
				(struct ion_heap_data __user *)data.query.heaps,
				data.query.cnt);
		break;
	}
	default:
		return -ENOTTY;
	}

	if (dir & _IOC_READ) {
		if (copy_to_user((void __user *)arg, &data, _IOC_SIZE(cmd))) {
			if (cleanup_handle)
				ion_free(client, cleanup_handle);
			return -EFAULT;
		}
	}
	return ret;
}
