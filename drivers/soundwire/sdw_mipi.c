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

#include <linux/property.h>
#include <linux/soundwire/soundwire.h>
#include "sdw_bus.h"

int sdw_master_read_prop(struct sdw_bus *bus)
{
	struct sdw_master_prop *prop = &bus->prop;
	struct fwnode_handle *link;
	bool b_val;
	int nval;
	unsigned int count = 0;
	char name[32];

	device_property_read_u32(bus->dev,
			"mipi-sdw-sw-interface-revision", &prop->revision);

	device_property_read_u32(bus->dev, "mipi-sdw-master-count", &count);

	/* find the link handle */
	snprintf(name, sizeof(name), "mipi-sdw-link-%d-subproperties", bus->link_id);
	link = device_get_named_child_node(bus->dev, name);
	if (!link) {
		dev_err(bus->dev, "Link node %s not found\n", name);
		return -EIO;
	}

	b_val = fwnode_property_read_bool(link,
			"mipi-sdw-clock-stop-mode0-supported");

	if (b_val)
		prop->clk_stop_mode = SDW_CLK_STOP_MODE0;

	b_val = fwnode_property_read_bool(link,
			"mipi-sdw-clock-stop-mode1-supported");

	if (b_val)
		prop->clk_stop_mode |= SDW_CLK_STOP_MODE1;

	fwnode_property_read_u32(link,
			"mipi-sdw-max-clock-frequency", &prop->max_freq);

	/* find count */
	nval = fwnode_property_read_u32_array(link,
			"mipi-sdw-clock-frequencies-supported", NULL, 0);

	if (nval > 0)
		bus->prop.num_freq = nval;

	if (bus->prop.num_freq) {
		bus->prop.freq = devm_kcalloc(bus->dev,
			sizeof(*bus->prop.freq), nval, GFP_KERNEL);
		if (!bus->prop.freq)
			return -ENOMEM;

		fwnode_property_read_u32_array(link,
			"mipi-sdw-clock-frequencies-supported",
			bus->prop.freq, nval);

	}

	nval = fwnode_property_read_u32_array(link,
			"mipi-sdw-supported-clock-gears", NULL, 0);
	if (nval > 0)
		bus->prop.num_clk_gears = nval;

	if (bus->prop.num_clk_gears) {
		bus->prop.clk_gears = devm_kcalloc(bus->dev,
			sizeof(*bus->prop.clk_gears), nval, GFP_KERNEL);
		if (!bus->prop.clk_gears)
			return -ENOMEM;

		fwnode_property_read_u32_array(link,
				"mipi-sdw-supported-clock-gears",
				bus->prop.clk_gears, nval);
	}

	fwnode_property_read_u32(link,
			"mipi-sdw-default-frame-rate", &bus->prop.default_freq);

	fwnode_property_read_u32(link,
			"mipi-sdw-default-frame-row-size", &bus->prop.default_rows);

	fwnode_property_read_u32(link,
			"mipi-sdw-default-frame-col-size", &bus->prop.default_col);

	bus->prop.dynamic_frame =  fwnode_property_read_bool(link,
			"mipi-sdw-dynamic-frame-shape");

	fwnode_property_read_u32(link,
			"mipi-sdw-command-error-threshold", &bus->prop.err_threshold);

	return 0;
}
EXPORT_SYMBOL(sdw_master_read_prop);

static int sdw_slave_read_dpn(struct sdw_slave *slave,
		struct sdw_dpn_prop *dpn, int count, int ports, char *type)
{
	struct fwnode_handle *node;
	char name[40];
	unsigned long addr;
	u32 bit, i = 0, nval;

	addr = ports;
	for_each_set_bit(bit, &addr, 32) {
		snprintf(name, sizeof(name), "mipi-sdw-dp-%d-%s-subproperties",
				bit, type);

		node = device_get_named_child_node(&slave->dev, name);
		if (!node) {
			dev_err(&slave->dev,"%s dpN not found\n", name);
			return -EIO;
		}

		dpn[i].port = bit;

		fwnode_property_read_u32(node, "mipi-sdw-port-max-wordlength",
						&dpn[i].max_word);
		fwnode_property_read_u32(node, "mipi-sdw-port-min-wordlength",
						&dpn[i].min_word);

		nval = fwnode_property_read_u32_array(node,
				"mipi-sdw-port-wordlength-configs", NULL, 0);
		if (nval > 0)
			dpn[i].num_words = nval;

		if (dpn[i].num_words) {
			dpn[i].words = devm_kcalloc(&slave->dev,
					sizeof(*dpn[i].words), nval, GFP_KERNEL);
			if (!dpn[i].words)
				return -ENOMEM;

			fwnode_property_read_u32_array(node,
					"mipi-sdw-port-wordlength-configs",
					&dpn[i].num_words, nval);
		}

		fwnode_property_read_u32(node,
				"mipi-sdw-data-port-type", &dpn[i].type);
		fwnode_property_read_u32(node,
				"mipi-sdw-max-grouping-supported",
				&dpn[i].max_grouping);
		dpn[i].simple_ch_prep_sm = fwnode_property_read_bool(node,
				"mipi-sdw-simplified-channelprepare-sm");
		fwnode_property_read_u32(node,
				"mipi-sdw-port-channelprepare-timeout",
				&dpn[i].ch_prep_timeout);
		fwnode_property_read_u32(node,
				"mipi-sdw-imp-def-dpn-interrupts-supported",
				&dpn[i].device_interrupts);
		fwnode_property_read_u32(node,
				"mipi-sdw-min-channel-number",
				&dpn[i].min_ch);
		fwnode_property_read_u32(node,
				"mipi-sdw-max-channel-number",
				&dpn[i].max_ch);

		nval = fwnode_property_read_u32_array(node,
				"mipi-sdw-channel-number-list", NULL, 0);
		if (nval > 0)
			dpn[i].num_ch = nval;

		if (dpn[i].num_ch) {
			dpn[i].ch = devm_kcalloc(&slave->dev,
					sizeof(*dpn[i].ch), nval, GFP_KERNEL);
			if (!dpn[i].ch)
				return -ENOMEM;

			fwnode_property_read_u32_array(node,
					"mipi-sdw-channel-number-list",
					dpn[i].ch, nval);
		}

		nval = fwnode_property_read_u32_array(node,
				"mipi-sdw-channel-combination-list", NULL, 0);
		if (nval > 0)
			dpn[i].num_ch_combinations = nval;

		if (dpn[i].num_ch_combinations) {
			dpn[i].ch_combinations = devm_kcalloc(&slave->dev,
					sizeof(*dpn[i].ch_combinations),
					nval, GFP_KERNEL);
			if (!dpn[i].ch_combinations)
				return -ENOMEM;

			fwnode_property_read_u32_array(node,
					"mipi-sdw-channel-combination-list",
					dpn[i].ch_combinations, nval);
		}

		fwnode_property_read_u32(node,
				"mipi-sdw-modes-supported", &dpn[i].modes);
		fwnode_property_read_u32(node,
				"mipi-sdw-max-async-buffer",
				&dpn[i].max_async_buffer);
		dpn[i].block_pack_mode = fwnode_property_read_bool(node,
				"mipi-sdw-block-packing-mode");

		fwnode_property_read_u32(node,
				"mipi-sdw-port-encoding-type",
				&dpn[i].port_encoding);

		i++;
	}
	return 0;
}

int sdw_slave_read_prop(struct sdw_slave *slave)
{
	struct sdw_slave_prop *prop = &slave->prop;
	struct device *dev = &slave->dev;
	struct fwnode_handle *port;
	bool b_val;
	int nval;

	device_property_read_u32(dev,
			"mipi-sdw-sw-interface-revision", &prop->mipi_revision);

	prop->wake_capable = device_property_read_bool(dev,
				"mipi-sdw-wake-up-unavailable");
	prop->wake_capable = !prop->wake_capable;

	prop->test_mode_capable = device_property_read_bool(dev,
			"mipi-sdw-test-mode-supported");

	b_val = device_property_read_bool(dev,
			"mipi-sdw-clock-stop-mode1-supported");
	if (b_val)
		prop->clk_stop_mode1 = true;
	else
		prop->clk_stop_mode1 = false;

	prop->simple_clk_stop_capable = device_property_read_bool(dev,
			"mipi-sdw-simplified-clockstopprepare-sm-supported");

	device_property_read_u32(dev, "mipi-sdw-clockstopprepare-timeout",
				&prop->clk_stop_timeout);

	device_property_read_u32(dev, "mipi-sdw-slave-channelprepare-timeout",
				&prop->ch_prep_timeout);

	device_property_read_u32(dev,
			"mipi-sdw-clockstopprepare-hard-reset-behavior",
			&prop->reset_behave);

	prop->high_PHY_capable = device_property_read_bool(dev,
			"mipi-sdw-highPHY-capable");

	prop->paging_support = device_property_read_bool(dev,
			"mipi-sdw-paging-support");

	prop->bank_delay_support = device_property_read_bool(dev,
			"mipi-sdw-bank-delay-support");

	device_property_read_u32(dev,
			"mipi-sdw-port15-read-behavior", &prop->p15_behave);

	device_property_read_u32(dev, "mipi-sdw-master-count",
				&prop->master_count);


	device_property_read_u32(dev, "mipi-sdw-source-port-list",
				&prop->source_ports);

	device_property_read_u32(dev, "mipi-sdw-sink-port-list",
				&prop->sink_ports);


	/* now read the dp0 properties */
	port = device_get_named_child_node(dev, "mipi-sdw-dp-0-subproperties");
	if (!port) {
		dev_err(dev, "DP0 node not found!!\n");
		return -EIO;
	}

	prop->dp0_prop = devm_kzalloc(&slave->dev,
			sizeof(*prop->dp0_prop), GFP_KERNEL);
	if (!prop->dp0_prop)
		return -ENOMEM;

	fwnode_property_read_u32(port, "mipi-sdw-port-max-wordlength",
			&prop->dp0_prop->max_word);

	fwnode_property_read_u32(port, "mipi-sdw-port-min-wordlength",
			&prop->dp0_prop->min_word);

	nval = fwnode_property_read_u32_array(port,
			"mipi-sdw-port-wordlength-configs", NULL, 0);
	if (nval > 0)
		prop->dp0_prop->num_words = nval;

	if (prop->dp0_prop->num_words) {
		prop->dp0_prop->words = devm_kcalloc(&slave->dev,
			sizeof(*prop->dp0_prop->words), nval, GFP_KERNEL);
		if (!prop->dp0_prop->words)
			return -ENOMEM;

		fwnode_property_read_u32_array(port,
				"mipi-sdw-port-wordlength-configs",
				prop->dp0_prop->words, nval);
	}

	prop->dp0_prop->flow_controlled = fwnode_property_read_bool(port,
			"mipi-sdw-bra-flow-controlled");

	prop->dp0_prop->simple_ch_prep_sm = fwnode_property_read_bool(port,
			"mipi-sdw-simplified-channel-prepare-sm");

	prop->dp0_prop->device_interrupts = fwnode_property_read_bool(port,
			"mipi-sdw-imp-def-dp0-interrupts-supported");


	/* now based on each DPn we find the src dpn properties */

	/* first we need to allocate memory for set bits in port lists */
	nval = hweight32(prop->source_ports);
	prop->src_dpn_prop = devm_kcalloc(&slave->dev,
			sizeof(*prop->src_dpn_prop), nval, GFP_KERNEL);
	if (!prop->src_dpn_prop)
		return -ENOMEM;

	/* call helper to read */
	sdw_slave_read_dpn(slave, prop->src_dpn_prop,
			nval, prop->source_ports, "source");

	/* do this again for sink now */

	nval = hweight32(prop->sink_ports);
	prop->sink_dpn_prop = devm_kcalloc(&slave->dev,
			sizeof(*prop->sink_dpn_prop), nval, GFP_KERNEL);
	if (!prop->sink_dpn_prop)
		return -ENOMEM;

	/* call helper to read */
	sdw_slave_read_dpn(slave, prop->sink_dpn_prop,
			nval, prop->sink_ports, "sink");

	return 0;
}
