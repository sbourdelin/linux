/*
 * sdw_priv.h - Private definition for SoundWire bus interface.
 *
 * Author: Hardik Shah <hardik.t.shah@intel.com>
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

#ifndef _LINUX_SDW_PRIV_H
#define _LINUX_SDW_PRIV_H

#include <linux/kthread.h>
#include <linux/spinlock.h>

/* Number of message(s) transferred on bus. */
#define SDW_NUM_OF_MSG1_XFRD	1
#define SDW_NUM_OF_MSG2_XFRD	2
#define SDW_NUM_OF_MSG3_XFRD	3
#define SDW_NUM_OF_MSG4_XFRD	4

/**
 * Below values are not defined in MIPI standard. Completely arbitrary
 * values that can be changed at will.
 */
#define SDW_MAX_STREAM_TAG_KEY_SIZE	80
#define SDW_NUM_STREAM_TAGS		100 /* Max number of stream tags */
#define SDW_DOUBLE_RATE_FACTOR		2 /* Double rate */

/* TODO: Description to be provided for SDW_FREQ_MOD_FACTOR */
#define SDW_FREQ_MOD_FACTOR		3000

/**
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

/* Maximum number of Data Ports. */
#define SDW_MAX_DATA_PORTS		15

/**
 * Max retries to service Slave interrupts, once Slave is in ALERT state.
 * Bus driver tries to service interrupt till Slave state is changed to
 * "ATTACHED_OK". In case Slave remains in ALERT state because of error
 * condition on Slave like, PLL not getting locked or continuous Jack
 * sensing, bus driver exits after MAX retries.
 */
#define SDW_INTR_STAT_READ_MAX_TRIES	10

extern struct snd_sdw_core snd_sdw_core;

/**
 * sdw_driver: Structure to typecast both Master and Slave driver to generic
 *	SoundWire driver, to find out the driver type.
 *
 * @driver_type: Type of SoundWire driver, Master or Slave.
 * @driver: Generic Linux driver.
 */
struct sdw_driver {
	enum sdw_driver_type driver_type;
	struct device_driver driver;
};
#define to_sdw_driver(d)			\
		container_of(d, struct sdw_driver, driver)

/**
 * sdw_stream_state: Stream state maintained by bus driver for performing
 *	stream operations.
 *
 * @SDW_STATE_STRM_ALLOC: New stream is allocated.
 * @SDW_STATE_STRM_CONFIG: Stream is configured. PCM/PDM parameters of the
 *	Stream is updated to bus driver.
 *
 * @SDW_STATE_STRM_PREPARE: Stream is Prepared. All the ports of Master and
 *	Slave associated with this stream is prepared for enabling.
 *
 * @SDW_STATE_STRM_ENABLE: Stream is enabled. All the ports of Master and
 *	Slave associated with this stream are enable and now stream is
 *	active.
 *
 * @SDW_STATE_STRM_DISABLE: Stream in disabled state, All the ports of
 *	Master and Slave associated with the stream are disabled, and stream
 *	is not active on bus.
 *
 * @SDW_STATE_STRM_DEPREPARE: Stream in de-prepare state. All the ports of
 *	Master and Slave associated with the stream are de-prepared.
 *
 * @SDW_STATE_STRM_RELEASE: Stream in release state. Stream is not having
 *	any PCM/PDM configuration. There is not Free state for stream, since
 *	memory for the stream gets freed, and there is no way to update
 *	stream as free.
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
 * sdw_update_bus_ops: Operations performed by bus driver for stream state
 *	transitions. Some of the operations are performed on individual
 *	streams, while some are global operations affecting all the streams
 *	on the bus.
 *
 * @SDW_BUS_PORT_PRE: Perform all the operations which is to be done before
 *	initiating the bank switch for stream getting enabled. Master and
 *	Slave driver may need to perform some operations before bank switch.
 *	Call Master and Slave handlers to accomplish device specific
 *	operations before initiating bank switch.
 *
 * @SDW_BUS_BANK_SWITCH: Initiate the bank switch operation by broadcasting
 *	SCP_FrameCtrl register. Depending upon the Master implementation
 *	broadcast will be finished as a part of this state, or Master may
 *	set some register as a part of PORT_POST below operation after which
 *	broadcast will be finished. Initiation of the broadcast message is
 *	done as part of this operation. Broadcast message gets transmitted
 *	on the bus during this or next operation is Master dependent.
 *
 * @SDW_BUS_PORT_POST: Perform all the operations which are do be done after
 *	initiating the Bank switch. Call Master and Slave handlers to
 *	perform Post bank switch operation.
 *
 * @SDW_BUS_BANK_SWITCH_WAIT: Bus driver waits here for the Bank switch to
 *	be completed. This is used for Master(s) running in aggregation mode
 *	where pre and post operations are performed before and after Bank
 *	switch operation. The Bank switch message broadcast will happen only
 *	when clock is enabled which is done as part of post Bank switch
 *	operation. After post Bank switch operation, bus driver waits for
 *	response of Bank switch. The bus driver provides SDW_BUS_PORT_PRE
 *	and SDW_BUS_PORT_POST for Bank switch operation which are as per
 *	Master implementation.
 *
 * @SDW_BUS_PORT_DIS_CHN: Disable all the ports of the alternate bank
 *	(unused bank) after the bank switch. Once Bank switch operation is
 *	successful, the running stream(s) enabled port channels on previous
 *	bank needs to be disabled for both Master(s) and Slave(s).
 */
enum sdw_update_bus_ops {
	SDW_BUS_PORT_PRE,
	SDW_BUS_BANK_SWITCH,
	SDW_BUS_PORT_POST,
	SDW_BUS_BANK_SWITCH_WAIT,
	SDW_BUS_PORT_DIS_CHN,
};

/**
 * sdw_stream_tag: Stream tag represents the unique SoundWire Audio stream.
 *	All the ports of the Master(s) and Slave(s) part of the same stream
 *	tags gets enabled/disabled as a part of single bank Switch.If
 *	samples of the stream are split between the Master(s), its Master
 *	responsibility of synchronizing the bank switch of two individual
 *	Masters.
 *
 * @stream_tag: Unique stream tag number.
 * @stream_lock: Lock for stream.
 * @ref_count: Number of times stream tag is allocated. Stream tag is is
 *	available for allocation if reference count is 0.
 *
 * @sdw_rt: Holds the stream runtime information.
 */
struct sdw_stream_tag {
	int stream_tag;
	struct mutex stream_lock;
	int ref_count;
	struct sdw_runtime *sdw_rt;
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
 *	Slave(s) port associated with the stream.
 *
 * @port_num: Port number.
 * @channel_mask: Channels of the Stream handled by this port.
 * @transport_params: Transport parameters of port.
 * @port_params: Port parameters
 * @port_node: Port runtime is added to the Port List of Master(s) or
 *	Slave(s) associated with stream. Node to add the Port runtime to
 *	Master(s) or Slave(s) list.
 */
struct sdw_port_runtime {
	int port_num;
	int channel_mask;
	struct sdw_transport_params transport_params;
	struct sdw_port_params port_params;
	struct list_head port_node;
};

/**
 * sdw_slave_runtime: Holds the Stream parameters for the Slave associated
 *	with the stream.
 *
 * @slv: Slave handle associated with this Stream.
 * @sdw_rt: Stream handle to which this Slave stream is associated.
 * @direction: Port Direction of the Slave for this Stream. Slave is
 *	transmitting the Data or receiving the Data.
 *
 * @stream_params: Stream parameters for Slave.
 * @port_rt_list: List of Slave Ports associated with this Stream.
 * @slave_strm_node: Stream runtime data structure maintains list of all the
 *	Slave runtime instances associated with stream. This is the node to
 *	add Slave runtime instance to that list. This list is used for the
 *	stream configuration.
 *
 * @slave_mstr_node: Master runtime data structure maintains list of all the
 *	Slave runtime instances. This is the node to add Slave runtime
 *	instance to that list. This list is used for Bandwidth calculation
 *	per bus. Slave runtime instance gets added to two list one for
 *	stream configuration and other for bandwidth calculation. Stream
 *	configuration is per stream where there may be multiple Masters and
 *	Slave associated with Stream. Bandwidth calculation is per Bus,
 *	where there is Single Master and Multiple Slaves associated with
 *	bus.
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
 * sdw_bus_runtime: This structure holds the transport params and BW
 *	required by the stream on the bus. There may be multiple bus
 *	associated with the stream. This holds bus specific parameters of
 *	stream. TODO: Currently sdw_bus_runtime is part of sdw_mstr_runtime
 *	Master handle. Once stream between Slave to Slave is supported by
 *	bus driver, this needs to be made part of sdw_runtime handle.
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
 * sdw_mstr_runtime: Holds the Stream parameters for the Master associated
 *		with the stream.
 *
 * @mstr: Master handle associated with this stream.
 * @sdw_rt: Stream handle to which this Master stream is associated.
 * @stream_params: Stream parameters.
 * @port_rt_list: List of this Master Ports associated with this Stream.
 * @mstr_strm_node: Stream runtime data structure maintains list of all the
 *	Master runtime instances associated with stream. This is the node to
 *	add Master runtime instance to that list. This list is used for the
 *	stream configuration.
 *
 * @mstr_node: Master data structure maintains list of all the Master
 *	runtime instances. This is the node to add Master runtime instance
 *	to to that list. This list is used for Bandwidth calculation per
 *	bus.  Master runtime instance gets added to two list one for stream
 *	configuration and other for bandwidth calculation. Stream
 *	configuration is per stream where there may be multiple Masters and
 *	Slave associated with Stream. Bandwidth calculation is per Bus,
 *	where there is Single Master and Multiple Slaves associated with
 *	bus.
 *
 * @slv_rt_list: List of the Slave_runtime instances associated with this
 *	Master_runtime. Its list of all the Slave(s) stream associated with
 *	this Master. There may be stereo stream from Master to two Slaves,
 *	where L and R samples from Master is received by two different
 *	Slave(s), so this list contains the runtime structure associated
 *	with both Slaves.
 *
 * @bus_rt: Bus parameters for the stream. There may be multiple bus
 *	associated with stream. This bus_rt is for current Master.
 */
struct sdw_mstr_runtime {
	struct sdw_master *mstr;
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
 * sdw_runtime: This structure holds runtime information for each unique
 *	SoundWire stream.
 *
 * @tx_ref_count: Number of Transmit devices of stream. This may include
 *	multiple Master(s) and Slave(s) based on how stream samples are
 *	split between Mater and Slaves.
 * @rx_ref_count: Number of Receive devices of stream. This may include
 *	multiple Master(s) and Slave(s) based on how stream samples are
 *	split between Mater and Slaves.
 *
 * @stream_params: Steam parameters.
 * @slv_rt_list: List of the slaves part of this stream.
 * @mstr_rt_list: List of Masters part of this stream.
 * @type: Stream type PCM or PDM.This is not SoundWire concept, its used
 *	inside bus driver for efficient BW management.
 *
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
 * sdw_slv_status: List of Slave status.
 *
 * @node: Node for adding status to list of Slave status.
 * @status: Slave status.
 */
struct sdw_slv_status {
	struct list_head node;
	enum sdw_slave_status status[SDW_MAX_DEVICES];
};

/**
 * sdw_bus: Bus structure holding bus related information.
 *
 * @bus_node: Node to add the bus in the sdw_core list.
 * @mstr: Master reference for the bus.
 * @clk_state: State of the clock.
 * @active_bank: Current bank in use.
 * @max_clk_dr_freq: Maximum double rate clock frequency. This is maximum
 * double clock rate supported per bus.
 * @curr_clk_dr_freq: Current double rate clock frequency in use. This is
 *	current clock rate at which bus is running.
 *
 * @clk_div: Current clock divider in use.
 * @bandwidth: Total bandwidth.
 * @system_interval: Bus System interval (Stream Synchronization Point).
 * @stream_interval: Stream interval.
 * @frame_freq: SoundWire Frame frequency on bus.
 * @col: Active columns.
 * @row: Active rows.
 * @status_thread: Thread to process the Slave status.
 * @kworker: Worker for updating the Slave status.
 * @kwork: Work for worker
 * @status_list: List where status update from master is added. List is
 *	executed one by one.
 *
 * @spinlock: Lock to protect the list between work thread and interrupt
 *	context. Bus driver does Slave status processing in the thread
 *	context, spinlock is used to put the status reported by Master into
 *	the status list which is processed by bus driver in thread context
 *	later.
 *
 * @data: Data to be provided by bus driver for calling xfer_msg_deferred
 *	callback of Master driver.

 */

struct sdw_bus {
	struct list_head bus_node;
	struct sdw_master *mstr;
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
	struct task_struct *status_thread;
	struct kthread_worker kworker;
	struct kthread_work kwork;
	struct list_head status_list;
	spinlock_t spinlock;
	struct sdw_deferred_xfer_data data;
};

/**
 * sdw_row_col_pair: Information for each row column pair. This is used by
 *	bus driver for quick BW calculation.
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
 * snd_sdw_core: Global SoundWire structure. It handles all the streams
 *	spawned across masters and has list of bus structure per every
 *	Master registered.
 *
 * @row_col_pair: Array holding all row-column pair possible as per MIPI
 *	1.1 Spec. This is used for quick reference for BW calculation
 *	algorithm.
 *
 * @bus_list: List of all the bus instance.
 * @core_mutex: Global lock for all bus instances.
 * @idr: For identifying the registered buses.
 */
struct snd_sdw_core {
	struct sdw_stream_tag stream_tags[SDW_NUM_STREAM_TAGS];
	struct sdw_row_col_pair row_col_pair[MAX_NUM_ROW_COLS];
	struct list_head bus_list;
	struct mutex core_mutex;
	struct idr idr;
};

/**
 * sdw_bank_switch_deferred: Initiate the transfer of the message but
 *	doesn't wait for the message to be completed. Bus driver waits
 *	outside context of this API for master driver to signal message
 *	transfer complete. This is not Public API, this is used by Bus
 *	driver only for Bank switch.
 *
 * @mstr: Master which will transfer the message.
 * @msg: Message to be transferred. Message length of only 1 is supported.
 * @data: Deferred information for the message to be transferred. This is
 *	filled by Master on message transfer complete.
 *
 * Returns immediately after initiating the transfer, Bus driver needs to
 * wait on xfer_complete, part of data, which is set by Master driver on
 * completion of message transfer.
 */
void sdw_bank_switch_deferred(struct sdw_master *mstr, struct sdw_msg *msg,
				struct sdw_deferred_xfer_data *data);

/**
 * sdw_index_to_col: Structure holding mapping of numbers to columns.
 *
 * @index: Holds index to number of columns.
 * @col: Holds actual columns.
 */
struct sdw_index_to_col {
	int index;
	int col;
};

/**
 * sdw_index_to_row: Structure holding mapping of numbers to rows.
 *
 * @index: Holds index to number of rows.
 * @row: Holds actual rows.
 */
struct sdw_index_to_row {
	int index;
	int row;
};

/**
 * sdw_group_params: Structure holding temporary variable while computing
 *	transport parameters of Master(s) and Slave(s).
 *
 * @rate: Holds stream rate.
 * @full_bw: Holds full bandwidth per group.
 * @payload_bw: Holds payload bandwidth per group.
 * @hwidth: Holds hwidth per group.
 */
struct sdw_group_params {
	int rate;
	int full_bw;
	int payload_bw;
	int hwidth;
};

/**
 * sdw_group_count: Structure holding group count and stream rate array
 *	while computing transport parameters of Master(s) and Slave(s).
 *
 * @group_count: Holds actual group count.
 * @max_size: Holds maximum capacity of array.
 * @stream_rates: Pointer to stream rates.
 */
struct sdw_group_count {
	unsigned int group_count;
	unsigned int max_size;
	unsigned int *stream_rates;
};

/**
 * sdw_enable_disable_dpn_intr: Enable or Disable Slave Data Port interrupt.
 *	This is called by bus driver before prepare and after deprepare of
 *	the ports.
 *
 * @sdw_slv: Slave handle.
 * @port_num: Port number.
 * @port_direction: Direction of the port configuration while doing
 *	prepare/deprepare.
 *
 * @enable: Enable (1) or disable (0) the port interrupts.
 */
int sdw_enable_disable_dpn_intr(struct sdw_slave *sdw_slv, int port_num,
					int port_direction, bool enable);

/**
 * sdw_create_row_col_pair: Initialization of bandwidth related operations.
 *	This is required to have fast path for the BW calculation when a new
 *	stream is prepared or deprepared. This is called only once as part
 *	of SoundWire Bus driver getting initialized.
 */
void sdw_create_row_col_pair(void);

/**
 * sdw_init_bus_params: Sets up bus data structure for BW calculation. This
 *	is called once per each Master interface registration to the
 *	SoundWire bus.
 *
 * @sdw_bus: Bus handle.
 */
void sdw_init_bus_params(struct sdw_bus *sdw_bus);

/**
 * sdw_prepare_and_enable_ops: This is called by the bus driver for doing
 *	operations related to stream prepare and enable. sdw_bus_ops are
 *	performed on bus for preparing and enabling of the streams.
 *
 * @stream_tag: Stream tag on which operations needs to be performed.
 */
int sdw_prepare_and_enable_ops(struct sdw_stream_tag *stream_tag);

/**
 * sdw_disable_and_deprepare_ops: This is called by the bus driver for doing
 *	operations related to stream disable and de-prepare.sdw_bus_ops are
 *	performed on bus for disabling and de-preparing of the streams.
 *
 * @stream_tag: Stream tag on which operations needs to be performed.
 */
int sdw_disable_and_deprepare_ops(struct sdw_stream_tag *stream_tag);

/**
 * sdw_get_slv_dpn_caps: Get the data port capabilities based on the port
 *	number and port direction.
 *
 * @slv_cap: Slave capabilities.
 * @direction: Port data direction.
 * @port_num: Port number.
 */
struct sdw_dpn_caps *sdw_get_slv_dpn_cap(struct sdw_slave_caps *slv_cap,
			enum sdw_data_direction direction,
			unsigned int port_num);

/* Return bus structure */
static inline struct sdw_bus *sdw_master_to_bus(struct sdw_master *mstr)
{
	return mstr->bus;
}

/* Reference count increment */
static inline void sdw_inc_ref_count(int *ref_count)
{
	(*ref_count)++;
}

/* Reference count decrement */
static inline void sdw_dec_ref_count(int *ref_count)
{
	(*ref_count)--;
}

/*
 * Helper function for bus driver to write messages. Since bus driver
 * operates on MIPI defined Slave registers, addr_page1 and addr_page2 is
 * set to 0.
 */
static inline int sdw_wr_msg(struct sdw_msg *msg, bool xmit_on_ssp, u16 addr,
					u16 len, u8 *buf, u8 dev_num,
					struct sdw_master *mstr,
					int num_msg)
{
	msg->xmit_on_ssp = xmit_on_ssp;
	msg->r_w_flag = SDW_MSG_FLAG_WRITE;
	msg->addr = addr;
	msg->len = len;
	msg->buf = buf;
	msg->dev_num = dev_num;
	msg->addr_page1 = 0x0;
	msg->addr_page2 = 0x0;

	return snd_sdw_slave_transfer(mstr, msg, num_msg);
}

/*
 * Helper function for bus driver to read messages. Since bus driver
 * operates on MIPI defined Slave registers, addr_page1 and addr_page2 is
 * set to 0.
 */
static inline int sdw_rd_msg(struct sdw_msg *msg, bool xmit_on_ssp, u16 addr,
					u16 len, u8 *buf, u8 dev_num,
					struct sdw_master *mstr,
					int num_msg)
{
	msg->xmit_on_ssp = xmit_on_ssp;
	msg->r_w_flag = SDW_MSG_FLAG_READ;
	msg->addr = addr;
	msg->len = len;
	msg->buf = buf;
	msg->dev_num = dev_num;
	msg->addr_page1 = 0x0;
	msg->addr_page2 = 0x0;

	return snd_sdw_slave_transfer(mstr, msg, num_msg);
}

/*
 * Helper function for bus driver to write messages (nopm version). Since
 * bus driver operates on MIPI defined Slave registers, addr_page1 and
 * addr_page2 is set to 0.
 */
static inline int sdw_wr_msg_nopm(struct sdw_msg *msg, bool xmit_on_ssp,
					u16 addr, u16 len, u8 *buf,
					u8 dev_num,
					struct sdw_master *mstr,
					int num_msg)
{
	msg->xmit_on_ssp = xmit_on_ssp;
	msg->r_w_flag = SDW_MSG_FLAG_WRITE;
	msg->addr = addr;
	msg->len = len;
	msg->buf = buf;
	msg->dev_num = dev_num;
	msg->addr_page1 = 0x0;
	msg->addr_page2 = 0x0;

	return snd_sdw_slave_transfer(mstr, msg, num_msg);
}

/*
 * Helper function for bus driver to read messages (nopm version). Since
 * bus driver operates on MIPI defined Slave registers, addr_page1 and
 * addr_page2 is set to 0.
 */
static inline int sdw_rd_msg_nopm(struct sdw_msg *msg, bool xmit_on_ssp,
					u16 addr, u16 len, u8 *buf,
					u8 dev_num,
					struct sdw_master *mstr,
					int num_msg)
{
	msg->xmit_on_ssp = xmit_on_ssp;
	msg->r_w_flag = SDW_MSG_FLAG_READ;
	msg->addr = addr;
	msg->len = len;
	msg->buf = buf;
	msg->dev_num = dev_num;
	msg->addr_page1 = 0x0;
	msg->addr_page2 = 0x0;

	return snd_sdw_slave_transfer(mstr, msg, num_msg);
}

/*
 * Helper function for bus driver to create read messages. Since bus driver
 * operates on MIPI defined Slave registers, addr_page1 and addr_page2 is
 * set to 0.
 */
static inline void sdw_create_rd_msg(struct sdw_msg *msg, bool xmit_on_ssp,
				u16 addr, u16 len, u8 *buf, u8 dev_num)
{
	msg->xmit_on_ssp = xmit_on_ssp;
	msg->r_w_flag = SDW_MSG_FLAG_READ;
	msg->addr = addr;
	msg->len = len;
	msg->buf = buf;
	msg->dev_num = dev_num;
	msg->addr_page1 = 0x0;
	msg->addr_page2 = 0x0;
}

/*
 * Helper function for bus driver to create write messages. Since bus driver
 * operates on MIPI defined Slave registers, addr_page1 and addr_page2 is
 * set to 0.
 */
static inline void sdw_create_wr_msg(struct sdw_msg *msg, bool xmit_on_ssp,
				u16 addr, u16 len, u8 *buf, u8 dev_num)
{
	msg->xmit_on_ssp = xmit_on_ssp;
	msg->r_w_flag = SDW_MSG_FLAG_WRITE;
	msg->addr = addr;
	msg->len = len;
	msg->buf = buf;
	msg->dev_num = dev_num;
	msg->addr_page1 = 0x0;
	msg->addr_page2 = 0x0;
}

/* Retrieve and return channel count from channel mask */
static inline int sdw_chn_mask_to_chn(int chn_mask)
{
	int c = 0;

	for (c = 0; chn_mask; chn_mask >>= 1)
		c += chn_mask & 1;

	return c;
}

/* Fill transport parameter data structure */
static inline void sdw_fill_xport_params(struct sdw_transport_params *params,
					int port_num,
					bool grp_ctrl_valid,
					int grp_ctrl,
					int off1, int off2,
					int hstart, int hstop,
					int pack_mode, int lane_ctrl)
{

	params->port_num = port_num;
	params->blk_grp_ctrl_valid = grp_ctrl_valid;
	params->blk_grp_ctrl = grp_ctrl;
	params->offset1 = off1;
	params->offset2 = off2;
	params->hstart = hstart;
	params->hstop = hstop;
	params->blk_pkg_mode = pack_mode;
	params->lane_ctrl = lane_ctrl;
}

#endif /* _LINUX_SDW_PRIV_H */
