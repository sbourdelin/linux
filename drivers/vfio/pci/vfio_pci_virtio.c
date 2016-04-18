/*
 * VFIO PCI Intel Graphics support
 *
 * Copyright (C) 2016 Red Hat, Inc.  All rights reserved.
 *	Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Register a device specific region through which to provide read-only
 * access to the Intel IGD opregion.  The register defining the opregion
 * address is also virtualized to prevent user modification.
 */

#include <linux/io.h>
#include <linux/pci.h>
#include <linux/uaccess.h>
#include <linux/vfio.h>
#include <linux/virtio_pci.h>
#include <linux/virtio_config.h>

#include "vfio_pci_private.h"

/**
 * virtio_pci_find_capability - walk capabilities to find device info.
 * @dev: the pci device
 * @cfg_type: the VIRTIO_PCI_CAP_* value we seek
 *
 * Returns offset of the capability, or 0.
 */
static inline int virtio_pci_find_capability(struct pci_dev *dev, u8 cfg_type)
{
	int pos;

	for (pos = pci_find_capability(dev, PCI_CAP_ID_VNDR);
	     pos > 0;
	     pos = pci_find_next_capability(dev, pos, PCI_CAP_ID_VNDR)) {
		u8 type;
		pci_read_config_byte(dev, pos + offsetof(struct virtio_pci_cap,
							 cfg_type),
				     &type);

		if (type != cfg_type)
			continue;

		/* Ignore structures with reserved BAR values */
		if (type != VIRTIO_PCI_CAP_PCI_CFG) {
			u8 bar;

			pci_read_config_byte(dev, pos +
					     offsetof(struct virtio_pci_cap,
						      bar),
					     &bar);
			if (bar > 0x5)
				continue;
		}

		return pos;
	}
	return 0;
}


int vfio_pci_virtio_quirk(struct vfio_pci_device *vdev, int noiommu)
{
	struct pci_dev *dev = vdev->pdev;
	int common, cfg;
	u32 features;
	u32 offset;
	u8 bar;

	/* Without an IOMMU, we don't care */
	if (noiommu)
		return 0;
	/* Check whether device enforces the IOMMU correctly */

	/*
	 * All modern devices must have common and cfg capabilities. We use cfg
	 * capability for access so that we don't need to worry about resource
	 * availability. Slow but sure.
	 * Note that all vendor-specific fields we access are little-endian
	 * which matches what pci config accessors expect, so they do byteswap
	 * for us if appropriate.
	 */
	common = virtio_pci_find_capability(dev, VIRTIO_PCI_CAP_COMMON_CFG);
	cfg = virtio_pci_find_capability(dev, VIRTIO_PCI_CAP_PCI_CFG);
	if (!cfg || !common) {
                dev_warn(&dev->dev,
                         "Virtio device lacks common or pci cfg.\n");
		return -ENODEV;
	}

	pci_read_config_byte(dev, common + offsetof(struct virtio_pci_cap,
						    bar),
			     &bar);
	pci_read_config_dword(dev, common + offsetof(struct virtio_pci_cap,
						    offset),
			     &offset);

	/* Program cfg capability for dword access into common cfg. */
	pci_write_config_byte(dev, cfg + offsetof(struct virtio_pci_cfg_cap,
						  cap.bar),
			      bar);
	pci_write_config_dword(dev, cfg + offsetof(struct virtio_pci_cfg_cap,
						   cap.length),
			       0x4);

	/* Select features dword that has VIRTIO_F_IOMMU_PLATFORM. */
	pci_write_config_dword(dev, cfg + offsetof(struct virtio_pci_cfg_cap,
						  cap.offset),
			       offset + offsetof(struct virtio_pci_common_cfg,
						 device_feature_select));
	pci_write_config_dword(dev, cfg + offsetof(struct virtio_pci_cfg_cap,
						  pci_cfg_data),
			       VIRTIO_F_IOMMU_PLATFORM / 32);

	/* Get the features dword. */
	pci_write_config_dword(dev, cfg + offsetof(struct virtio_pci_cfg_cap,
						  cap.offset),
			       offset + offsetof(struct virtio_pci_common_cfg,
						 device_feature));
	pci_read_config_dword(dev, cfg + offsetof(struct virtio_pci_cfg_cap,
						  pci_cfg_data),
			      &features);

	/* Does this device obey the platform's IOMMU? If not it's an error. */
	if (!(features & (0x1 << (VIRTIO_F_IOMMU_PLATFORM % 32)))) {
                dev_warn(&dev->dev,
                         "Virtio device lacks VIRTIO_F_IOMMU_PLATFORM.\n");
		return -ENODEV;
	}

	return 0;
}
