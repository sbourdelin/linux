/*
 * sdw_master.h - Definition for SoundWire Master interface. This file
 *	has all the definitions which are required for only Soundwire
 *	Master driver. Some interfaces are common for both Slave and
 *	Master driver. Please refer to sdw_bus.h for common interfaces.
 *
 * Author: Hardik Shah <hardik.t.shah@intel.com>
 *
 *
 * This header file refers to the MIPI SoundWire 1.1 Spec.
 * [1.1] https://members.mipi.org/wg/All-Members/document (accessible to
 * MIPI members).
 *
 * The comments in the file try to follow the same conventions with a
 * capital letter for all standard definitions such as Master, Slave,
 * Data Port, etc. When possible, the constant numeric values are kept
 * the same as in the MIPI specifications. All of the constants reflect
 * the MIPI SoundWire definitions and are not vendor specific. Some of
 * the constants are for bus driver usage and not MIPI defined. These
 * are specified at appropriate place in this file.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2016 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 *
 * BSD LICENSE
 *
 * Copyright(c) 2016 Intel Corporation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 */

#ifndef _LINUX_SDW_MASTER_H
#define _LINUX_SDW_MASTER_H

#include <sound/sdw_bus.h>

/**
 * sdw_deferred_xfer_data: Data to be provided by bus driver for calling
 *	xfer_msg_deferred callback of Master driver.
 *
 * @result: Result of the asynchronous transfer.
 * @xfer_complete: Bus driver will wait on this. Master needs to ack on this
 *	for transfer complete.
 *
 * @msg: Message to be transferred.
 */
struct sdw_deferred_xfer_data {
	int result;
	struct completion xfer_complete;
	struct sdw_msg *msg;
};

/**
 * sdw_master_dp0_caps: Master Data Port 0 capabilities.
 *
 * @max_bps: Maximum bits per sample supported by Port. This is same as word
 *	length in SoundWire Spec.
 *
 * @min_bps: Minimum bits per sample supported by Port. This is same as word
 *	length in SoundWire Spec.
 *
 * @num_bps: Array size of the buffer containing the supported bps use of
 *	max and min bps requires num_bps to be 0
 *
 * @bps_buf: Array containing supported sample sizes.
 * @bra_max_data_per_frame: Maximum Data size per BRA(Bulk Access Register)
 *	packet.
 */
struct sdw_master_dp0_caps {
	unsigned int max_bps;
	unsigned int min_bps;
	unsigned int num_bps;
	unsigned int *bps_buf;
	unsigned int bra_max_data_per_frame;
};

/**
 * sdw_master_caps: Capabilities of the Master. This is filled by the
 *	software registering Master device like board or device entry table.
 *	All fields are listed in order of the SoundWire DisCo Spec. Name of
 *	the fields are altered to follow Linux convention. All the fields
 *	which are required by bus driver but not present in DisCo Spec are
 *	listed at the end of the structure.
 *
 * @clk_stp_mode0: True if Master interface supports ClockStop Mode0
 * @clk_stp_mode1: True if Master interface supports ClockStop Mode1. Both
 *	of these fields are not used by bus driver as of now. This will be
 *	implemented in later version of the bus driver.
 *
 * @max_clk_freq: Max soundwire clock frequency at SDW clock line. This is
 *	in Hz.
 *
 * @num_clk_gears: Number of clock gears supported by Master.
 * @clk_gears: A array containing clock gears integer supported by Master.
 * @num_clk_freq: Number of clock frequencies supported by Master. This is
 *	alternate way of representing the clock gear. To use this
 *	num_clock_gears must be 0.
 *
 * @clk_freq_buf: Array representing the clock frequencies supported by the
 *	Master.
 *
 * @def_frame_rate: Master default frame rate in Hz. The frame rate defines
 *	the control bandwidth on a SoundWire bus.
 *
 * @def_frame_row_size: Number of rows 47<n<257. Not all values are valid as
 *	per Table 19 of the SoundWire MIPI specification Version 1.1.
 *
 * @def_frame_col_size: Number of rows 1<n<17. Only even numbers are
 *	valid.
 *
 * @dynamic_frame_shape: If false, bus driver may not change the frame shape
 *	dynamically and must only use the default/initial settings.
 *
 * @command_error_threshold: Number of times software may retry sending a
 *	single command. Once the threshold has been reached, an error
 *	condition exists and the command should not be retried.
 *
 * @bank_switch_timeout: Bus driver to wait for the specific timeout for
 *	bank switch to complete, after initiating the the bank switch
 *	operation. This is in milli-seconds.
 *
 * @monitor_handover_supported: Does Master support monitor handover.
 * @highphy_capable: Is Master High-PHY capable? Basic-PHY and High-PHY are
 *	two modes a device can run. Bus driver doesn't support High-PHY mode
 *	as of now.
 *
 * @sdw_dp0_present: Data port0 present?
 * @sdw_dp0_caps: Capabilities of the Data Port 0 of the Master.
 * @num_data_ports: Array size for the number of Data Ports present in
 *	Master.
 *
 * @sdw_dpn_caps: Array containing information about SoundWire Master Data
 *	Port Capabilities.
 */
struct sdw_master_caps {
	bool clk_stp_mode0;
	bool clk_stp_mode1;
	unsigned int max_clk_freq;
	unsigned int num_clk_gears;
	unsigned int *clk_gears;
	unsigned int num_clk_freq;
	unsigned int *clk_freq_buf;
	unsigned int def_frame_rate;
	unsigned int def_frame_row_size;
	unsigned int def_frame_col_size;
	bool dynamic_frame_shape;
	unsigned int command_error_threshold;
	unsigned int bank_switch_timeout;
	bool monitor_handover_supported;
	bool highphy_capable;
	bool sdw_dp0_present;
	struct sdw_master_dp0_caps sdw_dp0_caps;
	unsigned int num_data_ports;
	struct sdw_dpn_caps *sdw_dpn_caps;
};

/**
 * sdw_master: Structure representing the Master interface of the SoundWire.
 *
 * @dev: Master interface for this driver.
 * @bus: Bus handle for easy access of bus from Master.
 * @name: Name of the Master driver.
 * @nr: Master instance number. This represents SoundWire bus. This is
 *	logical number incremented per every Master getting registered.
 *
 * @timeout: Timeout before getting message response, in ms.
 * @retries: How many times to retry before giving up on Slave response.
 * @link_sync_mask: Bit mask representing all the other Master links with
 *	which this link is synchronized.
 *
 * @slv_list: List of SoundWire Slaves registered to the bus.
 * @sdw_addr: Array containing Slave SoundWire bus Slave address
 *	information. Its a part of Slave list as well, but for easier access
 *	its part of array to find the Slave reference by directly indexing
 *	the Slave number into the array.
 *
 * @lock: Global lock for bus functions. This lock is used for
 *	following
 *	1. Enumerating the Slaves and adding it to the Master list.
 *	2. Bus reconfiguration operations.
 *
 * @msg_lock: Used to serialize the messages on the bus.
 * @caps: Capabilities of the SoundWire Master interface.
 * @driver: Driver handling the Master.
 * @slv_released_complete: Flag to indicate Slave release completion.
 *	Internally used by bus driver. This is to make sure that Master is
 *	freed only after all Slaves devices are freed.
 *
 * @link_id: This is the link_id of the Master. This has direct relationship
 *	with the Slaves address. This is provided by the software
 *	registering the Master. This is read either from ACPI or from board
 *	data.
 *
 * @num_slv: Number of SoundWire Slaves assigned DeviceNumber after
 *	enumeration. The SoundWire specification does not place a
 *	restriction on how many Slaves are physically connected, as long as
 *	only 11 are active concurrently. This bus driver adds a pragmatic
 *	restriction to 11 Slaves, the current implementation assigns a
 *	DeviceNumber once and will never use the same DeviceNumber for
 *	different devices.
 */
struct sdw_master {
	struct device dev;
	struct sdw_bus *bus;
	char name[SOUNDWIRE_NAME_SIZE];
	unsigned int nr;
	unsigned int timeout;
	unsigned int retries;
	unsigned int link_sync_mask;
	struct list_head slv_list;
	struct sdw_slave_addr sdw_addr[SDW_MAX_DEVICES];
	struct mutex lock;
	struct mutex msg_lock;
	struct sdw_master_caps caps;
	struct sdw_master_driver *driver;
	struct completion slv_released_complete;
	struct list_head mstr_rt_list;
	unsigned int link_id;
	unsigned int num_slv;
};
#define to_sdw_master(d) container_of(d, struct sdw_master, dev)

/**
 * sdw_port_params: This is used to program the Data Port based on Data Port
 *	stream params. These parameters are not banked and not expected to
 *	change dynamically to avoid audio artifacts.
 *
 * @num: Port number.
 * @bps: Word length of the Port
 * @flow_mode: Port Data flow mode.
 * @data_mode: Test modes or normal mode.
 */
struct sdw_port_params {
	unsigned int num;
	unsigned int bps;
	unsigned int flow_mode;
	unsigned int data_mode;
};

/**
 * sdw_transport_params: This is used to program the Data Port based on Data
 *	Port transport params. All these parameters are banked and can be
 *	modified during a bank switch without any artefact's in audio
 *	stream. Bus driver modifies these parameters as part new stream
 *	getting enabled/disabled on the bus. Registers are explained next to
 *	each field where values will be filled. Those are MIPI defined
 *	registers for Slave devices.
 *
 * @blk_grp_ctrl_valid: Does Port implement block group control?
 * @port_num: Port number for which params are to be programmed.
 * @blk_grp_ctrl: Block group control value.
 * @sample_interval: Sample interval.
 * @offset1: Blockoffset of the payload Data.
 * @offset2: Blockoffset of the payload Data.
 * @hstart: Horizontal start of the payload Data.
 * @hstop: Horizontal stop of the payload Data.
 * @blk_pkg_mode: Block per channel or block per Port.
 * @lane_ctrl: Data lane Port uses for Data transfer. Currently only single
 *	data lane is supported in bus driver.
 */
struct sdw_transport_params {
	bool blk_grp_ctrl_valid;
	unsigned int port_num;
	unsigned int blk_grp_ctrl;	/* DPN_BlockCtrl2 */
	unsigned int sample_interval;	/* DPN_SampleCtrl1 & DPN_SampleCtrl2 */
	unsigned int offset1;		/* DPN_OffsetCtrl1 */
	unsigned int offset2;		/* DPN_OffsetCtrl2 */
	unsigned int hstart;		/* DPN_HCtrl */
	unsigned int hstop;		/* DPN_HCtrl */
	unsigned int blk_pkg_mode;	/* DPN_BlockCtrl3 */
	unsigned int lane_ctrl;		/* DPN_LaneCtrl */
};

/**
 * sdw_enable_ch: Enable/disable Data Port channel. This is for triggering
 *	On/Off the Port. This readily translates to the trigger functions of
 *	ALSA. Actual trigger/enabling of the channels of the Port is done
 *	through with a bank switch since transport parameters may need to
 *	change due to a clock or frame shape change.
 *
 * @num: Port number for which params are to be programmed.
 * @ch_mask: Active channel mask for this Port.
 * @enable: Enable/disable channel, true (enable) or false
 *	(disable).
 */
struct sdw_enable_ch {
	unsigned int num;
	unsigned int ch_mask;
	bool enable;
};

/**
 * sdw_master_port_ops: Callback functions from bus driver to Master driver
 *	to set Master Data ports. Since Master registers are not standard,
 *	commands are passed to Master from bus and Master converts commands
 *	to register settings based on Master register map.
 *
 * @dpn_set_port_params: Set the Port parameters for the Master Port. This
 *	is mandatory callback to be provided by Master, if it support data
 *	ports.
 *
 * @dpn_set_port_transport_params: Set transport parameters for the Master
 *	Port. This is mandatory callback to be provided by Master if it
 *	supports data ports.
 *
 * @dpn_port_prep: Port prepare operations for the Master Data Port. Called
 *	before and after Port prepare as well as for Master Data Port
 *	prepare.
 *
 * @dpn_port_enable_ch: Enable the channels of particular Master Port.
 *	Actual enabling of the port is done as a part of bank switch This
 *	call is to enable the channels in alternate bank. This is mandatory
 *	if Master supports data ports.
 */
struct sdw_master_port_ops {
	int (*dpn_set_port_params)(struct sdw_master *master,
			struct sdw_port_params *port_params, unsigned int bank);
	int (*dpn_set_port_transport_params)(struct sdw_master *master,
			struct sdw_transport_params *transport_params,
			unsigned int bank);
	int (*dpn_port_prep)(struct sdw_master *master,
				struct sdw_prepare_ch *prepare_ch,
				enum sdw_port_prep_ops prep_ops);
	int (*dpn_port_enable_ch)(struct sdw_master *master,
			struct sdw_enable_ch *enable_ch, unsigned int bank);
};

/**
 * sdw_master_ops: Callback operations from bus driver to Master driver.
 *	Bus driver calls these functions to control the bus parameters in
 *	Master hardware specific way. Its like i2c_algorithm to access the
 *	bus in Master specific way.
 *
 * @xfer_msg: Callback function to Master driver to read/write Slave
 *	registers. This is mandatory callback to be provided by Master
 *	driver to do Slave register read/writes.
 *
 * @reset_page_addr: Callback function to Master driver to reset the SCP
 *	page address registers of Slave. This is called by bus driver after
 *	message transfer, to reset the page address registers to 0. This is
 *	to make sure MIPI defined Slave registers are accessed in fast
 *	manner as all the MIPI defined Slave registers are till 0xFFFF,
 *	where SCP page address is always 0. xfer_msg callback can also be
 *	used to reset page address with dummy message. But this callback is
 *	more explicit where master driver is informed to reset the page
 *	address registers of the Slave. This is mandatory callback to be
 *	provided by Master driver.
 *
 * @xfer_bra: Callback function to Master driver for BRA transfer. This is
 *	required if BRA is supported on DP0.
 *
 * @monitor_handover: Allow monitor to be owner of command, if requested.
 *	This is optional based on Master support for handovers.
 *
 * @set_ssp_interval: Set SSP interval. This is mandatory callback to be
 *	provided by Master driver.
 *
 * @set_bus_params: Set the clock frequency and frame shape based on
 *	bandwidth requirement. Master driver sets the frequency and frame
 *	shape in hardware specific way. This is mandatory if Master supports
 *	dynamic frame shape and clock scaling.
 *
 * @pre_bank_switch: This is required to be implemented by Master driver if
 *	stream is handled by more than one Master. This is called by the the
 *	bus driver before bank switch operation is initiated. If there are
 *	two masters handling the stream, for both masters this will be
 *	called before scheduling bank switch operation. Normally master
 *	prepares itself in this call for synchronous bank switch.
 *
 * @xfer_msg_deferred: This is required to be implemented by Master driver
 *	if stream is handled by more than one Master interface. Bus driver
 *	initiates bank switch and wait for the bank switch to be completed.
 *	For streams handled by multiple Masters, bus driver initiates bank
 *	switch using this API for all Masters of the associated with the
 *	stream and waits for bank switch to be completed on all Masters. Its
 *	up to master implementation on how to synchronize bank switches on
 *	Multiple masters.
 *
 * @post_bank_switch: This is required to be implemented by Master driver if
 *	stream is handled by more than one Master. This is called by the bus
 *	driver after bank switch operation is initiated. If there are two
 *	masters handling the stream, for both masters this will be called
 *	after initiating bank switch operation. Normally master completes
 *	the bank switch operation on all the Masters associated with this
 *	stream in this call.
 */
struct sdw_master_ops {
	enum sdw_command_response (*xfer_msg)(struct sdw_master *master,
		struct sdw_msg *msg, bool program_scp_addr_page);
	enum sdw_command_response (*reset_page_addr)(struct sdw_master *master,
			unsigned int dev_num);
	int (*xfer_bra)(struct sdw_master *master,
		struct sdw_bra_block *block);
	int (*monitor_handover)(struct sdw_master *master,
		bool handover);
	int (*set_ssp_interval)(struct sdw_master *master,
			unsigned int ssp_interval, unsigned int bank);
	int (*set_bus_params)(struct sdw_master *master,
			struct sdw_bus_params *params);
	int (*pre_bank_switch)(struct sdw_master *master);
	void (*xfer_msg_deferred)(struct sdw_master *master,
		struct sdw_msg *msg, bool program_scp_addr_page,
		struct sdw_deferred_xfer_data *data);
	int (*post_bank_switch)(struct sdw_master *master);
};

/**
 * sdw_master_driver: Manage SoundWire Master device driver.
 *
 * @driver_type: To distinguish between Master and Slave driver. This should
 *	be set by driver based on its handling Slave or Master SoundWire
 *	interface.
 *
 * @probe: Binds this driver to a SoundWire Master.
 * @remove: Unbinds this driver from the SoundWire Master.
 * @shutdown: Standard shutdown callback used during power down/halt.
 * @suspend: Standard suspend callback used during system suspend
 * @resume: Standard resume callback used during system resume
 * @driver: SoundWire device drivers should initialize name and owner field
 *	of this structure.
 *
 * @ops: Callback operations from bus driver to Master driver for
 *	programming and controlling bus parameters and to program Slave
 *	registers.
 *
 * @port_ops: Commands to setup the Master ports. Master register map is
 *	not defined by standard. So these ops represents the commands to
 *	setup Master ports.
 *
 * @id_table: List of SoundWire devices supported by this driver. This list
 *	should be NULL terminated.
 */
struct sdw_master_driver {
	enum sdw_driver_type driver_type;
	struct device_driver driver;
	int (*probe)(struct sdw_master *master, const struct sdw_master_id *);
	int (*remove)(struct sdw_master *master);
	void (*shutdown)(struct sdw_master *master);
	int (*suspend)(struct sdw_master *master);
	int (*resume)(struct sdw_master *master);
	struct sdw_master_ops *ops;
	struct sdw_master_port_ops *port_ops;
	const struct sdw_master_id *id_table;
};
#define to_sdw_master_driver(d)			\
		container_of(d, struct sdw_master_driver, driver)

/**
 * snd_sdw_master_add: Registers the SoundWire Master interface. This needs
 *	to be called for each Master interface supported by SoC. This
 *	represents One clock and data line (Optionally multiple data lanes)
 *	of Master interface.
 *
 * @master: the Master to be added.
 */
int snd_sdw_master_add(struct sdw_master *master);

/**
 * snd_sdw_master_del - unregister SDW Master
 *
 * @master: the Master being unregistered
 */
void snd_sdw_master_del(struct sdw_master *master);

/**
 * snd_sdw_master_register_driver: SoundWire Master driver registration with
 *	bus. This API will register the Master driver with the SoundWire
 *	bus. It is typically called from the driver's module-init function.
 *
 * @driver: Master Driver to be associated with Master interface.
 * @owner: Module owner, generally THIS module.
 */
int snd_sdw_master_register_driver(struct sdw_master_driver *driver,
					struct module *owner);

/**
 * sdw_unregister_master_driver: Undo effects of
 *	snd_sdw_master_driver_register
 *
 * @drv: SoundWire Master driver to be unregistered
 */
static inline void sdw_master_unregister_driver(struct sdw_master_driver *drv)
{
	driver_unregister(&drv->driver);
}

/**
 * snd_sdw_master_update_slave_status: Update the status of the Slave to the
 *	bus driver. Master calls this function based on the interrupt it
 *	gets once the Slave changes its state or from interrupts for the
 *	Master hardware that caches status information reported in PING
 *	commands.
 *
 * @master: Master handle for which status is reported.
 * @status: Array of status of each Slave.
 *
 * This function can be called from interrupt context by Master driver to
 *	report Slave status without delay.
 */
int snd_sdw_master_update_slave_status(struct sdw_master *master,
				struct sdw_status *status);

/**
 * snd_sdw_master_get: Return the Master handle from Master number.
 *	Increments the reference count of the module. Similar to
 *	i2c_get_adapter.
 *
 * @nr: Master number.
 *
 * Returns Master handle on success, else NULL
 */
struct sdw_master *snd_sdw_master_get(int nr);

/**
 * snd_sdw_master_put: Reverses the effect of sdw_master_get
 *
 * @master: Master handle.
 */
void snd_sdw_master_put(struct sdw_master *master);

/**
 * snd_sdw_master_prepare_for_clk_stop: Prepare all the Slaves for clock
 *	stop. Iterate through each of the enumerated Slaves. Prepare each
 *	Slave according to the clock stop mode supported by Slave. Use
 *	dynamic value from Slave callback if registered, else use static
 *	values from Slave capabilities registered.
 *	1. Get clock stop mode for each Slave.
 *	2. Call pre_prepare callback of each Slave if registered.
 *	3. Write ClockStopPrepare bit in SCP_SystemCtrl register for each of
 *	the enumerated Slaves.
 *	4. Broadcast the read message to read the SCP_Stat register to make
 *	sure ClockStop Prepare is finished for all Slaves.
 *	5. Call post_prepare callback of each Slave if registered after
 *	Slaves are in ClockStopPrepare state.
 *
 * @master: Master handle for which clock state has to be changed.
 */
int snd_sdw_master_prepare_for_clk_stop(struct sdw_master *master);

/**
 * snd_sdw_master_deprepare_after_clk_start: De-prepare all the Slaves
 *	exiting clock stop mode 0 after clock resumes. Clock is already
 *	resumed before this. De-prepare for the Slaves which were there in
 *	clock stop mode 1 is done after they enumerated back. This is because
 *	Slave specific callbacks needs to be invoked as part of de-prepare,
 *	which can be invoked only after Slave enumerates.
 *	1. Get clock stop mode for each Slave.
 *	2. Call pre_prepare callback of each Slave exiting from clock stop
 *	mode 0.
 *	3. De-Prepare each Slave exiting from clock stop mode 0
 *	4. Broadcast the Read message to make sure all Slaves are
 *	de-prepared for clock stop.
 *	5. Call post_prepare callback of each Slave exiting from clock stop
 *	mode0
 *
 * @master: Master handle
 */
int snd_sdw_master_deprepare_after_clk_start(struct sdw_master *master);

/**
 * snd_sdw_master_stop_clock: Stop the clock. This function broadcasts the
 *	SCP_CTRL register with clock_stop_now bit set.
 *
 * @master: Master handle for which clock has to be stopped.
 */
int snd_sdw_master_stop_clock(struct sdw_master *master);

/* Return the adapter number for a specific adapter */
static inline int sdw_master_get_id(struct sdw_master *master)
{
	return master->nr;
}

static inline void *sdw_master_get_drvdata(const struct sdw_master *master)
{
	return dev_get_drvdata(&master->dev);
}

static inline void sdw_master_set_drvdata(struct sdw_master *master,
							void *data)
{
	dev_set_drvdata(&master->dev, data);
}
#endif /*  _LINUX_SDW_MASTER_H */
