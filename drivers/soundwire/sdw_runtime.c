/*
 *  This file is provided under a dual BSD/GPLv2 license.  When using or
 *  redistributing this file, you may do so under either license.
 *
 *  GPL LICENSE SUMMARY
 *
 *  Copyright(c) 2015-17 Intel Corporation.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of version 2 of the GNU General Public License as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  BSD LICENSE
 *
 *  Copyright(c) 2015-17 Intel Corporation.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in
 *      the documentation and/or other materials provided with the
 *      distribution.
 *    * Neither the name of Intel Corporation nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 * sdw_runtime.c - SoundWire Bus BW calculation & Stream runtime
 * operations.
 *
 * Author: Sanyog Kale <sanyog.r.kale@intel.com>
 */

#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/lcm.h>
#include <linux/soundwire/soundwire.h>
#include "sdw_bus.h"

/* Array of supported rows as per MIPI SoundWire Specification 1.1 */
static int rows[SDW_FRAME_MAX_ROWS] = {48, 50, 60, 64, 72, 75, 80, 90,
		     96, 125, 144, 147, 100, 120, 128, 150,
		     160, 180, 192, 200, 240, 250, 256};

/* Array of supported columns as per MIPI SoundWire Specification 1.1 */
static int cols[SDW_FRAME_MAX_COLS] = {2, 4, 6, 8, 10, 12, 14, 16};

/* Mapping of index to rows */
static struct sdw_index_to_row sdw_index_row_mapping[SDW_FRAME_MAX_ROWS] = {
	{0, 48}, {1, 50}, {2, 60}, {3, 64}, {4, 75}, {5, 80}, {6, 125},
	{7, 147}, {8, 96}, {9, 100}, {10, 120}, {11, 128}, {12, 150},
	{13, 160}, {14, 250}, {16, 192}, {17, 200}, {18, 240}, {19, 256},
	{20, 72}, {21, 144}, {22, 90}, {23, 180},
};

/* Mapping of index to columns */
static struct sdw_index_to_col sdw_index_col_mapping[SDW_FRAME_MAX_COLS] = {
	{0, 2}, {1, 4}, {2, 6}, {3, 8}, {4, 10}, {5, 12}, {6, 14}, {7, 16},
};

/**
 * sdw_create_row_col_pair: Initialization of bandwidth related operations
 *
 * This is required to have fast path for the BW calculation when a new stream
 * is prepared or deprepared. This is called only once as part of SoundWire Bus
 * getting initialized.
 */
void sdw_create_row_col_pair(void)
{
	int r, c, rc_count = 0;
	int control_bits = SDW_FRAME_CTRL_BITS;

	/* Run loop for all columns */
	for (c = 0; c < SDW_FRAME_MAX_COLS; c++) {

		/* Run loop for all rows */
		for (r = 0; r < SDW_FRAME_MAX_ROWS; r++) {

			sdw_core.row_col_pair[rc_count].col = cols[c];
			sdw_core.row_col_pair[rc_count].row = rows[r];
			sdw_core.row_col_pair[rc_count].control_bits =
								control_bits;
			sdw_core.row_col_pair[rc_count].data_bits =
				(cols[c] * rows[r]) - control_bits;

			rc_count++;
		}
	}
}

/**
 * sdw_find_col_index: Performs column to index mapping. The retrieved
 * number is used for programming register. This API is called by
 * sdw_bank_switch.
 *
 * @col: number of columns.
 *
 * Returns column index from the mapping else lowest column mapped index.
 */
static int sdw_find_col_index(int col)
{
	int i;

	for (i = 0; i <= SDW_FRAME_MAX_COLS; i++) {
		if (sdw_index_col_mapping[i].col == col)
			return sdw_index_col_mapping[i].index;
	}

	return 0; /* Lowest Column number = 2 */
}

/**
 * sdw_find_row_index: Performs row to index mapping. The retrieved number
 * is used for programming register. This API is called by sdw_bank_switch.
 *
 * @row: number of rows.
 *
 * Returns row index from the mapping else lowest row mapped index.
 */
static int sdw_find_row_index(int row)
{
	int i;

	for (i = 0; i <= SDW_FRAME_MAX_ROWS; i++) {
		if (sdw_index_row_mapping[i].row == row)
			return sdw_index_row_mapping[i].index;
	}

	return 0; /* Lowest Row number = 48 */
}

/**
 * sdw_init_bus_params: Sets up bus data structure for BW calculation. This
 * is called once per each Master interface registration to the SoundWire
 * bus.
 *
 * @sdw_bus: Bus handle.
 */

void sdw_init_bus_params(struct sdw_bus *bus)
{
	struct sdw_bus_params *params = &bus->params;
	struct sdw_master_prop *master_prop = &bus->prop;

	/* Initialize required parameters in bus structure */
	params->max_dr_clk_freq = master_prop->max_freq *
					SDW_DOUBLE_RATE_FACTOR;

	/*
	 * Assumption: At power on, bus is running at maximum frequency.
	 */
	params->curr_dr_clk_freq = params->max_dr_clk_freq;
}
