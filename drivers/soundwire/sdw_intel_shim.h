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

#ifndef __SDW_INTEL_SHIM_H
#define __SDW_INTEL_SHIM_H

#define SDW_MAX_LINKS		4

/**
 * enum sdw_ishim_sync: Sync register operations. These are various sync
 * operations which are required to be performed at different stages to
 * configure the Intel SHIM.
 *
 * @SDW_ISHIM_SYNCPRD: Set the Sync period
 * @SDW_ISHIM_SYNCGO: Set the Sync GO
 * @SDW_ISHIM_CMDSYNC: Set the CMDSYNC
 */
enum sdw_ishim_sync_ops {
	SDW_ISHIM_SYNCPRD = 0,
	SDW_ISHIM_SYNCGO,
	SDW_ISHIM_CMDSYNC,
};

/**
 * enum sdw_ireg_type: register type for shim configuration
 *
 * @SDW_REG_ISHIM: register type shim
 * @SDW_REG_IALH: register type ALH (audio link hub)
 */
enum sdw_ireg_type {
	SDW_REG_ISHIM = 0,
	SDW_REG_IALH = 1,
};

/**
 * struct sdw_cdns_stream_config: stream configuration
 *
 * @pcm_bd: number of bidirectional pcm streams supported
 * @pcm_in: number of input pcm streams supported
 * @pcm_out: number of output pcm streams supported
 * @pdm_bd: number of bidirectional pdm streams supported
 * @pdm_in: number of input pdm streams supported
 * @pdm_out: number of output pdm streams supported
 */
struct sdw_cdns_stream_config {
	unsigned int pcm_bd;
	unsigned int pcm_in;
	unsigned int pcm_out;
	unsigned int pdm_bd;
	unsigned int pdm_in;
	unsigned int pdm_out;
};

/**
 * struct sdw_cdns_pdi: pdi instance
 *
 * @assigned: is pdi assigned to a port
 * @pdi_num: pdi number
 * @stream_num: stream number
 * @l_ch_num: low channel for given PDI
 * @h_ch_num: high channel for given PDI
 * @ch_count: total channel count for the given PDI
 * @dir: data direction, input or output
 * @type: stream type PDM or PCM
 */
struct sdw_cdns_pdi {
	bool assigned;
	int pdi_num;
	int stream_num;
	int l_ch_num;
	int h_ch_num;
	int ch_count;
	enum sdw_data_direction dir;
	enum sdw_stream_type type;
};

struct sdw_ilink_data {
	struct platform_device *pdev;
	void __iomem *shim;
	void __iomem *alh;
};

struct sdw_ishim;

/**
 * struct sdw_ishim_ops: Callback operations for Cadence driver to invoke for Shim
 * configuration
 *
 * @link_power_down: Powers down the given link in shim
 * @link_power_up: Powers up the given link in shim
 * @init: Initialize and do configuration to shim after power up. Can be
 * invoked in driver probe and pm resume flows.
 * @sync: perform the given sync operation on the shim
 * @pdi_init: Initializes the Physical Data Interface (PDI)
 * @pdi_ch_cap: Query the channel capability of a given PDI in a Link in Shim
 * @pdi_conf: Query the PDI configuration of a given Link in Shim
 * @wake: wake up/down the shim for a given link in shim
 * @config_pdi: configure the pdi for a given a link
 * @config_stream: configure the stream with given hw_params
 */
struct sdw_ishim_ops {
	int (*link_power_down)(struct sdw_ishim *shim, unsigned int link_id);
	int (*link_power_up)(struct sdw_ishim *shim, unsigned int link_id);
	void (*init)(struct sdw_ishim *shim, unsigned int link_id);
	int (*sync)(struct sdw_ishim *shim, unsigned int link_id,
					enum sdw_ishim_sync_ops ops);
	void (*pdi_init)(struct sdw_ishim *shim, unsigned int link_id,
				struct sdw_cdns_stream_config *config);
	int (*pdi_ch_cap)(struct sdw_ishim *shim, unsigned int link_id,
					unsigned int pdi_num, bool pcm);
	int (*pdi_conf)(struct sdw_ishim *shim, unsigned int link_id,
					struct sdw_cdns_pdi *pdi,
					enum sdw_ireg_type reg_type);
	void (*wake)(struct sdw_ishim *shim, unsigned int link_id,
						bool wake_enable);
	int (*config_pdi)(struct sdw_ishim *shim, unsigned int link_id,
				struct sdw_cdns_pdi *pdi);
	int (*config_stream)(struct sdw_ishim *shim, unsigned int link_id,
				void *substream, void *dai, void *hw_params);
};

/**
 * struct sdw_ilink_res: Soundwire link resources
 * @registers: link IO registers base
 * @irq: interrupt line
 * @shim: shim pointer
 * @ops: shim callback ops
 *
 * this is set as pdata for each link instance so the link driver can
 * configure itself
 */
struct sdw_ilink_res {
	void __iomem *registers;
	int irq;
	struct sdw_ishim *shim;
	const struct sdw_ishim_ops *ops;
};

#endif /* __SDW_INTEL_SHIM_H */
