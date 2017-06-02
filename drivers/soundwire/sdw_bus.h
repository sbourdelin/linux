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

#ifndef __SDW_BUS_H
#define __SDW_BUS_H

#include <linux/init.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/acpi.h>
#include <linux/soundwire/soundwire.h>

#ifdef CONFIG_ACPI
int sdw_acpi_find_slaves(struct sdw_bus *bus);
#else
int sdw_acpi_find_slaves(struct sdw_bus *bus)
{
	return -ENXIO;
}
#endif

extern const struct attribute_group *slave_dev_attr_groups[];
extern struct sdw_core sdw_core;

/* Max number of stream tags */
#define SDW_NUM_STREAM_TAGS		100
#define SDW_DOUBLE_RATE_FACTOR		2

#define SDW_BANK_SWITCH_TO		3

#define SDW_FREQ_MOD_FACTOR		3000

/*
 * SDW_STRM_RATE_GROUPING is place holder number used to hold the frame rate
 * used in grouping stream for efficiently calculating bandwidth. All the
 * streams with same frame rates belong to same group. This number is
 * dynamically increased if the group count number increases above 12.
 */
#define SDW_STRM_RATE_GROUPING		12

/* Size of buffer in bytes. */
#define SDW_BUF_SIZE1			1
#define SDW_BUF_SIZE2			2
#define SDW_BUF_SIZE3			3
#define SDW_BUF_SIZE4			4

/* Number of message(s) transferred on bus. */
#define SDW_NUM_MSG1			1
#define SDW_NUM_MSG2			2
#define SDW_NUM_MSG3			3
#define SDW_NUM_MSG4			4

/**
 * enum sdw_stream_state: Stream state maintained by bus driver for performing
 * stream operations.
 *
 * @SDW_STATE_STRM_ALLOC: New stream is allocated.
 * @SDW_STATE_STRM_CONFIG:Stream is configured. PCM/PDM parameters of the
 * Stream is updated to bus driver.
 * @SDW_STATE_STRM_PREPARE: Stream is Prepared. All the ports of Master and
 * Slave associated with this stream is prepared for enabling.
 * @SDW_STATE_STRM_ENABLE: Stream is enabled. All the ports of Master and
 * Slave associated with this stream are enable and now stream is active.
 * @SDW_STATE_STRM_DISABLE: Stream in disabled state, All the ports of
 * Master and Slave associated with the stream are disabled, and stream is
 * not active on bus.
 * @SDW_STATE_STRM_DEPREPARE: Stream in de-prepare state. All the ports of
 * Master and Slave associated with the stream are de-prepared.
 * @SDW_STATE_STRM_RELEASE: Stream in release state. Stream is not having
 * any PCM/PDM configuration. There is not Free state for stream, since
 * memory for the stream gets freed, and there is no way to update stream as
 * free.
 */
enum sdw_stream_state {
	SDW_STATE_STRM_ALLOC = 0,
	SDW_STATE_STRM_CONFIG = 1,
	SDW_STATE_STRM_PREPARE = 2,
	SDW_STATE_STRM_ENABLE = 3,
	SDW_STATE_STRM_DISABLE = 4,
	SDW_STATE_STRM_DEPREPARE = 5,
	SDW_STATE_STRM_RELEASE = 6,
};

/**
 * enum sdw_update_bus_ops: Operations performed by bus driver for stream
 * state transitions. Some of the operations are performed on individual
 * streams, while some are global operations affecting all the streams on
 * the bus.
 *
 * @SDW_BUS_PORT_PRE: Perform all the operations which is to be done before
 * initiating the bank switch for stream getting enabled. Master and Slave
 * driver may need to perform some operations before bank switch.  Call
 * Master and Slave handlers to accomplish device specific operations before
 * initiating bank switch.
 * @SDW_BUS_BANK_SWITCH: Initiate the bank switch operation by broadcasting
 * SCP_FrameCtrl register. Depending upon the Master implementation
 * broadcast will be finished as a part of this state, or Master may set
 * some register as a part of PORT_POST below operation after which
 * broadcast will be finished. Initiation of the broadcast message is done
 * as part of this operation. Broadcast message gets transmitted on the bus
 * during this or next operation is Master dependent.
 * @SDW_BUS_PORT_POST: Perform all the operations which are do be done after
 * initiating the Bank switch. Call Master and Slave handlers to perform
 * Post bank switch operation.
 * @SDW_BUS_BANK_SWITCH_WAIT: Bus driver waits here for the Bank switch to
 * be completed. This is used for Master(s) running in aggregation mode
 * where pre and post operations are performed before and after Bank switch
 * operation. The Bank switch message broadcast will happen only when clock
 * is enabled which is done as part of post Bank switch operation. After
 * post Bank switch operation, bus driver waits for response of Bank switch.
 * The bus driver provides SDW_BUS_PORT_PRE and SDW_BUS_PORT_POST for Bank
 * switch operation which are as per Master implementation.
 * @SDW_BUS_PORT_DIS_CHN: Disable all the ports of the alternate bank
 * (unused bank) after the bank switch. Once Bank switch operation is
 * successful, the running stream(s) enabled port channels on previous bank
 * needs to be disabled for both Master(s) and Slave(s).
 */
enum sdw_update_bus_ops {
	SDW_BUS_PORT_PRE,
	SDW_BUS_BANK_SWITCH,
	SDW_BUS_PORT_POST,
	SDW_BUS_BANK_SWITCH_WAIT,
	SDW_BUS_PORT_DIS_CHN,
};

/**
 * sdw_stream_params: Stream parameters.
 *
 * @rate: Sampling frequency
 * @channel_count: Number of channels.
 * @bps: bits per sample.
 */
struct sdw_stream_params {
	unsigned int rate;
	unsigned int channel_count;
	unsigned int bps;
};

/**
 * sdw_port_runtime: Holds the port parameters for each of the Master(s)
 * Slave(s) port associated with the stream.
 *
 * @port_num: Port number
 * @channel_mask: Channels of the Stream handled by this port.
 * @transport_params: Transport parameters of port.
 * @port_params: Port parameters
 * @port_node: Node to add the Port runtime to Master(s) or Slave(s) list.
 *
 * Port runtime is added to the Port List of Master(s) or Slave(s)
 * associated with stream.
 */
struct sdw_port_runtime {
	int port_num;
	int channel_mask;
	struct sdw_transport_params transport_params;
	struct sdw_port_params port_params;
	struct list_head port_node;
};

/**
 * struct sdw_bus_runtime: This structure holds the transport params and BW
 * required by the stream on the bus. There may be multiple bus associated
 * with the stream. This holds bus specific parameters of stream.
 *
 * @stream_bw: Bus Bandwidth required by this stream (bps).
 * @hstart: Horizontal Start column for this stream.
 * @hstop: Horizontal stop column for this stream.
 * @block_offset: Block offset for this stream.
 * @sub_block_offset: Sub Block offset for this stream.
 */
struct sdw_bus_runtime {
	unsigned int stream_bw;
	int hstart;
	int hstop;
	int block_offset;
	int sub_block_offset;

};

/**
 * struct sdw_slave_runtime: Holds the Stream parameters for the Slave
 * associated with the stream.
 *
 * @slv: Slave handle associated with this Stream
 * @sdw_rt: Stream handle to which this Slave stream is associated
 * @direction: Port Direction of the Slave for this Stream. Slave is
 * transmitting the Data or receiving the Data.
 * @stream_params: Stream parameters for Slave.
 * @port_rt_list: List of Slave Ports associated with this Stream.
 * @slave_strm_node: Stream runtime data structure maintains list of all the
 * Slave runtime instances associated with stream. This is the node to add
 * Slave runtime instance to that list. This list is used for the stream
 * configuration.
 * @slave_mstr_node: Master runtime data structure maintains list of all the
 * Slave runtime instances. This is the node to add Slave runtime instance
 * to that list. This list is used for Bandwidth calculation per bus. Slave
 * runtime instance gets added to two list one for stream configuration and
 * other for bandwidth calculation. Stream configuration is per stream where
 * there may be multiple Masters and Slave associated with Stream. Bandwidth
 * calculation is per Bus, where there is Single Master and Multiple Slaves
 * associated with bus.
 */

struct sdw_slv_runtime {
	struct sdw_slave *slv;
	struct sdw_runtime *sdw_rt;
	int direction;
	struct sdw_stream_params stream_params;
	struct list_head port_rt_list;
	struct list_head slave_strm_node;
	struct list_head slave_mstr_node;
};

/**
 * struct sdw_mstr_runtime: Holds the Stream parameters for the Master
 * associated with the stream.
 *
 * @mstr: Master handle associated with this stream.
 * @sdw_rt: Stream handle to which this Master stream is associated.
 * @stream_params: Stream parameters.
 * @port_rt_list: List of this Master Ports associated with this Stream.
 * @mstr_strm_node: Stream runtime data structure maintains list of all the
 * Master runtime instances associated with stream. This is the node to add
 * Master runtime instance to that list. This list is used for the stream
 * configuration.
 * @mstr_node: Master data structure maintains list of all the Master
 * runtime instances. This is the node to add Master runtime instance to to
 * that list. This list is used for Bandwidth calculation per bus.  Master
 * runtime instance gets added to two list one for stream configuration and
 * other for bandwidth calculation. Stream configuration is per stream where
 * there may be multiple Masters and Slave associated with Stream. Bandwidth
 * calculation is per Bus, where there is Single Master and Multiple Slaves
 * associated with bus.
 * @slv_rt_list: List of the Slave_runtime instances associated with this
 * Master_runtime. Its list of all the Slave(s) stream associated with this
 * Master. There may be stereo stream from Master to two Slaves, where L and
 * R samples from Master is received by two different Slave(s), so this list
 * contains the runtime structure associated with both Slaves.
 * @bus_rt: Bus parameters for the stream. There may be multiple bus
 * associated with stream. This bus_rt is for current Master.
 */
struct sdw_mstr_runtime {
	struct sdw_bus *bus;
	struct sdw_runtime *sdw_rt;
	int direction;
	struct sdw_stream_params stream_params;
	struct list_head port_rt_list;
	struct list_head mstr_strm_node;
	struct list_head mstr_node;
	struct list_head slv_rt_list;
	struct sdw_bus_runtime bus_rt;
};

/**
 * struct sdw_runtime: This structure holds runtime information for each
 * unique SoundWire stream.
 *
 * @tx_ref_count: Number of Transmit devices of stream. This may include
 * multiple Master(s) and Slave(s) based on how stream samples are split
 * between Mater and Slaves
 * @rx_ref_count: Number of Receive devices of stream. This may include
 * multiple Master(s) and Slave(s) based on how stream samples are split
 * between Mater and Slaves.
 * @stream_params: Steam parameters.
 * @slv_rt_list: List of the slaves part of this stream.
 * @mstr_rt_list: List of Masters part of this stream.
 * @type: Stream type PCM or PDM. This is not SoundWire concept, its used
 * inside bus driver for efficient BW management.
 * @stream_state: Current State of the stream.
 */

struct sdw_runtime {
	int tx_ref_count;
	int rx_ref_count;
	struct sdw_stream_params stream_params;
	struct list_head slv_rt_list;
	struct list_head mstr_rt_list;
	enum sdw_stream_type type;
	enum sdw_stream_state stream_state;
};

/**
 * struct sdw_stream_tag: Stream tag represents the unique SoundWire Audio
 * stream.  All the ports of the Master(s) and Slave(s) part of the same
 * stream tags gets enabled/disabled as a part of single bank Switch.If
 * samples of the stream are split between the Master(s), its Master
 * responsibility of synchronizing the bank switch of two individual
 * Masters.
 *
 * @stream_tag: Unique stream tag number.
 * @stream_lock: Lock for stream.
 * @ref_count: Number of times stream tag is allocated. Stream tag is is
 * available for allocation if reference count is 0.
 * @sdw_rt: Holds the stream runtime information.
 */

struct sdw_stream_tag {
	int stream_tag;
	struct mutex stream_lock;
	int ref_count;
	struct sdw_runtime *sdw_rt;
};

/**
 * struct sdw_index_to_col: Structure holding mapping of numbers to columns.
 *
 * @index: Holds index to number of columns.
 * @col: Holds actual columns.
 */
struct sdw_index_to_col {
	int index;
	int col;
};

/**
 * struct dw_index_to_row: Structure holding mapping of numbers to rows.
 *
 * @index: Holds index to number of rows.
 * @row: Holds actual rows.
 */
struct sdw_index_to_row {
	int index;
	int row;
};

/**
 * struct sdw_row_col_pair: Information for each row column pair. This is
 * used by bus driver for quick BW calculation.
 *
 * @row: Number of rows.
 * @col: Number of columns
 * @control_bits: Number of controls bits for this row-column pair.
 * @data_bits: Number of controls bits for this row-column pair.
 */
struct sdw_row_col_pair {
	int row;
	int col;
	int control_bits;
	int data_bits;
};

/**
 * struct sdw_core: Global SoundWire structure. It handles all the streams
 * spawned across masters and has list of bus structure per every
 * Master registered.
 *
 * @row_col_pair: Array holding all row-column pair possible as per MIPI
 * 1.1 Spec. This is used for quick reference for BW calculation
 * algorithm.
 *
 * @bus_list: List of all the bus instance.
 * @core_lock: Global lock for all bus instances.
 * @idr: For identifying the registered buses
 */
struct sdw_core {
	struct sdw_stream_tag stream_tags[SDW_NUM_STREAM_TAGS];
	struct sdw_row_col_pair row_col_pair[SDW_FRAME_ROW_COLS];
	struct list_head bus_list;
	struct mutex core_lock;
	struct idr idr;
};

void sdw_create_row_col_pair(void);
void sdw_init_bus_params(struct sdw_bus *sdw_bus);
int sdw_configure_dpn_intr(struct sdw_slave *slave, int port,
					bool enable, int mask);
#endif /* __SDW_BUS_H */
