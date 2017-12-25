#ifndef __UFS_SYSFS_H__
#define __UFS_SYSFS_H__

#include <linux/sysfs.h>

#include "ufshcd.h"

void ufs_sysfs_add_device_management(struct ufs_hba *hba);
void ufs_sysfs_remove_device_management(struct ufs_hba *hba);
#endif
