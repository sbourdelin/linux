#ifndef __UFS_SYSFS_H__
#define __UFS_SYSFS_H__

#include <linux/sysfs.h>
#include <scsi/scsi_device.h>

#include "ufshcd.h"

void ufs_sysfs_add_device_management(struct ufs_hba *hba);
void ufs_sysfs_remove_device_management(struct ufs_hba *hba);

extern struct attribute_group ufs_sysfs_unit_descriptor_group;
#endif
