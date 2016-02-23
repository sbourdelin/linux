/*
 * VGPU interal definition
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *     Author:
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef VGPU_PRIVATE_H
#define VGPU_PRIVATE_H

struct vgpu_device *vgpu_drv_get_vgpu_device(uuid_le uuid, int instance);

int  create_vgpu_device(struct pci_dev *pdev, uuid_le uuid, uint32_t instance,
		       char *vgpu_params);
void destroy_vgpu_device(struct vgpu_device *vgpu_dev);

int  vgpu_bus_register(void);
void vgpu_bus_unregister(void);

/* Function prototypes for vgpu_sysfs */

extern struct class_attribute vgpu_class_attrs[];
extern const struct attribute_group *vgpu_dev_groups[];

int  vgpu_create_pci_device_files(struct pci_dev *dev);
void vgpu_remove_pci_device_files(struct pci_dev *dev);

void get_vgpu_supported_types(struct device *dev, char *str);
int  vgpu_start_callback(struct vgpu_device *vgpu_dev);
int  vgpu_shutdown_callback(struct vgpu_device *vgpu_dev);

#endif /* VGPU_PRIVATE_H */
