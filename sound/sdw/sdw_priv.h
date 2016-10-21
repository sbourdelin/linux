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
 * sdw_bus: Bus structure holding bus related information.
 *
 * @bus_node: Node to add the bus in the sdw_core list.
 * @mstr: Master reference for the bus.
 */

struct sdw_bus {
	struct list_head bus_node;
	struct sdw_master *mstr;
};

/**
 * snd_sdw_core: Global SoundWire structure. It handles all the streams
 *	spawned across masters and has list of bus structure per every
 *	Master registered.
 *
 * @bus_list: List of all the bus instance.
 * @core_mutex: Global lock for all bus instances.
 * @idr: For identifying the registered buses.
 */
struct snd_sdw_core {
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

#endif /* _LINUX_SDW_PRIV_H */
