/*
 * Mediated device interal definitions
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *     Author: Neo Jia <cjia@nvidia.com>
 *	       Kirti Wankhede <kwankhede@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef MDEV_PRIVATE_H
#define MDEV_PRIVATE_H

int  mdev_bus_register(void);
void mdev_bus_unregister(void);

/* Function prototypes for mdev_sysfs */

extern struct class_attribute mdev_class_attrs[];

int  mdev_create_sysfs_files(struct device *dev);
void mdev_remove_sysfs_files(struct device *dev);

int  create_mdev_device(struct device *dev, uuid_le uuid, uint32_t instance,
			char *mdev_params);
int  destroy_mdev_device(uuid_le uuid, uint32_t instance);
void get_mdev_supported_types(struct device *dev, char *str);
int  mdev_start_callback(uuid_le uuid, uint32_t instance);
int  mdev_shutdown_callback(uuid_le uuid, uint32_t instance);

#endif /* MDEV_PRIVATE_H */
