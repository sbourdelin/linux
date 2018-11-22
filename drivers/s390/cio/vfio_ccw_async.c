// SPDX-License-Identifier: GPL-2.0
/*
 * Async I/O region for vfio_ccw
 *
 * Copyright Red Hat, Inc. 2018
 *
 * Author(s): Cornelia Huck <cohuck@redhat.com>
 */

#include <linux/vfio.h>
#include <linux/mdev.h>

#include "vfio_ccw_private.h"

static size_t vfio_ccw_async_region_read(struct vfio_ccw_private *private,
					 char __user *buf, size_t count,
					 loff_t *ppos)
{
	unsigned int i = VFIO_CCW_OFFSET_TO_INDEX(*ppos) - VFIO_CCW_NUM_REGIONS;
	loff_t pos = *ppos & VFIO_CCW_OFFSET_MASK;
	struct ccw_cmd_region *region;

	if (pos + count > sizeof(*region))
		return -EINVAL;

	region = private->region[i].data;
	if (copy_to_user(buf, (void *)region + pos, count))
		return -EFAULT;

	return count;

}

static size_t vfio_ccw_async_region_write(struct vfio_ccw_private *private,
					  const char __user *buf, size_t count,
					  loff_t *ppos)
{
	unsigned int i = VFIO_CCW_OFFSET_TO_INDEX(*ppos) - VFIO_CCW_NUM_REGIONS;
	loff_t pos = *ppos & VFIO_CCW_OFFSET_MASK;
	struct ccw_cmd_region *region;

	if (pos + count > sizeof(*region))
		return -EINVAL;

	if (private->state == VFIO_CCW_STATE_NOT_OPER ||
	    private->state == VFIO_CCW_STATE_STANDBY)
		return -EACCES;

	region = private->region[i].data;
	if (copy_from_user((void *)region + pos, buf, count))
		return -EFAULT;

	switch (region->command) {
	case VFIO_CCW_ASYNC_CMD_HSCH:
		vfio_ccw_fsm_event(private, VFIO_CCW_EVENT_HALT_REQ);
		break;
	case VFIO_CCW_ASYNC_CMD_CSCH:
		vfio_ccw_fsm_event(private, VFIO_CCW_EVENT_CLEAR_REQ);
		break;
	default:
		return -EINVAL;
	}

	return region->ret_code ? region->ret_code : count;
}

static void vfio_ccw_async_region_release(struct vfio_ccw_private *private,
					  struct vfio_ccw_region *region)
{

}

const struct vfio_ccw_regops vfio_ccw_async_region_ops = {
	.read = vfio_ccw_async_region_read,
	.write = vfio_ccw_async_region_write,
	.release = vfio_ccw_async_region_release,
};

int vfio_ccw_register_async_dev_regions(struct vfio_ccw_private *private)
{
	return vfio_ccw_register_dev_region(private,
					    VFIO_REGION_SUBTYPE_CCW_ASYNC_CMD,
					    &vfio_ccw_async_region_ops,
					    sizeof(struct ccw_cmd_region),
					    VFIO_REGION_INFO_FLAG_READ |
					    VFIO_REGION_INFO_FLAG_WRITE,
					    private->cmd_region);
}
