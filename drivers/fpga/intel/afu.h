/*
 * Header file for Intel FPGA Accelerated Function Unit (AFU) Driver
 *
 * Copyright (C) 2017 Intel Corporation, Inc.
 *
 * Authors:
 *     Wu Hao <hao.wu@intel.com>
 *     Xiao Guangrong <guangrong.xiao@linux.intel.com>
 *     Joseph Grecco <joe.grecco@intel.com>
 *     Enno Luebbers <enno.luebbers@intel.com>
 *     Tim Whisonant <tim.whisonant@intel.com>
 *     Ananda Ravuri <ananda.ravuri@intel.com>
 *     Henry Mitchel <henry.mitchel@intel.com>
 *
 * This work is licensed under a dual BSD/GPLv2 license. When using or
 * redistributing this file, you may do so under either license. See the
 * LICENSE.BSD file under this directory for the BSD license and see
 * the COPYING file in the top-level directory for the GPLv2 license.
 */

#ifndef __INTEL_AFU_H
#define __INTEL_AFU_H

#include "feature-dev.h"

struct fpga_afu_region {
	u32 index;
	u32 flags;
	u64 size;
	u64 offset;
	u64 phys;
	struct list_head node;
};

struct fpga_afu_dma_region {
	u64 user_addr;
	u64 length;
	u64 iova;
	struct page **pages;
	struct rb_node node;
	bool in_use;
};

struct fpga_afu {
	u64 region_cur_offset;
	int num_regions;
	u8 num_umsgs;
	struct list_head regions;
	struct rb_root dma_regions;

	struct feature_platform_data *pdata;
};

void afu_region_init(struct feature_platform_data *pdata);
int afu_region_add(struct feature_platform_data *pdata, u32 region_index,
		   u64 region_size, u64 phys, u32 flags);
void afu_region_destroy(struct feature_platform_data *pdata);
int afu_get_region_by_index(struct feature_platform_data *pdata,
			    u32 region_index, struct fpga_afu_region *pregion);
int afu_get_region_by_offset(struct feature_platform_data *pdata,
			    u64 offset, u64 size,
			    struct fpga_afu_region *pregion);

void afu_dma_region_init(struct feature_platform_data *pdata);
void afu_dma_region_destroy(struct feature_platform_data *pdata);
long afu_dma_map_region(struct feature_platform_data *pdata,
		       u64 user_addr, u64 length, u64 *iova);
long afu_dma_unmap_region(struct feature_platform_data *pdata, u64 iova);
struct fpga_afu_dma_region *afu_dma_region_find(
		struct feature_platform_data *pdata, u64 iova, u64 size);

#endif
