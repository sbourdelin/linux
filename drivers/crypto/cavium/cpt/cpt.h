/*
 * Copyright (C) 2016 Cavium, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 */

#ifndef __CPT_H
#define __CPT_H

#include "cpt_common.h"

#define BASE_PROC_DIR	"cavium"

#define PF  0
#define VF  1

struct cpt_device;

struct microcode {
	uint8_t  is_mc_valid;
	uint8_t  is_ae;
	uint8_t  group;
	uint32_t code_size;
	void    *code;
	uint8_t  num_cores;
	uint64_t core_mask_low; /* Used as long as num # cores are <= 64 */
	uint64_t core_mask_hi;  /* Unused for now */
	uint8_t  version[32];

	/* Base info */
	dma_addr_t dma;
	dma_addr_t phys_base;
	void *base;
};

#define VF_STATE_DOWN	(0)
#define VF_STATE_UP	(1)

struct cpt_vf_info {
	uint8_t state;
	uint8_t priority;
	uint32_t qlen;
	union cpt_chipid_vfid id;
};

/**
 * cpt device structure
 */
struct cpt_device {
	uint32_t chip_id; /**< CPT Device ID */
	uint16_t core_freq; /**< CPT Device Frequency */
	uint16_t flags;	/**< Flags to hold device status bits */
	uint8_t idx; /**< Device Index (0...MAX_CPT_DEVICES) */
	uint8_t num_vf_en; /**< Number of VFs enabled (0...CPT_MAX_VF_NUM) */

	struct cpt_vf_info vfinfo[CPT_MAX_VF_NUM]; /* Per VF info */
	uint8_t next_mc_idx; /**< next microcode index */
	uint8_t next_group;

	uint8_t max_se_cores;
	uint8_t max_ae_cores;
	uint8_t avail_se_cores;
	uint8_t avail_ae_cores;

	void __iomem *reg_base; /* Register start address */

	/* MSI-X */
	bool msix_enabled;
	uint8_t	num_vec;
	struct msix_entry msix_entries[CPT_PF_MSIX_VECTORS];
	bool irq_allocated[CPT_PF_MSIX_VECTORS];

	bool mbx_lock[CPT_MAX_VF_NUM]; /* Mailbox locks per VF */

	struct pci_dev *pdev; /**< pci device handle */
	void *proc; /**< proc dir */
	struct microcode mcode[CPT_MAX_CORE_GROUPS];
};

struct cpt_device_list {
	/* device list lock */
	spinlock_t lock;
	uint32_t nr_device;
	struct cpt_device *device_ptr[MAX_CPT_DEVICES];
};

void cpt_mbox_intr_handler(struct cpt_device *cpt, int mbx);
#endif /* __CPT_H */
