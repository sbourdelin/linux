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

/**
 * PASID table data used to bind guest PASID table to the host IOMMU. This will
 * enable guest managed first level page tables.
 * @ptr		PASID table pointer in GPA
 * @size	size of the guest PASID table, must be <= host table size
 * @model	magic number tells vendor apart
 * @length	length of the opaque data
 * @opaque	architecture specific IOMMU data
 */
struct pasid_table_info {
	__u64	ptr;
	__u64	size;
	__u32	model;
#define INTEL_IOMMU	(1 << 0)
#define ARM_SMMU	(1 << 1)
	__u32	length;
	__u8	opaque[];
};

struct tlb_invalidate_info {
	__u32	model;
	__u8	opaque[];
};

#endif /* _UAPI_IOMMU_H */
