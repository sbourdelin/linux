/*
 * Copyright (C) 2015 Cavium, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 */

#ifndef __TBL_ACCESS_H__
#define __TBL_ACCESS_H__

#define TNS_MAX_TABLE	8

enum {
	TNS_TBL_TYPE_DT,
	TNS_TBL_TYPE_HT,
	TNS_TBL_TYPE_TT,
	TNS_TBL_TYPE_MAX
};

struct table_static_s {
	u8 tbl_type;
	u8 tbl_id;
	u8 valid;
	u8 rsvd;
	u16 key_size;
	u16 data_size;
	u16 data_width;
	u16 key_width;
	u32 depth;
	u64 key_base_addr;
	u64 data_base_addr;
	u8 tbl_name[32];
};

struct table_dynamic_s {
	unsigned long *bitmap;
};

struct tns_table_s {
	struct table_static_s sdata;
	struct table_dynamic_s ddata[MAX_NUMNODES];
};

enum {
	MAC_FILTER_TABLE = 102,
	VLAN_FILTER_TABLE = 103,
	MAC_EVIF_TABLE = 140,
	VLAN_EVIF_TABLE = 201,
	PORT_CONFIG_TABLE = 202,
	TABLE_ID_END
};

extern struct tns_table_s	tbl_info[TNS_MAX_TABLE];

struct filter_keymask_s {
	u8 is_valid;
	u64 key_value;
};

#endif /* __TBL_ACCESS_H__ */
