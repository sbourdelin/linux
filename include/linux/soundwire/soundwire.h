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

#ifndef __SOUNDWIRE_H
#define __SOUNDWIRE_H

#include <linux/device.h>
#include <linux/mod_devicetable.h>
#include <linux/soundwire/sdw_registers.h>

struct sdw_bus;
struct sdw_slave;

/* sdw spec defines and enums, as defined by MIPI 1.1. Spec */

/* SDW Broadcast addr */
#define SDW_BROADCAST_ADDR		15

/* SDW Enumeration addr */
#define SDW_ENUM_ADDR			0

/* frame shape defines */
#define SDW_FRAME_MAX_ROWS		23
#define SDW_FRAME_MAX_COLS		8
#define SDW_FRAME_ROW_COLS		(SDW_FRAME_MAX_ROWS * SDW_FRAME_MAX_COLS)
#define SDW_FRAME_CTRL_BITS		48

#define SDW_NUM_DEV_ID_REGISTERS	6
#define SDW_MAX_DEVICES			11

enum {
	SDW_PORT_DIRN_SINK = 0,
	SDW_PORT_DIRN_SOURCE,
	SDW_PORT_DIRN_MAX,
};

/*
 * constants for flow control, ports and transport
 *
 * these are bit masks as devices can have multiple capabilities
 */

/*
 * flow modes for SDW port. These can be isochronous, tx controlled,
 * rx controlled or async
 */
#define SDW_PORT_FLOW_MODE_ISOCH	BIT(0)
#define SDW_PORT_FLOW_MODE_TX_CNTRL	BIT(1)
#define SDW_PORT_FLOW_MODE_RX_CNTRL	BIT(2)
#define SDW_PORT_FLOW_MODE_ASYNC	BIT(3)

/*
 * sample packaging for block. It can be per port or por channel
 */
#define SDW_PORT_PACKG_PER_PORT		BIT(0)
#define SDW_PORT_PACKG_PER_CH		BIT(1)

/*
 * Port encoding mask definitions, these are from DisCo spec
 * it can be 2's compliment, type sign magnitude or IEEE 32 float.
 */
#define SDW_PORT_ENC_2COMPL		BIT(0)
#define SDW_PORT_ENC_SIGN_MAGN		BIT(1)
#define SDW_PORT_ENC_IEEE_32FLOAT	BIT(2)

/**
 * enum sdw_slave_status: slave status
 *
 * @SDW_SLAVE_NOT_PRESENT: slave is not present on bus
 * @SDW_SLAVE_PRESENT: slave is attached to bus. This also means that salve
 * is synchronized to sdw clock
 * @SDW_SLAVE_ALERT: Some alert condition on the Slave
 */
enum sdw_slave_status {
	SDW_SLAVE_NOT_PRESENT = 0,
	SDW_SLAVE_PRESENT = 1,
	SDW_SLAVE_ALERT = 2,
	SDW_SLAVE_RESERVED = 3,
};

/**
 * enum sdw_command_response: Command response as defined by SDW spec
 *
 * @SDW_CMD_OK: cmd was okay
 * @SDW_CMD_IGNORED: cmd is ignored
 * @SDW_CMD_FAILED: cmd failed
 *
 * The enum is different than actual Spec as response in the Spec is
 * combination of ACK/NAK bits
 */
enum sdw_command_response {
	SDW_CMD_OK = 0,
	SDW_CMD_IGNORED = 1,
	SDW_CMD_FAILED = 2,
};

/**
 * enum sdw_dp_type: Data port types
 *
 * @SDW_DP_TYPE_FULL: Full Data Port is supported
 * @SDW_DP_TYPE_SIMPLE: Simplified Data Port as defined in spec.
 * DPN_SampleCtrl2, DPN_OffsetCtrl2, DPN_HCtrl and DPN_BlockCtrl3
 * are not implemented.
 * @SDW_DP_TYPE_REDUCED: Reduced Data Port as defined in spec.
 * DPN_SampleCtrl2, DPN_HCtrl are not implemented.
 */
enum sdw_dp_type {
	SDW_DP_TYPE_FULL = 0,
	SDW_DP_TYPE_SIMPLE = 1,
	SDW_DP_TYPE_REDUCED = 2,
};

/* block group count enum */
enum sdw_dpn_grouping {
	SDW_BLK_GRP_CNT_1 = 0,
	SDW_BLK_GRP_CNT_2 = 1,
	SDW_BLK_GRP_CNT_3 = 2,
	SDW_BLK_GRP_CNT_4 = 3,
};

/**
 * enum sdw_stream_type: data stream type
 *
 * @SDW_STREAM_PCM: PCM data stream
 * @SDW_STREAM_PDM: PDM data stream
 *
 * spec doesn't define this, but is used in implementation
 */
enum sdw_stream_type {
	SDW_STREAM_PCM = 0,
	SDW_STREAM_PDM = 1,
};

/**
 * enum sdw_ch_prepare_mode: Channel prepare modes
 *
 * @SDW_CH_PREP_SIMPLE: Simplified channel prepare state machine
 * @SDW_CH_PREP_NORMAL: Normal channel prepare state machine.
 */
enum sdw_ch_prepare_mode {
	SDW_CH_PREP_SIMPLE = 0,
	SDW_CH_PREP_NORMAL = 1,
};

/**
 * enum sdw_data_direction: Data direction w.r.t Port
 *
 * @SDW_DATA_DIR_IN: Data is going into Port
 * @SDW_DATA_DIR_OUT: Data is going out of Port
 *
 * For playback, data direction for the Master Port will be OUT as PORT is
 * doing tx. Slave port will be IN as PORT is doing rx
 */
enum sdw_data_direction {
	SDW_DATA_DIR_IN = 0,
	SDW_DATA_DIR_OUT = 1,
};

/**
 * enum sdw_port_data_mode: Data Port mode. It can be normal mode where audio
 * data is received and transmitted, or any of the 3 different test modes
 * Test modes are normally used for testing at component and system level.
 *
 * @SDW_PORT_DATA_MODE_NORMAL: Normal data mode where audio data is received
 * and transmitted.
 * @SDW_PORT_DATA_MODE_STATIC_1: Simple test mode which uses static value of
 * logic 1. The encoding will result in signal transitions at every bitslot
 * owned by this Port.
 * @SDW_PORT_DATA_MODE_STATIC_0: Simple test mode which uses static value of
 * logic 0. The encoding will result in no signal transitions
 * @SDW_PORT_DATA_MODE_PRBS: Test mode which uses a PRBS generator to produce
 * a pseudo random data pattern that is transferred
 */
enum sdw_port_data_mode {
	SDW_PORT_DATA_MODE_NORMAL = 0,
	SDW_PORT_DATA_MODE_STATIC_1 = 1,
	SDW_PORT_DATA_MODE_STATIC_0 = 2,
	SDW_PORT_DATA_MODE_PRBS = 3,
};

/**
 * enum sdw_port_prep_ops: Prepare operations for Master Data Ports
 *
 * @SDW_OPS_PORT_PRE_PREP: Pre prepare operation for the Ports.
 * @SDW_OPS_PORT_PREP: Prepare operation for the Ports.
 * @SDW_OPS_PORT_POST_PREP: Post prepare operation for the Ports.
 */
enum sdw_port_prep_ops {
	SDW_OPS_PORT_PRE_PREP = 0,
	SDW_OPS_PORT_PREP = 1,
	SDW_OPS_PORT_POST_PREP = 2,
};

/*
 * SDW properties, defined in MIPI DisCo spec v1.0
 */
enum sdw_clk_stop_reset_behave {
	SDW_CLK_STOP_KEEP_STATUS = 1,
};

enum sdw_p15_behave {
	SDW_P15_READ_IGNORED = 0,
	SDW_P15_CMD_OK = 1,
};

/**
 * struct sdw_dp0_prop: DP0 properties
 *
 * @max_word: Maximum number of bits in a Payload Channel Sample, 1 – 64
 * (inclusive)
 * @min_word: Maximum number of bits in a Payload Channel Sample, 1 – 64
 * (inclusive)
 * @num_words: number of wordlengths supported
 * @words: wordlengths supported
 * @flow_controlled: Can Slave implementation result in an OK_NotReady
 * response
 * @simple_ch_prep_sm: If channel prepare sequence is required
 * @device_interrupts: If implementation-defined interrupts are supported
 *
 * NOTE: the wordlengths are specified by Spec as max, min AND number of
 * discrete values, implementation can define based on the wordlengths they
 * support
 */
struct sdw_dp0_prop {
	u32 max_word;
	u32 min_word;
	u32 num_words;
	u32 *words;
	bool flow_controlled;
	bool simple_ch_prep_sm;
	bool device_interrupts;
};

enum sdw_dpn_type {
	SDW_DPN_FULL = 0,
	SDW_DPN_SIMPLE = 1,
	SDW_DPN_REDUCED = 2,
};

enum sdw_mode {
	SDW_MODE_ISOCHRONOUS = BIT(0),
	SDW_MODE_TX = BIT(1),
	SDW_MODE_RX = BIT(2),
	SDW_MODE_ASYNC = BIT(3),
};

/**
 * enum sdw_clk_stop_mode: Clock Stop modes
 *
 * @SDW_CLK_STOP_MODE_0: Clock Stop mode 0. This mode indicates Slave can
 * continue operation seamlessly on clock restart
 * @SDW_CLK_STOP_MODE_1: Clock Stop mode 1. Indicates Slave may have entered
 * a deeper power-saving mode, so is not capable of continuing operation
 * seamlessly when the clock restarts
 */
enum sdw_clk_stop_mode {
	SDW_CLK_STOP_MODE0 = 1,
	SDW_CLK_STOP_MODE1 = 2,
};

/**
 * struct sdw_dpn_audio_mode: Audio mode properties for DPn
 *
 * @bus_min_freq: Minimum bus frequency of this mode, in Hz
 * @bus_max_freq: Maximum bus frequency of this mode, in Hz
 * @bus_num_freq: Number of discrete frequency supported of this mode
 * @bus_freq: Discrete bus frequencies of this mode, in Hz
 * @bus_min_freq: Minimum sampling frequency of this mode, in Hz
 * @bus_max_freq: Maximum sampling bus frequency of this mode, in Hz
 * @bus_num_freq: Number of discrete sampling frequency supported of this mode
 * @bus_freq: Discrete sampling frequencies of this mode, in Hz
 * @prep_ch_behave: Specifies the dependencies between Channel Prepare
 * sequence and bus clock configuration
 * If 0, Channel Prepare can happen at any Bus clock rate
 * If 1, Channel Prepare sequence shall happen only after Bus clock is
 * changed to a frequency supported by this mode or compatible modes
 * described by the next field
 * @glitchless: Bitmap describing possible glitchless transitions from this
 * Audio Mode to other Audio Modes
 */
struct sdw_dpn_audio_mode {
	u32 bus_min_freq;
	u32 bus_max_freq;
	u32 bus_num_freq;
	u32 *bus_freq;
	u32 max_freq;
	u32 min_freq;
	u32 num_freq;
	u32 *freq;
	u32 prep_ch_behave;
	u32 glitchless;
};

/**
 * struct sdw_dpn_prop: Data Port DPn properties
 *
 * @port: port number
 * @max_word: Maximum number of bits in a Payload Channel Sample, 1 – 64
 * (inclusive)
 * @min_word: Minimum number of bits in a Payload Channel Sample, 1 – 64
 * (inclusive)
 * @num_words: Number of discrete supported wordlengths
 * @words: Discrete supported wordlength
 * @type: Data port type, Full or Simplified
 * @max_grouping: Maximum number of samples that can be grouped together for
 * a full data port
 * @simple_ch_prep_sm: If the channel prepare sequence is not required,
 * and the Port Ready interrupt is not supported
 * @ch_prep_timeout: Port-specific timeout value in milliseconds
 * @device_interrupts: If set, each bit corresponds to support for
 * implementation-defined interrupts
 * @max_ch: Minimum channels supported
 * @min_ch: Maximum channels supported
 * @num_ch: Number of discrete channels supported
 * @ch: Discrete channels supported
 * @num_ch_combinations: Number of channel combinations supported
 * @ch_combinations: Channel combinations supported
 * @modes: SDW mode supported
 * @max_async_buffer: Number of samples that this port can buffer in
 * asynchronous modes
 * @block_pack_mode: Type of block port mode supported
 * @port_encoding: Payload Channel Sample encoding schemes supported
 * @audio_mode: Audio mode supported
 */
struct sdw_dpn_prop {
	u32 port;
	u32 max_word;
	u32 min_word;
	u32 num_words;
	u32 *words;
	enum sdw_dpn_type type;
	u32 max_grouping;
	bool simple_ch_prep_sm;
	u32 ch_prep_timeout;
	u32 device_interrupts;
	u32 max_ch;
	u32 min_ch;
	u32 num_ch;
	u32 *ch;
	u32 num_ch_combinations;
	u32 *ch_combinations;
	enum sdw_mode modes;
	u32 max_async_buffer;
	bool block_pack_mode;
	u32 port_encoding;
	struct sdw_dpn_audio_mode audio_mode;
};

/**
 * struct sdw_slave_prop: SoundWire Slave properties
 *
 * @mipi_revision: Spec version of the implementation
 * @wake_capable: If wake-up events are supported
 * @test_mode_capable: If test mode is supported
 * @clk_stop_mode1: If Clock-Stop Mode 1 is supported
 * @simple_clk_stop_capable: If Simple clock mode is supported
 * @clk_stop_timeout: Worst-case latency of the Clock Stop Prepare state
 * machine transitions, in milliseconds.
 * @ch_prep_timeout: Worst-case latency of the Channel Prepare state machine
 * transitions, in milliseconds
 * @reset_behave: If Slave keeps the status of the SlaveStopClockPrepare
 * state machine (P=1 SCSP_SM) after exit from clock-stop mode1
 * @high_PHY_capable: If Slave is HighPHY capable
 * @paging_support: Does Slave implement paging registers SCP_AddrPage1
 * and SCP_AddrPage2
 * @bank_delay_support: Does Slave implements bank delay/bridge support
 * registers SCP_BankDelay and SCP_NextFrame
 * @p15_behave: Slave behavior when the Master attempts a Read to the Port15
 * alias
 * @master_count: Number of Masters present on this Slave
 * @source_ports: Bitmap identifying source ports on the Device
 * @sink_ports: Bitmap identifying sink ports on the Device
 * @dp0_prop: Data Port 0 properties
 * @src_dpn_prop: Source Data Port N properties
 * @sink_dpn_prop: Sink Data Port N properties
 */
struct sdw_slave_prop {
	u32 mipi_revision;
	bool wake_capable;
	bool test_mode_capable;
	bool clk_stop_mode1;
	bool simple_clk_stop_capable;
	u32 clk_stop_timeout;
	u32 ch_prep_timeout;
	enum sdw_clk_stop_reset_behave reset_behave;
	bool high_PHY_capable;
	bool paging_support;
	bool bank_delay_support;
	enum sdw_p15_behave p15_behave;
	u32 master_count;
	u32 source_ports;
	u32 sink_ports;
	struct sdw_dp0_prop *dp0_prop;
	struct sdw_dpn_prop *src_dpn_prop;
	struct sdw_dpn_prop *sink_dpn_prop;
};

/**
 * struct sdw_port_params: This is used to program the Data Port based on
 * Data Port stream params. These parameters are not banked and not expected
 * to change dynamically to avoid audio artifacts.
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
 * struct sdw_transport_params: This is used to program the Data Port based
 * on Data Port transport params. All these parameters are banked and can be
 * modified during a bank switch without any artefact's in audio stream. Bus
 * driver modifies these parameters as part new stream getting
 * enabled/disabled on the bus. Registers are explained next to each field
 * where values will be filled. Those are MIPI defined registers for Slave
 * devices.
 *
 * @blk_grp_ctrl_valid: If Port implement block group control
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
	unsigned int blk_grp_ctrl;
	unsigned int sample_interval;
	unsigned int offset1;
	unsigned int offset2;
	unsigned int hstart;
	unsigned int hstop;
	unsigned int blk_pkg_mode;
	unsigned int lane_ctrl;
};

/**
 * struct sdw_bus_conf: Bus params for the Slave/Master to be ready for next
 * bus changes.
 *
 * @clk_freq: Clock frequency in Hertz
 * @num_rows: Number of rows in frame
 * @num_cols: Number of columns in frame
 * @bank: Register bank, which bank Slave/Master driver should program for
 * implementation defined registers. This is the inverted value of the
 * current bank.
 *
 * The implementation of this bus driver follows the recommendations of the
 * MIPI specification and will never modify registers in the current bank to
 * avoid audible issues and bus conflicts. Bus reconfigurations are always
 * handled through a synchronized bank switch mechanism. The use of
 * Port-specific banks is not supported for now.
 */
struct sdw_bus_conf {
	unsigned int clk_freq;
	u16 num_rows;
	unsigned int num_cols;
	unsigned int bank;
};

int sdw_master_read_prop(struct sdw_bus *bus);
int sdw_slave_read_prop(struct sdw_slave *slave);

/*
 * sdw bus defines
 */

extern struct bus_type sdw_bus_type;

/**
 * struct sdw_slave_id: Slave ID
 *
 * @mfg_id: MIPI Manufacturing code
 * @part_id: Device Part ID
 * @class_id: MIPI Class ID
 * @unique_id: Device unique ID
 * @sdw_version: SDW version implemented
 * @link_id: link instance number
 */
struct sdw_slave_id {
	__u16 mfg_id;
	__u16 part_id;
	__u8 class_id;
	__u8 unique_id:4;
	__u8 sdw_version:4;
	__u16 link_id;
};

struct sdw_slave_intr_status {
	u8 control_port;
	u8 port[15];
};

enum sdw_clok_stop_type {
	SDW_CLK_PRE_STOP = 0,
	SDW_CLK_POST_STOP,
	SDW_CLK_PRE_START,
	SDW_CLK_POST_START,
};

/**
 * struct sdw_slave_ops: Slave driver callback ops
 *
 * @read_prop: read slave properties callback
 * @interrupt_callback: callback for device interrupt notification (invoked
 * in thread context)
 * @update_status: callback to update slave status
 * @get_clk_stop_mode: query the clock mode supported, shall return mode
 * based on dynamic if present or property
 * @clk_stop: clock stop callback
 */
struct sdw_slave_ops {
	int (*read_prop)(struct sdw_slave *sdw);
	int (*interrupt_callback)(struct sdw_slave *slave,
			struct sdw_slave_intr_status *status);
	int (*update_status)(struct sdw_slave *slave,
			enum sdw_slave_status status);
	int (*get_clk_stop_mode)(struct sdw_slave *slave);
	int (*clk_stop)(struct sdw_slave *slave,
			enum sdw_clk_stop_mode mode,
			enum sdw_clok_stop_type type);
};

/**
 * struct sdw_slave: SoundWire Slave
 *
 * @id: MIPI device ID
 * @dev: Linux device
 * @status: device enumeration status
 * @bus: bus for this slave
 * @ops: slave callback ops
 * @prop: slave properties
 * @sysfs: sysfs for this slave
 * @node: node for bus list of slaves
 * @addr: Logical address
 */
struct sdw_slave {
	struct sdw_slave_id id;
	struct device dev;
	enum sdw_slave_status status;
	struct sdw_bus *bus;
	const struct sdw_slave_ops *ops;
	struct sdw_slave_prop prop;
	struct sdw_slave_sysfs *sysfs;
	struct list_head node;
	u16 addr;
};

#define dev_to_sdw_dev(_dev) container_of(_dev, struct sdw_slave, dev)

/**
 * struct sdw_master_prop: master properties
 *
 * @revision: MIPI spec version of the implementation
 * @clk_stop_mode: Clocks Stop modes supported
 * @max_freq: Maximum Bus clock in Hz for this Master
 * @num_clk_gears: Number of clock gears supported
 * @clk_gears: The clock gears supported
 * @num_freq: Number of clock frequencies (in Hz) supported
 * @freq: The clock frequencies (in Hz) supported
 * @default_freq: Controller default Frame rate in Hz
 * @default_rows: Number of rows
 * @default_col: Number of columns
 * @dynamic_frame: Is dynamic frame supported
 * @err_threshold: Number of times that software may retry sending a single
 * command
 * @dpn_prop: Data Port N properties
 */
struct sdw_master_prop {
	u32 revision;
	enum sdw_clk_stop_mode clk_stop_mode;
	u32 max_freq;
	u32 num_clk_gears;
	u32 *clk_gears;
	u32 num_freq;
	u32 *freq;
	u32 default_freq;
	u32 default_rows;
	u32 default_col;
	bool dynamic_frame;
	u32 err_threshold;
	struct sdw_dpn_prop *dpn_prop;
};

struct sdw_master_sysfs;
struct sdw_msg;
struct sdw_wait;

/**
 * struct sdw_master_ops: master driver ops
 *
 * @read_prop: read the properties of a master
 * @xfer_msg: the transfer message callback
 * @xfer_msg_async: the async version of transfer message callback
 * @set_ssp_interval: set the Stream Synchronization Point (SSP) interval
 * @set_bus_conf: Set the given bus configuration
 * @pre_bank_switch: callback before doing the bank switch
 * @post_bank_switch: callback after doing the bank switch
 */
struct sdw_master_ops {
	int (*read_prop)(struct sdw_bus *bus);

	enum sdw_command_response (*xfer_msg)
			(struct sdw_bus *bus, struct sdw_msg *msg, int page);
	enum sdw_command_response (*xfer_msg_async)
			(struct sdw_bus *bus, struct sdw_msg *msg,
			int page, struct sdw_wait *wait);
};

/**
 * struct sdw_bus_params: Structure holding bus configuration information
 *
 * @clk_state: State of the clock (ON/OFF)
 * @active_bank: Current bank in use (BANK0/BANK1)
 * @max_clk_dr_freq: Maximum double rate clock frequency. This is maximum
 * double clock rate supported per bus
 * @curr_clk_dr_freq: Current double rate clock frequency in use. This is
 * current clock rate at which bus is running
 * @clk_div: Current clock divider in use
 * @bandwidth: Total bandwidth
 * @system_interval: Bus System interval (Stream Synchronization Point).
 * @stream_interval: Stream interval
 * @frame_freq: SoundWire Frame frequency on bus
 * @col: Active columns
 * @row: Active rows
 */
struct sdw_bus_params {
	unsigned int clk_state;
	unsigned int active_bank;
	unsigned int max_dr_clk_freq;
	unsigned int curr_dr_clk_freq;
	unsigned int clk_div;
	unsigned int bandwidth;
	unsigned int system_interval;
	unsigned int stream_interval;
	unsigned int frame_freq;
	unsigned int col;
	unsigned int row;
};

/**
 * struct sdw_msg: Message to be sent on sdw bus
 *
 * @addr: the register address of the slave
 * @len: number of messages i.e reads/writes to be performed
 * @addr_page1: SCP address page 1 Slave register
 * @addr_page2: SCP address page 2 Slave register
 * @flags: transfer flags, indicate if xfer is read or write
 * @buf: message data buffer
 * @ssp_sync: Send message at SSP (Stream Synchronization Point)
 */
struct sdw_msg {
	u16 addr;
	u16 len;
	u16 device;
	u8 addr_page1;
	u8 addr_page2;
	u8 flags;
	u8 *buf;
	bool ssp_sync;
};

struct sdw_wait {
	int length;
	struct completion complete;
	struct sdw_msg *msg;
};

/**
 * struct sdw_bus: the SoundWire bus
 *
 * @bus_node: Node for bus list
 * @dev: master device
 * @acpi_enabled: is this bus acpi enabled or not
 * @link_id: Link id number, can be 0 to N
 * @slaves: list of slaves on this bus
 * @assigned: logical addresses assigned
 * @lock: bus lock
 * @ops: master callback ops
 * @port_ops: master port callback ops
 * @params: current bus parameters
 * @prop: master properties
 * @sysfs: bus sysfs
 * @wait_msg: wait messages for async messages
 */
struct sdw_bus {
	struct list_head bus_node;
	struct device *dev;
	bool acpi_enabled;
	unsigned int link_id;
	struct list_head slaves;
	bool assigned[SDW_MAX_DEVICES + 1];
	spinlock_t lock;
	const struct sdw_master_ops *ops;
	const struct sdw_master_port_ops *port_ops;
	struct sdw_bus_params params;
	struct sdw_master_prop prop;
	struct sdw_master_sysfs *sysfs;
	struct sdw_wait wait_msg;
};

int sdw_add_bus_master(struct sdw_bus *bus);
void sdw_delete_bus_master(struct sdw_bus *bus);

struct sdw_slave_sysfs;

int sdw_sysfs_bus_init(struct sdw_bus *bus);
void sdw_sysfs_bus_exit(struct sdw_bus *bus);
int sdw_sysfs_slave_init(struct sdw_slave *slave);
void sdw_sysfs_slave_exit(struct sdw_slave *slave);

struct sdw_driver {
	const char *name;

	int (*probe)(struct sdw_slave *sdw,
			const struct sdw_device_id *id);
	int (*remove)(struct sdw_slave *sdw);
	void (*shutdown)(struct sdw_slave *sdw);

	const struct sdw_device_id *id_table;
	const struct sdw_slave_ops *ops;

	struct device_driver driver;
};

#define drv_to_sdw_driver(_drv) container_of(_drv, struct sdw_driver, driver)

int sdw_register_driver(struct sdw_driver *drv, struct module *owner);
void sdw_unregister_driver(struct sdw_driver *drv);

/* messaging and data api's */

s8 sdw_read(struct sdw_slave *slave, u16 addr);
int sdw_write(struct sdw_slave *slave, u16 addr, u8 value);
int sdw_nread(struct sdw_slave *slave, u16 addr, size_t count, u8 *val);
int sdw_nwrite(struct sdw_slave *slave, u16 addr, size_t count, u8 *val);

enum {
	SDW_MSG_FLAG_READ = 0,
	SDW_MSG_FLAG_WRITE,
};

int sdw_transfer(struct sdw_bus *bus, struct sdw_slave *slave,
			struct sdw_msg *msg);
int sdw_transfer_async(struct sdw_bus *bus, struct sdw_slave *slave,
			struct sdw_msg *msg, struct sdw_wait *wait);

int sdw_handle_slave_status(struct sdw_bus *bus,
			enum sdw_slave_status status[]);

int sdw_bus_prep_clk_stop(struct sdw_bus *bus);
int sdw_bus_clk_stop(struct sdw_bus *bus);
int sdw_bus_clk_stop_exit(struct sdw_bus *bus);

#endif /* __SOUNDWIRE_H */
