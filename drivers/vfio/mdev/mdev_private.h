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

int  mdev_device_create(struct device *dev, uuid_le uuid, uint32_t instance,
			char *mdev_params);
int  mdev_device_destroy(struct device *dev, uuid_le uuid, uint32_t instance);
void mdev_device_supported_config(struct device *dev, char *str);
int  mdev_device_start(uuid_le uuid);
int  mdev_device_stop(uuid_le uuid);

#endif /* MDEV_PRIVATE_H */
