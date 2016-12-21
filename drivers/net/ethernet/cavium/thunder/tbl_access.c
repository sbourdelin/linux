/*
 * Copyright (C) 2015 Cavium, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 */

#include <linux/uaccess.h>
#include "pf_globals.h"
#include "pf_locals.h"
#include "tbl_access.h"

struct tns_table_s *get_table_information(int table_id)
{
	int i;

	for (i = 0; i < TNS_MAX_TABLE; i++) {
		if (!tbl_info[i].sdata.valid)
			continue;

		if (tbl_info[i].sdata.tbl_id == table_id)
			return &tbl_info[i];
	}

	return NULL;
}

int tbl_write(int node, int table_id, int tbl_index, void *key, void *mask,
	      void *data)
{
	int i;
	struct tns_table_s *tbl = get_table_information(table_id);
	int bck_cnt, data_index, data_offset;
	u64 data_entry[4];

	if (!tbl) {
		filter_dbg(FERR, "Invalid Table ID: %d\n", table_id);
		return TNS_ERR_INVALID_TBL_ID;
	}

	bck_cnt = tbl->sdata.data_width / tbl->sdata.data_size;
	data_index = (tbl_index / bck_cnt);
	data_offset = (tbl_index % bck_cnt);
	//TCAM Table, we need to parse key & mask into single array
	if (tbl->sdata.tbl_type == TNS_TBL_TYPE_TT) {
		struct filter_keymask_s *tk = (struct filter_keymask_s *)key;
		struct filter_keymask_s *tm = (struct filter_keymask_s *)mask;
		u8 km[32];
		u64 mod_key, mod_mask, temp_mask;
		int index = 0, offset = 0;

		memset(km, 0x0, 32);

/* TCAM truth table data creation. Translation from data/mask to following
 * truth table:
 *
 *         Mask   Data     Content
 *          0     0         X
 *          0     1         1
 *          1     0         0
 *          1     1         Always Mismatch
 *
 */
		mod_mask = ~tk->key_value;
		temp_mask = tm->key_value;
		mod_key = tk->key_value;
		mod_key = mod_key & (~temp_mask);
		mod_mask = mod_mask & (~temp_mask);

		for (i = 0; i < 64; i++) {
			km[index] = km[index] | (((mod_mask >> i) & 0x1) <<
						 offset);
			km[index] = km[index] | (((mod_key >> i) & 0x1) <<
						 (offset + 1));
			offset += 2;
			if (offset == 8) {
				offset = 0;
				index += 1;
			}
		}
		km[index] = 0x2;
		if (tns_write_register_indirect(node,
						(tbl->sdata.key_base_addr +
						 (tbl_index * 32)), 32,
						(void *)&km[0])) {
			filter_dbg(FERR, "key write failed node %d tbl ID %d",
				   node, table_id);
			filter_dbg(FERR, " index %d\n", tbl_index);
			return TNS_ERR_DRIVER_WRITE;
		}
	}

	/* Data Writes are ReadModifyWrite */
	if (tns_read_register_indirect(node, (tbl->sdata.data_base_addr +
					      (data_index * 32)), 32,
				       (void *)&data_entry[0])) {
		filter_dbg(FERR, "data read failed node %d tbl ID %d idx %d\n",
			   node, table_id, tbl_index);
		return TNS_ERR_DRIVER_READ;
	}
	memcpy(&data_entry[data_offset], data, tbl->sdata.data_size / 8);
	if (tns_write_register_indirect(node, (tbl->sdata.data_base_addr +
					       (data_index * 32)), 32,
					(void *)&data_entry[0])) {
		filter_dbg(FERR, "data write failed node %d tbl ID %d idx %d\n",
			   node, table_id, tbl_index);
		return TNS_ERR_DRIVER_WRITE;
	}

	return TNS_NO_ERR;
}

int tbl_read(int node, int table_id, int tbl_index, void *key, void *mask,
	     void *data)
{
	struct tns_table_s *tbl = get_table_information(table_id);
	int i, bck_cnt, data_index, data_offset;
	u64 data_entry[4];
	u8 km[32];

	if (!tbl) {
		filter_dbg(FERR, "Invalid Table ID: %d\n", table_id);
		return TNS_ERR_INVALID_TBL_ID;
	}

	bck_cnt = tbl->sdata.data_width / tbl->sdata.data_size;
	data_index = (tbl_index / bck_cnt);
	data_offset = (tbl_index % bck_cnt);

	//TCAM Table, we need to parse key & mask into single array
	if (tbl->sdata.tbl_type == TNS_TBL_TYPE_TT) {
		memset(km, 0x0, 32);

		if (tns_read_register_indirect(node, (tbl->sdata.key_base_addr +
						      (tbl_index * 32)), 32,
					       (void *)&km[0])) {
			filter_dbg(FERR, "key read failed node %d tbl ID %d",
				   node, table_id);
			filter_dbg(FERR, " idx %d\n", tbl_index);
			return TNS_ERR_DRIVER_READ;
		}
		if (!(km[((tbl->sdata.key_size * 2) / 8)] == 0x2))
			return TNS_ERR_MAC_FILTER_INVALID_ENTRY;
	}

	if (tns_read_register_indirect(node, (tbl->sdata.data_base_addr +
					      (data_index * 32)), 32,
				       (void *)&data_entry[0])) {
		filter_dbg(FERR, "data read failed node %d tbl ID %d idx %d\n",
			   node, table_id, tbl_index);
		return TNS_ERR_DRIVER_READ;
	}
	memcpy(data, (void *)(&data_entry[data_offset]),
	       (tbl->sdata.data_size / 8));

	if (tbl->sdata.tbl_type == TNS_TBL_TYPE_TT) {
		struct filter_keymask_s *tk = (struct filter_keymask_s *)key;
		struct filter_keymask_s *tm = (struct filter_keymask_s *)mask;
		u8 temp_km;
		int index = 0, offset = 0;

		tk->key_value = 0x0ull;
		tm->key_value = 0x0ull;
		temp_km = km[0];
		for (i = 0; i < 64; i++) {
			tm->key_value = tm->key_value |
					 ((temp_km & 0x1ull) << i);
			temp_km >>= 1;
			tk->key_value = tk->key_value |
					 ((temp_km & 0x1ull) << i);
			temp_km >>= 1;
			offset += 2;
			if (offset == 8) {
				offset = 0;
				index += 1;
				temp_km = km[index];
			}
		}
		tm->key_value = ~tm->key_value & ~tk->key_value;
		tk->is_valid = 1;
		tm->is_valid = 0;
	}

	return TNS_NO_ERR;
}

int invalidate_table_entry(int node, int table_id, int tbl_idx)
{
	struct tns_table_s *tbl = get_table_information(table_id);

	if (!tbl) {
		filter_dbg(FERR, "Invalid Table ID: %d\n", table_id);
		return TNS_ERR_INVALID_TBL_ID;
	}

	if (tbl->sdata.tbl_type == TNS_TBL_TYPE_TT) {
		u8 km[32];

		memset(km, 0x0, 32);
		km[((tbl->sdata.key_size * 2) / 8)] = 0x1;

		if (tns_write_register_indirect(node,
						(tbl->sdata.key_base_addr +
						 (tbl_idx * 32)), 32,
						(void *)&km[0])) {
			filter_dbg(FERR, "%s failed node %d tbl ID %d idx %d\n",
				   __func__, node, table_id, tbl_idx);
			return TNS_ERR_DRIVER_WRITE;
		}
	}

	return TNS_NO_ERR;
}

int alloc_table_index(int node, int table_id, int *index)
{
	int err = 0;
	struct tns_table_s *tbl = get_table_information(table_id);

	if (!tbl) {
		filter_dbg(FERR, "%s Invalid TableID %d\n", __func__, table_id);
		return TNS_ERR_INVALID_TBL_ID;
	}

	if (*index == -1) {
		*index = find_first_zero_bit(tbl->ddata[node].bitmap,
					     tbl->sdata.depth);

		if (*index < 0 || *index >= tbl->sdata.depth)
			err = -ENOSPC;
		else
			__set_bit(*index, tbl->ddata[node].bitmap);

		return err;
	} else if (*index < 0 || *index >= tbl->sdata.depth) {
		filter_dbg(FERR, "%s Out of bound index %d requested[0...%d]\n",
			   __func__, *index, tbl->sdata.depth);
		return TNS_ERR_MAC_FILTER_INVALID_ENTRY;
	}
	if (test_and_set_bit(*index, tbl->ddata[node].bitmap))
		filter_dbg(FDEBUG, "%s Entry Already exists\n", __func__);

	return err;
}

void free_table_index(int node, int table_id, int index)
{
	struct tns_table_s *tbl = get_table_information(table_id);

	if (!tbl) {
		filter_dbg(FERR, "%s Invalid TableID %d\n", __func__, table_id);
		return;
	}
	if (index < 0 || index >= tbl->sdata.depth) {
		filter_dbg(FERR, "%s Invalid Index %d Max Limit %d\n",
			   __func__, index, tbl->sdata.depth);
		return;
	}

	__clear_bit(index, tbl->ddata[node].bitmap);
}
