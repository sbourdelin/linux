/*
 * Copyright (C) 2015 Cavium, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/version.h>
#include <linux/proc_fs.h>
#include <linux/device.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/err.h>
#include <linux/device.h>
#include "pf_globals.h"
#include "pf_locals.h"
#include "nic.h"

u32 intr_to_ingressgrp[MAX_NUMNODES][TNS_MAC_FILTER_MAX_SYS_PORTS];
struct vf_register_s vf_reg_data[MAX_NUMNODES][TNS_MAX_VF];
struct ing_grp_gblvif ingressgrp_gblvif[MAX_NUMNODES][TNS_MAX_INGRESS_GROUP];

u32 macfilter_freeindex[MAX_NUMNODES];
u32 vlanfilter_freeindex[MAX_NUMNODES];

int tns_filter_valid_entry(int node, int req_type, int vf, int vlan)
{
	if (req_type == NIC_MBOX_MSG_UC_MC) {
		if (vf_reg_data[node][vf].vf_in_mcast_promis ||
		    (macfilter_freeindex[node] >= TNS_MAC_FILTER_MAX_INDEX))
			return TNS_ERR_MAX_LIMIT;
		if (vf_reg_data[node][vf].filter_count >= TNS_MAX_MAC_PER_VF) {
			tns_enable_mcast_promis(node, vf);
			vf_reg_data[node][vf].vf_in_mcast_promis = 1;
			return TNS_ERR_MAX_LIMIT;
		}
	} else if (req_type == NIC_MBOX_MSG_VLAN ||
		   req_type == NIC_MBOX_MSG_ADMIN_VLAN) {
		if (vf_reg_data[node][vf].vlan_count >= TNS_MAX_VLAN_PER_VF)
			return TNS_ERR_MAX_LIMIT;

		if (vlanfilter_freeindex[node] >= TNS_VLAN_FILTER_MAX_INDEX) {
			int ret;
			struct vlan_filter_entry tbl_entry;
			int vlan_tbl_idx = -1;

			tbl_entry.key.is_valid = 1;
			tbl_entry.key.key_type.key_value  = 0x0ull;
			tbl_entry.mask.key_type.key_value = ~0x0ull;
			tbl_entry.key.key_type.s.ingress_grp =
				intr_to_ingressgrp[node][vf];
			tbl_entry.mask.key_type.s.ingress_grp = 0x0;
			tbl_entry.key.key_type.s.vlan = vlan;
			tbl_entry.mask.key_type.s.vlan = 0x0;

			ret = filter_tbl_lookup(node, VLAN_FILTER_TABLE,
						&tbl_entry, &vlan_tbl_idx);
			if (ret || vlan_tbl_idx == -1)
				return TNS_ERR_MAX_LIMIT;
		}
	} else {
		filter_dbg(FERR, "Invalid Request %d VF %d\n", req_type, vf);
	}

	return TNS_NO_ERR;
}

int dump_port_cfg_etry(struct itt_entry_s *port_cfg_entry)
{
	filter_dbg(FINFO, "PortConfig Entry\n");
	filter_dbg(FINFO, "pkt_dir:			0x%x\n",
		   port_cfg_entry->pkt_dir);
	filter_dbg(FINFO, "is_admin_vlan_enabled:	0x%x\n",
		   port_cfg_entry->is_admin_vlan_enabled);
	filter_dbg(FINFO, "default_evif:		0x%x\n",
		   port_cfg_entry->default_evif);
	filter_dbg(FINFO, "admin_vlan:			0x%x\n",
		   port_cfg_entry->admin_vlan);

	return TNS_NO_ERR;
}

int dump_evif_entry(struct evif_entry *evif_dat)
{
	filter_dbg(FINFO, "EVIF Entry\n");
	filter_dbg(FINFO, "prt_bmap_136_73: 0x%llx\n",
		   evif_dat->prt_bmap_136_73);
	filter_dbg(FINFO, "prt_bmap_72_9:   0x%llx\n",
		   evif_dat->prt_bmap_72_9);
	filter_dbg(FINFO, "prt_bmap_8:      0x%x\n", evif_dat->prt_bmap_8);
	filter_dbg(FINFO, "mre_ptr:         0x%x\n", evif_dat->mre_ptr);
	filter_dbg(FINFO, "insert_ptr2:     0x%x\n", evif_dat->insert_ptr2);
	filter_dbg(FINFO, "insert_ptr1:     0x%x\n", evif_dat->insert_ptr1);
	filter_dbg(FINFO, "insert_ptr0:     0x%x\n", evif_dat->insert_ptr0);
	filter_dbg(FINFO, "data31_0:        0x%x\n", evif_dat->data31_0);
	filter_dbg(FINFO, "rewrite_ptr1:    0x%x\n", evif_dat->rewrite_ptr1);
	filter_dbg(FINFO, "rewrite_ptr0:    0x%x\n", evif_dat->rewrite_ptr0);
	filter_dbg(FINFO, "prt_bmap7_0:     0x%x\n", evif_dat->prt_bmap7_0);
	filter_dbg(FINFO, "q_mirror_en:     0x%x\n", evif_dat->q_mirror_en);
	filter_dbg(FINFO, "mirror_en:       0x%x\n", evif_dat->mirror_en);
	filter_dbg(FINFO, "mtu_prf:         0x%x\n", evif_dat->mtu_prf);
	filter_dbg(FINFO, "truncate:        0x%x\n", evif_dat->truncate);
	filter_dbg(FINFO, "rsp_type:        0x%x\n", evif_dat->rsp_type);

	return TNS_NO_ERR;
}

static inline int validate_port(int port_num)
{
	if (port_num < 0 && port_num >= TNS_MAC_FILTER_MAX_SYS_PORTS) {
		filter_dbg(FERR, "%s Invalid Port: %d (Valid range 0-136)\n",
			   __func__, port_num);
		return TNS_ERR_WRONG_PORT_NUMBER;
	}
	return TNS_NO_ERR;
}

int enable_port(int port_num, struct evif_entry *tbl_entry)
{
	s64 port_base;

	if (validate_port(port_num))
		return TNS_ERR_WRONG_PORT_NUMBER;

	if (port_num < 8) {
		tbl_entry->prt_bmap7_0 = tbl_entry->prt_bmap7_0 |
					 (0x1 << port_num);
	} else if (port_num == 8) {
		tbl_entry->prt_bmap_8 = 1;
	} else if (port_num <= 72) {
		port_base = port_num - 9;
		tbl_entry->prt_bmap_72_9 = tbl_entry->prt_bmap_72_9 |
						(0x1ull << port_base);
	} else if (port_num <= TNS_MAC_FILTER_MAX_SYS_PORTS) {
		port_base = port_num - 73;
		tbl_entry->prt_bmap_136_73 = tbl_entry->prt_bmap_136_73 |
						(0x1ull << port_base);
	}

	return TNS_NO_ERR;
}

int disable_port(int port_num, struct evif_entry *tbl_entry)
{
	s64 port_base;

	if (validate_port(port_num))
		return TNS_ERR_WRONG_PORT_NUMBER;

	if (port_num < 8) {
		tbl_entry->prt_bmap7_0 = tbl_entry->prt_bmap7_0 &
					 ~(0x1 << port_num);
	} else if (port_num == 8) {
		tbl_entry->prt_bmap_8 = 0;
	} else if (port_num <= 72) {
		port_base = port_num - 9;
		tbl_entry->prt_bmap_72_9 = tbl_entry->prt_bmap_72_9 &
						~(0x1ull << port_base);
	} else if (port_num <= TNS_MAC_FILTER_MAX_SYS_PORTS) {
		port_base = port_num - 73;
		tbl_entry->prt_bmap_136_73 = tbl_entry->prt_bmap_136_73 &
						~(0x1ull << port_base);
	}

	return TNS_NO_ERR;
}

int disable_all_ports(struct evif_entry *tbl_entry)
{
	tbl_entry->prt_bmap_136_73 = 0x0ull;
	tbl_entry->prt_bmap_72_9 = 0x0ull;
	tbl_entry->prt_bmap_8 = 0x0;
	tbl_entry->prt_bmap7_0 = 0x0;

	return TNS_NO_ERR;
}

int is_vlan_port_enabled(int vf, vlan_port_bitmap_t vlan_vif)
{
	int port_base = (vf / 8), port_offset = (vf % 8);

	if (validate_port(vf))
		return TNS_ERR_WRONG_PORT_NUMBER;

	if (vlan_vif[port_base] & (1 << port_offset))
		return 1;

	return 0;
}

int enable_vlan_port(int port_num, vlan_port_bitmap_t vlan_vif)
{
	int port_base = (port_num / 8), port_offset = (port_num % 8);

	if (validate_port(port_num))
		return TNS_ERR_WRONG_PORT_NUMBER;

	vlan_vif[port_base] = vlan_vif[port_base] | (1 << port_offset);

	return TNS_NO_ERR;
}

int disable_vlan_port(int port_num, vlan_port_bitmap_t vlan_vif)
{
	int port_base = (port_num / 8), port_offset = (port_num % 8);

	if (validate_port(port_num))
		return TNS_ERR_WRONG_PORT_NUMBER;

	vlan_vif[port_base] = vlan_vif[port_base] & ~(1 << port_offset);

	return TNS_NO_ERR;
}

int disable_vlan_vif_ports(vlan_port_bitmap_t vlan_vif)
{
	memset((void *)(&vlan_vif[0]), 0x0, sizeof(vlan_port_bitmap_t));

	return TNS_NO_ERR;
}

int dump_vlan_vif_portss(vlan_port_bitmap_t vlan_vif)
{
	int i;

	filter_dbg(FINFO, "Port Bitmap (0...135) 0x ");
	for (i = 0; i < (TNS_MAC_FILTER_MAX_SYS_PORTS / 8); i++)
		filter_dbg(FINFO, "%x ", vlan_vif[i]);
	filter_dbg(FINFO, "\n");

	return TNS_NO_ERR;
}

static inline int getingress_grp(int node, int vf)
{
	int i;

	for (i = 0; i < TNS_MAX_INGRESS_GROUP; i++) {
		if (ingressgrp_gblvif[node][i].is_valid &&
		    (ingressgrp_gblvif[node][i].ingress_grp ==
		     intr_to_ingressgrp[node][vf]))
			return i;
	}
	return -1;
}

inline int vf_bcast_vif(int node, int vf, int *bcast_vif)
{
	int ing_grp = getingress_grp(node, vf);

	if (ing_grp == -1)
		return TNS_ERR_ENTRY_NOT_FOUND;

	*bcast_vif = ingressgrp_gblvif[node][ing_grp].bcast_vif;

	return TNS_NO_ERR;
}

inline int vf_mcast_vif(int node, int vf, int *mcast_vif)
{
	int ing_grp = getingress_grp(node, vf);

	if (ing_grp == -1)
		return TNS_ERR_ENTRY_NOT_FOUND;

	*mcast_vif = ingressgrp_gblvif[node][ing_grp].mcast_vif;

	return TNS_NO_ERR;
}

inline int vf_pfvf_id(int node, int vf, int *pfvf)
{
	int ing_grp = getingress_grp(node, vf);

	if (ing_grp == -1)
		return TNS_ERR_ENTRY_NOT_FOUND;

	*pfvf = ingressgrp_gblvif[node][ing_grp].pf_vf;

	return TNS_NO_ERR;
}

bool is_vf_registered_entry(int node, int vf, int index)
{
	int i;

	for (i = 0; i < vf_reg_data[node][vf].filter_count; i++) {
		if (vf_reg_data[node][vf].filter_index[i] == index)
			return true;
	}

	return false;
}

bool is_vlan_registered(int node, int vf, int vlan)
{
	int i;

	for (i = 0; i < vf_reg_data[node][vf].vlan_count; i++) {
		if (vf_reg_data[node][vf].vlan[i] == vlan)
			return true;
	}

	return false;
}

int is_empty_vif(int node, int vf, struct evif_entry *evif_dat)
{
	int i;

	for (i = 0; i < TNS_MAX_VF; i++)
		if (intr_to_ingressgrp[node][vf] ==
		    intr_to_ingressgrp[node][i] &&
		    (vf_reg_data[node][i].vf_in_promis ||
		     vf_reg_data[node][i].vf_in_mcast_promis))
			disable_port(i, evif_dat);
	disable_port(intr_to_ingressgrp[node][vf], evif_dat);

	if (evif_dat->prt_bmap7_0 || evif_dat->prt_bmap_8 ||
	    evif_dat->prt_bmap_72_9 || evif_dat->prt_bmap_136_73)
		return 0;

	return 1;
}

int is_empty_vlan(int node, int vf, int vlan, vlan_port_bitmap_t vlan_vif)
{
	int i, pf_vf;
	int ret;

	ret = vf_pfvf_id(node, vf, &pf_vf);
	if (ret)
		return ret;

	if (vf_reg_data[node][pf_vf].vf_in_promis &&
	    !is_vlan_registered(node, pf_vf, vlan))
		disable_vlan_port(pf_vf, vlan_vif);

	disable_vlan_port(intr_to_ingressgrp[node][vf], vlan_vif);
	for (i = 0; i < sizeof(vlan_port_bitmap_t); i++)
		if (vlan_vif[i])
			break;

	if (i == sizeof(vlan_port_bitmap_t))
		return 1;

	return 0;
}

int filter_tbl_lookup(int node, int table_id, void *entry, int *index)
{
	switch (table_id) {
	case MAC_FILTER_TABLE:
	{
		struct mac_filter_entry tbl_entry;
		struct mac_filter_entry *inp = (struct mac_filter_entry *)entry;
		int i;
		int ret;

		for (i = 0; i < TNS_MAC_FILTER_MAX_INDEX; i++) {
			ret = tbl_read(node, MAC_FILTER_TABLE, i,
				       &tbl_entry.key, &tbl_entry.mask,
				       &tbl_entry.data);

			if (ret && (ret != TNS_ERR_MAC_FILTER_INVALID_ENTRY))
				return ret;
			else if (ret == TNS_ERR_MAC_FILTER_INVALID_ENTRY)
				continue;

			if ((tbl_entry.key.key_type.key_value ==
			     inp->key.key_type.key_value) &&
			    (tbl_entry.mask.key_type.key_value ==
			     inp->mask.key_type.key_value)) {
				//Found an Entry
				*index = i;
				inp->data.data = tbl_entry.data.data;
				return TNS_NO_ERR;
			}
			//Unable to find entry
			*index = -1;
		}
		break;
	}
	case VLAN_FILTER_TABLE:
	{
		struct vlan_filter_entry tbl_entry;
		struct vlan_filter_entry *inp_entry;
		int i;
		int ret;

		inp_entry = (struct vlan_filter_entry *)entry;
		for (i = 1; i < TNS_VLAN_FILTER_MAX_INDEX; i++) {
			ret = tbl_read(node, VLAN_FILTER_TABLE, i,
				       &tbl_entry.key, &tbl_entry.mask,
					&tbl_entry.data);
			if (ret && (ret != TNS_ERR_MAC_FILTER_INVALID_ENTRY))
				return ret;
			else if (ret == TNS_ERR_MAC_FILTER_INVALID_ENTRY)
				continue;

			if ((tbl_entry.key.key_type.key_value ==
			     inp_entry->key.key_type.key_value) &&
			    (tbl_entry.mask.key_type.key_value ==
			     inp_entry->mask.key_type.key_value)) {
				//Found an Entry
				*index = i;
				inp_entry->data.data = tbl_entry.data.data;
				return TNS_NO_ERR;
			}
		}
		//Unable to find entry
		*index = -1;
		break;
	}
	default:
		filter_dbg(FERR, "Wrong Table ID: %d\n", table_id);
		return TNS_ERR_INVALID_TBL_ID;
	}

	return TNS_NO_ERR;
}

int tns_enable_mcast_promis(int node, int vf)
{
	int mcast_vif;
	int j;
	int ret;
	struct evif_entry evif_dat;
	int ing_grp = getingress_grp(node, vf);
	int pports;

	if (ing_grp == -1)
		return TNS_ERROR_INVALID_ARG;

	ret = vf_mcast_vif(node, vf, &mcast_vif);
	if (ret) {
		filter_dbg(FERR, "Error: Unable to get multicast VIF\n");
		return ret;
	}

	ret = tbl_read(node, MAC_EVIF_TABLE, mcast_vif, NULL, NULL, &evif_dat);
	if (ret)
		return ret;

	enable_port(vf, &evif_dat);
	dump_evif_entry(&evif_dat);
	ret = tbl_write(node, MAC_EVIF_TABLE, mcast_vif, NULL, NULL,
			(void *)&evif_dat);
	if (ret)
		return ret;

	pports = ingressgrp_gblvif[node][ing_grp].valid_mcast_promis_ports;
	//Enable VF in multicast MAC promiscuous group
	for (j = 0; j < pports; j++) {
		if (MCAST_PROMIS(node, ing_grp, j) == vf) {
			filter_dbg(FDEBUG, "VF found in MCAST promis group\n");
			return TNS_NO_ERR;
		}
	}
	MCAST_PROMIS(node, ing_grp, pports) = vf;
	ingressgrp_gblvif[node][ing_grp].valid_mcast_promis_ports += 1;
	filter_dbg(FINFO, "VF %d permanently entered into MCAST promisc mode\n",
		   vf);

	return TNS_NO_ERR;
}

int remove_vf_from_regi_mcast_vif(int node, int vf)
{
	int ret;
	int mcast_vif;
	struct evif_entry evif_dat;

	ret = vf_mcast_vif(node, vf, &mcast_vif);
	if (ret) {
		filter_dbg(FERR, "Error: Unable to get multicast VIF\n");
		return ret;
	}

	ret = tbl_read(node, MAC_EVIF_TABLE, mcast_vif, NULL, NULL, &evif_dat);
	if (ret)
		return ret;
	disable_port(vf, &evif_dat);
	dump_evif_entry(&evif_dat);
	ret = tbl_write(node, MAC_EVIF_TABLE, mcast_vif, NULL, NULL,
			(void *)&evif_dat);
	if (ret)
		return ret;

	return TNS_NO_ERR;
}

int remove_vf_from_mcast_promis_grp(int node, int vf)
{
	int j, k;
	int ing_grp = getingress_grp(node, vf);
	int pports;

	if (ing_grp == -1)
		return TNS_ERROR_INVALID_ARG;

	pports = ingressgrp_gblvif[node][ing_grp].valid_mcast_promis_ports;
	for (j = 0; j < pports; j++) {
		if (MCAST_PROMIS(node, ing_grp, j) != vf)
			continue;

		filter_dbg(FDEBUG, "VF found in MCAST promis group %d\n",
			   intr_to_ingressgrp[node][vf]);
		for (k = j; k < (pports - 1); k++)
			MCAST_PROMIS(node, ing_grp, k) =
			 MCAST_PROMIS(node, ing_grp, (k + 1));
		VALID_MCAST_PROMIS(node, ing_grp) -= 1;
		remove_vf_from_regi_mcast_vif(node, vf);
		return TNS_NO_ERR;
	}
	filter_dbg(FDEBUG, "VF %d not found in multicast promiscuous group\n",
		   vf);

	return TNS_ERR_ENTRY_NOT_FOUND;
}

int registered_vf_filter_index(int node, int vf, int mac_idx, int action)
{
	int f_count = vf_reg_data[node][vf].filter_count, j;

	if (!action) {
		for (j = 0; j < f_count; j++) {
			if (vf_reg_data[node][vf].filter_index[j] == mac_idx) {
				int i, k = j + 1;

				for (i = j; i < f_count - 1; i++, k++)
					vf_reg_data[node][vf].filter_index[i] =
					 vf_reg_data[node][vf].filter_index[k];
				break;
			}
		}
		if (j == vf_reg_data[node][vf].filter_count)
			filter_dbg(FDEBUG, "VF not in registered filtr list\n");
		else
			vf_reg_data[node][vf].filter_count -= 1;
	} else {
		vf_reg_data[node][vf].filter_index[f_count] = mac_idx;
		vf_reg_data[node][vf].filter_count += 1;
		filter_dbg(FINFO, "%s Added at Filter count %d Index %d\n",
			   __func__, vf_reg_data[node][vf].filter_count,
			   mac_idx);
	}

	/* We are restricting each VF to register atmost 11 filter entries
	 * (including unicast & multicast)
	 */
	if (vf_reg_data[node][vf].filter_count <= TNS_MAX_MAC_PER_VF) {
		vf_reg_data[node][vf].vf_in_mcast_promis = 0;
		if (!vf_reg_data[node][vf].vf_in_promis)
			remove_vf_from_mcast_promis_grp(node, vf);
		filter_dbg(FINFO, "VF %d removed from MCAST promis mode\n", vf);
	}

	return TNS_NO_ERR;
}

int add_mac_filter_mcast_entry(int node, int table_id, int vf, int mac_idx,
			       void *mac_DA)
{
	int ret;
	struct mac_filter_entry tbl_entry;
	struct mac_filter_keymask_s key, mask;
	union mac_filter_data_s data;
	int vif = -1, k, j;
	struct evif_entry evif_dat;
	int ing_grp = getingress_grp(node, vf);

	if (ing_grp == -1)
		return TNS_ERROR_INVALID_ARG;

	if (vf_reg_data[node][vf].filter_count >= TNS_MAX_MAC_PER_VF) {
		if (!vf_reg_data[node][vf].vf_in_mcast_promis) {
			tns_enable_mcast_promis(node, vf);
			vf_reg_data[node][vf].vf_in_mcast_promis = 1;
		}
		return TNS_ERR_MAX_LIMIT;
	}

	tbl_entry.key.is_valid = 1;
	tbl_entry.key.key_type.s.ingress_grp = intr_to_ingressgrp[node][vf];
	tbl_entry.mask.key_type.s.ingress_grp = 0x0;
	for (j = 5, k = 0; j >= 0; j--, k++) {
		tbl_entry.key.key_type.s.mac_DA[k] = ((u8 *)mac_DA)[j];
		tbl_entry.mask.key_type.s.mac_DA[k] = 0x0;
	}
	ret = filter_tbl_lookup(node, MAC_FILTER_TABLE, &tbl_entry, &mac_idx);
	if (ret)
		return ret;
	if (mac_idx != -1 &&
	    !(mac_idx >= (TNS_MAC_FILTER_MAX_INDEX - TNS_MAX_INGRESS_GROUP) &&
	      mac_idx < TNS_MAC_FILTER_MAX_INDEX)) {
		int evif = tbl_entry.data.s.evif;

		filter_dbg(FINFO, "Multicast MAC found at %d evif: %d\n",
			   mac_idx, evif);
		ret = tbl_read(node, MAC_EVIF_TABLE, evif, NULL, NULL,
			       &evif_dat);
		if (ret)
			return ret;
		if (is_vf_registered_entry(node, vf, mac_idx)) {
			//No Need to register again
			return TNS_NO_ERR;
		}
		enable_port(vf, &evif_dat);
		ret = tbl_write(node, MAC_EVIF_TABLE, evif, NULL, NULL,
				(void *)&evif_dat);
		if (ret)
			return ret;
		registered_vf_filter_index(node, vf, mac_idx, 1);
		dump_evif_entry(&evif_dat);
		return TNS_NO_ERR;
	}

	//New multicast MAC registration
	if (alloc_table_index(node, MAC_FILTER_TABLE, &mac_idx)) {
		filter_dbg(FERR, "%s Filter Table Full\n", __func__);
		return TNS_ERR_MAX_LIMIT;
	}
	key.is_valid = 1;
	mask.is_valid = 1;
	key.key_type.s.ingress_grp = intr_to_ingressgrp[node][vf];
	mask.key_type.s.ingress_grp = 0;
	for (j = 5, k = 0; j >= 0; j--, k++) {
		key.key_type.s.mac_DA[k] = ((u8 *)mac_DA)[j];
		mask.key_type.s.mac_DA[k] = 0x0;
	}
	if (alloc_table_index(node, MAC_EVIF_TABLE, &vif)) {
		filter_dbg(FERR, "%s EVIF Table Full\n", __func__);
		return TNS_ERR_MAX_LIMIT;
	}
	evif_dat.insert_ptr0 = 0xFFFF;
	evif_dat.insert_ptr1 = 0xFFFF;
	evif_dat.insert_ptr2 = 0xFFFF;
	evif_dat.mre_ptr = 0x7FFF;
	evif_dat.rewrite_ptr0 = 0xFF;
	evif_dat.rewrite_ptr1 = 0xFF;
	evif_dat.data31_0 = 0x0;
	evif_dat.q_mirror_en = 0x0;
	evif_dat.mirror_en = 0x0;
	evif_dat.mtu_prf = 0x0;
	evif_dat.truncate = 0x0;
	evif_dat.rsp_type = 0x3;
	disable_all_ports(&evif_dat);
	for (j = 0; j < VALID_MCAST_PROMIS(node, ing_grp); j++)
		enable_port(MCAST_PROMIS(node, ing_grp, j), &evif_dat);
	enable_port(vf, &evif_dat);
	ret = tbl_write(node, MAC_EVIF_TABLE, vif, NULL, NULL, &evif_dat);
	if (ret)
		return ret;
	data.data = 0x0ull;
	data.s.evif = vif;
	ret = tbl_write(node, MAC_FILTER_TABLE, mac_idx, &key, &mask, &data);
	if (ret)
		return ret;
	macfilter_freeindex[node] += 1;
	registered_vf_filter_index(node, vf, mac_idx, 1);

	return TNS_NO_ERR;
}

int del_mac_filter_entry(int node, int table_id, int vf, int mac_idx,
			 void *mac_DA, int addr_type)
{
	int ret;
	struct mac_filter_entry tbl_entry;
	int old_mac_idx = -1, vif;
	int j, k;

	tbl_entry.key.is_valid = 1;
	tbl_entry.key.key_type.s.ingress_grp = intr_to_ingressgrp[node][vf];
	tbl_entry.mask.key_type.s.ingress_grp = 0x0;

	for (j = 5, k = 0; j >= 0; j--, k++) {
		tbl_entry.key.key_type.s.mac_DA[k] = ((u8 *)mac_DA)[j];
		tbl_entry.mask.key_type.s.mac_DA[k] = 0x0;
	}

	ret = filter_tbl_lookup(node, MAC_FILTER_TABLE, (void *)&tbl_entry,
				&old_mac_idx);
	if (ret)
		return ret;

	if (old_mac_idx == -1) {
		filter_dbg(FDEBUG, "Invalid Delete, entry not found\n");
		return TNS_ERR_ENTRY_NOT_FOUND;
	}
	if (mac_idx != -1 && mac_idx != old_mac_idx) {
		filter_dbg(FDEBUG, "Found and requested are mismatched\n");
		return TNS_ERR_ENTRY_NOT_FOUND;
	}
	if (old_mac_idx == vf) {
		filter_dbg(FDEBUG, "Primary Unicast MAC delete not allowed\n");
		return TNS_ERR_MAC_FILTER_INVALID_ENTRY;
	}

	//Remove MAC Filter entry from VF register MAC filter list
	registered_vf_filter_index(node, vf, old_mac_idx, 0);

	//Remove VIF entry (output portmask) related to this filter entry
	vif = tbl_entry.data.s.evif;
	if (addr_type) {
		struct evif_entry evif_dat;

		ret = tbl_read(node, MAC_EVIF_TABLE, vif, NULL, NULL,
			       &evif_dat);
		if (ret)
			return ret;

		disable_port(vf, &evif_dat);
		ret = tbl_write(node, MAC_EVIF_TABLE, vif, NULL, NULL,
				&evif_dat);
		if (ret)
			return ret;

		dump_evif_entry(&evif_dat);
		//In case of multicast MAC check for empty portmask
		if (!is_empty_vif(node, vf, &evif_dat))
			return TNS_NO_ERR;
	}
	invalidate_table_entry(node, MAC_FILTER_TABLE, old_mac_idx);
	free_table_index(node, MAC_FILTER_TABLE, old_mac_idx);
	free_table_index(node, MAC_EVIF_TABLE, vif);
	macfilter_freeindex[node] -= 1;

	return TNS_NO_ERR;
}

int add_mac_filter_entry(int node, int table_id, int vf, int mac_idx,
			 void *mac_DA)
{
	int ret;
	struct mac_filter_entry tbl_entry;
	int old_mac_idx = -1;
	int j, k;
	struct mac_filter_keymask_s key, mask;
	union mac_filter_data_s data;

	/* We are restricting each VF to register atmost 11 filter entries
	 * (including unicast & multicast)
	 */
	if (mac_idx != vf &&
	    vf_reg_data[node][vf].filter_count >= TNS_MAX_MAC_PER_VF) {
		if (!vf_reg_data[node][vf].vf_in_mcast_promis) {
			tns_enable_mcast_promis(node, vf);
			vf_reg_data[node][vf].vf_in_mcast_promis = 1;
		}
		return TNS_ERR_MAX_LIMIT;
	}

	//Adding Multicast MAC will be handled differently
	if ((((u8 *)mac_DA)[0]) & 0x1) {
		filter_dbg(FDEBUG, "%s It is multicast MAC entry\n", __func__);
		return add_mac_filter_mcast_entry(node, table_id, vf, mac_idx,
						  mac_DA);
	}

	tbl_entry.key.is_valid = 1;
	tbl_entry.key.key_type.s.ingress_grp = intr_to_ingressgrp[node][vf];
	tbl_entry.mask.key_type.s.ingress_grp = 0x0;
	for (j = 5, k = 0; j >= 0; j--, k++) {
		tbl_entry.key.key_type.s.mac_DA[k] = ((u8 *)mac_DA)[j];
		tbl_entry.mask.key_type.s.mac_DA[k] = 0x0;
	}
	ret = filter_tbl_lookup(node, MAC_FILTER_TABLE, (void *)&tbl_entry,
				&old_mac_idx);
	if (ret)
		return ret;
	if (old_mac_idx != -1) {
		filter_dbg(FINFO, "Duplicate entry found at %d\n", old_mac_idx);
		if (tbl_entry.data.s.evif != vf) {
			filter_dbg(FDEBUG, "Registered VF %d Requested VF %d\n",
				   (int)tbl_entry.data.s.evif, (int)vf);
			return TNS_ERR_DUPLICATE_MAC;
		}
		return TNS_NO_ERR;
	}
	if (alloc_table_index(node, MAC_FILTER_TABLE, &mac_idx)) {
		filter_dbg(FERR, "(%s) Filter Table Full\n", __func__);
		return TNS_ERR_MAX_LIMIT;
	}
	if (mac_idx == -1) {
		filter_dbg(FERR, "!!!ERROR!!! reached maximum limit\n");
		return TNS_ERR_MAX_LIMIT;
	}
	key.is_valid = 1;
	mask.is_valid = 1;
	key.key_type.s.ingress_grp = intr_to_ingressgrp[node][vf];
	mask.key_type.s.ingress_grp = 0;
	for (j = 5, k = 0; j >= 0; j--, k++) {
		key.key_type.s.mac_DA[k] = ((u8 *)mac_DA)[j];
		mask.key_type.s.mac_DA[k] = 0x0;
	}
	filter_dbg(FINFO, "VF id: %d with ingress_grp: %d ", vf,
		   key.key_type.s.ingress_grp);
	filter_dbg(FINFO, "MAC: %x: %x: %x %x: %x %x Added at Index: %d\n",
		   ((u8 *)mac_DA)[0], ((u8 *)mac_DA)[1],
		   ((u8 *)mac_DA)[2], ((u8 *)mac_DA)[3],
		   ((u8 *)mac_DA)[4], ((u8 *)mac_DA)[5], mac_idx);

	data.data = 0x0ull;
	data.s.evif = vf;
	ret = tbl_write(node, MAC_FILTER_TABLE, mac_idx, &key, &mask, &data);
	if (ret)
		return ret;

	if (mac_idx != vf) {
		registered_vf_filter_index(node, vf, mac_idx, 1);
		macfilter_freeindex[node] += 1;
	}

	return TNS_NO_ERR;
}

int vf_interface_up(int node, int tbl_id, int vf, void *mac_DA)
{
	int ret;

	//Enable unicast MAC entry for this VF
	ret = add_mac_filter_entry(node, tbl_id, vf, vf, mac_DA);
	if (ret)
		return ret;

	return TNS_NO_ERR;
}

int del_vlan_entry(int node, int vf, int vlan, int vlanx)
{
	int ret;
	struct vlan_filter_entry tbl_entry;
	int vlan_tbl_idx = -1, i;
	vlan_port_bitmap_t vlan_vif;
	int vlan_cnt = vf_reg_data[node][vf].vlan_count;

	tbl_entry.key.is_valid = 1;
	tbl_entry.key.key_type.key_value  = 0x0ull;
	tbl_entry.mask.key_type.key_value = 0xFFFFFFFFFFFFFFFFull;
	tbl_entry.key.key_type.s.ingress_grp = intr_to_ingressgrp[node][vf];
	tbl_entry.mask.key_type.s.ingress_grp = 0x0;
	tbl_entry.key.key_type.s.vlan = vlan;
	tbl_entry.mask.key_type.s.vlan = 0x0;

	filter_dbg(FINFO, "%s VF %d with ingress_grp %d VLANID %d\n",
		   __func__, vf, tbl_entry.key.key_type.s.ingress_grp,
		   tbl_entry.key.key_type.s.vlan);

	ret = filter_tbl_lookup(node, VLAN_FILTER_TABLE, &tbl_entry,
				&vlan_tbl_idx);
	if (ret)
		return ret;

	if (vlan_tbl_idx == -1) {
		filter_dbg(FINFO, "VF %d VLAN %d filter not registered\n",
			   vf, vlan);
		return TNS_NO_ERR;
	}

	if (vlan_tbl_idx < 1 && vlan_tbl_idx >= TNS_VLAN_FILTER_MAX_INDEX) {
		filter_dbg(FERR, "Invalid VLAN Idx: %d\n", vlan_tbl_idx);
		return TNS_ERR_VLAN_FILTER_INVLAID_ENTRY;
	}

	vlanx = tbl_entry.data.s.filter_idx;
	ret = tbl_read(node, VLAN_EVIF_TABLE, vlanx, NULL, NULL,
		       (void *)(&vlan_vif[0]));
	if (ret)
		return ret;

	disable_vlan_port(vf, vlan_vif);
	ret = tbl_write(node, VLAN_EVIF_TABLE, vlanx, NULL, NULL,
			(void *)(&vlan_vif[0]));
	if (ret)
		return ret;

	for (i = 0; i < vlan_cnt; i++) {
		if (vf_reg_data[node][vf].vlan[i] == vlan) {
			int j;

			for (j = i; j < vlan_cnt - 1; j++)
				vf_reg_data[node][vf].vlan[j] =
				 vf_reg_data[node][vf].vlan[j + 1];
			vf_reg_data[node][vf].vlan_count -= 1;
			break;
		}
	}
	if (is_empty_vlan(node, vf, vlan, vlan_vif)) {
		free_table_index(node, VLAN_FILTER_TABLE, vlanx);
		vlanfilter_freeindex[node] -= 1;
		invalidate_table_entry(node, VLAN_FILTER_TABLE, vlanx);
	}

	return TNS_NO_ERR;
}

int add_vlan_entry(int node, int vf, int vlan, int vlanx)
{
	int ret;
	int pf_vf;
	struct vlan_filter_entry tbl_entry;
	int vlan_tbl_idx = -1;
	vlan_port_bitmap_t vlan_vif;

	if (vf_reg_data[node][vf].vlan_count >= TNS_MAX_VLAN_PER_VF) {
		filter_dbg(FDEBUG, "Reached maximum limit per VF count: %d\n",
			   vf_reg_data[node][vf].vlan_count);
		return TNS_ERR_MAX_LIMIT;
	}

	tbl_entry.key.is_valid = 1;
	tbl_entry.key.key_type.key_value  = 0x0ull;
	tbl_entry.mask.key_type.key_value = 0xFFFFFFFFFFFFFFFFull;
	tbl_entry.key.key_type.s.ingress_grp = intr_to_ingressgrp[node][vf];
	tbl_entry.mask.key_type.s.ingress_grp = 0x0;
	tbl_entry.key.key_type.s.vlan = vlan;
	tbl_entry.mask.key_type.s.vlan = 0x0;

	ret = filter_tbl_lookup(node, VLAN_FILTER_TABLE, &tbl_entry,
				&vlan_tbl_idx);
	if (ret)
		return ret;

	if (vlan_tbl_idx != -1) {
		filter_dbg(FINFO, "Duplicate entry found at %d\n",
			   vlan_tbl_idx);
		if (vlan_tbl_idx < 1 &&
		    vlan_tbl_idx >= TNS_VLAN_FILTER_MAX_INDEX) {
			filter_dbg(FDEBUG, "Invalid VLAN Idx %d\n",
				   vlan_tbl_idx);
			return TNS_ERR_VLAN_FILTER_INVLAID_ENTRY;
		}

		vlanx = tbl_entry.data.s.filter_idx;
		ret = tbl_read(node, VLAN_EVIF_TABLE, vlanx, NULL, NULL,
			       (void *)(&vlan_vif[0]));
		if (ret)
			return ret;

		enable_vlan_port(vf, vlan_vif);
		ret = tbl_write(node, VLAN_EVIF_TABLE, vlanx, NULL, NULL,
				(void *)(&vlan_vif[0]));
		if (ret)
			return ret;

		vf_reg_data[node][vf].vlan[vf_reg_data[node][vf].vlan_count] =
		 vlan;
		vf_reg_data[node][vf].vlan_count += 1;

		return TNS_NO_ERR;
	}

	if (alloc_table_index(node, VLAN_FILTER_TABLE, &vlanx)) {
		filter_dbg(FDEBUG, "%s VLAN Filter Table Full\n", __func__);
		return TNS_ERR_MAX_LIMIT;
	}
	disable_vlan_vif_ports(vlan_vif);
	enable_vlan_port(vf, vlan_vif);
	enable_vlan_port(intr_to_ingressgrp[node][vf], vlan_vif);
	ret = vf_pfvf_id(node, vf, &pf_vf);

	if (ret)
		return ret;

	if (vf_reg_data[node][pf_vf].vf_in_promis)
		enable_vlan_port(pf_vf, vlan_vif);

	dump_vlan_vif_portss(vlan_vif);
	ret = tbl_write(node, VLAN_EVIF_TABLE, vlanx, NULL, NULL,
			(void *)(&vlan_vif[0]));
	if (ret)
		return ret;

	tbl_entry.key.is_valid = 1;
	tbl_entry.key.key_type.s.ingress_grp = intr_to_ingressgrp[node][vf];
	tbl_entry.key.key_type.s.vlan = vlan;
	tbl_entry.key.key_type.s.reserved = 0x0;
	tbl_entry.key.key_type.s.reserved1 = 0x0;
	tbl_entry.mask.is_valid = 1;
	tbl_entry.mask.key_type.s.ingress_grp = 0x0;
	tbl_entry.mask.key_type.s.vlan = 0x0;
	tbl_entry.mask.key_type.s.reserved = 0xF;
	tbl_entry.mask.key_type.s.reserved1 = 0xFFFFFFFF;
	tbl_entry.data.data = 0x0ull;
	tbl_entry.data.s.filter_idx = vlanx;
	ret = tbl_write(node, VLAN_FILTER_TABLE, vlanx, &tbl_entry.key,
			&tbl_entry.mask, &tbl_entry.data);
	if (ret)
		return ret;

	filter_dbg(FINFO, "VF %d with ingress_grp %d VLAN %d Added at %d\n",
		   vf, tbl_entry.key.key_type.s.ingress_grp,
		   tbl_entry.key.key_type.s.vlan, vlanx);

	vlanfilter_freeindex[node] += 1;
	vf_reg_data[node][vf].vlan[vf_reg_data[node][vf].vlan_count] = vlan;
	vf_reg_data[node][vf].vlan_count += 1;

	return TNS_NO_ERR;
}

int enable_promiscuous_mode(int node, int vf)
{
	int ret = tns_enable_mcast_promis(node, vf);
	int pf_vf;

	if (ret)
		return ret;

	vf_reg_data[node][vf].vf_in_promis = 1;
	ret = vf_pfvf_id(node, vf, &pf_vf);
	if (ret)
		return ret;

	if (vf == pf_vf) {
		//PFVF interface, enable full promiscuous mode
		int i;
		int vif = intr_to_ingressgrp[node][vf];
		struct evif_entry evif_dat;
		struct itt_entry_s port_cfg_entry;

		for (i = 0; i < macfilter_freeindex[node]; i++) {
			struct mac_filter_entry tbl_entry;

			ret = tbl_read(node, MAC_FILTER_TABLE, i,
				       &tbl_entry.key, &tbl_entry.mask,
				       &tbl_entry.data);
			if (ret && (ret != TNS_ERR_MAC_FILTER_INVALID_ENTRY))
				return ret;
			else if (ret == TNS_ERR_MAC_FILTER_INVALID_ENTRY)
				continue;

			if (tbl_entry.key.key_type.s.ingress_grp ==
			    intr_to_ingressgrp[node][vf]) {
				int vif = tbl_entry.data.s.evif;
				struct evif_entry evif_dat;

				ret = tbl_read(node, MAC_EVIF_TABLE, vif, NULL,
					       NULL, &evif_dat);
				if (ret)
					return ret;

				enable_port(vf, &evif_dat);
				dump_evif_entry(&evif_dat);
				ret = tbl_write(node, MAC_EVIF_TABLE, vif, NULL,
						NULL, (void *)&evif_dat);
				if (ret)
					return ret;
			}
		}
		/*If pfVf interface enters in promiscuous mode we will forward
		 * packets destined to corresponding LMAC
		 */

		ret = tbl_read(node, MAC_EVIF_TABLE, vif, NULL, NULL,
			       &evif_dat);
		if (ret)
			return ret;
		enable_port(vf, &evif_dat);
		dump_evif_entry(&evif_dat);
		ret = tbl_write(node, MAC_EVIF_TABLE, vif, NULL, NULL,
				(void *)&evif_dat);
		if (ret)
			return ret;

		/* Update default_evif of LMAC from NULLVif to pfVf interface,
		 * so that pfVf will shows all dropped packets as well
		 */
		ret = tbl_read(node, PORT_CONFIG_TABLE,
			       intr_to_ingressgrp[node][vf], NULL, NULL,
			       &port_cfg_entry);
		if (ret)
			return ret;

		port_cfg_entry.default_evif = vf;
		ret = tbl_write(node, PORT_CONFIG_TABLE,
				intr_to_ingressgrp[node][vf], NULL, NULL,
				(void *)&port_cfg_entry);
		if (ret)
			return ret;

		filter_dbg(FINFO, "%s Port %d pkt_dir %d defaultVif %d",
			   __func__, vf, port_cfg_entry.pkt_dir,
			   port_cfg_entry.default_evif);
		filter_dbg(FINFO, " adminVlan %d %s\n",
			   port_cfg_entry.admin_vlan,
			   port_cfg_entry.is_admin_vlan_enabled ? "Enable" :
				"Disable");

		for (i = 1; i < vlanfilter_freeindex[node]; i++) {
			struct vlan_filter_entry tbl_entry;

			ret = tbl_read(node, VLAN_FILTER_TABLE, i,
				       &tbl_entry.key, &tbl_entry.mask,
				       &tbl_entry.data);
			if (ret && (ret != TNS_ERR_MAC_FILTER_INVALID_ENTRY))
				return ret;
			else if (ret == TNS_ERR_MAC_FILTER_INVALID_ENTRY)
				continue;

			if (tbl_entry.key.key_type.s.ingress_grp ==
			    intr_to_ingressgrp[node][vf]) {
				int vlanx = tbl_entry.data.s.filter_idx;
				vlan_port_bitmap_t vlan_vif;

				ret = tbl_read(node, VLAN_EVIF_TABLE, vlanx,
					       NULL, NULL,
					       (void *)(&vlan_vif[0]));
				if (ret)
					return ret;
				enable_vlan_port(vf, vlan_vif);
				ret = tbl_write(node, VLAN_EVIF_TABLE, vlanx,
						NULL, NULL,
						(void *)(&vlan_vif[0]));
				if (ret)
					return ret;
			}
		}
	} else {
		//VF interface enable multicast promiscuous mode
		int i;
		int ret;

		for (i = TNS_MAX_VF; i < macfilter_freeindex[node]; i++) {
			struct mac_filter_entry tbl_entry;

			ret = tbl_read(node, MAC_FILTER_TABLE, i,
				       &tbl_entry.key, &tbl_entry.mask,
				       &tbl_entry.data);
			if (ret && (ret != TNS_ERR_MAC_FILTER_INVALID_ENTRY))
				return ret;
			else if (ret == TNS_ERR_MAC_FILTER_INVALID_ENTRY)
				continue;

			/* We found filter entry, lets verify either this is
			 * unicast or multicast
			 */
			if (((((u8 *)tbl_entry.key.key_type.s.mac_DA)[5]) &
			       0x1) && (tbl_entry.key.key_type.s.ingress_grp ==
					intr_to_ingressgrp[node][vf])) {
				int vif = tbl_entry.data.s.evif;
				struct evif_entry evif_dat;

				ret = tbl_read(node, MAC_EVIF_TABLE, vif, NULL,
					       NULL, &evif_dat);
				if (ret)
					return ret;
				enable_port(vf, &evif_dat);
				dump_evif_entry(&evif_dat);
				ret = tbl_write(node, MAC_EVIF_TABLE, vif, NULL,
						NULL, (void *)&evif_dat);
				if (ret)
					return ret;
			}
		}
	}

	return TNS_NO_ERR;
}

int disable_promiscuous_mode(int node, int vf)
{
	int i, pf_vf;
	int ret;

	vf_reg_data[node][vf].vf_in_promis = 0;
	ret = vf_pfvf_id(node, vf, &pf_vf);
	if (ret)
		return ret;

	for (i = TNS_MAX_VF; i < macfilter_freeindex[node]; i++) {
		struct mac_filter_entry tbl_entry;

		ret = tbl_read(node, MAC_FILTER_TABLE, i, &tbl_entry.key,
			       &tbl_entry.mask, &tbl_entry.data);
		if (ret && (ret != TNS_ERR_MAC_FILTER_INVALID_ENTRY))
			return ret;
		else if (ret == TNS_ERR_MAC_FILTER_INVALID_ENTRY)
			continue;

		//We found an entry belongs to this group
		if (tbl_entry.key.key_type.s.ingress_grp ==
		    intr_to_ingressgrp[node][vf]) {
			int vif = tbl_entry.data.s.evif;
			struct evif_entry evif_dat;

			if (is_vf_registered_entry(node, vf, i))
				continue;

			//Is this multicast entry
			if (((((u8 *)tbl_entry.key.key_type.s.mac_DA)[5]) &
			       0x1) && vf_reg_data[node][vf].vf_in_mcast_promis)
				continue;

			//Disable port bitmap in EVIF entry
			ret = tbl_read(node, MAC_EVIF_TABLE, vif, NULL,
				       NULL, &evif_dat);
			if (ret)
				return ret;
			disable_port(vf, &evif_dat);
			dump_evif_entry(&evif_dat);
			ret = tbl_write(node, MAC_EVIF_TABLE, vif, NULL, NULL,
					(void *)&evif_dat);
			if (ret)
				return ret;
		}
	}
	/* If pfVf interface exit from promiscuous mode, then  we will change
	 * portbitmap corresponding to LMAC
	 */
	if (vf == pf_vf) {
		int vif = intr_to_ingressgrp[node][vf];
		struct evif_entry evif_dat;
		struct itt_entry_s port_cfg_entry;

		ret = tbl_read(node, MAC_EVIF_TABLE, vif, NULL, NULL,
			       &evif_dat);
		if (ret)
			return ret;

		disable_port(vf, &evif_dat);
		dump_evif_entry(&evif_dat);
		ret = tbl_write(node, MAC_EVIF_TABLE, vif, NULL, NULL,
				(void *)&evif_dat);
		if (ret)
			return ret;

		for (i = 1; i < vlanfilter_freeindex[node]; i++) {
			struct vlan_filter_entry tbl_entry;

			ret = tbl_read(node, VLAN_FILTER_TABLE, i,
				       &tbl_entry.key, &tbl_entry.mask,
				       &tbl_entry.data);
			if (ret && (ret != TNS_ERR_MAC_FILTER_INVALID_ENTRY))
				return ret;
			else if (ret == TNS_ERR_MAC_FILTER_INVALID_ENTRY)
				continue;

			if (tbl_entry.key.key_type.s.ingress_grp ==
			    intr_to_ingressgrp[node][vf]) {
				int vlanx = tbl_entry.data.s.filter_idx;
				vlan_port_bitmap_t vlan_vif;
				int vlan = tbl_entry.key.key_type.s.vlan;

				if (!is_vlan_registered(node, vf, vlan)) {
					ret = tbl_read(node, VLAN_EVIF_TABLE,
						       vlanx, NULL, NULL,
						       (void *)(&vlan_vif[0]));
					if (ret)
						return ret;
					disable_vlan_port(vf, vlan_vif);
					ret = tbl_write(node, VLAN_EVIF_TABLE,
							vlanx, NULL, NULL,
							(void *)(&vlan_vif[0]));
					if (ret)
						return ret;
				}
			}
		}
		//Update default_evif of LMAC to NULLVif
		ret = tbl_read(node, PORT_CONFIG_TABLE,
			       intr_to_ingressgrp[node][vf], NULL, NULL,
			       &port_cfg_entry);
		if (ret)
			return ret;

		port_cfg_entry.default_evif = TNS_NULL_VIF;
		ret = tbl_write(node, PORT_CONFIG_TABLE,
				intr_to_ingressgrp[node][vf], NULL, NULL,
				(void *)&port_cfg_entry);
		if (ret)
			return ret;
		filter_dbg(FINFO, "%s Port %d pkt_dir %d defaultVif %d ",
			   __func__, vf, port_cfg_entry.pkt_dir,
			   port_cfg_entry.default_evif);
		filter_dbg(FINFO, "adminVlan %d %s\n",
			   port_cfg_entry.admin_vlan,
			   port_cfg_entry.is_admin_vlan_enabled ? "Enable" :
			   "Disable");
	}
	if (!vf_reg_data[node][vf].vf_in_mcast_promis)
		remove_vf_from_mcast_promis_grp(node, vf);

	return TNS_NO_ERR;
}

/* CRB-1S configuration
 * Valid LMAC's - 3 (128, 132, & 133)
 * PFVF - 3 (0, 64, & 96)
 * bcast_vif - 3 (136, 140, & 141)
 * mcast_vif - 3 (144, 148, & 149)
 * null_vif - 1 (152)
 */
int mac_filter_config(void)
{
	int node, j;

	for (node = 0; node < nr_node_ids; node++) {
		int lmac;

		//Reset inerface to Ingress Group
		for (j = 0; j < TNS_MAC_FILTER_MAX_SYS_PORTS; j++)
			intr_to_ingressgrp[node][j] = j;

		if (!pf_vf_map_data[node].valid)
			continue;

		for (j = 0; j < TNS_MAX_INGRESS_GROUP; j++)
			ingressgrp_gblvif[node][j].is_valid = 0;

		for (lmac = 0; lmac < pf_vf_map_data[node].lmac_cnt; lmac++) {
			int slm = pf_vf_map_data[node].pf_vf[lmac].sys_lmac;
			int valid_pf = pf_vf_map_data[node].pf_vf[lmac].pf_id;
			int num_vfs = pf_vf_map_data[node].pf_vf[lmac].num_vfs;
			struct evif_entry evif_dat;
			int bvif, mvif;
			int ret;

			bvif = TNS_BASE_BCAST_VIF + slm;
			mvif = TNS_BASE_MCAST_VIF + slm;

			//Map inerface to Ingress Group
			for (j = valid_pf; j < (valid_pf + num_vfs); j++) {
				struct itt_entry_s port_cfg_entry;
				int ret;

				intr_to_ingressgrp[node][j] = TNS_MAX_VF + slm;

				ret = tbl_read(node, PORT_CONFIG_TABLE, j, NULL,
					       NULL, (void *)&port_cfg_entry);
				if (ret)
					return ret;
				port_cfg_entry.default_evif =
					intr_to_ingressgrp[node][j];
				ret = tbl_write(node, PORT_CONFIG_TABLE, j,
						NULL, NULL,
						(void *)&port_cfg_entry);
				if (ret)
					return ret;
			}

			//LMAC Configuration
			ingressgrp_gblvif[node][slm].is_valid = 1;
			ingressgrp_gblvif[node][slm].ingress_grp = TNS_MAX_VF +
								     slm;
			ingressgrp_gblvif[node][slm].pf_vf = valid_pf;
			ingressgrp_gblvif[node][slm].bcast_vif = bvif;
			ingressgrp_gblvif[node][slm].mcast_vif = mvif;
			ingressgrp_gblvif[node][slm].null_vif = TNS_NULL_VIF;
			MCAST_PROMIS(node, slm, 0) = TNS_MAX_VF + slm;
			VALID_MCAST_PROMIS(node, slm) = 1;

			filter_dbg(FINFO, "lmac %d syslm %d num_vfs %d ",
				   lmac, slm,
				   pf_vf_map_data[node].pf_vf[lmac].num_vfs);
			filter_dbg(FINFO, "ingress_grp %d pfVf %d bCast %d ",
				   ingressgrp_gblvif[node][slm].ingress_grp,
				   ingressgrp_gblvif[node][slm].pf_vf,
				   ingressgrp_gblvif[node][slm].bcast_vif);
			filter_dbg(FINFO, "mCast: %d\n",
				   ingressgrp_gblvif[node][slm].mcast_vif);

			ret = tbl_read(node, MAC_EVIF_TABLE, bvif, NULL, NULL,
				       &evif_dat);
			if (ret)
				return ret;

			evif_dat.rewrite_ptr0 = 0xFF;
			evif_dat.rewrite_ptr1 = 0xFF;
			enable_port(ingressgrp_gblvif[node][slm].ingress_grp,
				    &evif_dat);

			ret = tbl_write(node, MAC_EVIF_TABLE, bvif, NULL, NULL,
					(void *)&evif_dat);
			if (ret)
				return ret;

			ret = tbl_read(node, MAC_EVIF_TABLE, mvif, NULL, NULL,
				       &evif_dat);
			if (ret)
				return ret;

			evif_dat.rewrite_ptr0 = 0xFF;
			evif_dat.rewrite_ptr1 = 0xFF;
			enable_port(ingressgrp_gblvif[node][slm].ingress_grp,
				    &evif_dat);

			ret = tbl_write(node, MAC_EVIF_TABLE, mvif, NULL, NULL,
					(void *)&evif_dat);
			if (ret)
				return ret;

			ret = tbl_read(node, MAC_EVIF_TABLE, TNS_NULL_VIF, NULL,
				       NULL, &evif_dat);
			if (ret)
				return ret;

			evif_dat.rewrite_ptr0 = 0xFF;
			evif_dat.rewrite_ptr1 = 0xFF;

			ret = tbl_write(node, MAC_EVIF_TABLE, TNS_NULL_VIF,
					NULL, NULL, (void *)&evif_dat);
			if (ret)
				return ret;
		}
		j = 0;
		alloc_table_index(node, VLAN_FILTER_TABLE, &j);

		for (j = 0; j < TNS_MAX_VF; j++) {
			vf_reg_data[node][j].vf_in_mcast_promis = 0;
			vf_reg_data[node][j].filter_count = 1;
			vf_reg_data[node][j].filter_index[0] = j;
			vf_reg_data[node][j].vlan_count = 0;
			alloc_table_index(node, MAC_FILTER_TABLE, &j);
		}
		for (j = 0; j <= TNS_NULL_VIF; j++)
			alloc_table_index(node, MAC_EVIF_TABLE, &j);
		macfilter_freeindex[node] = TNS_MAX_VF;
		vlanfilter_freeindex[node] = 1;
	}

	return TNS_NO_ERR;
}

int add_admin_vlan(int node, int vf, int vlan)
{
	int index = -1;
	int ret;
	struct itt_entry_s port_cfg_entry;

	ret = add_vlan_entry(node, vf, vlan, index);
	if (ret) {
		filter_dbg(FERR, "Add admin VLAN for VF: %d Failed %d\n",
			   vf, ret);
		return ret;
	}

	ret = tbl_read(node, PORT_CONFIG_TABLE, vf, NULL, NULL,
		       (void *)&port_cfg_entry);
	if (ret)
		return ret;
	port_cfg_entry.is_admin_vlan_enabled = 1;
	port_cfg_entry.admin_vlan = vlan;
	ret = tbl_write(node, PORT_CONFIG_TABLE, vf, NULL, NULL,
			(void *)&port_cfg_entry);
	if (ret)
		return ret;
	filter_dbg(FINFO, "%s Port %d dir %d defaultVif %d adminVlan %d %s\n",
		   __func__, vf, port_cfg_entry.pkt_dir,
		   port_cfg_entry.default_evif, port_cfg_entry.admin_vlan,
		   port_cfg_entry.is_admin_vlan_enabled ? "Enable" : "Disable");

	return TNS_NO_ERR;
}

int del_admin_vlan(int node, int vf, int vlan)
{
	int index = -1;
	int ret;
	struct itt_entry_s port_cfg_entry;

	ret = del_vlan_entry(node, vf, vlan, index);
	if (ret) {
		filter_dbg(FERR, "Delete admin VLAN: %d for VF %d failed %d\n",
			   vlan, vf, ret);
		return ret;
	}

	ret = tbl_read(node, PORT_CONFIG_TABLE, vf, NULL, NULL,
		       (void *)&port_cfg_entry);
	if (ret)
		return ret;
	port_cfg_entry.is_admin_vlan_enabled = 0;
	port_cfg_entry.admin_vlan = 0x0;
	ret = tbl_write(node, PORT_CONFIG_TABLE, vf, NULL, NULL,
			(void *)&port_cfg_entry);
	if (ret)
		return ret;
	filter_dbg(FINFO, "%s Port %d dir %d defaultVif %d adminVlan %d %s\n",
		   __func__, vf, port_cfg_entry.pkt_dir,
		   port_cfg_entry.default_evif, port_cfg_entry.admin_vlan,
		   port_cfg_entry.is_admin_vlan_enabled ? "Enable" : "Disable");

	return TNS_NO_ERR;
}

void link_status_notification(int node, int vf, void *arg)
{
	int status =  *((int *)arg);
	int bcast_vif;
	int ret;
	struct evif_entry evif_dat;

	filter_dbg(FINFO, "VF %d Link %s\n", vf, status ? "up " : "down");
	if (status) {
		ret = vf_bcast_vif(node, vf, &bcast_vif);
		if (ret)
			return;

		ret = tbl_read(node, MAC_EVIF_TABLE, bcast_vif, NULL, NULL,
			       &evif_dat);
		if (ret)
			return;

		enable_port(vf, &evif_dat);
		dump_evif_entry(&evif_dat);
		ret = tbl_write(node, MAC_EVIF_TABLE, bcast_vif, NULL, NULL,
				(void *)&evif_dat);
		if (ret)
			return;
	} else {
		ret = vf_bcast_vif(node, vf, &bcast_vif);
		if (ret)
			return;

		ret = tbl_read(node, MAC_EVIF_TABLE, bcast_vif, NULL, NULL,
			       &evif_dat);
		if (ret)
			return;

		disable_port(vf, &evif_dat);
		dump_evif_entry(&evif_dat);
		ret = tbl_write(node, MAC_EVIF_TABLE, bcast_vif, NULL, NULL,
				(void *)&evif_dat);
		if (ret)
			return;
	}
}

void mac_update_notification(int node, int vf_id, void *arg)
{
	u8 *mac = (u8 *)arg;

	filter_dbg(FINFO, "VF:%d MAC %02x:%02x:%02x:%02x:%02x:%02x Updated\n",
		   vf_id, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	vf_interface_up(node, MAC_FILTER_TABLE, vf_id, arg);
}

void promisc_update_notification(int node, int vf_id, void *arg)
{
	int on = *(int *)arg;

	filter_dbg(FERR, "VF %d %s promiscuous mode\n", vf_id,
		   on ? "enter" : "left");
	if (on)
		enable_promiscuous_mode(node, vf_id);
	else
		disable_promiscuous_mode(node, vf_id);
}

void uc_mc_update_notification(int node, int vf_id, void *arg)
{
	struct uc_mc_msg *uc_mc_cfg = (struct uc_mc_msg *)arg;
	u8 *mac;

	mac = (u8 *)uc_mc_cfg->mac_addr;
	if (uc_mc_cfg->is_flush) {
		filter_dbg(FINFO, "\nNOTIFICATION VF:%d %s %s\n", vf_id,
			   uc_mc_cfg->addr_type ? "mc" : "uc", "flush");
	} else {
		filter_dbg(FINFO, "\nNOTIFICATION VF:%d %s %s ", vf_id,
			   uc_mc_cfg->addr_type ? "mc" : "uc",
			   uc_mc_cfg->is_add ? "add" : "del");
		filter_dbg(FINFO, "MAC ADDRESS %02x:%02x:%02x:%02x:%02x:%02x\n",
			   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		if (uc_mc_cfg->is_add) {
			if (uc_mc_cfg->addr_type)
				add_mac_filter_mcast_entry(node,
							   MAC_FILTER_TABLE,
							   vf_id, -1, mac);
			else
				add_mac_filter_entry(node, MAC_FILTER_TABLE,
						     vf_id, -1, mac);
		} else {
			del_mac_filter_entry(node, MAC_FILTER_TABLE, vf_id, -1,
					     mac, uc_mc_cfg->addr_type);
		}
	}
}

void admin_vlan_update_notification(int node, int vf_id, void *arg)
{
	struct vlan_msg *vlan_cfg = (struct vlan_msg *)arg;

	filter_dbg(FINFO, "\nNOTIFICATION ADMIN VF %d VLAN id %d %s\n", vf_id,
		   vlan_cfg->vlan_id, (vlan_cfg->vlan_add) ? "add" : "del");
	if (vlan_cfg->vlan_add)
		add_admin_vlan(node, vf_id, vlan_cfg->vlan_id);
	else
		del_admin_vlan(node, vf_id, vlan_cfg->vlan_id);
}

void vlan_update_notification(int node, int vf_id, void *arg)
{
	struct vlan_msg *vlan_cfg = (struct vlan_msg *)arg;

	filter_dbg(FINFO, "\nNOTIFICATION VF %d VLAN id %d %s\n", vf_id,
		   vlan_cfg->vlan_id, (vlan_cfg->vlan_add) ? "add" : "del");
	if (vlan_cfg->vlan_add && vlan_cfg->vlan_id) {
		int index = -1;
		int ret = add_vlan_entry(node, vf_id, vlan_cfg->vlan_id,
					      index);

		if (ret)
			filter_dbg(FERR, "Adding VLAN failed: %d\n", ret);
		else
			filter_dbg(FINFO, "VF: %d with VLAN: %d added\n",
				   vf_id, vlan_cfg->vlan_id);
	} else if (!vlan_cfg->vlan_add && vlan_cfg->vlan_id) {
		int index = -1;
		int ret = del_vlan_entry(node, vf_id, vlan_cfg->vlan_id,
						index);

		if (ret)
			filter_dbg(FERR, "Deleting VLAN failed: %d\n", ret);
		else
			filter_dbg(FINFO, "VF: %d with VLAN: %d deleted\n",
				   vf_id, vlan_cfg->vlan_id);
	}
}

void pf_notify_msg_handler(int node, void *arg)
{
	union nic_mbx *mbx = (union nic_mbx *)arg;
	int status;

	switch (mbx->msg.msg) {
	case NIC_MBOX_MSG_ADMIN_VLAN:
		admin_vlan_update_notification(node, mbx->vlan_cfg.vf_id,
					       &mbx->vlan_cfg);
		break;
	case NIC_MBOX_MSG_VLAN:
		vlan_update_notification(node, mbx->vlan_cfg.vf_id,
					 &mbx->vlan_cfg);
		break;
	case NIC_MBOX_MSG_UC_MC:
		uc_mc_update_notification(node, mbx->vlan_cfg.vf_id,
					  &mbx->uc_mc_cfg);
		break;
	case NIC_MBOX_MSG_SET_MAC:
		mac_update_notification(node, mbx->mac.vf_id,
					(void *)mbx->mac.mac_addr);
		break;
	case NIC_MBOX_MSG_CFG_DONE:
	case NIC_MBOX_MSG_OP_UP:
		status = true;
		link_status_notification(node, mbx->mac.vf_id, (void *)&status);
		break;
	case NIC_MBOX_MSG_SHUTDOWN:
	case NIC_MBOX_MSG_OP_DOWN:
		status = false;
		link_status_notification(node, mbx->mac.vf_id, (void *)&status);
		break;
	case NIC_MBOX_MSG_PROMISC:
		status = mbx->promisc_cfg.on;
		promisc_update_notification(node, mbx->promisc_cfg.vf_id,
					    (void *)&status);
		break;
	}
}

int pf_filter_init(void)
{
	mac_filter_config();

	return 0;
}
