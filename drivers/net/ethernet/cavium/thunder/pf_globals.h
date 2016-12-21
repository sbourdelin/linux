/*
 * Copyright (C) 2015 Cavium, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 */

#ifndef NIC_PF_H
#define	NIC_PF_H

#include <linux/netdevice.h>
#include <linux/interrupt.h>
#include <linux/firmware.h>
#include "thunder_bgx.h"
#include "tbl_access.h"

#define TNS_MAX_LMAC	8
#define TNS_MIN_LMAC    0

struct tns_global_st {
	u64 magic;
	char     version[16];
	u64 reg_cnt;
	struct table_static_s tbl_info[TNS_MAX_TABLE];
};

#define PF_COUNT 3
#define PF_1	0
#define PF_2	64
#define PF_3	96
#define PF_END	128

int is_pf(int node_id, int vf);
int get_pf(int node_id, int vf);
void get_vf_group(int node_id, int lmac, int *start_vf, int *end_vf);
int vf_to_pport(int node_id, int vf);
int pf_filter_init(void);
int tns_init(const struct firmware *fw, struct device *dev);
void tns_exit(void);
void pf_notify_msg_handler(int node_id, void *arg);
void nic_init_pf_vf_mapping(void);
int nic_set_pf_vf_mapping(int node_id);
int get_bgx_id(int node_id, int vf_id, int *bgx_id, int *lmac);
int phy_port_to_bgx_lmac(int node, int port, int *bgx, int *lmac);
int tns_filter_valid_entry(int node, int req_type, int vf, int vlan);
void nic_enable_valid_vf(int max_vf_cnt);

union nic_pf_qsx_rqx_bp_cfg {
	u64 u;
	struct nic_pf_qsx_rqx_bp_cfg_s {
		u64 bpid		: 8;
		u64 cq_bp		: 8;
		u64 rbdr_bp		: 8;
		u64 reserved_24_61	: 38;
		u64 cq_bp_ena		: 1;
		u64 rbdr_bp_ena		: 1;
	} s;
};

#define NIC_PF_QSX_RQX_BP_CFG	0x20010500ul
#define RBDR_CQ_BP		129

union nic_pf_intfx_bp_cfg {
	u64 u;
	struct bdk_nic_pf_intfx_bp_cfg_s {
		u64 bp_id		: 4;
		u64 bp_type		: 1;
		u64 reserved_5_62	: 58;
		u64 bp_ena		: 1;
	} s;
};

#define NIC_PF_INTFX_BP_CFG	0x208ull

#define FW_NAME	"tns_firmware.bin"

#endif
