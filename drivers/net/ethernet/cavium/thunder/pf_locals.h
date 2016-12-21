/*
 * Copyright (C) 2015 Cavium, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 */

#ifndef __PF_LOCALS__
#define __PF_LOCALS__

#include <linux/printk.h>

#define XP_TOTAL_PORTS	(137)
#define MAX_SYS_PORTS	XP_TOTAL_PORTS
//Loopback port was invalid in MAC filter design
#define TNS_MAC_FILTER_MAX_SYS_PORTS	(MAX_SYS_PORTS - 1)
//Maximum LMAC available
#define TNS_MAX_INGRESS_GROUP	8
#define TNS_MAX_VF	(TNS_MAC_FILTER_MAX_SYS_PORTS - TNS_MAX_INGRESS_GROUP)
#define TNS_VLAN_FILTER_MAX_INDEX	256
#define TNS_MAC_FILTER_MAX_INDEX	1536
#define TNS_MAX_VLAN_PER_VF	16

#define TNS_NULL_VIF		152
#define TNS_BASE_BCAST_VIF	136
#define TNS_BASE_MCAST_VIF	144
#define TNS_FW_MAX_SIZE         1048576

/* We are restricting each VF to register atmost 11 filter entries
 * (including unicast & multicast)
 */
#define TNS_MAX_MAC_PER_VF	11

#define FERR		0
#define FDEBUG		1
#define FINFO		2

#define FILTER_DBG_GBL		FERR
#define filter_dbg(dbg_lvl, fmt, args...) \
	({ \
	if ((dbg_lvl) <= FILTER_DBG_GBL) \
		pr_info(fmt, ##args); \
	})

typedef u8 mac_addr_t[6];		///< User define type for Mac Address
typedef u8 vlan_port_bitmap_t[32];

enum {
	TNS_NO_ERR = 0,

	/* Error in indirect read watch out the status */
	TNS_ERROR_INDIRECT_READ = 4,
	/* Error in indirect write watch out the status */
	TNS_ERROR_INDIRECT_WRITE = 5,
	/* Data too large for Read/Write */
	TNS_ERROR_DATA_TOO_LARGE = 6,
	/* Invalid arguments supplied to the IOCTL */
	TNS_ERROR_INVALID_ARG = 7,

	TNS_ERR_MAC_FILTER_INVALID_ENTRY,
	TNS_ERR_MAC_FILTER_TBL_READ,
	TNS_ERR_MAC_FILTER_TBL_WRITE,
	TNS_ERR_MAC_EVIF_TBL_READ,
	TNS_ERR_MAC_EVIF_TBL_WRITE,

	TNS_ERR_VLAN_FILTER_INVLAID_ENTRY,
	TNS_ERR_VLAN_FILTER_TBL_READ,
	TNS_ERR_VLAN_FILTER_TBL_WRITE,
	TNS_ERR_VLAN_EVIF_TBL_READ,
	TNS_ERR_VLAN_EVIF_TBL_WRITE,

	TNS_ERR_PORT_CONFIG_TBL_READ,
	TNS_ERR_PORT_CONFIG_TBL_WRITE,
	TNS_ERR_PORT_CONFIG_INVALID_ENTRY,

	TNS_ERR_DRIVER_READ,
	TNS_ERR_DRIVER_WRITE,

	TNS_ERR_WRONG_PORT_NUMBER,
	TNS_ERR_INVALID_TBL_ID,
	TNS_ERR_ENTRY_NOT_FOUND,
	TNS_ERR_DUPLICATE_MAC,
	TNS_ERR_MAX_LIMIT,

	TNS_STATUS_NUM_ENTRIES
};

struct ing_grp_gblvif {
	u32 ingress_grp;
	u32 pf_vf;
	u32 bcast_vif;
	u32 mcast_vif;
	u32 null_vif;
	u32 is_valid; //Is this Ingress Group or LMAC is valid
	u8 mcast_promis_grp[TNS_MAC_FILTER_MAX_SYS_PORTS];
	u8 valid_mcast_promis_ports;
};

struct vf_register_s {
	int filter_index[16];
	u32 filter_count;
	int vf_in_mcast_promis;
	int vf_in_promis;
	int vlan[TNS_MAX_VLAN_PER_VF];
	u32 vlan_count;
};

union mac_filter_keymask_type_s {
	u64 key_value;

	struct {
		u32	ingress_grp: 16;
		mac_addr_t	mac_DA;
	} s;
};

struct mac_filter_keymask_s {
	u8 is_valid;
	union mac_filter_keymask_type_s key_type;
};

union mac_filter_data_s {
	u64 data;
	struct {
		u64 evif: 16;
		u64 Reserved0 : 48;
	} s;
};

struct mac_filter_entry {
	struct mac_filter_keymask_s key;
	struct mac_filter_keymask_s mask;
	union mac_filter_data_s data;
};

union vlan_filter_keymask_type_s {
	u64 key_value;

	struct {
		u32	ingress_grp: 16;
		u32	vlan: 12;
		u32	reserved: 4;
		u32	reserved1;
	} s;
};

struct vlan_filter_keymask_s {
	u8 is_valid;
	union vlan_filter_keymask_type_s key_type;
};

union vlan_filter_data_s {
	u64 data;
	struct {
		u64 filter_idx: 16;
		u64 Reserved0 : 48;
	} s;
};

struct vlan_filter_entry {
	struct vlan_filter_keymask_s key;
	struct vlan_filter_keymask_s mask;
	union vlan_filter_data_s data;
};

struct evif_entry {
	u64	rsp_type: 2;
	u64	truncate: 1;
	u64	mtu_prf: 3;
	u64	mirror_en: 1;
	u64	q_mirror_en: 1;
	u64	prt_bmap7_0: 8;
	u64	rewrite_ptr0: 8;
	u64	rewrite_ptr1: 8;
	/* Byte 0 is data31_0[7:0] and byte 3 is data31_0[31:24] */
	u64	data31_0: 32;
	u64	insert_ptr0: 16;
	u64	insert_ptr1: 16;
	u64	insert_ptr2: 16;
	u64	mre_ptr: 15;
	u64	prt_bmap_8: 1;
	u64	prt_bmap_72_9;
	u64	prt_bmap_136_73;
};

struct itt_entry_s {
	u32 rsvd0 : 30;
	u32 pkt_dir : 1;
	u32 is_admin_vlan_enabled : 1;
	u32 reserved0 : 6;
	u32 default_evif : 8;
	u32 admin_vlan : 12;
	u32 Reserved1 : 6;
	u32 Reserved2[6];
};

static inline u64 TNS_TDMA_SST_ACC_RDATX(unsigned long param1)
{
	return 0x00000480ull + (param1 & 7) * 0x10ull;
}

static inline u64 TNS_TDMA_SST_ACC_WDATX(unsigned long param1)
{
	return 0x00000280ull + (param1 & 7) * 0x10ull;
}

union tns_tdma_sst_acc_cmd {
	u64 u;
	struct  tns_tdma_sst_acc_cmd_s {
		u64 reserved_0_1	: 2;
		u64 addr		: 30;
		u64 size		: 4;
		u64 op			: 1;
		u64 go			: 1;
		u64 reserved_38_63	: 26;
	} s;
};

#define TDMA_SST_ACC_CMD 0x00000270ull

union tns_tdma_sst_acc_stat_t {
	u64 u;
	struct  tns_tdma_sst_acc_stat_s {
		u64 cmd_done		: 1;
		u64 error		: 1;
		u64 reserved_2_63	: 62;
	} s;
};

#define TDMA_SST_ACC_STAT 0x00000470ull
#define TDMA_NB_INT_STAT 0x01000110ull

union tns_acc_data {
	u64 u;
	struct tns_acc_data_s {
		u64 lower32 : 32;
		u64 upper32 : 32;
	} s;
};

union tns_tdma_config {
	u64 u;
	struct  tns_tdma_config_s {
		u64 clk_ena		: 1;
		u64 clk_2x_ena		: 1;
		u64 reserved_2_3	: 2;
		u64 csr_access_ena	: 1;
		u64 reserved_5_7	: 3;
		u64 bypass0_ena		: 1;
		u64 bypass1_ena		: 1;
		u64 reserved_10_63	: 54;
	} s;
};

#define TNS_TDMA_CONFIG_OFFSET  0x00000200ull

union tns_tdma_cap {
	u64 u;
	struct tns_tdma_cap_s {
		u64 switch_capable	: 1;
		u64 reserved_1_63	: 63;
	} s;
};

#define TNS_TDMA_CAP_OFFSET 0x00000400ull
#define TNS_RDMA_CONFIG_OFFSET 0x00001200ull

union tns_tdma_lmacx_config {
	u64 u;
	struct tns_tdma_lmacx_config_s {
		u64 fifo_cdts		: 14;
		u64 reserved_14_63	: 50;
	} s;
};

union _tns_sst_config {
	u64 data;
	struct {
#ifdef __BIG_ENDIAN
		u64 powerof2stride	: 1;
		u64 run			: 11;
		u64 reserved		: 14;
		u64 req_type		: 2;
		u64 word_cnt		: 4;
		u64 byte_addr		: 32;
#else
		u64 byte_addr		: 32;
		u64 word_cnt		: 4;
		u64 req_type		: 2;
		u64 reserved		: 14;
		u64 run			: 11;
		u64 powerof2stride	: 1;
#endif
	} cmd;
	struct {
#ifdef __BIG_ENDIAN
		u64 do_not_copy		: 26;
		u64 do_copy		: 38;
#else
		u64 do_copy		: 38;
		u64 do_not_copy		: 26;
#endif
	} copy;
	struct {
#ifdef __BIG_ENDIAN
		u64 magic		: 48;
		u64 major_version_BCD	: 8;
		u64 minor_version_BCD	: 8;
#else
		u64 minor_version_BCD	: 8;
		u64 major_version_BCD	: 8;
		u64 magic		: 48;
#endif
	} header;
};

static inline u64 TNS_TDMA_LMACX_CONFIG_OFFSET(unsigned long param1)
			 __attribute__ ((pure, always_inline));
static inline u64 TNS_TDMA_LMACX_CONFIG_OFFSET(unsigned long param1)
{
	return 0x00000300ull + (param1 & 7) * 0x10ull;
}

#define TNS_TDMA_RESET_CTL_OFFSET 0x00000210ull

int read_register_indirect(u64 address, u8 size, u8 *kern_buffer);
int write_register_indirect(u64 address, u8 size, u8 *kern_buffer);
int tns_write_register_indirect(int node, u64 address, u8 size,
				u8 *kern_buffer);
int tns_read_register_indirect(int node, u64 address, u8 size,
			       u8 *kern_buffer);
u64 tns_read_register(u64 start, u64 offset);
void tns_write_register(u64 start, u64 offset, u64 data);
int tbl_write(int node, int tbl_id, int tbl_index, void *key, void *mask,
	      void *data);
int tbl_read(int node, int tbl_id, int tbl_index, void *key, void *mask,
	     void *data);
int invalidate_table_entry(int node, int tbl_id, int tbl_idx);
int alloc_table_index(int node, int table_id, int *index);
void free_table_index(int node, int table_id, int index);

struct pf_vf_data {
	int pf_id;
	int num_vfs;
	int lmac;
	int sys_lmac;
	int bgx_idx;
};

struct pf_vf_map_s {
	bool valid;
	int lmac_cnt;
	struct pf_vf_data pf_vf[TNS_MAX_LMAC];
};

extern struct pf_vf_map_s pf_vf_map_data[MAX_NUMNODES];
int tns_enable_mcast_promis(int node, int vf);
int filter_tbl_lookup(int node, int tblid, void *entry, int *idx);

#define MCAST_PROMIS(a, b, c) ingressgrp_gblvif[(a)][(b)].mcast_promis_grp[(c)]
#define VALID_MCAST_PROMIS(a, b) \
	ingressgrp_gblvif[(a)][(b)].valid_mcast_promis_ports

#endif /*__PF_LOCALS__*/
