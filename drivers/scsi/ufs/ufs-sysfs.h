/*
* UFS Device Management sysfs
*
* Copyright (C) 2017 Western Digital Corporation
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License version
* 2 as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful, but
* WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* General Public License for more details.
*
*/
#ifndef __UFS_SYSFS_H__
#define __UFS_SYSFS_H__

#include <linux/sysfs.h>
#include <scsi/scsi_device.h>

#include "ufshcd.h"

void ufs_sysfs_add_device_management(struct ufs_hba *hba);
void ufs_sysfs_remove_device_management(struct ufs_hba *hba);

extern struct attribute_group ufs_sysfs_unit_descriptor_group;
#endif
