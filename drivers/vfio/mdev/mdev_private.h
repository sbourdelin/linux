/*
 * Mediated device interal definitions
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *     Author: Neo Jia <cjia@nvidia.com>
 *	       Kirti Wankhede <kwankhede@nvidia.com>
 *
 * Copyright (c) 2016 Intel Corporation.
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
int  mdev_create_sysfs_files(struct device *dev);
void mdev_remove_sysfs_files(struct device *dev);

int  mdev_device_create(struct device *dev, uuid_le uuid, char *mdev_params);
int  mdev_device_destroy(struct device *dev, uuid_le uuid);
void mdev_device_supported_config(struct device *dev, char *str);

#endif /* MDEV_PRIVATE_H */
