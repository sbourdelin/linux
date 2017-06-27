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


#endif /* _UAPI_IOMMU_H */
