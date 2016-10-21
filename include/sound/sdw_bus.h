/*
 * sdw_bus.h - Definition for SoundWire bus interface. This file has all
 *	the SoundWire bus interfaces common to both Master and Slave
 *	interfaces.
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

#ifndef _LINUX_SDW_BUS_H
#define _LINUX_SDW_BUS_H

#include <linux/device.h>
#include <linux/mod_devicetable.h>


/* Broadcast address for Slaves. */
#define SDW_SLAVE_BDCAST_ADDR		15 /* As per MIPI 1.1 Spec */

/* Total number of valid rows possible for SoundWire frame. */
#define MAX_NUM_ROWS			23 /* As per MIPI 1.1 Spec */

/* Total number of valid columns possible for SoundWire frame. */
#define MAX_NUM_COLS			8 /* As per MIPI 1.1 Spec */

/* Number of control bits in SoundWire frame. */
#define SDW_BUS_CONTROL_BITS		48 /* As per MIPI 1.1 Spec */

/**
 * Number of dev_id registers. This represents the device ID of slaves
 *	which includes manufacturer ID, part ID and unique device ID.
 */
#define SDW_NUM_DEV_ID_REGISTERS		6 /* As per MIPI 1.1 Spec */

#define SDW_MAX_DEVICES				11 /* Max. Slave devices on bus */

#define SDW_SLAVE_ENUM_ADDR			0 /* Enumeration addr. for Slaves */

#define SDW_PORT_SINK				0x0 /* Sink Port */

#define SDW_PORT_SOURCE				0x1 /* Source Port */

#define SDW_MAX_PORT_DIRECTIONS			0x2 /* Source or Sink */

#define SDW_MSG_FLAG_READ			0x0 /* Read Flag for MSG and BRA transfer  */

#define SDW_MSG_FLAG_WRITE			0x1 /* Write Flag MSG and BRA transfer */

/* Total number of Row and Column combination possible for SoundWire frame. */
#define MAX_NUM_ROW_COLS		(MAX_NUM_ROWS * MAX_NUM_COLS)

/**
 * The following set of constants for flow control, Port type and transport
 * packing are bit masks since devices can expose combinations of
 * capabilities.
 */

/* Port flow mask for ISOCHRONOUS mode. */
#define SDW_PORT_FLOW_MODE_ISOCH		0x1


/* Port flow mask for TX_CONTROLLED mode. */
#define SDW_PORT_FLOW_MODE_TX_CNTRL		0x2

/* Port flow mask for RX_CONTROLLED mode. */
#define SDW_PORT_FLOW_MODE_RX_CNTRL		0x4

/* Port flow mask for ASYNCHRONOUS mode. */
#define SDW_PORT_FLOW_MODE_ASYNC		0x8

/* Sample packaging mask for Block per Port mode. */
#define SDW_PORT_BLK_PER_PORT			0x1

/* Sample packaging mask for Block per Channel mode. */
#define SDW_PORT_BLK_PER_CH			0x2


/* Port encoding mask definitions are part of SoundWire DisCo Spec */

/* Mask to specify data encoding supported by Port, type 2's complement. */
#define SDW_PORT_ENC_TWOS_CMPLMNT	0x1

/* Mask to specify data encoding supported by Port, type sign magnitude. */
#define SDW_PORT_ENC_SIGN_MAGNITUDE	0x2

/* Mask to specify data encoding supported by Port, type IEEE 32 float. */
#define SDW_PORT_ENC_IEEE_32_FLOAT	0x4

/* sdw_driver_type: Driver type on SoundWire bus. */
enum sdw_driver_type {
	SDW_DRIVER_TYPE_MASTER = 0,
	SDW_DRIVER_TYPE_SLAVE = 1,
};

/**
 * sdw_command_response: Response to the command. Command responses are
 *	part of SoundWire Spec. The values in the enum is different than
 *	actual Spec, since the response in the Spec is defined based on
 *	the combination of ACK/NAK bits.
 */
enum sdw_command_response {
	SDW_CMD_OK = 0,
	SDW_CMD_IGNORED = 1,
	SDW_CMD_FAILED = 2,
};

/**
 * sdw_dpn_type: Data Port type
 *
 * @SDW_DP_TYPE_FULL: Full Data Port supported.
 * @SDW_DP_TYPE_SIMPLE: Simplified Data Port defined in SoundWire 1.1
 *	Spec. DPN_SampleCtrl2, DPN_OffsetCtrl2, DPN_HCtrl and
 *	DPN_BlockCtrl3 are not implemented for Simplified Data Port.
 * @SDW_DP_TYPE_REDUCED: Reduced SoundWire Data Port, defined in SoundWire
 *	1.1 Spec. DPN_SampleCtrl2, DPN_HCtrl are not implemented for
 *	Reduced Data Port.
 */
enum sdw_dpn_type {
	SDW_DP_TYPE_FULL = 0,
	SDW_DP_TYPE_SIMPLE = 1,
	SDW_DP_TYPE_REDUCED = 2,
};

/* sdw_dpn_grouping: Maximum block group count supported. */
enum sdw_dpn_grouping {
	SDW_BLK_GRP_CNT_1 = 0,
	SDW_BLK_GRP_CNT_2 = 1,
	SDW_BLK_GRP_CNT_3 = 2,
	SDW_BLK_GRP_CNT_4 = 3,
};

/**
 * sdw_prep_ch_behavior: Specifies the dependencies between channel prepare
 *	sequence and bus clock configuration. This property is not required
 *	for ports implementing a simplified ChannelPrepare State Machine
 *	(SCPSM) This is not part of SoundWire Spec, but defined in SoundWire
 *	DisCo properties.
 *
 * @SDW_CHAN_PREP_RATE_ANY: Channel prepare can happen at any bus clock
 *	rate.
 * @SDW_CHAN_PREP_RATE_COMPAT: Channel prepare sequence needs to happen
 *	after bus clock is changed to a frequency supported by this mode.
 *
 *	TODO: This flag is not used, prepare is done after bus clock is
 *	changed. Bus driver will be extended later to use this.
 */
enum sdw_prep_ch_behavior {
	SDW_CHAN_PREP_RATE_ANY = 0,
	SDW_CHAN_PREP_RATE_COMPAT = 1,
};

/**
 * sdw_slave_status: Slave status reported by PING commands.
 *
 * @SDW_SLAVE_STAT_NOT_PRESENT: Slave is not present.
 * @SDW_SLAVE_STAT_ATTACHED_OK: Slave is attached to the bus.
 * @SDW_SLAVE_STAT_ALERT: Some alert condition on the Slave. An alert status
 *	will be reported by Slave, if the relevant Slave interrupt mask
 *	register is programmed. By default, on reset interrupt masks are not
 *	enabled.
 * @SDW_SLAVE_STAT_RESERVED: Reserved.
 */
enum sdw_slave_status {
	SDW_SLAVE_STAT_NOT_PRESENT = 0,
	SDW_SLAVE_STAT_ATTACHED_OK = 1,
	SDW_SLAVE_STAT_ALERT = 2,
	SDW_SLAVE_STAT_RESERVED = 3,
};

/**
 * sdw_stream_type: Stream type used by bus driver for the bandwidth
 *	allocation. This is not SoundWire Spec definition but required by
 *	implementations that need to route PDM data through decimator
 *	hardware.
 */
enum sdw_stream_type {
	SDW_STREAM_PCM = 0,
	SDW_STREAM_PDM = 1,
};

/**
 * sdw_ch_prepare_mode: Channel prepare state machine.
 *
 * @SDW_CP_MODE_SIMPLE: Simplified channel prepare state machine
 * @SDW_CP_MODE_NORMAL: Normal channel prepare state machine.
 */
enum sdw_ch_prepare_mode {
	SDW_CP_MODE_SIMPLE = 0,
	SDW_CP_MODE_NORMAL = 1,
};

/**
 * sdw_clk_stop_mode: Clock Stop mode.
 *
 * @SDW_CLOCK_STOP_MODE_0: Clock Stop mode 0. ClockStopMode0 is used when
 *	the Slave is capable of continuing operation seamlessly when the
 *	Clock is restarted, as though time had stood still while the Clock
 *	was stopped.
 *
 * @SDW_CLOCK_STOP_MODE_1: Clock Stop mode 1. ClockStopMode1 is used when
 *	the Slave might have entered a deeper power-saving mode that does
 *	not retain state while the Clock is stopped, so is not capable of
 *	continuing operation seamlessly when the Clock restarts.
 */
enum sdw_clk_stop_mode {
	SDW_CLOCK_STOP_MODE_0 = 0,
	SDW_CLOCK_STOP_MODE_1 = 1,
};

/**
 * sdw_data_direction: Data direction w.r.t Port. e.g for playback between
 *	the Master and Slave, where Slave is codec, data direction for the
 *	Master Port will be OUT, since its transmitting the data, while for
 *	the Slave (codec) it will be IN, since its receiving the data. This
 *	is not SoundWire Spec definition, it is used by bus driver for
 *	stream configuration. For all the sink Ports data direction is
 *	always IN, while for all the source Ports data direction is always
 *	OUT.
 *
 * @SDW_DATA_DIR_IN: Data is input to Port.
 * @SDW_DATA_DIR_OUT: Data is output from Port.
 */
enum sdw_data_direction {
	SDW_DATA_DIR_IN = 0,
	SDW_DATA_DIR_OUT = 1,
};

/**
 * sdw_port_data_mode: Data Port mode. It can be normal mode where audio
 *	data is received and transmitted, or any of the 3 different test
 *	modes defined by the SoundWire Spec 1.1. Test modes are normally
 *	used for testing at component and system level.
 *
 * @SDW_PORT_DATA_MODE_NORMAL: Normal data mode where audio data is received
 *	and transmitted.
 *
 * @SDW_PORT_DATA_MODE_STATIC_1: This simple test mode uses static value of
 *	logic 1. The encoding will result in signal transitions at every
 *	bitslot owned by this Port.
 *
 * @SDW_PORT_DATA_MODE_STATIC_0: This simple test mode uses static value of
 *	logic 0. The encoding will result in no signal transitions (the
 *	voltage level prior to the bitSlots owned by the Port will be
 *	maintained by the bus holder). Here enum says as _0 but enum value
 *	is 2. This is confusing while debugging, but kept to match the Spec
 *	values.
 *
 * @SDW_PORT_DATA_MODE_PRBS: This test mode uses a PRBS generator to produce
 *	a pseudo random data pattern that is transported from source Data
 *	Port to sink Data Port where similar pattern generators check the
 *	received pattern.
 */
enum sdw_port_data_mode {
	SDW_PORT_DATA_MODE_NORMAL = 0,
	SDW_PORT_DATA_MODE_STATIC_1 = 1,
	SDW_PORT_DATA_MODE_STATIC_0 = 2,
	SDW_PORT_DATA_MODE_PRBS = 3,
};

/**
 * sdw_port_prep_ops: Prepare operations for Master Data Ports
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

struct sdw_master;
struct sdw_slave;

/**
 * sdw_port_aud_mode_prop: Audio properties for the Port mode. The
 *	SoundWire specification only requires command and control to be
 *	supported for all bus frequencies. Audio transport may be restricted
 *	to specific modes (bus frequencies, sample rate, etc). The following
 *	capabilities are defined in the SoundWire DisCo Spec and provide
 *	additional information needed to configure the bus. All frequencies
 *	and sampling rates are in Hz.
 *
 * @max_bus_freq: Maximum frequency Port can support for the clock. The use
 *	of max_ and min_ frequency requires num_bus_freq_cfgs to
 *	be zero
 *
 * @min_bus_freq: Minimum frequency Port can support for the clock.
 * @num_bus_freq_cfgs: Array size for the frequencies supported by Port.
 * @clk_freq_buf: Buffer of frequencies supported by the Port.
 * @max_sample_rate: Maximum sampling rate supported by this Port. Use of
 *	max_ and min_ requires num_sample_rate_cfgs to be zero.
 *
 * @min_sample_rate: Minimum sampling rate supported by Port.
 * @num_sample_rate_cfgs: Array size for the number of sampling freq.
 *	supported by Port.
 *
 * @sample_rate_buf: Array for the sampling freq configs supported.
 * @ch_prepare_behavior: Specifies the dependencies between Channel Prepare
 *	Prepare sequence and Bus clock configuration.
 *	0: Channel prepare can happen at any bus clock rate
 *	1: Channel prepare sequence shall happen only after Bus clock is
 *	changed to a frequency supported by this mode or compatible modes
 *	described by the next field. This may be required, e.g. when the
 *	Slave internal audio clocks are derived from the bus clock. This
 *	property applies to all channels within this port. This property may
 *	be omitted for ports implementing a Simplified ChannelPrepare State
 *	Machine (SCPSM). Currently bus driver ignores this fields and always
 *	prepares the port after Bus clock is changed. This will be enhanced
 *	in future to optimize the prepare time.
 *
 * @glitchless_transitions_mask: Glitchless transition mask from one mode to
 *	other mode. Each bit refers to a mode number that can be reached
 *	from current mode without audible issues.
 */
struct sdw_port_aud_mode_prop {
	unsigned int max_bus_freq;
	unsigned int min_bus_freq;
	unsigned int num_bus_freq_cfgs;
	unsigned int *clk_freq_buf;
	unsigned int max_sample_rate;
	unsigned int min_sample_rate;
	unsigned int num_sample_rate_cfgs;
	unsigned int *sample_rate_buf;
	enum sdw_prep_ch_behavior ch_prepare_behavior;
	unsigned int glitchless_transitions_mask;

};

/**
 * sdw_slave_addr: Structure representing the read-only unique device ID
 *	and the SoundWire logical device number (assigned by bus driver).
 *
 * @slave: Slave device reference for easy access, to which this address is
 *	assigned.
 *
 * @dev_id: 6-byte device ID of the Slave
 * @dev_num: Slave address, "Device Number" field of the SCP_DevNumber
 *	registers. This gets programmed into the register. Currently
 *	Group_Id field is not used.
 *
 * @assigned: Flag if the logical address(aka Slave device number) is
 *	assigned to some Slave or is free.
 *
 * @status: mirrors Slave status reported by PING commands.
 */
struct sdw_slave_addr {
	struct sdw_slave *slave;
	unsigned int dev_id[SDW_NUM_DEV_ID_REGISTERS];
	unsigned int dev_num;
	bool assigned;
	enum sdw_slave_status status;
};

/**
 * sdw_dpn_caps: Capabilities of the Data Port, other than Data Port 0 for
 *	SoundWire Master and Slave. This follows the definitions in the
 *	SoundWire DisCo Spec. All fields are listed in order of the
 *	SoundWire DisCo Spec. Name of the fields are altered to follow Linux
 *	convention. All the fields which are required by bus driver but not
 *	present in DisCo Spec are listed at the end of the structure. There
 *	is a common dpn capability structure for Master and Slave ports.
 *	Some of the fields doesn't make sense for Master which are called
 *	out explicitly in fields description. Port properties can be
 *	different for same Port, based on Port is configured as source or
 *	sink Port.
 *
 * @max_bps: Maximum bits per sample supported by Port. This is same as word
 *	length in SoundWire Spec.
 *
 * @min_bps: Minimum bits per sample supported by Port. This is same as word
 *	length in SoundWire Spec.
 *
 * @num_bps: Length of supported bits per sample buffer. The use of max_ and
 *	min_ bps requires num_bps to be zero
 *
 * @bps_buf: Array of the bits per sample buffer.
 * @type: Type of Data Port. Simplified or Normal Data Port.
 * @grouping: Max Block group count supported for this Port.
 * @prepare_ch: Channel prepare scheme. Simplified channel prepare or normal
 *	channel prepare. All Channels in the Port are assumed to share the
 *	same scheme. For Masters this should be always simplified channel
 *	prepare.
 *
 * @ch_prep_timeout: A port-specific timeout value in milliseconds. This
 *	value indicates the worst-case latency of the channel prepare state
 *	machine transitions. After receiving a successful channel
 *	prepare/de-prepare command, the slave should complete the operation
 *	before the expiration of the timeout. Software stack may poll for
 *	completion periodically or wait for this timeout value. If the
 *	requested action has not completed at the end of this timeout,
 *	software shall interpret this as an error condition. This is don't
 *	care for Master.
 *
 * @imp_def_intr_mask: Implementation defined interrupt mask. Master should
 *	always set this to 0. Bus driver doesn't use this for Master.
 *
 * @min_ch_cnt: Minimum number of channels supported.
 * @max_ch_cnt: Maximum number of channels supported.
 * @num_ch_cnt: Buffer length for the channels supported. The use of max_ and
 *	min_ ch_num requires num_ch_cnt to be zero.
 *
 * @ch_cnt_buf: Array of the channel count.
 * @port_flow_mode_mask: Transport flow modes supported by Port.
 * @max_async_buffer: Number of samples this port can buffer in asynchronous
 *	modes. The SoundWire specification only requires minimal buffering,
 *	this property is only required if the Slave implements a buffer that
 *	exceeds the SoundWire requirements. Bus driver currently doesn't
 *	support this.
 *
 * @blk_pack_mode: Block packing mode mask.
 * @port_encoding: Port Data encoding type mask.
 * @num_audio_modes: Number of audio modes supported by Port. This should be
 *	0 for Master. Bus driver doesn't use this for Master.
 *
 * @mode_properties: Port audio mode properties buffer of size
 *	num_audio_modes. This is not used for Master. This needs to be
 *	populated only for Slave.
 *
 * @port_number: Port number.
 */
struct sdw_dpn_caps {
	unsigned int max_bps;
	unsigned int min_bps;
	unsigned int num_bps;
	unsigned int *bps_buf;
	enum sdw_dpn_type type;
	enum sdw_dpn_grouping grouping;
	enum sdw_ch_prepare_mode prepare_ch;
	unsigned int ch_prep_timeout;
	u8 imp_def_intr_mask;
	unsigned int min_ch_cnt;
	unsigned int max_ch_cnt;
	unsigned int num_ch_cnt;
	unsigned int *ch_cnt_buf;
	unsigned int port_flow_mode_mask;
	unsigned int blk_pack_mode;
	unsigned int port_encoding;
	unsigned int num_audio_modes;
	struct sdw_port_aud_mode_prop *mode_properties;
	unsigned int port_number;
};

/**
 * sdw_prepare_ch: Prepare/De-prepare the Data Port channel. This is similar
 *	to the prepare API of Alsa, where most of the hardware interfaces
 *	are prepared for the playback/capture to start. All the parameters
 *	are known to the hardware at this point for starting
 *	playback/capture next.
 *
 * @num: Port number.
 * @ch_mask: Channels to be prepared/deprepared specified by ch_mask.
 * @prepare: Prepare/de-prepare channel, true (prepare) or false
 *	(de-prepare).
 *
 * @bank: Register bank, which bank Slave/Master driver should program for
 *	implementation defined registers. This is the inverted value of the
 *	current bank.
 */
struct sdw_prepare_ch {
	unsigned int num;
	unsigned int ch_mask;
	bool prepare;
	unsigned int bank;
};

/**
 * sdw_bus_params: Bus params for the Slave/Master to be ready for next
 *	bus changes.
 *
 * @clk_freq: Clock frequency for the bus in Hz.
 * @num_rows: Number of rows in new frame to be enabled
 * @num_cols: Number of columns in new frame to be enabled. The MIPI
 *	specification lists restrictions on values for rows and cols that
 *	will be enforced by the bus driver.
 *
 * @bank: Register bank, which bank Slave/Master driver should program for
 *	implementation defined registers. This is the inverted value of the
 *	current bank. The implementation of this bus driver follows the
 *	recommendations of the MIPI specification and will never modify
 *	registers in the current bank to avoid audible issues and bus
 *	conflicts. Bus reconfigurations are always handled through a
 *	synchronized bank switch mechanism. The use of Port-specific banks
 *	is not supported for now.
 */
struct sdw_bus_params {
	unsigned int clk_freq;
	u16 num_rows;
	unsigned int num_cols;
	unsigned int bank;
};

/**
 * sdw_bra_block: Data block to be sent/received using SoundWire Bulk
 *	Register Access Protocol (BRA).
 *
 * @dev_num: Slave address, "Device Number" field of the SCP_DevNumber
 *	registers. This gets programmed into the register. Currently
 *	Group_Id field is not used.
 *
 * @r_w_flag: Read/Write operation. Read(0) or Write(1) based on above #def
 * @num_bytes: Number of Data bytes to be transferred.
 * @reg_offset: Register offset for the first byte of the read/write
 *	transaction.
 *
 * @values: Array containing value for write operation and to be filled for
 *	read operation.
 */
struct sdw_bra_block {
	unsigned int dev_num;
	u8 r_w_flag;
	unsigned int num_bytes;
	unsigned int reg_offset;
	u8 *values;
};

/**
 * sdw_msg: Message to be sent on bus. This is similar to i2c_msg on I2C
 *	bus. Message is sent from the bus driver to the Slave device. Slave
 *	driver can also initiate transfer of the message to program
 *	implementation defined registers. Message is formatted and
 *	transmitted on to the bus by Master interface in hardware-specific
 *	way. Bus driver initiates the Master interface callback to transmit
 *	the message on bus.
 *
 * @addr: Address of the register to be read.
 * @len: Length of the message to be transferred. Successive increment in
 *	the register address for every message. Bus driver sets this field
 *	to 0, if transfer fails. E.g. If calling function sets len = 2 for
 *	two byte transfer, and if transfer fails for 2nd byte, "len" field
 *	is set to 0 by bus driver.
 *
 * @dev_num: Slave address, "Device Number" field of the SCP_DevNumber
 *	registers. Currently Group_Id field is not used.
 *
 * @addr_page1: SCP address page 1 Slave register. 32-bit address of the
 *	SoundWire Slave register is constructed using 16-bit lower bit
 *	address as part of command, 8-bit as part of this register and MSB
 *	8-bits as a part of addr_page2 register. This registers needs to be
 *	programmed first for any transfer with register address greater than
 *	0xFFFF.
 *
 * @addr_page2: SCP address page 2
 * @r_w_flag: Message operation to be read or write.
 * @buf: Buf to be written or read from the register.
 * @xmit_on_ssp: Send message at SSP (Stream Synchronization Point). It
 *	should be used when a command needs to be issued during the next
 *	SSP. For all normal reads/writes this should be zero. This will be
 *	used for broadcast write to SCP_FrameCtrl register by bus driver
 *	only. Slave driver should always set xmit_on_ssp to 0. This flag
 *	applies only if len = 1 in message. Multiple length messages cannot
 *	be transmitted on SSP, for this caller needs to create multiple
 *	messages.
 *
 */
struct sdw_msg {
	u16 addr;
	u16 len;
	u8 dev_num;
	u8 addr_page1;
	u8 addr_page2;
	u8 r_w_flag;
	u8 *buf;
	bool xmit_on_ssp;
};

/**
 * sdw_stream_config: Stream configuration set by the Master(s) and Slave(s)
 *	via sdw_config_stream API. Both Master(s) and Slave(s) needs to
 *	provide stream configuration because stream configuration for
 *	Master(s) and Slave(s) could be different for a given stream. E.g.
 *	in case of Stream associated with Single Master and a Slave, both
 *	Master and Slave shall have same "sdw_stream_config" except the
 *	"direction" (assuming Master and Slave supports stereo channels per
 *	port). In case of stereo stream attached to single Master and two
 *	Slaves, "channel_count" for each Slave shall be 1 and for the Master
 *	it shall be 2, (assuming Master supports stereo channels per port
 *	and Slave supports mono channel per port).
 *
 * @frame_rate: Audio frame rate of the stream (not the bus frame rate
 *	defining command bandwidth).
 *
 * @channel_count: Channel count of the stream.
 * @bps: Number of bits per audio sample.
 * @direction: Data direction w.r.t Port. This is used by bus driver to
 *	identify source and sink of the stream.
 *
 * @type: Stream type PCM or PDM. This is internally used by bus driver for
 *	bandwidth allocation.
 */
struct sdw_stream_config {
	unsigned int frame_rate;
	unsigned int channel_count;
	unsigned int bps;
	enum sdw_data_direction direction;
	enum sdw_stream_type type;
};

/**
 * sdw_port_config: SoundWire Port configuration for a given port (Master
 *	and Slave) associated with the stream.
 *
 * @num: Port number to be configured
 * @ch_mask: Which channels needs to be enabled for this Port.
 */
struct sdw_port_config {
	unsigned int num;
	unsigned int ch_mask;
};

/**
 * sdw_ports_config: Ports configuration set by the Master(s) and
 *	Slave(s) via "sdw_config_ports" API. Both Master(s) and Slave(s)
 *	needs to provide port configuration because port configuration for
 *	Master(s) and Slave(s) could be different for a given stream. E.g.
 *	in case of Stream associated with Single Master and a Slave, both
 *	Master and Slave shall have same "sdw_ports_config" except the
 *	"num" inside "sdw_port_config"(assuming Master and Slave
 *	supports stereo channels per port). In case of stereo stream
 *	attached to single Master and two Slaves, "num_ports" for each Slave
 *	shall be 1 and for the Master it shall be 2 for "sdw_ports_config",
 *	"ch_mask" for Slave which handles left channel shall be 0x1, and for
 *	Slave which handles right channel shall be 0x2. "num" should
 *	be based on port allocated by Master and Slaves(assuming Master
 *	supports stereo channels per port and Slave supports mono channel
 *	per port).
 *
 * @num_ports: Number of ports to be configured.
 * @port_cfg: Port configuration for each Port.
 */
struct sdw_ports_config {
	unsigned int num_ports;
	struct sdw_port_config *port_config;
};

/**
 * sdw_slave_status: Status of all the SoundWire Slave devices.
 *
 * @status: Array of status of SoundWire Slave devices. There can be 11
 *	Slaves with non-zero device number, before enumeration all Slaves
 *	reports as device0.
 */
struct sdw_status {
	enum sdw_slave_status status[SDW_MAX_DEVICES + 1];
};

/**
 * snd_sdw_alloc_stream_tag: Allocates unique stream_tag. Stream tag is
 *	a unique identifier for each SoundWire stream across all SoundWire
 *	bus instances. Stream tag is a software concept defined by bus
 *	driver for stream management and not by MIPI SoundWire Spec. Each
 *	SoundWire Stream is individually configured and controlled using the
 *	stream tag. Multiple Master(s) and Slave(s) associated with the
 *	stream, uses stream tag as an identifier. All the operations on the
 *	stream e.g. stream configuration, port configuration, prepare and
 *	enable of the ports are done based on stream tag. This API shall be
 *	called once per SoundWire stream either by the Master or Slave
 *	associated with the stream.
 *
 * @stream_tag: Stream tag returned by bus driver.
 */
int snd_sdw_alloc_stream_tag(unsigned int *stream_tag);

/**
 * snd_sdw_release_stream_tag: Free the already assigned stream tag.
 *	Reverses effect of "sdw_alloc_stream_tag"
 *
 * @stream_tag: Stream tag to be freed.
 */
void snd_sdw_release_stream_tag(unsigned int stream_tag);

/**
 * snd_sdw_config_stream: Configures the SoundWire stream. All the Master(s)
 *	and Slave(s) associated with the stream calls this API with
 *	"sdw_stream_config". This API configures SoundWire stream based on
 *	"sdw_stream_config" provided by each Master(s) and Slave(s)
 *	associated with the stream. Master calls this function with Slave
 *	handle as NULL, Slave calls this with Master handle as NULL.
 *
 * @mstr: Master handle.
 * @slave: SoundWire Slave handle, Null if stream configuration is called by
 *	Master driver.
 *
 * @stream_config: Stream configuration for the SoundWire audio stream.
 * @stream_tag: Stream_tag representing the audio stream. All Masters and
 *	Slaves part of the same stream has same stream tag. So Bus driver
 *	holds information of all Masters and Slaves associated with stream
 *	tag.
 */
int snd_sdw_config_stream(struct sdw_master *mstr, struct sdw_slave *slave,
			struct sdw_stream_config *stream_config,
			unsigned int stream_tag);

/**
 * snd_sdw_release_stream: De-associates Master(s) and Slave(s) from stream.
 *	Reverse effect of the sdw_config_stream. Master calls this with
 *	Slave handle as NULL, Slave calls this with Master handle as NULL.
 *
 * @mstr: Master handle,
 * @slave: SoundWire Slave handle, Null if stream configuration is called by
 *	Master driver.
 *
 * @stream_tag: Stream_tag representing the audio stream. All Masters and
 *	Slaves part of the same stream has same stream tag. So Bus driver
 *	holds information of all Masters and Slaves associated with stream
 *	tag.
 */
int snd_sdw_release_stream(struct sdw_master *mstr, struct sdw_slave *slave,
						unsigned int stream_tag);

/**
 * snd_sdw_config_ports: Configures Master or Slave Port(s) associated with
 *	the stream. All the Master(s) and Slave(s) associated with the
 *	stream calls this API with "sdw_ports_config". Master calls this
 *	function with Slave handle as NULL, Slave calls this with Master
 *	handle as NULL.
 *
 * @mstr: Master handle where the Slave is connected.
 * @slave: Slave handle.
 * @ports_config: Port configuration for each Port of SoundWire Slave.
 * @stream_tag: Stream tag, where this Port is connected.
 */
int snd_sdw_config_ports(struct sdw_master *mstr, struct sdw_slave *slave,
				struct sdw_ports_config *ports_config,
				unsigned int stream_tag);

/**
 * snd_sdw_prepare_and_enable: Prepare and enable all the ports of all the
 *	Master(s) and Slave(s) associated with this stream tag. Following
 *	will be done as part of prepare operation.
 *	1. Bus parameters such as bandwidth, frame shape, clock frequency,
 *	SSP interval are computed based on current stream as well as already
 *	active streams on bus. Re-computation is required to accommodate
 *	current stream on the bus.
 *	2. Transport parameters of all Master and Slave ports are computed
 *	for the current as well as already active stream based on above
 *	calculated frame shape and clock frequency.
 *	3. Computed bus and transport parameters are programmed in Master
 *	and Slave registers. The banked registers programming is done on the
 *	alternate bank (bank currently unused). Port channels are enabled
 *	for the already active streams on the alternate bank (bank currently
 *	unused). This is done in order to not to disrupt already active
 *	stream.
 *	4. Once all the new values are programmed, switch is made to
 *	alternate bank. Once switch is successful, the port channels enabled
 *	on previous bank for already active streams are disabled.
 *	5. Ports of Master and Slave for new stream are prepared.
 *
 *	Following will be done as part of enable operation.
 *	1. All the values computed in SDW_STATE_STRM_PREPARE state are
 *	programmed in alternate bank (bank currently unused). It includes
 *	programming of already active streams as well.
 *	2. All the Master and Slave port channels for the new stream are
 *	enabled on alternate bank (bank currently unused).
 *	3. Once all the new values are programmed, switch is made on the
 *	alternate bank. Once the switch is successful, the port channels
 *	enabled on previous bank for already active streams are disabled.
 *
 *	This shall be called either by Master or Slave, which is responsible
 *	for doing data transfer between SoundWire link and the system
 *	memory.
 *
 * @stream_tag: Audio stream to be enabled. Each stream has unique
 *	stream_tag. All the channels of all the ports of Slave(s) and
 *	Master(s) attached to this stream will be prepared and enabled
 *	simultaneously with bank switch.
 */
int snd_sdw_prepare_and_enable(unsigned int stream_tag);

/**
 * snd_sdw_disable_and_deprepare: Disable and de-prepare all the ports of
 *	all the Master(s) and Slave(s) associated with stream tag. Following
 *	will be done as part of disable operation.
 *	1. Disable for Master and Slave ports channels is performed on
 *	alternate bank (bank currently unused) registers for current stream.
 *	2. All the current configuration of bus and Master and Slave ports
 *	are programmed into alternate bank (bank currently unused). It
 *	includes programming of already active streams port channels on
 *	alternate bank (bank currently unused).
 *	3. Switch is made on new bank. Once the switch is successful, the
 *	port channels of current stream are disabled. All the port channels
 *	enabled on previous bank for active stream are disabled.
 *
 *	Following will be done as part of de-prepare operation.
 *	1. Check the bandwidth required per Master. If its zero, de-prepare
 *	current stream and move stream state SDW_STATE_STRM_UNPREPARE, rest
 *	of the steps are not required. If bandwidth required per Master is
 *	non zero that means some more streams are running on Master and
 *	continue with next step.
 *	2. Bus parameters and transport parameters are computed for the
 *	streams active on the given Master.
 *	3. All the computed values for active stream are programmed into
 *	alternate bank (bank currently unused) in Master and Slave registers
 *	including already active streams port channels on alternate bank
 *	(bank currently unused).
 *	4. Switch is made to alternate bank where all the values for active
 *	stream were programmed. On successful switch of bank, all the port
 *	channels enabled on previous bank for active stream are disabled.
 *	5. De-prepare ports of the Master and Slave associated with current
 *	stream.
 *
 *	This shall be called either by Master or Slave, which is
 *	responsible for doing data transfer between SoundWire link and the
 *	system memory.
 *	Note: Both disable and de-prepare operations are performed in single
 *	call. De-prepare operation can be deferred for some specific timeout
 *	value after disable operation, to avoid bus re-configurations
 *	between short play and pause periods.
 *
 * @stream_tag: Audio stream to be disabled. Each stream has unique
 *	stream_tag. All the channels of all the ports of Slave(s) and
 *	Master(s) attached to this stream will be disabled and de-prepared
 *	simultaneously with bank switch.
 */
int snd_sdw_disable_and_deprepare(unsigned int stream_tag);

/**
 * snd_sdw_slave_transfer: Transfer message on bus.
 *
 * @master: Master which will transfer the message.
 * @msg: Array of messages to be transferred.
 * @num: Number of messages to be transferred, messages include read and
 *	write messages, but not the ping commands. The read and write
 *	messages are transmitted as a part of read and write SoundWire
 *	commands with a parameter containing the payload.
 *
 * Returns the number of messages successfully transferred else appropriate
 * error code.
 */
int snd_sdw_slave_transfer(struct sdw_master *master, struct sdw_msg *msg,
						unsigned int num);
#endif /*  _LINUX_SDW_BUS_H */

