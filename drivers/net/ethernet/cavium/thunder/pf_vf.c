/*
 * Copyright (C) 2015 Cavium, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 */

#include "nic_reg.h"
#include "nic.h"
#include "pf_globals.h"
#include "pf_locals.h"

#define PFVF_DAT(gidx, lidx) \
	pf_vf_map_data[gidx].pf_vf[lidx]

struct pf_vf_map_s pf_vf_map_data[MAX_NUMNODES];

void nic_init_pf_vf_mapping(void)
{
	int i;

	for (i = 0 ; i < MAX_NUMNODES; i++) {
		pf_vf_map_data[i].lmac_cnt = 0;
		pf_vf_map_data[i].valid = false;
	}
}

/* Based on available LMAC's we create physical group called ingress group
 * Designate first VF as acted PF of this group, called PfVf interface.
 */
static inline void set_pf_vf_global_data(int node, int valid_vf_cnt)
{
	unsigned int bgx_map;
	int bgx;
	int lmac, lmac_cnt = 0;

	if (pf_vf_map_data[node].valid)
		return;

	bgx_map = bgx_get_map(node);
	for (bgx = 0; bgx < MAX_BGX_PER_CN88XX; bgx++)	{
		if (!(bgx_map & (1 << bgx)))
			continue;
		pf_vf_map_data[node].valid = true;
		lmac_cnt = bgx_get_lmac_count(node, bgx);

		for (lmac = 0; lmac < lmac_cnt; lmac++)	{
			int slc = lmac + pf_vf_map_data[node].lmac_cnt;

			PFVF_DAT(node, slc).pf_id = (bgx * 64) + (lmac *
								 valid_vf_cnt);
			PFVF_DAT(node, slc).num_vfs = valid_vf_cnt;
			PFVF_DAT(node, slc).lmac = lmac;
			PFVF_DAT(node, slc).bgx_idx = bgx;
			PFVF_DAT(node, slc).sys_lmac = bgx * MAX_LMAC_PER_BGX +
						      lmac;
		}
		pf_vf_map_data[node].lmac_cnt += lmac_cnt;
	}
}

/* We have 2 NIC pipes in each node.Each NIC pipe associated with BGX interface
 * Each BGX contains atmost 4 LMACs (or PHY's) and supports 64 VF's
 * Hardware doesn't have any physical PF, one of VF acts as PF.
 */
int nic_set_pf_vf_mapping(int node_id)
{
	unsigned int bgx_map;
	int node = 0;
	int bgx;
	int lmac_cnt = 0, valid_vf_cnt = 64;

	do {
		bgx_map = bgx_get_map(node);
		/* Calculate Maximum VF's in each physical port group */
		for (bgx = 0; bgx < MAX_BGX_PER_CN88XX; bgx++) {
			if (!(bgx_map & (1 << bgx)))
				continue;
			lmac_cnt = bgx_get_lmac_count(node, bgx);
			//Maximum 64 VF's for each BGX
			if (valid_vf_cnt > (64 / lmac_cnt))
				valid_vf_cnt = (64 / lmac_cnt);
		}
	} while (++node < nr_node_ids);

	nic_enable_valid_vf(valid_vf_cnt);
	node = 0;
	do {
		set_pf_vf_global_data(node, valid_vf_cnt);
	} while (++node < nr_node_ids);

	return 0;
}

/* Find if VF is a acted PF */
int is_pf(int node, int vf)
{
	int i;

	/* Invalid Request, Init not done properly */
	if (!pf_vf_map_data[node].valid)
		return 0;

	for (i = 0; i < pf_vf_map_data[node].lmac_cnt; i++)
		if (vf == PFVF_DAT(node, i).pf_id)
			return 1;

	return 0;
}

/* Get the acted PF corresponding to this VF */
int get_pf(int node, int vf)
{
	int i;

	/* Invalid Request, Init not done properly */
	if (!pf_vf_map_data[node].valid)
		return 0;

	for (i = 0; i < pf_vf_map_data[node].lmac_cnt; i++)
		if ((vf >= PFVF_DAT(node, i).pf_id) &&
		    (vf < (PFVF_DAT(node, i).pf_id +
			   PFVF_DAT(node, i).num_vfs)))
			return pf_vf_map_data[node].pf_vf[i].pf_id;

	return -1;
}

/* Get the starting vf and ending vf number of the LMAC group */
void get_vf_group(int node, int lmac, int *start_vf, int *end_vf)
{
	int i;

	/* Invalid Request, Init not done properly */
	if (!pf_vf_map_data[node].valid)
		return;

	for (i = 0; i < pf_vf_map_data[node].lmac_cnt; i++) {
		if (lmac == (PFVF_DAT(node, i).sys_lmac)) {
			*start_vf = PFVF_DAT(node, i).pf_id;
			*end_vf = PFVF_DAT(node, i).pf_id +
				  PFVF_DAT(node, i).num_vfs;
			return;
		}
	}
}

/* Get the physical port # of the given vf */
int vf_to_pport(int node, int vf)
{
	int i;

	/* Invalid Request, Init not done properly */
	if (!pf_vf_map_data[node].valid)
		return 0;

	for (i = 0; i < pf_vf_map_data[node].lmac_cnt; i++)
		if ((vf >= PFVF_DAT(node, i).pf_id) &&
		    (vf < (PFVF_DAT(node, i).pf_id +
		     PFVF_DAT(node, i).num_vfs)))
			return PFVF_DAT(node, i).sys_lmac;

	return -1;
}

/* Get BGX # and LMAC # corresponding to VF */
int get_bgx_id(int node, int vf, int *bgx_idx, int *lmac)
{
	int i;

	/* Invalid Request, Init not done properly */
	if (!pf_vf_map_data[node].valid)
		return 1;

	for (i = 0; i < pf_vf_map_data[node].lmac_cnt; i++) {
		if ((vf >= PFVF_DAT(node, i).pf_id) &&
		    (vf < (PFVF_DAT(node, i).pf_id +
			   PFVF_DAT(node, i).num_vfs))) {
			*bgx_idx = pf_vf_map_data[node].pf_vf[i].bgx_idx;
			*lmac = pf_vf_map_data[node].pf_vf[i].lmac;
			return 0;
		}
	}

	return 1;
}

/* Get BGX # and LMAC # corresponding to physical port */
int phy_port_to_bgx_lmac(int node, int port, int *bgx, int *lmac)
{
	int i;

	/* Invalid Request, Init not done properly */
	if (!pf_vf_map_data[node].valid)
		return 1;

	for (i = 0; i < pf_vf_map_data[node].lmac_cnt; i++) {
		if (port == (PFVF_DAT(node, i).sys_lmac)) {
			*bgx = pf_vf_map_data[node].pf_vf[i].bgx_idx;
			*lmac = pf_vf_map_data[node].pf_vf[i].lmac;
			return 0;
		}
	}

	return 1;
}
