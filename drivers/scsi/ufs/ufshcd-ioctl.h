/* Copyright (c) 2013-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * UFS ioctl - this architecture can be used to configure driver and
 * device/host parameters, that are otherwise unavailable for such operation
 */

#ifndef _UFSHCD_IOCTL_H
#define _UFSHCD_IOCTL_H

#include <linux/errno.h>

#include "ufshcd.h"

#ifdef CONFIG_SCSI_UFSHCD_IOCTL
int ufshcd_ioctl(struct scsi_device *dev, int cmd, void __user *buffer);
#else
static int ufshcd_ioctl(struct scsi_device *dev, int cmd, void __user *buffer)
{
	return -ENOIOCTLCMD;
}
#endif

#endif /* End of Header */
