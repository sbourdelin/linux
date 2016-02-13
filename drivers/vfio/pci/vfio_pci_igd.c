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

#include <drm/i915_drm.h>
#include <drm/i915_pciids.h>

#include "vfio_pci_private.h"

#define OPREGION_SIGNATURE	"IntelGraphicsMem"
#define OPREGION_SIZE		(8 * 1024)
#define OPREGION_PCI_ADDR	0xfc
#define BDSM_PCI_ADDR		0x5c /* Base Data Stolen Memory */

static size_t vfio_pci_igd_rw(struct vfio_pci_device *vdev, char __user *buf,
			      size_t count, loff_t *ppos, bool iswrite)
{
	unsigned int i = VFIO_PCI_OFFSET_TO_INDEX(*ppos) - VFIO_PCI_NUM_REGIONS;
	void *base = vdev->region[i].data;
	loff_t pos = *ppos & VFIO_PCI_OFFSET_MASK;

	if (pos >= vdev->region[i].size || iswrite)
		return -EINVAL;

	count = min(count, (size_t)(vdev->region[i].size - pos));

	if (copy_to_user(buf, base + pos, count))
		return -EFAULT;

	*ppos += count;

	return count;
}

static void vfio_pci_igd_release(struct vfio_pci_device *vdev,
				 struct vfio_pci_region *region)
{
	memunmap(region->data);
}

static const struct vfio_pci_regops vfio_pci_igd_regops = {
	.rw		= vfio_pci_igd_rw,
	.release	= vfio_pci_igd_release,
};

static int vfio_pci_igd_opregion_init(struct vfio_pci_device *vdev)
{
	__le32 *dwordp = (__le32 *)(vdev->vconfig + OPREGION_PCI_ADDR);
	u32 addr, size;
	void *base;
	int ret;

	ret = pci_read_config_dword(vdev->pdev, OPREGION_PCI_ADDR, &addr);
	if (ret)
		return ret;

	if (!addr || !(~addr))
		return -ENODEV;

	base = memremap(addr, OPREGION_SIZE, MEMREMAP_WB);
	if (!base)
		return -ENOMEM;

	if (memcmp(base, OPREGION_SIGNATURE, 16)) {
		memunmap(base);
		return -EINVAL;
	}

	size = le32_to_cpu(*(__le32 *)(base + 16));
	if (!size) {
		memunmap(base);
		return -EINVAL;
	}

	size *= 1024; /* In KB */

	if (size != OPREGION_SIZE) {
		memunmap(base);
		base = memremap(addr, size, MEMREMAP_WB);
		if (!base)
			return -ENOMEM;
	}

	ret = vfio_pci_register_dev_region(vdev,
		PCI_VENDOR_ID_INTEL | VFIO_REGION_TYPE_PCI_VENDOR_TYPE,
		VFIO_REGION_SUBTYPE_INTEL_IGD_OPREGION,
		&vfio_pci_igd_regops, size, VFIO_REGION_INFO_FLAG_READ, base);
	if (ret) {
		memunmap(base);
		return ret;
	}

	/* Fill vconfig with the hw value and virtualize register */
	*dwordp = cpu_to_le32(addr);
	memset(vdev->pci_config_map + OPREGION_PCI_ADDR,
	       PCI_CAP_ID_INVALID_VIRT, 4);

	return ret;
}

static size_t vfio_pci_igd_cfg_rw(struct vfio_pci_device *vdev,
				  char __user *buf, size_t count, loff_t *ppos,
				  bool iswrite)
{
	unsigned int i = VFIO_PCI_OFFSET_TO_INDEX(*ppos) - VFIO_PCI_NUM_REGIONS;
	struct pci_dev *pdev = vdev->region[i].data;
	loff_t pos = *ppos & VFIO_PCI_OFFSET_MASK;
	size_t size;
	int ret;

	if (pos >= vdev->region[i].size || iswrite)
		return -EINVAL;

	size = count = min(count, (size_t)(vdev->region[i].size - pos));

	if ((pos & 1) && size) {
		u8 val;

		ret = pci_user_read_config_byte(pdev, pos, &val);
		if (ret)
			return pcibios_err_to_errno(ret);

		if (copy_to_user(buf + count - size, &val, 1))
			return -EFAULT;

		pos++;
		size--;
	}

	if ((pos & 3) && size > 2) {
		u16 val;

		ret = pci_user_read_config_word(pdev, pos, &val);
		if (ret)
			return pcibios_err_to_errno(ret);

		val = cpu_to_le16(val);
		if (copy_to_user(buf + count - size, &val, 2))
			return -EFAULT;

		pos += 2;
		size -= 2;
	}

	while (size > 3) {
		u32 val;

		ret = pci_user_read_config_dword(pdev, pos, &val);
		if (ret)
			return pcibios_err_to_errno(ret);

		val = cpu_to_le32(val);
		if (copy_to_user(buf + count - size, &val, 4))
			return -EFAULT;

		pos += 4;
		size -= 4;
	}

	while (size >= 2) {
		u16 val;

		ret = pci_user_read_config_word(pdev, pos, &val);
		if (ret)
			return pcibios_err_to_errno(ret);

		val = cpu_to_le16(val);
		if (copy_to_user(buf + count - size, &val, 2))
			return -EFAULT;

		pos += 2;
		size -= 2;
	}

	while (size) {
		u8 val;

		ret = pci_user_read_config_byte(pdev, pos, &val);
		if (ret)
			return pcibios_err_to_errno(ret);

		if (copy_to_user(buf + count - size, &val, 1))
			return -EFAULT;

		pos++;
		size--;
	}

	*ppos += count;

	return count;
}

static void vfio_pci_igd_cfg_release(struct vfio_pci_device *vdev,
				     struct vfio_pci_region *region)
{
	struct pci_dev *pdev = region->data;

	pci_dev_put(pdev);
}

static const struct vfio_pci_regops vfio_pci_igd_cfg_regops = {
	.rw		= vfio_pci_igd_cfg_rw,
	.release	= vfio_pci_igd_cfg_release,
};

static int vfio_pci_igd_cfg_init(struct vfio_pci_device *vdev)
{
	struct pci_dev *host_bridge, *lpc_bridge;
	int ret;

	host_bridge = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0, 0));
	if (!host_bridge)
		return -ENODEV;

	if (host_bridge->vendor != PCI_VENDOR_ID_INTEL ||
	    host_bridge->class != (PCI_CLASS_BRIDGE_HOST << 8)) {
		pci_dev_put(host_bridge);
		return -EINVAL;
	}

	ret = vfio_pci_register_dev_region(vdev,
		PCI_VENDOR_ID_INTEL | VFIO_REGION_TYPE_PCI_VENDOR_TYPE,
		VFIO_REGION_SUBTYPE_INTEL_IGD_HOST_CFG,
		&vfio_pci_igd_cfg_regops, host_bridge->cfg_size,
		VFIO_REGION_INFO_FLAG_READ, host_bridge);
	if (ret) {
		pci_dev_put(host_bridge);
		return ret;
	}

	lpc_bridge = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0x1f, 0));
	if (!lpc_bridge)
		return -ENODEV;

	if (lpc_bridge->vendor != PCI_VENDOR_ID_INTEL ||
	    lpc_bridge->class != (PCI_CLASS_BRIDGE_ISA << 8)) {
		pci_dev_put(lpc_bridge);
		return -EINVAL;
	}

	ret = vfio_pci_register_dev_region(vdev,
		PCI_VENDOR_ID_INTEL | VFIO_REGION_TYPE_PCI_VENDOR_TYPE,
		VFIO_REGION_SUBTYPE_INTEL_IGD_LPC_CFG,
		&vfio_pci_igd_cfg_regops, lpc_bridge->cfg_size,
		VFIO_REGION_INFO_FLAG_READ, lpc_bridge);
	if (ret) {
		pci_dev_put(lpc_bridge);
		return ret;
	}

	return 0;
}

struct vfio_pci_igd_info {
	u16 gmch_gsm_mask;
};

static const struct vfio_pci_igd_info igd_gen6 = {
	.gmch_gsm_mask = SNB_GMCH_GMS_MASK << SNB_GMCH_GMS_SHIFT,
};

static const struct vfio_pci_igd_info igd_gen8 = {
	.gmch_gsm_mask = BDW_GMCH_GMS_MASK << BDW_GMCH_GMS_SHIFT,
};

static const struct pci_device_id vfio_pci_igd_ids[] = {
	/* Gen6 - SandyBridge */
	INTEL_SNB_D_IDS(&igd_gen6),
	INTEL_SNB_M_IDS(&igd_gen6),
	/* Gen7 - IvyBridge, ValleyView, Haswell */
	INTEL_IVB_D_IDS(&igd_gen6),
	INTEL_IVB_M_IDS(&igd_gen6),
	INTEL_IVB_Q_IDS(&igd_gen6),
	INTEL_VLV_M_IDS(&igd_gen6),
	INTEL_VLV_D_IDS(&igd_gen6),
	INTEL_HSW_D_IDS(&igd_gen6),
	INTEL_HSW_M_IDS(&igd_gen6),
	/* Gen8 - BroadWell, CherryView */
	INTEL_BDW_GT12D_IDS(&igd_gen8),
	INTEL_BDW_GT12M_IDS(&igd_gen8),
	INTEL_BDW_GT3D_IDS(&igd_gen8),
	INTEL_BDW_GT3M_IDS(&igd_gen8),
	INTEL_CHV_IDS(&igd_gen8),
	/* Gen9 - SkyLake, Broxton, KabyLake */
	INTEL_SKL_GT1_IDS(&igd_gen8),
	INTEL_SKL_GT2_IDS(&igd_gen8),
	INTEL_SKL_GT3_IDS(&igd_gen8),
	INTEL_SKL_GT4_IDS(&igd_gen8),
	INTEL_BXT_IDS(&igd_gen8),
	INTEL_KBL_GT1_IDS(&igd_gen8),
	INTEL_KBL_GT2_IDS(&igd_gen8),
	INTEL_KBL_GT3_IDS(&igd_gen8),
	INTEL_KBL_GT4_IDS(&igd_gen8),
	{ 0 }
};

static struct vfio_pci_igd_info *vfio_pci_igd_info(struct pci_dev *pdev)
{
	const struct pci_device_id *id;

	id = pci_match_id(vfio_pci_igd_ids, pdev);
	if (!id)
		return NULL;

	return (struct vfio_pci_igd_info *)id->driver_data;
}

int vfio_pci_igd_init(struct vfio_pci_device *vdev)
{
	struct vfio_pci_igd_info *info;
	u16 gmch;
	int ret;

	ret = vfio_pci_igd_opregion_init(vdev);
	if (ret)
		return ret;

	ret = vfio_pci_igd_cfg_init(vdev);
	if (ret)
		return ret;

	memset(vdev->vconfig + BDSM_PCI_ADDR, 0, 4);
	memset(vdev->pci_config_map + BDSM_PCI_ADDR,
	       PCI_CAP_ID_INVALID_VIRT, 4);

	info = vfio_pci_igd_info(vdev->pdev);
	if (!info) {
		dev_warn(&vdev->pdev->dev,
			 "Unknown/Unsupported Intel IGD device\n");
		return 0;
	}

	ret = pci_read_config_word(vdev->pdev, SNB_GMCH_CTRL, &gmch);
	if (ret)
		return ret;

	gmch &= ~info->gmch_gsm_mask;
	*(__le16 *)(vdev->vconfig + SNB_GMCH_CTRL) = cpu_to_le16(gmch);
	memset(vdev->pci_config_map + SNB_GMCH_CTRL,
	       PCI_CAP_ID_INVALID_VIRT, 2);

	return 0;
}
