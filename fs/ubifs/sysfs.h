#ifndef __UBIFS_SYSFS_H__
#define __UBIFS_SYSFS_H__

struct ubifs_info;

int ubifs_sysfs_init(void);
void ubifs_sysfs_exit(void);
int ubifs_sysfs_register(struct ubifs_info *c);
void ubifs_sysfs_unregister(struct ubifs_info *c);

#endif
