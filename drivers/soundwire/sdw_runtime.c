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

/**
 * sdw_program_slv_xport_params: Programs Slave transport registers on
 * alternate bank (bank currently unused). This API is called by
 * sdw_program_xport_params.
 *
 * @sdw_bus: Bus handle.
 * @slv_rt: Slave runtime handle.
 * @t_slv_params: Transport parameters to be configured.
 * @p_slv_params: Port parameters to be configured.
 */
static int sdw_program_slv_xport_params(struct sdw_bus *bus,
			struct sdw_slv_runtime *slv_rt,
			struct sdw_transport_params *t_slv_params,
			struct sdw_port_params *p_slv_params)
{
	int ret;
	int bank_to_use, type;
	u16 addr, len;
	u8 wbuf[SDW_BUF_SIZE3] = {0, 0, 0};
	u8 wbuf1[SDW_BUF_SIZE4] = {0, 0, 0, 0};
	u8 wbuf3[SDW_BUF_SIZE2] = {0, 0};
	u8 rbuf;
	struct sdw_dpn_prop *dpn_prop;

	dpn_prop = sdw_get_slv_dpn_prop(slv_rt->slv, slv_rt->direction,
				t_slv_params->port_num);
	if (!dpn_prop)
		return -EINVAL;

	/* Get port capability info */
	type = dpn_prop->type;

	/*
	 * Optimization scope: Reduce number of writes on the bus.
	 * Mirroring should be considered.
	 */

	/*
	 * Fill buffer contents for all messages.
	 * 1. wr_msg holds values to program blockctrl2, samplectrl1 and
	 * samplectrl2 registers.
	 * 2. wr_msg1 holds values to program offset_ctrl1, offset_ctrl2,
	 * hctrl and blockctrl3 registers.
	 * 3. wr_msg2 holds values to program lanectrl register.
	 * 4. wr_msg3 holds values to program portctrl and blockctrl1
	 * register. If the Slave port(s) doesn't implement block group,
	 * then blockctrl2 and blockctrl3 registers are not programmed.
	 * Similarly if the Slave port(s) doesn't support lane control, then
	 * lanectrl register is not programmed.
	 */

	/* Fill DPN_BlockCtrl2 value */
	wbuf[0] = t_slv_params->blk_grp_ctrl;

	/* Fill DPN_SampleCtrl1 value */
	wbuf[1] = (t_slv_params->sample_interval - 1) &
			SDW_DPN_SAMPLECTRL1_LOW_MASK;

	 /* Fill DPN_SampleCtrl2 register value */
	wbuf[2] = ((t_slv_params->sample_interval - 1) &
			SDW_DPN_SAMPLECTRL2_LOW_MASK) >>
			SDW_DPN_SAMPLECTRL2_SHIFT;

	/* Fill DPN_OffsetCtrl1 register value */
	wbuf1[0] = t_slv_params->offset1;

	/* Fill DPN_OffsetCtrl1 register value */
	wbuf1[1] = t_slv_params->offset2;

	/* Fill DPN_HCtrl register value */
	wbuf1[2] = (t_slv_params->hstop |
			(t_slv_params->hstart << SDW_DPN_HCTRL_HSTART_SHIFT));

	/* Fill DPN_BlockCtrl3 register value */
	wbuf1[3] = t_slv_params->blk_pkg_mode;

	/* Get current bank in use from bus structure */
	bank_to_use = !bus->params.active_bank;

	addr = SDW_DPN_PORTCTRL +
		(SDW_NUM_DATA_PORT_REGISTERS * t_slv_params->port_num);

	/* Read port_ctrl Slave register */
	rbuf = sdw_read(slv_rt->slv, addr);

	/* Fill DP0_PortCtrl register value */
	wbuf3[0] = (p_slv_params->flow_mode | (p_slv_params->data_mode <<
				SDW_DPN_PORTCTRL_DATAMODE_SHIFT) | rbuf);

	/* Fill DP0_BlockCtrl1 register value */
	wbuf3[1] = (p_slv_params->bps - 1);

	addr = ((SDW_DPN_BLOCKCTRL2 +
			(1 * (!t_slv_params->blk_grp_ctrl_valid)) +
			(SDW_BANK1_REGISTER_OFFSET * bank_to_use)) +
			(SDW_NUM_DATA_PORT_REGISTERS * t_slv_params->port_num));

	if (type == SDW_DP_TYPE_FULL)
		len = (SDW_BUF_SIZE2 +
			(1 * (t_slv_params->blk_grp_ctrl_valid)));
	else
		len = (SDW_BUF_SIZE1 +
			(1 * (t_slv_params->blk_grp_ctrl_valid)));

	ret = sdw_nwrite(slv_rt->slv, addr, len, &wbuf[0]);
	if (ret != SDW_NUM_MSG1) {
		ret = -EINVAL;
		dev_err(bus->dev, "Slave block_ctrl2/sample_ctrl1/2 reg write failed\n");
		goto out;
	}

	/* Create write message wr_msg1 to program transport Slave register */
	addr = ((SDW_DPN_OFFSETCTRL1 +
			(SDW_BANK1_REGISTER_OFFSET * bank_to_use)) +
			(SDW_NUM_DATA_PORT_REGISTERS * t_slv_params->port_num));

	if (type == SDW_DP_TYPE_FULL)
		len = SDW_BUF_SIZE4;
	else
		len = SDW_BUF_SIZE1;

	ret = sdw_nwrite(slv_rt->slv, addr, len, &wbuf1[0]);
	if (ret != SDW_NUM_MSG1) {
		ret = -EINVAL;
		dev_err(bus->dev, "Slave offset_ctrl1/2/h_ctrl/block_ctrl2 reg write failed\n");
		goto out;
	}

	addr = SDW_DPN_PORTCTRL +
		(SDW_NUM_DATA_PORT_REGISTERS * t_slv_params->port_num);

	ret = sdw_nwrite(slv_rt->slv, addr, SDW_BUF_SIZE2, &wbuf3[0]);
	if (ret != SDW_NUM_MSG1) {
		ret = -EINVAL;
		dev_err(bus->dev, "Slave port_ctrl/block_ctrl1 reg write failed\n");
		goto out;
	}

out:
	return ret;
}

/**
 * sdw_program_mstr_xport_params: Programs Master transport parameters
 * registers on alternate bank (bank currently unused). This API is called
 * by sdw_program_xport_params.
 *
 * @sdw_bus: Bus handle.
 * @t_mstr_params: Transport parameters to be configured.
 * @p_mstr_params: Port parameters to be configured.
 */
static int sdw_program_mstr_xport_params(struct sdw_bus *bus,
		struct sdw_transport_params *t_mstr_params,
		struct sdw_port_params *p_mstr_params)
{
	int bank_to_use, ret;

	/* Get current bank in use from bus structure */
	bank_to_use = !bus->params.active_bank;

	/* Perform Master transport parameters API call */
	ret = bus->port_ops->dpn_set_port_transport_params(bus, t_mstr_params,
						bank_to_use);
	if (ret < 0)
		return ret;

	/* Perform Master port parameters API call */
	return bus->port_ops->dpn_set_port_params(bus, p_mstr_params,
						bank_to_use);
}

/**
 * sdw_program_xport_params: Programs transport parameters of Master and
 * Slave registers. This function calls individual Master and Slave API to
 * configure transport and port parameters. This API is called by
 * sdw_program_params.
 *
 * @sdw_bus: Bus handle.
 * @sdw_mstr_rt: Runtime Master handle.
 */
static int sdw_program_xport_params(struct sdw_bus *bus,
			struct sdw_mstr_runtime *sdw_mstr_rt)
{
	struct sdw_slv_runtime *slv_rt = NULL;
	struct sdw_port_runtime *port_rt, *port_slv_rt;
	struct sdw_transport_params *t_params, *t_slv_params;
	struct sdw_port_params *p_params, *p_slv_params;
	int ret = 0;

	/*
	 * Check stream state before programming transport parameters There
	 * are two flows in which transport parameters are programmed.
	 * 1. For new stream enabling, no stream state check required.
	 * 2. For active streams enabling, stream state check is required.
	 * For second flow, transport parameters will be only programmed if
	 * stream is in de-prepare state. It applies for both Master and
	 * Slave.
	 */

	if (sdw_mstr_rt->sdw_rt->stream_state == SDW_STATE_STRM_DEPREPARE)
		return 0;

	/* Iterate for all Slave(s) in Slave list */
	list_for_each_entry(slv_rt,
			&sdw_mstr_rt->slv_rt_list, slave_mstr_node) {

		/* Iterate for all Slave port(s) in port list */
		list_for_each_entry(port_slv_rt, &slv_rt->port_rt_list,
							port_node) {

			/* Transport and port parameters for Slave */
			t_slv_params = &port_slv_rt->transport_params;
			p_slv_params = &port_slv_rt->port_params;

			/* Assign port parameters */
			p_slv_params->num = port_slv_rt->port_num;
			p_slv_params->bps = slv_rt->stream_params.bps;

			/*
			 * TODO: Currently only Isochronous mode supported,
			 * asynchronous support to be added.
			 */

			/* Isochronous Mode */
			port_slv_rt->port_params.flow_mode =
				SDW_PORT_FLOW_MODE_ISOCH;

			/* Normal Mode */
			port_slv_rt->port_params.data_mode =
				SDW_PORT_DATA_MODE_NORMAL;

			/* Program transport & port parameters for Slave */
			ret = sdw_program_slv_xport_params(bus, slv_rt,
					t_slv_params, p_slv_params);
			if (ret < 0)
				return ret;
		}
	}

	/* Iterate for all Master port(s) in port list */
	list_for_each_entry(port_rt,
			&sdw_mstr_rt->port_rt_list, port_node) {

		/* Transport and port parameters for Master*/
		t_params = &port_rt->transport_params;
		p_params = &port_rt->port_params;

		/* Assign port parameters */
		p_params->num = port_rt->port_num;
		p_params->bps = sdw_mstr_rt->stream_params.bps;

		/*
		 * TODO: Currently only Isochronous mode supported,
		 * asynchronous support to be added.
		 */

		/* Isochronous Mode */
		p_params->flow_mode = SDW_PORT_FLOW_MODE_ISOCH;

		/* Normal Mode */
		p_params->data_mode = 0x0;

		/* Program transport & port parameters for Slave */
		ret = sdw_program_mstr_xport_params(bus, t_params,
							p_params);
		if (ret < 0)
			return ret;
	}

	return ret;
}

/**
 * sdw_enable_disable_slv_ports: Enable & disables port(s) channel(s) for
 * Slave(s). The Slave(s) port(s) channel(s) are enable or disabled on
 * alternate bank (bank currently unused). This API is called by
 * sdw_enable_disable_ports.
 *
 * @sdw_bus: Bus handle.
 * @sdw_slv_rt: Runtime Slave handle.
 * @port_slv_rt: Runtime port handle.
 * @chn_en: Enable or disable the channel.
 */
static int sdw_enable_disable_slv_ports(struct sdw_bus *bus,
				struct sdw_slv_runtime *sdw_slv_rt,
				struct sdw_port_runtime *port_slv_rt,
				bool chn_en)
{
	int ret;
	int bank_to_use;
	u16 addr;
	u8 wbuf, rbuf;

	/* Get current bank in use from bus structure */
	bank_to_use = !bus->params.active_bank;

	/* Get channel enable register address for Slave */
	addr = ((SDW_DPN_CHANNELEN +
			(SDW_BANK1_REGISTER_OFFSET * bank_to_use)) +
			(SDW_NUM_DATA_PORT_REGISTERS *
			port_slv_rt->port_num));

	/* Read channel enable Slave register */
	rbuf = sdw_read(sdw_slv_rt->slv, addr);

	if (chn_en)
		wbuf = (rbuf | port_slv_rt->channel_mask);
	else
		wbuf = (rbuf & ~(port_slv_rt->channel_mask));

	/* Write channel enable Slave register */
	ret = sdw_write(sdw_slv_rt->slv, addr, wbuf);
	if (ret != SDW_NUM_MSG1) {
		dev_err(bus->dev,
				"Channel enable write failed\n");
		return -EINVAL;
	}

	return ret;
}

/**
 * sdw_enable_disable_mstr_ports: Enable & disables port(s) channel(s) for
 * Master. The Master port(s) channel(s) are enable or disabled on alternate
 * bank (bank currently unused). This API is called by
 * sdw_enable_disable_ports.
 *
 * @sdw_bus: Bus handle.
 * @sdw_mstr_rt: Runtime Master handle.
 * @port_slv_rt: Runtime port handle.
 * @chn_en: Operations to be performed.
 */
static int sdw_enable_disable_mstr_ports(struct sdw_bus *bus,
				struct sdw_mstr_runtime *sdw_mstr_rt,
				struct sdw_port_runtime *port_rt,
				bool chn_en)
{
	struct sdw_enable_ch enable_ch;
	int bank_to_use, ret = 0;

	/* Fill enable_ch data structure with values */
	enable_ch.num = port_rt->port_num;
	enable_ch.ch_mask = port_rt->channel_mask;
	enable_ch.enable = chn_en; /* Enable/Disable */


	/* Get current bank in use from bus structure */
	bank_to_use = !bus->params.active_bank;

	/* Perform Master port(s) channel(s) enable/disable API call */
	if (bus->port_ops->dpn_port_enable_ch) {
		ret = bus->port_ops->dpn_port_enable_ch(bus, &enable_ch,
					bank_to_use);
		if (ret < 0)
			return ret;
	}

	return ret;
}

/**
 * sdw_enable_disable_ports: Enable/disable port(s) channel(s) for Master
 * and Slave. This function calls individual API's of Master and Slave
 * respectively to perform enable or disable operation. This API is called
 * by sdw_program_params, sdw_update_bus_params_ops and sdw_disable_op.
 *
 * @sdw_bus: Bus handle.
 * @sdw_rt: Runtime stream handle.
 * @sdw_mstr_rt: Runtime Master handle.
 * @chn_en: Operations to be performed.
 */
static int sdw_enable_disable_ports(struct sdw_bus *bus,
				struct sdw_runtime *sdw_rt,
				struct sdw_mstr_runtime *sdw_mstr_rt,
				bool chn_en)
{
	struct sdw_slv_runtime *slv_rt = NULL;
	struct sdw_mstr_runtime *mstr_rt = NULL;
	struct sdw_port_runtime *port_slv, *port_mstr;
	int ret = 0;

	/*
	 * There are two flows in which channels are enabled and disabled.
	 * 1. For new stream enabling/disabling, no stream state check
	 * required.
	 * 2. For active streams enabling/disabling, stream state check is
	 * required.
	 * Currently goto is used in API to select above operation
	 * TODO: Avoid usage of goto statement
	 */
	if (sdw_mstr_rt == NULL)
		goto sdw_rt_ops;

	/* Iterate for all Slave(s) in Slave list */
	list_for_each_entry(slv_rt, &sdw_mstr_rt->slv_rt_list,
						slave_mstr_node) {

		/*
		 * Do not perform enable/disable operation if stream is in
		 * ENABLE state.
		 */
		if (slv_rt->sdw_rt->stream_state == SDW_STATE_STRM_ENABLE) {

			/* Iterate for all Slave port(s) in port list */
			list_for_each_entry(port_slv, &slv_rt->port_rt_list,
							port_node) {

				/*
				 * Enable/Disable Slave port(s) channel(s)
				 */
				ret = sdw_enable_disable_slv_ports(bus,
						slv_rt, port_slv, chn_en);
				if (ret < 0)
					return ret;
			}
		}
	}

	/*
	 * Do not perform enable/disable operation if stream is in ENABLE
	 * state.
	 */
	if (sdw_mstr_rt->sdw_rt->stream_state == SDW_STATE_STRM_ENABLE) {


		/* Iterate for all Master port(s) in port list */
		list_for_each_entry(port_mstr,
				&sdw_mstr_rt->port_rt_list, port_node) {

			/* Enable/Disable Master port(s) channel(s) */
			ret = sdw_enable_disable_mstr_ports(bus,
				sdw_mstr_rt, port_mstr, chn_en);
			if (ret < 0)
				return ret;
		}
	}

sdw_rt_ops:

	/* Enable/Disable operation based on stream */
	if (sdw_rt == NULL)
		return ret;

	/* Iterate for all Slave(s) in Slave list */
	list_for_each_entry(slv_rt, &sdw_rt->slv_rt_list, slave_strm_node) {

		/* Iterate for all Slave port(s) in port list */
		list_for_each_entry(port_slv, &slv_rt->port_rt_list,
						port_node) {

			/* Enable/Disable Slave port(s) channel(s) */
			ret = sdw_enable_disable_slv_ports(bus, slv_rt,
							port_slv, chn_en);
			if (ret < 0)
				return ret;

		}
	}

	/* Iterate for all Master(s) in Master list */
	list_for_each_entry(mstr_rt, &sdw_rt->mstr_rt_list, mstr_strm_node) {

		/* Iterate for all Master port(s) in port list */
		list_for_each_entry(port_mstr, &mstr_rt->port_rt_list,
							port_node) {

			/* Enable/Disable Master port(s) channel(s) */
			ret = sdw_enable_disable_mstr_ports(bus, mstr_rt,
							port_mstr, chn_en);
			if (ret < 0)
				return ret;
		}
	}

	return ret;
}

/**
 * sdw_check_slv_clock_cap: Slave capabilities are checked for each clock
 * computed. If Slave support given clock, it returns true else false.  This
 * API is called by sdw_compute_bus_params.
 *
 * @mode_prop: Port properties.
 * @clock_reqd: clock rate.
 *
 * Returns true if clock rate is OK else false.
 */
static bool
sdw_check_slv_clock_cap(struct sdw_dpn_audio_mode *mode_prop, int clock_reqd)
{
	int value = 0, j;
	bool clock_ok = false;

	/*
	 * Slave(s) can provide supported clock rates or minimum/maximum
	 * range. First check for clock rates, if not available then check
	 * with minimum/maximum range.
	 */
	if (mode_prop->bus_num_freq) {
		/* Run loop for all clock rates */
		for (j = 0; j < mode_prop->bus_num_freq; j++) {
			value = mode_prop->bus_freq[j];
			if (clock_reqd == value) {
				clock_ok = true;
				break;
			}
			if (j == mode_prop->bus_num_freq) {
				clock_ok = false;
				break;
			}

		}

	} else {
		if ((clock_reqd < mode_prop->bus_min_freq) ||
				(clock_reqd > mode_prop->bus_max_freq))
			clock_ok = false;
		else
			clock_ok = true;
	}

	return clock_ok;
}

/**
 * sdw_compute_bus_params: Based on the bandwidth computed per bus, clock
 * and frame shape required for bus is calculate. Each clock frequency
 * selected is checked with all the Slave(s) capabilities in use on bus.
 * Based on frame shape, frame interval is also computed. This API is called
 * by sdw_compute_params.
 *
 * @sdw_bus: Bus handle.
 * @frame_int: Return frame interval computed.
 * @sdw_mstr_rt: Runtime Master handle.
 */
static int sdw_compute_bus_params(struct sdw_bus *bus, int *frame_int,
				struct sdw_mstr_runtime *sdw_mstr_rt)
{
	struct sdw_master_prop *master_prop = NULL;
	struct sdw_dpn_prop *dpn_prop = NULL;
	struct sdw_dpn_audio_mode *mode_prop = NULL;
	struct sdw_slv_runtime *slv_rt = NULL;
	struct sdw_port_runtime *port_slv_rt = NULL;
	unsigned int double_rate_freq, clock_reqd;
	int i, rc, num_clk_gears, gear;
	int frame_interval = 0, frame_frequency = 0;
	int sel_row = 0, sel_col = 0, pn = 0;
	bool clock_ok = false;

	/* Get Master capabilities handle */
	master_prop = &bus->prop;

	/*
	 * Note:
	 * Below loop is executed using number of clock gears supported.
	 * TODO: Need to add support further if clock to be computed using
	 * number of clock frequencies.
	 */
	num_clk_gears = master_prop->num_clk_gears;
	if (!num_clk_gears)
		return -EINVAL;

	/* Double clock rate */
	double_rate_freq = master_prop->max_freq * SDW_DOUBLE_RATE_FACTOR;

	/*
	 * Find nearest clock frequency needed by bus for given bandwidth
	 */
	for (i = 0; i < num_clk_gears; i++) {

		gear = master_prop->clk_gears[i];

		/* TODO: Explanation for SDW_FREQ_MOD_FACTOR */
		if (((double_rate_freq / gear) <= bus->params.bandwidth) ||
			(((double_rate_freq / gear) %
				SDW_FREQ_MOD_FACTOR) != 0))
			continue;

		/* Selected clock frequency */
		clock_reqd = master_prop->max_freq / gear;

		/*
		 * Check all the Slave(s) device capabilities here and find
		 * whether given clock rate is supported by all Slaves
		 */

		/* Iterate for all Slave(s) in Slave list */
		list_for_each_entry(slv_rt, &sdw_mstr_rt->slv_rt_list,
				slave_mstr_node) {

			/* Iterate for all Slave port(s) in port list */
			list_for_each_entry(port_slv_rt, &slv_rt->port_rt_list,
								port_node) {

				/* Get port number */
				pn = port_slv_rt->port_num;

				/* Get port capabilities */
				dpn_prop = sdw_get_slv_dpn_prop(slv_rt->slv,
						slv_rt->direction, pn);
				if (!dpn_prop)
					return -EINVAL;

				mode_prop = &dpn_prop->audio_mode;

				/*
				 * Perform slave capabilities check API call
				 * where current selected clock is checked
				 * against clock rates supported by Slave(s)
				 */
				clock_ok = sdw_check_slv_clock_cap(mode_prop,
								clock_reqd);
				/*
				 * Don't check next Slave port capabilities,
				 * go for next clock frequency
				 */
				if (!clock_ok)
					break;
			}

			/* Go for next clock frequency */
			if (!clock_ok)
				break;
		}

		/* None of clock rate matches, return error */
		if (i == num_clk_gears)
			return -EINVAL;

		/* check for next clock divider */
		if (!clock_ok)
			continue;

		/*
		 * At this point clock rate and clock gear is computed, now
		 * find frame shape for bus.
		 */
		for (rc = 0; rc < SDW_FRAME_ROW_COLS; rc++) {

			/* Compute frame interval and frame frequency */
			frame_interval =
				sdw_core.row_col_pair[rc].row *
				sdw_core.row_col_pair[rc].col;
			frame_frequency =
				(double_rate_freq/gear)/frame_interval;

			/* Run loop till frame shape is OK */
			if (((double_rate_freq/gear) -
						(frame_frequency *
						 sdw_core.row_col_pair[rc].
						 control_bits)) <
						bus->params.bandwidth)
				continue;

			break;
		}

		/* Valid frame shape not found, check for next clock freq */
		if (rc == SDW_FRAME_ROW_COLS)
			continue;

		/* Fill all the computed values in data structures */
		sel_row = sdw_core.row_col_pair[rc].row;
		sel_col = sdw_core.row_col_pair[rc].col;
		bus->params.frame_freq = frame_frequency;
		bus->params.curr_dr_clk_freq = double_rate_freq/gear;
		bus->params.clk_div = gear;
		clock_ok = false;
		*frame_int = frame_interval;
		bus->params.col = sel_col;
		bus->params.row = sel_row;

		break;
	}

	return 0;
}

/**
 * sdw_compute_system_interval: This function computes system interval and
 * stream interval per Master based on the current and active streams
 * running on bus. This API is called by sdw_compute_params.
 *
 * @sdw_bus: Bus handle.
 * @frame_interval: Computed Frame interval.
 */
static int sdw_compute_system_interval(struct sdw_bus *bus,
					int frame_interval)
{
	struct sdw_transport_params *t_params = NULL, *t_slv_params = NULL;
	struct sdw_port_runtime *port_rt, *port_slv_rt;
	struct sdw_mstr_runtime *sdw_mstr_rt = NULL;
	struct sdw_slv_runtime *slv_rt = NULL;
	int lcmnum1 = 0, lcmnum2 = 0, lcmnum3 = 0, div = 0;
	int sample_interval;

	/*
	 * once bandwidth and frame shape for bus is found, run a loop for
	 * current and all the active streams on bus and compute stream
	 * interval & sample_interval.
	 */

	/* Iterate for all Master(s) in Master list */
	list_for_each_entry(sdw_mstr_rt, &bus->mstr_rt_list, mstr_node) {

		/*
		 * Do not compute system and stream interval if stream state
		 * is in DEPREPARE
		 */
		if (sdw_mstr_rt->sdw_rt->stream_state ==
				SDW_STATE_STRM_DEPREPARE)
			continue;

		/* Calculate sample interval */
		sample_interval = (bus->params.curr_dr_clk_freq/
				sdw_mstr_rt->stream_params.rate);


		/*
		 * Iterate for all Master port(s) in port list and assign
		 * sample interval per port.
		 */
		list_for_each_entry(port_rt, &sdw_mstr_rt->port_rt_list,
							port_node) {

			/* Assign sample interval for each port */
			t_params = &port_rt->transport_params;
			t_params->sample_interval = sample_interval;
		}

		/* Calculate LCM */
		lcmnum2 = sample_interval;

		if (!lcmnum1)
			lcmnum1 = lcm(lcmnum2, lcmnum2);
		else
			lcmnum1 = lcm(lcmnum1, lcmnum2);

		/* Iterate for all Slave(s) in Slave list */
		list_for_each_entry(slv_rt, &sdw_mstr_rt->slv_rt_list,
						slave_mstr_node) {

			/*
			 * Iterate for all Slave port(s) in port list and
			 * assign sample interval per port.
			 */
			list_for_each_entry(port_slv_rt, &slv_rt->port_rt_list,
							port_node) {

				/*
				 * Assign sample interval for each port.
				 * Assumption is that sample interval per
				 * port for given Slave will be same.
				 */
				t_slv_params = &port_slv_rt->transport_params;
				t_slv_params->sample_interval = sample_interval;
			}
		}
	}

	/* Assign stream interval */
	bus->params.stream_interval = lcmnum1;

	/*
	 * If system interval already calculated, return successful, it can
	 * be case in pause/resume, under-run scenario.
	 */
	if (bus->params.system_interval)
		return 0;

	/* Compute system_interval */
	if (bus->params.curr_dr_clk_freq) {

		/* Get divide value */
		div = (bus->params.max_dr_clk_freq /
					bus->params.curr_dr_clk_freq);

		/* Get LCM of stream interval and frame interval */
		if ((lcmnum1) && (frame_interval))

			lcmnum3 = lcm(lcmnum1, frame_interval);
		else
			return -EINVAL;

		/* Fill computed system interval value */
		bus->params.system_interval = ((div * lcmnum3) /
							frame_interval);
	}

	/*
	 * Something went wrong while computing system interval may be lcm
	 * value may be 0, return error accordingly.
	 */
	if (!bus->params.system_interval)
		return -EINVAL;

	return 0;
}
