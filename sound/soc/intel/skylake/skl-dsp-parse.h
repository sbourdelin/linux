/*
 *  skl-dsp-parse.h
 *
 *  Copyright (C) 2016 Intel Corp
 *  Author: Shreyas NC <shreyas.nc@intel.com>
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <asm/types.h>
#include <linux/types.h>
#include <linux/uuid.h>
#include "skl-tplg-interface.h"
#include "../common/sst-dsp-priv.h"

#define SKL_ADSP_FW_BIN_HDR_OFFSET 0x284
#define UUID_STR_SIZE 37
#define DEFAULT_HASH_SHA256_LEN 32

struct skl_dfw_module_mod {
	char name[100];
	struct skl_dfw_module skl_dfw_mod;
};

struct UUID {
	u8  id[16];
};

union seg_flags {
	u32 ul;
	struct {
		u32 contents : 1;
		u32 alloc    : 1;
		u32 load     : 1;
		u32 read_only : 1;
		u32 code     : 1;
		u32 data     : 1;
		u32 _rsvd0   : 2;
		u32 type     : 4;
		u32 _rsvd1   : 4;
		u32 length   : 16;
	} r;
} __packed;

struct segment_desc {
	union seg_flags flags;
	u32 v_base_addr;
	u32 file_offset;
};

struct module_type {
	u32 load_type  : 4;
	u32 auto_start : 1;
	u32 domain_ll  : 1;
	u32 domain_dp  : 1;
	u32 rsvd_      : 25;
} __packed;

struct adsp_module_entry {
	u32 struct_id;
	u8  name[8];
	struct UUID uuid;
	struct module_type type;
	u8  hash1[DEFAULT_HASH_SHA256_LEN];
	u32 entry_point;
	u16 cfg_offset;
	u16 cfg_count;
	u32 affinity_mask;
	u16 instance_max_count;
	u16 instance_bss_size;
	struct segment_desc segments[3];
} __packed;

struct adsp_fw_hdr {
	u32 header_id;
	u32 header_len;
	u8  name[8];
	u32 preload_page_count;
	u32 fw_image_flags;
	u32 feature_mask;
	u16 major_version;
	u16 minor_version;
	u16 hotfix_version;
	u16 build_version;
	u32 num_module_entries;
	u32 hw_buf_base_addr;
	u32 hw_buf_length;
	u32 load_offset;
} __packed;

struct uuid_tbl {
	uuid_le uuid;
	int module_id;
	int is_loadable;
};

int snd_skl_parse_fw_bin(struct sst_dsp *ctx);
int snd_skl_get_module_info(struct skl_sst *ctx, u8 *uuid,
		struct skl_dfw_module *dfw_config);
