/*
 * IOMMU user API definitions
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _UAPI_IOMMU_H
#define _UAPI_IOMMU_H

#include <linux/types.h>

enum iommu_model {
	IOMMU_MODEL_INTEL_VTD,
	IOMMU_MODEL_ARM_SMMU,
};

/*
 * PASID table data used to bind guest PASID table to the host IOMMU. This will
 * enable guest managed first level page tables.
 * @ptr		PASID table pointer
 * @size	size of the guest PASID table in bytes, must be <= host table size
 * @model	iommu_model number
 * @length	length of the opaque data in bytes
 * @opaque	model specific IOMMU data
 */
struct pasid_table_info {
	__u64	ptr;
	__u64	size;
	__u32	model;
	__u32	length;
	__u8	opaque[];
};

/*
 * Translation cache invalidation information, contains IOMMU model specific
 * data which can be parsed based on model ID by model specific drivers.
 *
 * @model	iommu_model number
 * @length	length of the opaque data in bytes
 * @opaque	model specific IOMMU data
 */
struct tlb_invalidate_info {
	__u32	model;
	__u32	length;
	__u8	opaque[];
};

/*
 * Generic fault event notification data, used by all IOMMU models
 *
 * - PCI and non-PCI devices
 * - Recoverable faults (e.g. page request) & un-recoverable faults
 * - DMA remapping and IRQ remapping faults
 *
 * @dev The device which faults are reported by IOMMU
 * @addr tells the offending address
 * @pasid contains process address space ID, used in shared virtual memory (SVM)
 * @prot page access protection flag, e.g. IOMMU_READ, IOMMU_WRITE
 * @flags contains fault type, etc.
 * @length tells the size of the buf in bytes
 * @buf contains any raw or arch specific data
 *
 */
struct iommu_fault_event {
	struct device *dev;
	__u64 addr;
	__u32 pasid;
	__u32 prot;
	__u32 flags;
/* page request as result of recoverable translation fault */
#define IOMMU_FAULT_PAGE_REQ	BIT(0)
/* unrecoverable fault, e.g. invalid device context  */
#define IOMMU_FAULT_UNRECOV	BIT(1)
/* unrecoverable fault related to interrupt remapping */
#define IOMMU_FAULT_IRQ_REMAP	BIT(2)
/* unrecoverable fault on invalidation of translation caches */
#define IOMMU_FAULT_INVAL	BIT(3)
	__u32 length;
	__u8  buf[];
};

#endif /* _UAPI_IOMMU_H */
