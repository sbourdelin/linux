/*
 * sdw.c - SoundWire bus driver implementation.
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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/pm_domain.h>
#include <sound/sdw_bus.h>
#include <sound/sdw_master.h>
#include <sound/sdw_slave.h>
#include <sound/sdw/sdw_registers.h>

#include "sdw_priv.h"

#define CREATE_TRACE_POINTS
#include <trace/events/sdw.h>
/*
 * Global SoundWire core instance contains list of Masters registered, core
 *	lock and SoundWire stream tags.
 */
struct snd_sdw_core snd_sdw_core;

static void sdw_slv_release(struct device *dev)
{
	kfree(to_sdw_slave(dev));
}

static void sdw_mstr_release(struct device *dev)
{
	struct sdw_master *mstr = to_sdw_master(dev);

	complete(&mstr->slv_released_complete);
}

static struct device_type sdw_slv_type = {
	.groups		= NULL,
	.release	= sdw_slv_release,
};

static struct device_type sdw_mstr_type = {
	.groups		= NULL,
	.release	= sdw_mstr_release,
};

/**
 * sdw_slv_verify - return parameter as sdw_slave, or NULL
 * @dev: device, probably from some driver model iterator
 *
 * When traversing the driver model tree, perhaps using driver model
 * iterators like @device_for_each_child(), you can't assume very much
 * about the nodes you find. Use this function to avoid oopses caused
 * by wrongly treating some non-SDW device as an sdw_slave.
 */
static struct sdw_slave *sdw_slv_verify(struct device *dev)
{
	return (dev->type == &sdw_slv_type)
			? to_sdw_slave(dev)
			: NULL;
}

/**
 * sdw_mstr_verify: return parameter as sdw_master, or NULL
 *
 * @dev: device, probably from some driver model iterator
 *
 * When traversing the driver model tree, perhaps using driver model
 * iterators like @device_for_each_child(), you can't assume very much
 * about the nodes you find. Use this function to avoid oopses caused
 * by wrongly treating some non-SDW device as an sdw_master.
 */
static struct sdw_master *sdw_mstr_verify(struct device *dev)
{
	return (dev->type == &sdw_mstr_type)
			? to_sdw_master(dev)
			: NULL;
}

static const struct sdw_slave_id *sdw_match_slv(const struct sdw_slave_id *id,
					const struct sdw_slave *sdw_slv)
{
	const struct sdw_slave_priv *slv_priv = &sdw_slv->priv;

	if (!id)
		return NULL;

	/*
	 * IDs should be NULL terminated like the last ID in the list should
	 * be null, as done for drivers like platform, i2c etc.
	 */
	while (id->name[0]) {
		if (strncmp(slv_priv->name, id->name, SOUNDWIRE_NAME_SIZE) == 0)
			return id;

		id++;
	}

	return NULL;
}

static const struct sdw_master_id *sdw_match_mstr(
			const struct sdw_master_id *id,
			const struct sdw_master *sdw_mstr)
{
	if (!id)
		return NULL;

	/*
	 * IDs should be NULL terminated like the last ID in the list should
	 * be null, as done for drivers like platform, i2c etc.
	 */
	while (id->name[0]) {
		if (strncmp(sdw_mstr->name, id->name, SOUNDWIRE_NAME_SIZE) == 0)
			return id;
		id++;
	}
	return NULL;
}

static int sdw_slv_match(struct device *dev, struct device_driver *driver)
{
	struct sdw_slave *sdw_slv;
	struct sdw_driver *sdw_drv = to_sdw_driver(driver);
	struct sdw_slave_driver *drv;
	int ret = 0;


	if (sdw_drv->driver_type != SDW_DRIVER_TYPE_SLAVE)
		return ret;

	drv = to_sdw_slave_driver(driver);
	sdw_slv = to_sdw_slave(dev);

	/*
	 * We are matching based on the dev_id field, dev_id field is unique
	 * based on part_id and manufacturer id. Device will be registered
	 * based on dev_id and driver will also have same dev_id for device
	 * its controlling.
	 */
	ret = (sdw_match_slv(drv->id_table, sdw_slv) != NULL);

	if (ret < 0)
		sdw_slv->priv.driver = drv;

	return ret;
}

static int sdw_mstr_match(struct device *dev, struct device_driver *driver)
{
	struct sdw_master *sdw_mstr;
	struct sdw_driver *sdw_drv = to_sdw_driver(driver);
	struct sdw_master_driver *drv;
	int ret = 0;

	if (sdw_drv->driver_type != SDW_DRIVER_TYPE_MASTER)
		return ret;

	drv = to_sdw_master_driver(driver);
	sdw_mstr = to_sdw_master(dev);

	ret = (sdw_match_mstr(drv->id_table, sdw_mstr) != NULL);

	if (driver->name && !ret)
		ret = (strncmp(sdw_mstr->name, driver->name,
			SOUNDWIRE_NAME_SIZE) == 0);

	if (ret < 0)
		sdw_mstr->driver = drv;

	return ret;
}

static int sdw_mstr_probe(struct device *dev)
{
	const struct sdw_master_driver *sdrv =
					to_sdw_master_driver(dev->driver);
	struct sdw_master *mstr = to_sdw_master(dev);
	int ret;

	ret = dev_pm_domain_attach(dev, true);

	if (ret != -EPROBE_DEFER) {
		ret = sdrv->probe(mstr, sdw_match_mstr(sdrv->id_table, mstr));
		if (ret < 0)
			dev_pm_domain_detach(dev, true);
	}

	return ret;
}

static int sdw_slv_probe(struct device *dev)
{
	const struct sdw_slave_driver *sdrv = to_sdw_slave_driver(dev->driver);
	struct sdw_slave *sdwslv = to_sdw_slave(dev);
	int ret;

	ret = dev_pm_domain_attach(dev, true);

	if (ret != -EPROBE_DEFER) {
		ret = sdrv->probe(sdwslv, sdw_match_slv(sdrv->id_table,
							sdwslv));
		if (ret < 0)
			dev_pm_domain_detach(dev, true);
	}

	return ret;
}

static int sdw_mstr_remove(struct device *dev)
{
	const struct sdw_master_driver *sdrv =
				to_sdw_master_driver(dev->driver);
	int ret;

	ret = sdrv->remove(to_sdw_master(dev));
	dev_pm_domain_detach(dev, true);
	return ret;

}

static int sdw_slv_remove(struct device *dev)
{
	const struct sdw_slave_driver *sdrv = to_sdw_slave_driver(dev->driver);
	int ret;

	ret = sdrv->remove(to_sdw_slave(dev));
	dev_pm_domain_detach(dev, true);

	return ret;
}

static void sdw_slv_shutdown(struct device *dev)
{
	const struct sdw_slave_driver *sdrv =
				to_sdw_slave_driver(dev->driver);

	sdrv->shutdown(to_sdw_slave(dev));
}

static void sdw_mstr_shutdown(struct device *dev)
{
	const struct sdw_master_driver *sdrv =
				to_sdw_master_driver(dev->driver);

	sdrv->shutdown(to_sdw_master(dev));
}

static int sdw_match(struct device *dev, struct device_driver *driver)
{
	struct sdw_slave *sdw_slv;
	struct sdw_master *sdw_mstr;

	sdw_slv = sdw_slv_verify(dev);
	if (sdw_slv)
		return sdw_slv_match(dev, driver);

	sdw_mstr = sdw_mstr_verify(dev);
	if (sdw_mstr)
		return sdw_mstr_match(dev, driver);

	/*
	 * Returning 0 to calling function means match not found, so calling
	 * function will not call probe
	 */
	return 0;

}

static const struct dev_pm_ops soundwire_pm = {
	.suspend = pm_generic_suspend,
	.resume = pm_generic_resume,
	SET_RUNTIME_PM_OPS(
		pm_generic_runtime_suspend,
		pm_generic_runtime_resume,
		NULL)
};

static struct bus_type sdw_bus_type = {
	.name		= "soundwire",
	.match		= sdw_match,
	.pm		= &soundwire_pm,
};
static struct static_key sdw_trace_msg = STATIC_KEY_INIT_FALSE;

void sdw_transfer_trace_reg(void)
{
	static_key_slow_inc(&sdw_trace_msg);
}

void sdw_transfer_trace_unreg(void)
{
	static_key_slow_dec(&sdw_trace_msg);
}

static int sdw_find_free_dev_num(struct sdw_master *mstr,
				struct sdw_msg *msg)
{
	int i, ret = -EINVAL;

	mutex_lock(&mstr->lock);

	for (i = 1; i <= SDW_MAX_DEVICES; i++) {
		if (mstr->sdw_addr[i].assigned == true)
			continue;

		mstr->sdw_addr[i].assigned = true;

		memcpy(mstr->sdw_addr[i].dev_id, msg->buf,
				SDW_NUM_DEV_ID_REGISTERS);

		ret = i;
		break;
	}

	mutex_unlock(&mstr->lock);
	return ret;
}

static int sdw_program_dev_num(struct sdw_master *mstr, u8 dev_num)
{
	struct sdw_msg msg;
	u8 buf;
	int ret;

	buf = dev_num;
	ret = sdw_wr_msg(&msg, 0, SDW_SCP_DEVNUMBER, 1, &buf, 0x0, mstr,
						SDW_NUM_OF_MSG1_XFRD);
	if (ret != SDW_NUM_OF_MSG1_XFRD) {
		ret = -EINVAL;
		dev_err(&mstr->dev, "Program Slave address failed ret = %d\n", ret);
		return ret;
	}

	return 0;
}

static bool sdw_find_slv(struct sdw_master *mstr, struct sdw_msg *msg,
						unsigned int *dev_num)
{
	struct sdw_slave_addr *sdw_addr;
	int i, comparison;
	bool found = false;

	mutex_lock(&mstr->lock);

	/*
	 * Device number resets to 0, when Slave gets unattached. Find the
	 * already registered Slave, mark it as present and program the
	 * Slave address again with same value.
	 */
	sdw_addr = mstr->sdw_addr;

	for (i = 1; i <= SDW_MAX_DEVICES; i++) {
		comparison = memcmp(sdw_addr[i].dev_id, msg->buf,
				SDW_NUM_DEV_ID_REGISTERS);

		if ((!comparison) && (sdw_addr[i].assigned == true)) {
			found = true;
			*dev_num = i;
			break;
		}
	}

	mutex_unlock(&mstr->lock);

	return found;
}

static void sdw_free_dev_num(struct sdw_master *mstr, int dev_num)
{
	int i;

	mutex_lock(&mstr->lock);

	for (i = 0; i <= SDW_MAX_DEVICES; i++) {

		if (dev_num == mstr->sdw_addr[i].dev_num) {

			mstr->sdw_addr[dev_num].assigned = false;
			memset(&mstr->sdw_addr[dev_num].dev_id[0], 0x0,
					SDW_NUM_DEV_ID_REGISTERS);
			break;
		}
	}

	mutex_unlock(&mstr->lock);
}

static int sdw_slv_register(struct sdw_master *mstr)
{
	int ret, i;
	struct sdw_msg msg;
	u8 buf[SDW_NUM_DEV_ID_REGISTERS];
	struct sdw_slave *sdw_slave;
	int dev_num = -1;
	bool found = false;

	/* Create message to read the 6 dev_id registers */
	sdw_create_rd_msg(&msg, 0, SDW_SCP_DEVID_0, SDW_NUM_DEV_ID_REGISTERS,
								buf, 0x0);

	/*
	 * Multiple Slaves may report an Attached_OK status as Device0.
	 * Since the enumeration relies on a hardware arbitration and is
	 * done one Slave at a time, a loop needs to run until all Slaves
	 * have been assigned a non-zero DeviceNumber. The loop exits when
	 * the reads from Device0 devID registers are no longer successful,
	 * i.e. there is no Slave left to enumerate
	 */
	while ((ret = (snd_sdw_slave_transfer(mstr, &msg, SDW_NUM_OF_MSG1_XFRD))
					== SDW_NUM_OF_MSG1_XFRD)) {

		/*
		 * Find is Slave is re-enumerating, and was already
		 * registered earlier.
		 */
		found = sdw_find_slv(mstr, &msg, &dev_num);

		/*
		 * Reprogram the Slave device number if its getting
		 * re-enumerated. If that fails we continue finding new
		 * slaves, we flag error but don't stop since there may be
		 * new Slaves trying to get enumerated.
		 */
		if (found) {
			ret = sdw_program_dev_num(mstr, dev_num);
			if (ret < 0)
				dev_err(&mstr->dev, "Re-registering slave failed ret = %d", ret);

			continue;

		}

		/*
		 * Find the free device_number for the new Slave getting
		 * enumerated 1st time.
		 */
		dev_num = sdw_find_free_dev_num(mstr, &msg);
		if (dev_num < 0) {
			dev_err(&mstr->dev, "Failed to find free dev_num ret = %d\n", ret);
			goto dev_num_assign_fail;
		}

		/*
		 * Allocate and initialize the Slave device on first
		 * enumeration
		 */
		sdw_slave = kzalloc(sizeof(*sdw_slave), GFP_KERNEL);
		if (!sdw_slave) {
			ret = -ENOMEM;
			goto mem_alloc_failed;
		}

		/*
		 * Initialize the allocated Slave device, set bus type and
		 * device type to SoundWire.
		 */
		sdw_slave->mstr = mstr;
		sdw_slave->dev.parent = &sdw_slave->mstr->dev;
		sdw_slave->dev.bus = &sdw_bus_type;
		sdw_slave->dev.type = &sdw_slv_type;
		sdw_slave->priv.addr = &mstr->sdw_addr[dev_num];
		sdw_slave->priv.addr->slave = sdw_slave;

		for (i = 0; i < SDW_NUM_DEV_ID_REGISTERS; i++)
			sdw_slave->priv.dev_id[i] = msg.buf[i];

		dev_dbg(&mstr->dev, "SDW slave slave id found with values\n");
		dev_dbg(&mstr->dev, "dev_id0 to dev_id5: %x:%x:%x:%x:%x:%x\n",
			msg.buf[0], msg.buf[1], msg.buf[2],
			msg.buf[3], msg.buf[4], msg.buf[5]);
		dev_dbg(&mstr->dev, "Dev number assigned is %x\n", dev_num);

		/*
		 * Set the Slave device name, its based on the dev_id and
		 * to bus which it is attached.
		 */
		dev_set_name(&sdw_slave->dev, "sdw-slave%d-%02x:%02x:%02x:%02x:%02x:%02x",
			sdw_master_get_id(mstr),
			sdw_slave->priv.dev_id[0],
			sdw_slave->priv.dev_id[1],
			sdw_slave->priv.dev_id[2],
			sdw_slave->priv.dev_id[3],
			sdw_slave->priv.dev_id[4],
			sdw_slave->priv.dev_id[5]);

		/*
		 * Set name based on dev_id. This will be used in match
		 * function to bind the device and driver.
		 */
		sprintf(sdw_slave->priv.name, "%02x:%02x:%02x:%02x:%02x:%02x",
				sdw_slave->priv.dev_id[0],
				sdw_slave->priv.dev_id[1],
				sdw_slave->priv.dev_id[2],
				sdw_slave->priv.dev_id[3],
				sdw_slave->priv.dev_id[4],
				sdw_slave->priv.dev_id[5]);
		ret = device_register(&sdw_slave->dev);
		if (ret) {
			dev_err(&mstr->dev, "Register slave failed ret = %d\n", ret);
			goto reg_slv_failed;
		}

		ret = sdw_program_dev_num(mstr, dev_num);
		if (ret < 0) {
			dev_err(&mstr->dev, "Programming slave address failed ret = %d\n", ret);
			goto program_slv_failed;
		}

		dev_dbg(&mstr->dev, "Slave registered with bus id %s\n",
			dev_name(&sdw_slave->dev));

		sdw_slave->dev_num = dev_num;

		/*
		 * Max number of Slaves that can be attached is 11. This
		 * check is performed in sdw_find_free_dev_num function.
		 */
		mstr->num_slv++;

		mutex_lock(&mstr->lock);
		list_add_tail(&sdw_slave->priv.node, &mstr->slv_list);
		mutex_unlock(&mstr->lock);

	}

	return ret;

program_slv_failed:
	device_unregister(&sdw_slave->dev);
reg_slv_failed:
	kfree(sdw_slave);
mem_alloc_failed:
	sdw_free_dev_num(mstr, dev_num);
dev_num_assign_fail:
	return ret;

}

/**
 * sdw_transfer: Local function where logic is placed to handle NOPM and PM
 *	variants of the Slave transfer functions.
 *
 * @mstr: Handle to SDW Master
 * @msg: One or more messages to be transferred
 * @num: Number of messages to be transferred.
 *
 * Returns negative error, else the number of messages transferred.
 *
 */
static int sdw_transfer(struct sdw_master *mstr, struct sdw_msg *msg, int num,
				struct sdw_deferred_xfer_data *data)
{
	unsigned long orig_jiffies;
	int ret, try, i;
	int program_scp_addr_page = false;
	u8 prev_adr_pg1 = 0;
	u8 prev_adr_pg2 = 0;

	/*
	 * sdw_trace_msg gets enabled when trace point sdw_slave_transfer gets
	 * enabled.  This is an efficient way of keeping the for-loop from
	 * being executed when not needed.
	 */
	if (static_key_false(&sdw_trace_msg)) {
		int j;

		for (j = 0; j < num; j++)
			if (msg[j].r_w_flag & SDW_MSG_FLAG_READ)
				trace_sdw_read(mstr, &msg[j], j);
			else
				trace_sdw_write(mstr, &msg[j], j);
	}

	for (i = 0; i < num; i++) {

		/* Reset timeout for every message */
		orig_jiffies = jiffies;

		/* Inform Master driver to program SCP addr or not */
		if ((prev_adr_pg1 != msg[i].addr_page1) ||
			(prev_adr_pg2 != msg[i].addr_page2))
			program_scp_addr_page = true;

		for (ret = 0, try = 0; try <= mstr->retries; try++) {

			/* Call deferred or sync handler based on call */
			if (!data)
				ret = mstr->driver->ops->xfer_msg(mstr,
					&msg[i], program_scp_addr_page);

			else if (mstr->driver->ops->xfer_msg_deferred)
				mstr->driver->ops->xfer_msg_deferred(
						mstr, &msg[i],
						program_scp_addr_page,
						data);
			else
				return -ENOTSUPP;
			if (ret != -EAGAIN)
				break;

			if (time_after(jiffies, orig_jiffies + mstr->timeout))
				break;
		}


		/*
		 * Set previous address page as current once message is
		 * transferred.
		 */
		prev_adr_pg1 = msg[i].addr_page1;
		prev_adr_pg2 = msg[i].addr_page2;
	}

	orig_jiffies = jiffies;

	ret = 0;

	/* Reset page address if its other than 0 */
	if (msg[i].addr_page1 && msg[i].addr_page2) {
		for (try = 0; try <= mstr->retries; try++) {
			/*
			 * Reset the page address to 0, so that always there
			 * is fast path access to MIPI defined Slave
			 * registers.
			 */

			ret = mstr->driver->ops->reset_page_addr(
					mstr, msg[0].dev_num);

			if (ret != -EAGAIN)
				break;

			if (time_after(jiffies, orig_jiffies + mstr->timeout))
				break;
		}
	}

	if (static_key_false(&sdw_trace_msg)) {
		int j;

		for (j = 0; j < msg->len; j++)
			if (msg[j].r_w_flag & SDW_MSG_FLAG_READ)
				trace_sdw_reply(mstr, &msg[j], j);
		trace_sdw_result(mstr, j, ret);
	}

	if (!ret)
		return i + 1;

	return ret;
}

/*
 * NO PM version of Slave transfer. Called from power management APIs
 * to avoid dead locks. This is called by bus driver only.
 */
static int sdw_slv_transfer_nopm(struct sdw_master *mstr,
			struct sdw_msg *msg, int num)
{
	int ret;

	/*
	 * If calling from atomic context, return immediately if previous
	 * message has not completed executing
	 */
	if (in_atomic() || irqs_disabled()) {
		ret = mutex_trylock(&mstr->msg_lock);
		if (!ret) {
			/* SDW activity is ongoing. */
			ret = -EAGAIN;
			goto out;
		}
	} else {
		mutex_lock(&mstr->msg_lock);
	}

	ret = sdw_transfer(mstr, msg, num, NULL);

	mutex_unlock(&mstr->lock);
out:
	return ret;
}

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
 *
 */
void sdw_bank_switch_deferred(struct sdw_master *mstr, struct sdw_msg *msg,
				struct sdw_deferred_xfer_data *data)
{

	pm_runtime_get_sync(&mstr->dev);

	sdw_transfer(mstr, msg, 1, data);

	pm_runtime_mark_last_busy(&mstr->dev);
	pm_runtime_put_sync_autosuspend(&mstr->dev);

}

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
							unsigned int num)
{
	int ret;

	/*
	 * Master reports the successfully transmitted messages onto the
	 * bus. If there are N message to be transmitted onto bus, and if
	 * Master gets error at (N-2) message it will report number of
	 * message transferred as N-2 Error is reported if ACK is not
	 * received for all messages or NACK is received for any of the
	 * transmitted messages. Currently both ACK not getting received
	 * and NACK is treated as error. But for upper level like regmap,
	 * both (Absence of ACK or NACK) errors are same as failure.
	 */

	/*
	 * Make sure Master is woken up before message transfer Ideally
	 * function calling this should have wokenup Master as this will be
	 * called by Slave driver, and it will do runtime_get for itself,
	 * which will make sure Master is woken up as Master is parent Linux
	 * device of Slave. But if Slave is not implementing RTPM, it may
	 * not do this, so bus driver has to do it always irrespective of
	 * what Slave does.
	 */
	pm_runtime_get_sync(&master->dev);

	if (in_atomic() || irqs_disabled()) {
		ret = mutex_trylock(&master->msg_lock);
		if (!ret) {
			ret = -EAGAIN;
			goto out;
		}
	} else {
		mutex_lock(&master->msg_lock);
	}

	ret = sdw_transfer(master, msg, num, NULL);

	mutex_unlock(&master->msg_lock);
out:
	/* Put Master to sleep once message is transferred */
	pm_runtime_mark_last_busy(&master->dev);
	pm_runtime_put_sync_autosuspend(&master->dev);

	return ret;
}
EXPORT_SYMBOL_GPL(snd_sdw_slave_transfer);

static int sdw_handle_dp0_interrupts(struct sdw_master *mstr,
			struct sdw_slave *sdw_slv, unsigned int *status)
{
	int ret;
	struct sdw_msg rd_msg, wr_msg;
	int impl_def_mask = 0;
	u8 rbuf, wbuf;
	struct sdw_slave_dp0_caps *dp0_cap;
	struct sdw_slave_priv *slv_priv = &sdw_slv->priv;

	dp0_cap = slv_priv->caps.dp0_caps;

	/* Read the DP0 interrupt status register and parse the bits */
	ret = sdw_rd_msg(&rd_msg, 0x0, SDW_DP0_INTSTAT, 1, &rbuf,
						sdw_slv->dev_num, mstr,
						SDW_NUM_OF_MSG1_XFRD);
	if (ret != SDW_NUM_OF_MSG1_XFRD) {
		ret = -EINVAL;
		dev_err(&mstr->dev, "Intr status read failed for slave %x\n",
				sdw_slv->dev_num);
		goto out;
	}

	if (rd_msg.buf[0] & SDW_DP0_INTSTAT_TEST_FAIL_MASK) {
		dev_err(&mstr->dev, "Test fail for slave %d port 0\n",
				sdw_slv->dev_num);
		wr_msg.buf[0] |= SDW_DP0_INTCLEAR_TEST_FAIL_MASK;
	}

	if ((dp0_cap->prepare_ch == SDW_CP_MODE_NORMAL) &&
		(rd_msg.buf[0] & SDW_DP0_INTSTAT_PORT_READY_MASK)) {
		complete(&slv_priv->port_ready[0]);
		wr_msg.buf[0] |= SDW_DP0_INTCLEAR_PORT_READY_MASK;
	}

	if (rd_msg.buf[0] & SDW_DP0_INTMASK_BRA_FAILURE_MASK) {
		/* TODO: Handle BRA failure */
		dev_err(&mstr->dev, "BRA failed for slave %d\n",
				sdw_slv->dev_num);
		wr_msg.buf[0] |= SDW_DP0_INTCLEAR_BRA_FAILURE_MASK;
	}

	impl_def_mask = SDW_DP0_INTSTAT_IMPDEF1_MASK |
			SDW_DP0_INTSTAT_IMPDEF2_MASK |
			SDW_DP0_INTSTAT_IMPDEF3_MASK;
	if (rd_msg.buf[0] & impl_def_mask) {
		wr_msg.buf[0] |= impl_def_mask;
		*status = wr_msg.buf[0];
	}

	/* Ack DP0 interrupts */
	ret = sdw_wr_msg(&wr_msg, 0x0, SDW_DP0_INTCLEAR, 1, &wbuf,
						sdw_slv->dev_num, mstr,
						SDW_NUM_OF_MSG1_XFRD);
	if (ret != SDW_NUM_OF_MSG1_XFRD) {
		ret = -EINVAL;
		dev_err(&mstr->dev, "Ack DP0 interrupts failed\n");
		goto out;
	}

out:
	return ret;

}

static int sdw_handle_port_interrupts(struct sdw_master *mstr,
		struct sdw_slave *sdw_slv, int port_num,
		unsigned int *status)
{
	int ret;
	struct sdw_msg rd_msg, wr_msg;
	u8 rbuf, wbuf;
	int impl_def_mask = 0;
	u16 intr_clr_addr, intr_stat_addr;
	struct sdw_slave_priv *slv_priv = &sdw_slv->priv;

	/*
	 * Handle the Data port0 interrupt separately since the interrupt
	 * mask and stat register is different than other DPn registers
	 */
	if (port_num == 0 && slv_priv->caps.dp0_present)
		return sdw_handle_dp0_interrupts(mstr, sdw_slv, status);

	intr_stat_addr = SDW_DPN_INTSTAT + (SDW_NUM_DATA_PORT_REGISTERS *
							port_num);

	/* Read the interrupt status register of port and parse bits */
	ret = sdw_rd_msg(&rd_msg, 0x0, intr_stat_addr, 1, &rbuf,
						sdw_slv->dev_num, mstr,
						SDW_NUM_OF_MSG1_XFRD);
	if (ret != SDW_NUM_OF_MSG1_XFRD) {
		ret = -EINVAL;
		dev_err(&mstr->dev, "Port Status read failed for slv %x port %x\n",
			sdw_slv->dev_num, port_num);
		goto out;
	}

	if (rd_msg.buf[0] & SDW_DPN_INTSTAT_TEST_FAIL_MASK) {
		dev_err(&mstr->dev, "Test fail for slave %x port %x\n",
					sdw_slv->dev_num, port_num);
		wr_msg.buf[0] |= SDW_DPN_INTCLEAR_TEST_FAIL_MASK;
	}

	/*
	 * Port Ready interrupt is only for Normal Channel prepare state
	 * machine
	 */
	if ((rd_msg.buf[0] & SDW_DPN_INTSTAT_PORT_READY_MASK)) {
		complete(&slv_priv->port_ready[port_num]);
		wr_msg.buf[0] |= SDW_DPN_INTCLEAR_PORT_READY_MASK;
	}

	impl_def_mask = SDW_DPN_INTSTAT_IMPDEF1_MASK |
			SDW_DPN_INTSTAT_IMPDEF2_MASK |
			SDW_DPN_INTSTAT_IMPDEF3_MASK;
	if (rd_msg.buf[0] & impl_def_mask) {
		wr_msg.buf[0] |= impl_def_mask;
		*status = wr_msg.buf[0];
	}

	intr_clr_addr = SDW_DPN_INTCLEAR +
			(SDW_NUM_DATA_PORT_REGISTERS * port_num);

	/* Clear and Ack the Port interrupt */
	ret =	sdw_wr_msg(&wr_msg, 0x0, intr_clr_addr, 1, &wbuf,
						sdw_slv->dev_num, mstr,
						SDW_NUM_OF_MSG1_XFRD);
	if (ret != SDW_NUM_OF_MSG1_XFRD) {
		ret = -EINVAL;
		dev_err(&mstr->dev, "Clear and ACK port interrupt failed for slv %x port %x\n",
						sdw_slv->dev_num, port_num);
		goto out;
	}

out:
	return ret;

}

/*
 * Get the Slave status
 */
static int sdw_get_slv_intr_stat(struct sdw_master *mstr, struct sdw_slave *slv,
							u8 *intr_stat_buf)
{
	struct sdw_msg rd_msg[3];
	int ret;
	int num_rd_messages = 1;
	struct sdw_slave_priv *slv_priv = &slv->priv;

	sdw_create_rd_msg(&rd_msg[0], 0x0, SDW_SCP_INTSTAT1, 1,
				&intr_stat_buf[0], slv->dev_num);

	/*
	 * Create read message for reading the Instat2 registers if Slave
	 * supports more than 4 ports
	 */
	if (slv_priv->caps.num_ports > SDW_CASC_PORT_START_INTSTAT2) {
		sdw_create_rd_msg(&rd_msg[1], 0x0, SDW_SCP_INTSTAT2, 1,
				&intr_stat_buf[1], slv->dev_num);
		num_rd_messages = 2;

	}

	if (slv_priv->caps.num_ports > SDW_CASC_PORT_START_INTSTAT3) {
		sdw_create_rd_msg(&rd_msg[2], 0x0, SDW_SCP_INTSTAT3, 1,
				&intr_stat_buf[2], slv->dev_num);
		num_rd_messages = 3;
	}

	/* Read Instat1, 2 and 3 registers */
	ret = snd_sdw_slave_transfer(mstr, rd_msg, num_rd_messages);
	if (ret != num_rd_messages) {
		ret = -EINVAL;
		dev_err(&mstr->dev, "Intr Status read failed for slv %x\n", slv->dev_num);
	}

	return ret;

}

static int sdw_ack_slv_intr(struct sdw_master *mstr, u8 dev_num,
						u8 *intr_clr_buf)
{
	struct sdw_msg wr_msg;
	int ret;

	/* Ack the interrupts */
	ret = sdw_wr_msg(&wr_msg, 0x0, SDW_SCP_INTCLEAR1, 1,
					intr_clr_buf, dev_num, mstr,
					SDW_NUM_OF_MSG1_XFRD);
	if (ret != SDW_NUM_OF_MSG1_XFRD) {
		ret = -EINVAL;
		dev_err(&mstr->dev, "Intr clear write failed for slv\n");
	}

	return ret;

}

static int sdw_handle_casc_port_intr(struct sdw_master *mstr, struct sdw_slave
					*sdw_slv, u8 cs_port_start,
					unsigned int *port_status,
					u8 *intr_stat_buf)
{
	int i, ret;
	int cs_port_mask, cs_port_reg_offset, num_cs_ports;

	switch (cs_port_start) {

	case SDW_CASC_PORT_START_INTSTAT1:
		/* Number of port status bits in this register */
		num_cs_ports = SDW_NUM_CASC_PORT_INTSTAT1;
		/* Bit mask for the starting port intr status */
		cs_port_mask = SDW_CASC_PORT_MASK_INTSTAT1;
		/* Register offset to read Cascaded instat 1 */
		cs_port_reg_offset = SDW_CASC_PORT_REG_OFFSET_INTSTAT1;
		break;

	case SDW_CASC_PORT_START_INTSTAT2:
		num_cs_ports = SDW_NUM_CASC_PORT_INTSTAT2;
		cs_port_mask = SDW_CASC_PORT_MASK_INTSTAT2;
		cs_port_reg_offset = SDW_CASC_PORT_REG_OFFSET_INTSTAT2;
		break;

	case SDW_CASC_PORT_START_INTSTAT3:
		num_cs_ports = SDW_NUM_CASC_PORT_INTSTAT3;
		cs_port_mask = SDW_CASC_PORT_MASK_INTSTAT3;
		cs_port_reg_offset = SDW_CASC_PORT_REG_OFFSET_INTSTAT3;
		break;

	default:
		return -EINVAL;

	}

	/*
	 * Look for cascaded port interrupts, if found handle port
	 * interrupts. Do this for all the Int_stat registers.
	 */
	for (i = cs_port_start; i < cs_port_start + num_cs_ports; i++) {
		if (intr_stat_buf[cs_port_reg_offset] & cs_port_mask) {
			ret = sdw_handle_port_interrupts(mstr, sdw_slv,
						cs_port_start + i,
						&port_status[i]);
			if (ret < 0) {
				dev_err(&mstr->dev, "Handling port intr failed ret = %d\n", ret);
				return ret;
			}
		}
			cs_port_mask = cs_port_mask << i;
	}
	return 0;
}

static int sdw_handle_impl_def_intr(struct sdw_slave *sdw_slv,
		struct sdw_impl_def_intr_stat *intr_status,
		unsigned int *port_status,
		u8 *control_port_stat)
{
	int ret, i;
	struct sdw_slave_priv *slv_priv = &sdw_slv->priv;

	/* Update the implementation defined status to Slave */
	for (i = 1; i < slv_priv->caps.num_ports; i++) {

		intr_status->portn_stat[i].status = port_status[i];
		intr_status->portn_stat[i].num = i;
	}

	intr_status->port0_stat = port_status[0];
	intr_status->control_port_stat = control_port_stat[0];

	ret = slv_priv->driver->slave_irq(sdw_slv, intr_status);
	if (ret < 0) {
		dev_err(&sdw_slv->mstr->dev, "Impl defined interrupt handling failed ret = %d\n", ret);
		return ret;
	}
	return 0;
}

/*
 * sdw_handle_slv_alerts: This function handles the Slave alert. Following
 *	things are done as part of handling Slave alert. Attempt is done to
 *	complete the interrupt handling in as less read/writes as possible
 *	based on number of ports defined by Slave.
 *
 *	1. Get the interrupt status of the Slave (sdw_get_slv_intr_stat) 1a.
 *	Read Instat1, Instat2 and Intstat3 registers based on on number of
 *	ports defined by the Slave.
 *
 *	2. Parse Interrupt Status registers for the SCP interrupts and take
 *	action.
 *
 *	3. Parse the interrupt status registers for the Port interrupts and
 *	take action.
 *
 *	4. Ack port interrupts.
 *	5. Call the Slave implementation defined interrupt, if Slave has
 *	registered for it.
 *
 *	6. Ack the Slave interrupt.
 *	7. Get interrupt status of the Slave again, to make sure no new
 *	interrupt came when we were servicing the interrupts.
 *
 *	8. Goto step 2 if any interrupt pending.
 *
 *	9. Return if no new interrupt pending.
 *	TODO: Poorly-designed or faulty Slaves may continuously generate
 *	interrupts and delay handling of interrupts signaled by other
 *	Slaves. A better QoS could rely on a priority scheme, where Slaves
 *	with the lowest DeviceNumber are handled first. Currently the
 *	priority is based on the enumeration sequence and arbitration;
 *	additional information would be needed from firmware/BIOS or module
 *	parameters to rank Slaves by relative interrupt processing priority.
 */
static int sdw_handle_slv_alerts(struct sdw_master *mstr,
				struct sdw_slave *sdw_slv)
{
	int slave_stat, count = 0, ret;
	int max_tries = SDW_INTR_STAT_READ_MAX_TRIES;
	unsigned int port_status[SDW_MAX_DATA_PORTS] = {0};
	struct sdw_impl_def_intr_stat intr_status;
	struct sdw_portn_intr_stat portn_stat;
	struct sdw_slave_priv *slv_priv = &sdw_slv->priv;
	u8 intr_clr_buf[SDW_NUM_INT_CLEAR_REGISTERS];
	u8 intr_stat_buf[SDW_NUM_INT_STAT_REGISTERS] = {0};
	u8 cs_port_start;

	mstr->sdw_addr[sdw_slv->dev_num].status = SDW_SLAVE_STAT_ALERT;
	/*
	 * Keep on servicing interrupts till Slave interrupts are ACKed and
	 * device returns to attached state instead of ALERT state
	 */
	ret = sdw_get_slv_intr_stat(mstr, sdw_slv, intr_stat_buf);
	if (ret < 0)
		return ret;

	do {

		if (intr_stat_buf[0] & SDW_SCP_INTSTAT1_PARITY_MASK) {
			dev_err(&mstr->dev, "Parity error detected\n");
			intr_clr_buf[0] |= SDW_SCP_INTCLEAR1_PARITY_MASK;
		}

		if (intr_stat_buf[0] & SDW_SCP_INTSTAT1_BUS_CLASH_MASK) {
			dev_err(&mstr->dev, "Bus clash error detected\n");
			intr_clr_buf[0] |= SDW_SCP_INTCLEAR1_BUS_CLASH_MASK;
		}

		/* Handle implementation defined mask */
		if (intr_stat_buf[0] & SDW_SCP_INTSTAT1_IMPL_DEF_MASK)
			intr_clr_buf[0] |= SDW_SCP_INTCLEAR1_IMPL_DEF_MASK;

		cs_port_start = SDW_NUM_CASC_PORT_INTSTAT1;

		/* Handle Cascaded Port interrupts from Instat_1 registers */
		ret = sdw_handle_casc_port_intr(mstr, sdw_slv, cs_port_start,
					port_status, intr_stat_buf);
		if (ret < 0)
			return ret;

		/*
		 * If there are more than 4 ports and cascaded interrupt is
		 * set, handle those interrupts
		 */
		if (intr_stat_buf[0] & SDW_SCP_INTSTAT1_SCP2_CASCADE_MASK) {
			cs_port_start = SDW_NUM_CASC_PORT_INTSTAT2;
			ret = sdw_handle_casc_port_intr(mstr, sdw_slv,
					cs_port_start, port_status,
					intr_stat_buf);
		}

		/*
		 * Handle cascaded interrupts from instat_2 register, if no
		 * cascaded interrupt from SCP2 cascade move to impl_def
		 * intrs
		 */
		if (intr_stat_buf[1] & SDW_SCP_INTSTAT2_SCP3_CASCADE_MASK) {
			cs_port_start = SDW_NUM_CASC_PORT_INTSTAT3;
			ret = sdw_handle_casc_port_intr(mstr, sdw_slv,
					cs_port_start, port_status,
					intr_stat_buf);
		}

		/*
		 * Handle implementation defined interrupts if Slave has
		 * registered for it.
		 */
		intr_status.portn_stat = &portn_stat;
		if (slv_priv->driver->slave_irq) {

			ret = sdw_handle_impl_def_intr(sdw_slv, &intr_status,
					port_status, intr_clr_buf);
			if (ret < 0)
				return ret;
		}

		/* Ack the Slave interrupt */
		ret = sdw_ack_slv_intr(mstr, sdw_slv->dev_num, intr_clr_buf);
		if (ret < 0) {
			dev_err(&mstr->dev, "Slave interrupt ack failed ret = %d\n", ret);
			return ret;
		}

		/*
		 * Read status once again before exiting loop to make sure
		 * no new interrupts came while we were servicing the
		 * interrupts
		 */
		ret = sdw_get_slv_intr_stat(mstr, sdw_slv, intr_stat_buf);
		if (ret < 0)
			return ret;

		/* Make sure no interrupts are pending */
		slave_stat = intr_stat_buf[0] || intr_stat_buf[1] ||
					intr_stat_buf[2];
		/*
		 * Exit loop if Slave is continuously in ALERT state even
		 * after servicing the interrupt multiple times.
		 */
		count++;

	} while (slave_stat != 0 && count < max_tries);

	return 0;
}

/*
 * Enable the Slave Control Port (SCP) interrupts and DP0 interrupts if
 * Slave supports DP0. Enable implementation defined interrupts based on
 * Slave interrupt mask.
 * This function enables below interrupts.
 * 1. Bus clash interrupt for SCP
 * 2. Parity interrupt for SCP.
 * 3. Enable implementation defined interrupt if slave requires.
 * 4. Port ready interrupt for the DP0 if required based on Slave support
 * for DP0 and normal channel prepare supported by DP0 port. For simplified
 * channel prepare Port ready interrupt is not required to be enabled.
 */
static int sdw_enable_scp_intr(struct sdw_slave *sdw_slv, int mask)
{
	struct sdw_msg rd_msg, wr_msg;
	u8 buf;
	int ret;
	struct sdw_master *mstr = sdw_slv->mstr;
	struct sdw_slave_priv *slv_priv = &sdw_slv->priv;
	u32 reg_addr = SDW_SCP_INTMASK1;


	ret = sdw_rd_msg(&rd_msg, 0, reg_addr, 1, &buf,
					sdw_slv->dev_num, mstr,
					SDW_NUM_OF_MSG1_XFRD);
	if (ret != SDW_NUM_OF_MSG1_XFRD) {
		dev_err(&mstr->dev, "SCP Intr mask read failed for slave %x\n",
				sdw_slv->dev_num);
		return -EINVAL;
	}

	buf |= mask;

	buf |= SDW_SCP_INTMASK1_BUS_CLASH_MASK;
	buf |= SDW_SCP_INTMASK1_PARITY_MASK;

	ret =	sdw_wr_msg(&wr_msg, 0, reg_addr, 1, &buf,
					sdw_slv->dev_num, mstr,
					SDW_NUM_OF_MSG1_XFRD);
	if (ret != SDW_NUM_OF_MSG1_XFRD) {
		dev_err(&mstr->dev, "SCP Intr mask write failed for slave %x\n",
				sdw_slv->dev_num);
		return -EINVAL;
	}

	if (!slv_priv->caps.dp0_present)
		return 0;

	reg_addr = SDW_DP0_INTMASK;
	mask = slv_priv->caps.dp0_caps->imp_def_intr_mask;
	buf = 0;

	ret = sdw_rd_msg(&rd_msg, 0, reg_addr, 1, &buf,
				sdw_slv->dev_num, mstr,
				SDW_NUM_OF_MSG1_XFRD);
	if (ret != SDW_NUM_OF_MSG1_XFRD) {
		dev_err(&mstr->dev, "DP0 Intr mask read failed for slave %x\n",
				sdw_slv->dev_num);
		return -EINVAL;
	}

	buf |= mask;

	if (slv_priv->caps.dp0_caps->prepare_ch == SDW_CP_MODE_NORMAL)
		buf |= SDW_DPN_INTMASK_PORT_READY_MASK;

	ret = sdw_wr_msg(&wr_msg, 0, reg_addr, 1, &buf,
					sdw_slv->dev_num,
					mstr,
					SDW_NUM_OF_MSG1_XFRD);
	if (ret != SDW_NUM_OF_MSG1_XFRD) {
		dev_err(&mstr->dev, "DP0 Intr mask write failed for slave %x\n",
				sdw_slv->dev_num);
		return -EINVAL;
	}

	return 0;
}

int sdw_enable_disable_dpn_intr(struct sdw_slave *sdw_slv, int port_num,
					int port_direction, bool enable)
{

	struct sdw_msg rd_msg, wr_msg;
	u8 buf;
	int ret;
	struct sdw_master *mstr = sdw_slv->mstr;
	struct sdw_slave_priv *slv_priv = &sdw_slv->priv;
	u32 reg_addr;
	struct sdw_dpn_caps *dpn_caps;
	u8 mask;

	reg_addr = SDW_DPN_INTMASK +
		(SDW_NUM_DATA_PORT_REGISTERS * port_num);

	dpn_caps = &slv_priv->caps.dpn_caps[port_direction][port_num];
	mask = dpn_caps->imp_def_intr_mask;

	/* Read DPn interrupt mask register */
	ret = sdw_rd_msg(&rd_msg, 0, reg_addr, 1, &buf,
				sdw_slv->dev_num, mstr,
				SDW_NUM_OF_MSG1_XFRD);
	if (ret != SDW_NUM_OF_MSG1_XFRD) {
		dev_err(&mstr->dev, "DPn Intr mask read failed for slave %x\n",
				sdw_slv->dev_num);
		return -EINVAL;
	}

	/* Enable the Slave defined interrupts. */
	buf |= mask;

	/*
	 * Enable port prepare interrupt only if port is not having
	 * simplified channel prepare state machine
	 */
	if (dpn_caps->prepare_ch == SDW_CP_MODE_NORMAL)
		buf |= SDW_DPN_INTMASK_PORT_READY_MASK;

	/* Enable DPn interrupt */
	ret = sdw_wr_msg(&wr_msg, 0, reg_addr, 1, &buf,
				sdw_slv->dev_num, mstr,
				SDW_NUM_OF_MSG1_XFRD);
	if (ret != SDW_NUM_OF_MSG1_XFRD) {
		dev_err(&mstr->dev, "DPn Intr mask write failed for slave %x\n",
				sdw_slv->dev_num);
		return -EINVAL;
	}
	return 0;
}

/**
 * snd_sdw_slave_set_intr_mask: Set the implementation defined interrupt
 *	mask. Slave sets the implementation defined interrupt mask as part
 *	of registering Slave capabilities. Slave driver can also modify
 *	implementation defined interrupt dynamically using below function.
 *
 * @slave: SoundWire Slave handle for which interrupt needs to be enabled.
 * @intr_mask: Implementation defined interrupt mask.
 */
int snd_sdw_slave_set_intr_mask(struct sdw_slave *slave,
	struct sdw_impl_def_intr_mask *intr_mask)
{
	int ret, i, j;
	struct sdw_slave_caps *caps = &slave->priv.caps;
	struct sdw_slave_dp0_caps *dp0_caps = caps->dp0_caps;
	u8 ports;

	caps->scp_impl_def_intr_mask = intr_mask->control_port_mask;

	if (caps->dp0_present)
		dp0_caps->imp_def_intr_mask = intr_mask->port0_mask;

	for (i = 0; i < SDW_MAX_PORT_DIRECTIONS; i++) {
		if (i == 0)
			ports = caps->num_src_ports;
		else
			ports = caps->num_sink_ports;
		for (j = 0; j < ports; j++) {
			caps->dpn_caps[i][j].imp_def_intr_mask =
				intr_mask->portn_mask[i][j].mask;
		}
	}

	ret = sdw_enable_scp_intr(slave, caps->scp_impl_def_intr_mask);
	if (ret < 0)
		return ret;

	return 0;

}
EXPORT_SYMBOL(snd_sdw_slave_set_intr_mask);

static int sdw_program_slv(struct sdw_slave *sdw_slv)
{
	struct sdw_slave_caps *cap;
	int ret;
	struct sdw_master *mstr = sdw_slv->mstr;
	struct sdw_slave_priv *slv_priv = &sdw_slv->priv;

	cap = &slv_priv->caps;

	/* Enable DP0 and SCP interrupts */
	ret = sdw_enable_scp_intr(sdw_slv, cap->scp_impl_def_intr_mask);
	if (ret < 0) {
		dev_err(&mstr->dev, "SCP program failed ret = %d\n", ret);
		return ret;
	}

	return ret;
}

static void sdw_update_slv_status_event(struct sdw_slave *slave,
				enum sdw_slave_status status)

{
	struct sdw_slave_priv *slv_priv = &slave->priv;
	struct sdw_slave_driver *slv_drv = slv_priv->driver;

	if (slv_drv->status_change_event)
		slv_drv->status_change_event(slave, status);
}

static int sdw_wait_for_clk_stp_deprep(struct sdw_slave *slave,
			unsigned int prep_timeout)
{
	int ret;
	struct sdw_msg msg;
	u8 buf = 0;
	int count = 0;
	struct sdw_master *mstr = slave->mstr;

	sdw_create_rd_msg(&msg, 0x0, SDW_SCP_STAT, 1, &buf, slave->dev_num);

	/*
	 * Read the ClockStopNotFinished bit from the SCP_Stat register of
	 * particular Slave to make sure that clock stop prepare is done
	 */
	do {
		ret = sdw_slv_transfer_nopm(mstr, &msg, SDW_NUM_OF_MSG1_XFRD);
		if (ret != SDW_NUM_OF_MSG1_XFRD) {
			WARN_ONCE(1, "Clock stop status read failed\n");
			break;
		}

		if (!(buf & SDW_SCP_STAT_CLK_STP_NF_MASK)) {
			ret = 0;
			break;
		}

		usleep_range(1000, 1200);
		count++;

	} while (count != prep_timeout);

	if (!(buf & SDW_SCP_STAT_CLK_STP_NF_MASK))

		dev_info(&mstr->dev, "Clock stop prepare done\n");
	else
		WARN_ONCE(1, "Clk stp deprepare failed for slave %d\n",
			slave->dev_num);

	return ret;
}

/*
 * This function does one of two things based on "prep" flag.
 * 1. Prepare Slave for clock stop, if "prep" flag is true.
 * 2. De-prepare Slave after clock resume, if "prep" flag is false.
 */
static void sdw_prepare_slv_for_clk_stp(struct sdw_master *mstr,
			struct sdw_slave *slave,
			enum sdw_clk_stop_mode clock_stop_mode,
			bool prep)
{
	bool wake_en;
	struct sdw_slave_caps *cap;
	u8 buf = 0;
	struct sdw_msg msg;
	int ret;

	cap = &slave->priv.caps;

	wake_en = !cap->wake_up_unavailable;

	if (prep) {
		/*
		 * Even if its simplified clock stop prepare, setting
		 * prepare bit wont harm Here we are not doing write modify
		 * write since we are updating all fields of SystemCtrl
		 * registers. Currently highphy is not supported, so
		 * setting that bit to always 0
		 */
		buf |= (1 << SDW_SCP_SYSTEMCTRL_CLK_STP_PREP_SHIFT);
		buf |= clock_stop_mode <<
			SDW_SCP_SYSTEMCTRL_CLK_STP_MODE_SHIFT;
		buf |= wake_en << SDW_SCP_SYSTEMCTRL_WAKE_UP_EN_SHIFT;
	} else
		buf = 0;

	/*
	 * We are calling NOPM version of the transfer API, because Master
	 * controllers calls this from the suspend handler, so if we call
	 * the normal transfer API, it tries to resume controller, which
	 * results in deadlock
	 */
	ret = sdw_wr_msg_nopm(&msg, 0x0, SDW_SCP_SYSTEMCTRL, 1, &buf,
					slave->dev_num, mstr,
					SDW_NUM_OF_MSG1_XFRD);

	/* We should continue even if it fails for some Slave */
	if (ret != SDW_NUM_OF_MSG1_XFRD)
		WARN_ONCE(1, "Clock Stop prepare failed for slave %d\n",
						slave->dev_num);
}

/*
 * This function checks if the Slave is in "prepared" or "de-prepared" state
 * This is used to de-prepare Slaves which are in "prepared" state after
 * resuming from ClockStop Mode 1
 */
static int sdw_check_for_prep_bit(struct sdw_slave *slave)
{
	u8 buf = 0;
	struct sdw_msg msg;
	int ret;
	struct sdw_master *mstr = slave->mstr;

	ret = sdw_rd_msg_nopm(&msg, 0x0, SDW_SCP_SYSTEMCTRL, 1, &buf,
						slave->dev_num, mstr,
						SDW_NUM_OF_MSG1_XFRD);
	if (ret != SDW_NUM_OF_MSG1_XFRD) {
		dev_err(&mstr->dev, "SCP_SystemCtrl read failed for Slave %d\n",
				slave->dev_num);
		return -EINVAL;
	}

	return !(buf & SDW_SCP_SYSTEMCTRL_CLK_STP_PREP_MASK);
}

/*
 * This function De-prepares particular Slave which is resuming from
 * ClockStop mode1. It does following things.
 * 1. Check if Slave requires de-prepare based on Slave capabilities.
 * 2. Check for the "Prepare" bit in SystemCtrl register.
 * 3. If prepare bit is set Deprepare the Slave.
 * 4. Wait till Slave is deprepared
 */
static int sdw_deprepare_slv_clk_stp1(struct sdw_slave *slave)
{
	struct sdw_slave_caps *cap;
	int ret;
	struct sdw_master *mstr = slave->mstr;
	struct sdw_slave_priv *slv_priv = &slave->priv;
	int prep_timeout = 0;

	cap = &slv_priv->caps;

	/*
	 * Slave might have enumerated 1st time or from clock stop mode 1
	 * return if Slave doesn't require deprepare
	 */
	if (!cap->clk_stp_prep_hard_reset_behavior)
		return 0;

	/*
	 * If Slave requires de-prepare after exiting from Clock Stop mode
	 * 1, then check for ClockStopPrepare bit in SystemCtrl register if
	 * its 1, de-prepare Slave from clock stop prepare, else return
	 */
	ret = sdw_check_for_prep_bit(slave);

	if (ret < 0)
		return ret;

	if (slv_priv->driver->pre_clk_stop_prep) {
		ret = slv_priv->driver->pre_clk_stop_prep(slave,
				cap->clk_stp1_mode, false);
		if (ret < 0) {
			dev_warn(&mstr->dev, "Pre de-prepare failed for Slave %d\n",
				slave->dev_num);
			return ret;
		}
	}

	prep_timeout = cap->clk_stp_prep_timeout;
	sdw_prepare_slv_for_clk_stp(slave->mstr, slave, cap->clk_stp1_mode,
				false);

	/* Make sure de-prepare is complete */
	ret = sdw_wait_for_clk_stp_deprep(slave, prep_timeout);

	if (ret < 0)
		return ret;

	if (slv_priv->driver->post_clk_stop_prep) {
		ret = slv_priv->driver->post_clk_stop_prep(slave,
				cap->clk_stp1_mode, false);

		if (ret < 0)
			dev_err(&mstr->dev, "Post de-prepare failed for Slave %d ret = %d\n",
						slave->dev_num, ret);
	}

	return ret;
}

/*
 * Following thing are done in below loop for each of the registered Slaves.
 * This handles only Slaves which were already registered before update
 * status.
 * 1. Mark Slave as not present, if status is unattached from from bus and
 * logical address assigned is true, update status to Slave driver.
 *
 * 2. Handle the Slave alerts, if the Status is Alert for any of the Slaves.
 * 3. Mark the Slave as present, if Status is Present and logical address is
 * assigned.
 *	3a. Update the Slave status to driver, driver will use to make sure
 *	its enumerated before doing read/writes.
 *
 *	3b. De-prepare if the Slave is exiting from clock stop mode 1 and
 *	capability is updated as "de-prepare" required after exiting clock
 *	stop mode 1.
 *
 *	3c. Program Slave registers for the implementation defined
 *	interrupts and wake enable based on Slave capabilities.
 */
static void sdw_process_slv_status(struct sdw_master *mstr,
			struct sdw_slv_status *status)
{
	int i, ret;

	for (i = 1; i <= SDW_MAX_DEVICES; i++) {

		if (mstr->sdw_addr[i].assigned != true)
			continue;
		/*
		 * If current state of device is same as previous
		 * state, nothing to be done for this device.
		 */
		else if (status->status[i] == mstr->sdw_addr[i].status)
			continue;

		/*
		 * If Slave got unattached, mark it as not present
		 * Slave can get unattached from attached state or
		 * Alert State
		 */
		if (status->status[i] == SDW_SLAVE_STAT_NOT_PRESENT) {

			mstr->sdw_addr[i].status =
				SDW_SLAVE_STAT_NOT_PRESENT;

		/*
		 * If Slave is in alert state, handle the Slave
		 * interrupts. Slave can get into alert state from
		 * attached state only.
		 */
		} else if (status->status[i] == SDW_SLAVE_STAT_ALERT) {

			ret = sdw_handle_slv_alerts(mstr,
					mstr->sdw_addr[i].slave);

		/*
		 * If Slave is re-attaching on the bus program all
		 * the interrupt and wake_en registers based on
		 * capabilities. De-prepare the Slave based on
		 * capability. Slave can move from Alert to
		 * Attached_Ok, but nothing needs to be done on that
		 * transition, it can also move from Not_present to
		 * Attached_ok, in this case only registers needs to
		 * be reprogrammed and deprepare needs to be done.
		 */
		} else if (status->status[i] ==
				SDW_SLAVE_STAT_ATTACHED_OK &&
				mstr->sdw_addr[i].status ==
				SDW_SLAVE_STAT_NOT_PRESENT) {

			ret = sdw_program_slv(mstr->sdw_addr[i].slave);

			if (ret < 0)
				continue;

			ret = sdw_deprepare_slv_clk_stp1(
					mstr->sdw_addr[i].slave);

			if (ret < 0)
				continue;


			mstr->sdw_addr[i].status =
				SDW_SLAVE_STAT_ATTACHED_OK;
		}

		/*
		 * Update the status to Slave, This is used by Slave
		 * during resume to make sure its enumerated before
		 * Slave register access
		 */
		sdw_update_slv_status_event(mstr->sdw_addr[i].slave,
					mstr->sdw_addr[i].status);
	}
}

/*
 * sdw_handle_slv_status: Worker thread to handle the Slave status.
 */
static void sdw_handle_slv_status(struct kthread_work *work)
{
	int ret = 0;
	struct sdw_slv_status *status, *__status__;
	struct sdw_bus *bus =
		container_of(work, struct sdw_bus, kwork);
	struct sdw_master *mstr = bus->mstr;
	unsigned long flags;

	/*
	 * Loop through each of the status nodes. Each node contains status
	 * for all Slaves. Master driver reports Slave status for all Slaves
	 * in interrupt context. Bus driver adds it to list and schedules
	 * this thread.
	 */
	list_for_each_entry_safe(status, __status__, &bus->status_list, node) {

		/*
		 * Handle newly attached Slaves, Register the Slaves with
		 * bus for all newly attached Slaves. Slaves may be
		 * attaching first time to bus or may have re-enumerated
		 * after hard or soft reset or clock stop exit 1.
		 */
		if (status->status[0] == SDW_SLAVE_STAT_ATTACHED_OK) {
			ret = sdw_slv_register(mstr);
			if (ret < 0)
				/*
				 * Even if adding new Slave fails, we will
				 * continue to add Slaves till we find all
				 * the enumerated Slaves.
				 */
				dev_err(&mstr->dev, "Register new slave failed ret = %d\n", ret);
		}

		sdw_process_slv_status(mstr, status);

		spin_lock_irqsave(&bus->spinlock, flags);
		list_del(&status->node);
		spin_unlock_irqrestore(&bus->spinlock, flags);
		kfree(status);
	}
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
					struct sdw_status *status)
{
	struct sdw_bus *bus = NULL;
	struct sdw_slv_status *slv_status;
	unsigned long flags;

	bus = master->bus;

	slv_status = kzalloc(sizeof(*slv_status), GFP_ATOMIC);
	if (!slv_status)
		return -ENOMEM;

	memcpy(slv_status->status, status, sizeof(*slv_status));

	/*
	 * Bus driver will take appropriate action for Slave status change
	 * in thread context. Master driver can call this from interrupt
	 * context as well. Memory for the Slave status will be freed in
	 * workqueue, once its handled.
	 */
	spin_lock_irqsave(&bus->spinlock, flags);
	list_add_tail(&slv_status->node, &bus->status_list);
	spin_unlock_irqrestore(&bus->spinlock, flags);

	kthread_queue_work(&bus->kworker, &bus->kwork);
	return 0;
}
EXPORT_SYMBOL_GPL(snd_sdw_master_update_slave_status);

/**
 * snd_sdw_master_register_driver: This API will register the Master driver
 *	with the SoundWire bus. It is typically called from the driver's
 *	module-init function.
 *
 * @driver: Master Driver to be associated with Master interface.
 * @owner: Module owner, generally THIS module.
 */
int snd_sdw_master_register_driver(struct sdw_master_driver *driver,
				struct module *owner)
{
	int ret;

	if (!driver->probe)
		return -EINVAL;

	if (!driver->ops->xfer_msg || !driver->ops->reset_page_addr)
		return -EINVAL;

	if (!driver->port_ops->dpn_set_port_params ||
		!driver->port_ops->dpn_set_port_transport_params ||
		!driver->port_ops->dpn_port_enable_ch)
		return -EINVAL;

	driver->driver.probe = sdw_mstr_probe;

	if (driver->remove)
		driver->driver.remove = sdw_mstr_remove;
	if (driver->shutdown)
		driver->driver.shutdown = sdw_mstr_shutdown;

	/* add the driver to the list of sdw drivers in the driver core */
	driver->driver.owner = owner;
	driver->driver.bus = &sdw_bus_type;

	/*
	 * When registration returns, the driver core will have called
	 * probe() for all matching-but-unbound Slaves, devices which are
	 * not bind to any driver still.
	 */
	ret = driver_register(&driver->driver);
	if (ret)
		return ret;

	pr_debug("sdw-core: driver [%s] registered\n", driver->driver.name);

	return 0;
}
EXPORT_SYMBOL_GPL(snd_sdw_master_register_driver);

/**
 * snd_sdw_slave_driver_register: SoundWire Slave driver registration with
 *	bus. This API will register the Slave driver with the SoundWire bus.
 *	It is typically called from the driver's module-init function.
 *
 * @driver: Driver to be associated with Slave.
 * @owner: Module owner, generally THIS module.
 */
int snd_sdw_slave_driver_register(struct sdw_slave_driver *driver,
				struct module *owner)
{
	int ret;

	if (driver->probe)
		driver->driver.probe = sdw_slv_probe;
	if (driver->remove)
		driver->driver.remove = sdw_slv_remove;
	if (driver->shutdown)
		driver->driver.shutdown = sdw_slv_shutdown;

	/* Add the driver to the list of sdw drivers in the driver core */
	driver->driver.owner = owner;
	driver->driver.bus = &sdw_bus_type;

	/*
	 * When registration returns, the driver core will have called
	 * probe() for all matching-but-unbound Slaves.
	 */
	ret = driver_register(&driver->driver);
	if (ret)
		return ret;

	pr_debug("sdw-core: driver [%s] registered\n", driver->driver.name);

	return 0;
}
EXPORT_SYMBOL_GPL(snd_sdw_slave_driver_register);

static int sdw_copy_aud_mod_prop(struct sdw_port_aud_mode_prop *slv_prop,
				struct sdw_port_aud_mode_prop *prop)
{
	/*
	 * Currently goto is used in API to perform different
	 * operations. TODO: Avoid usage of goto statement
	 */
	memcpy(slv_prop, prop, sizeof(*prop));

	if (!prop->num_bus_freq_cfgs)
		goto handle_sample_rate;

	slv_prop->clk_freq_buf = kcalloc(prop->num_bus_freq_cfgs,
					sizeof(unsigned int),
					GFP_KERNEL);

	if (!slv_prop->clk_freq_buf)
		goto mem_error;

	memcpy(slv_prop->clk_freq_buf, prop->clk_freq_buf,
				(prop->num_bus_freq_cfgs *
				sizeof(unsigned int)));

handle_sample_rate:

	if (!prop->num_sample_rate_cfgs)
		return 0;

	slv_prop->sample_rate_buf = kcalloc(prop->num_sample_rate_cfgs,
					sizeof(unsigned int),
					GFP_KERNEL);

	if (!slv_prop->sample_rate_buf)
		goto mem_error;

	memcpy(slv_prop->sample_rate_buf, prop->sample_rate_buf,
				(prop->num_sample_rate_cfgs *
				sizeof(unsigned int)));

	return 0;

mem_error:
	kfree(prop->clk_freq_buf);
	kfree(slv_prop->sample_rate_buf);
	return -ENOMEM;

}

static int sdw_update_dpn_caps(struct sdw_dpn_caps *slv_dpn_cap,
					struct sdw_dpn_caps *dpn_cap)
{
	int j, ret = 0;
	struct sdw_port_aud_mode_prop *slv_prop, *prop;

	/*
	 * Currently goto is used in API to perform different
	 * operations. TODO: Avoid usage of goto statement
	 */

	/*
	 * slv_prop and prop are using to make copy of mode properties.
	 * prop holds mode properties received which needs to be updated to
	 * slv_prop.
	 */

	memcpy(slv_dpn_cap, dpn_cap, sizeof(*dpn_cap));

	/*
	 * Copy bps (bits per sample) buffer as part of Slave capabilities
	 */
	if (!dpn_cap->num_bps)
		goto handle_ch_cnt;

	slv_dpn_cap->bps_buf = kcalloc(dpn_cap->num_bps, sizeof(u8),
							GFP_KERNEL);

	if (!slv_dpn_cap->bps_buf) {
		ret = -ENOMEM;
		goto error;
	}

	memcpy(slv_dpn_cap->bps_buf, dpn_cap->bps_buf,
			(dpn_cap->num_bps * sizeof(u8)));

handle_ch_cnt:
	if (!dpn_cap->num_ch_cnt)
		goto handle_audio_mode_prop;

	slv_dpn_cap->ch_cnt_buf = kcalloc(dpn_cap->num_ch_cnt, sizeof(u8),
							GFP_KERNEL);
	if (!dpn_cap->num_ch_cnt) {
		ret = -ENOMEM;
		goto error;
	}

	/* Copy channel count buffer as part of Slave capabilities */
	memcpy(slv_dpn_cap->ch_cnt_buf, dpn_cap->ch_cnt_buf,
			(dpn_cap->num_ch_cnt * sizeof(u8)));

handle_audio_mode_prop:

	slv_dpn_cap->mode_properties = kzalloc((sizeof(*slv_prop) *
				dpn_cap->num_audio_modes),
				GFP_KERNEL);

	if (!slv_dpn_cap->mode_properties) {
		ret = -ENOMEM;
		goto error;
	}

	for (j = 0; j < dpn_cap->num_audio_modes; j++) {

		prop = &dpn_cap->mode_properties[j];
		slv_prop = &slv_dpn_cap->mode_properties[j];

		/* Copy audio properties as part of Slave capabilities */
		ret = sdw_copy_aud_mod_prop(slv_prop, prop);
		if (ret < 0)
			goto error;
	}

	return ret;

error:
	kfree(slv_dpn_cap->mode_properties);
	kfree(slv_dpn_cap->ch_cnt_buf);
	kfree(slv_dpn_cap->bps_buf);
	return ret;

}

/* Free all the memory allocated for registering the capabilities */
static void sdw_unregister_slv_caps(struct sdw_slave *sdw,
		unsigned int num_port_direction)
{
	int i, j, k;
	struct sdw_slave_caps *caps = &sdw->priv.caps;
	struct sdw_dpn_caps *dpn_cap;
	struct sdw_port_aud_mode_prop *mode_prop;
	u8 ports;

	for (i = 0; i < num_port_direction; i++) {

		if (i == SDW_DATA_DIR_OUT)
			ports = caps->num_src_ports;
		else
			ports = caps->num_sink_ports;
		for (j = 0; j < ports; j++) {
			dpn_cap = &caps->dpn_caps[i][j];
			kfree(dpn_cap->bps_buf);
			kfree(dpn_cap->ch_cnt_buf);

			for (k = 0; k < dpn_cap->num_audio_modes; k++) {
				mode_prop = dpn_cap->mode_properties;
				kfree(mode_prop->clk_freq_buf);
				kfree(mode_prop->sample_rate_buf);
			}
		}
	}
}

static inline void sdw_copy_slv_caps(struct sdw_slave *sdw,
				struct sdw_slave_caps *caps)
{
	struct sdw_slave_caps *slv_caps;

	slv_caps = &sdw->priv.caps;

	memcpy(slv_caps, caps, sizeof(*slv_caps));
}

/**
 * snd_sdw_slave_register_caps: Register Slave device capabilities to the
 *	bus driver. Since bus driver handles bunch of Slave register
 *	programming it should be aware of Slave device capabilities. Slave
 *	device is attached to bus based on enumeration. Once Slave driver is
 *	attached to device and probe of Slave driver is called on device and
 *	driver binding, Slave driver should call this function to register
 *	its capabilities to bus. This should be the very first function to
 *	bus driver from Slave driver once Slave driver is registered and
 *	probed.
 *
 * @slave: SoundWire Slave handle.
 * @cap: Slave caps to be registered to bus driver.
 */
int snd_sdw_slave_register_caps(struct sdw_slave *slave,
					struct sdw_slave_caps *cap)
{
	struct sdw_slave_caps *caps;
	struct sdw_dpn_caps *slv_dpn_cap, *dpn_cap;
	int i, j, ret;
	u8 ports;

	caps = &slave->priv.caps;

	sdw_copy_slv_caps(slave, cap);

	for (i = 0; i < SDW_MAX_PORT_DIRECTIONS; i++) {
		if (i == SDW_DATA_DIR_OUT)
			ports = caps->num_src_ports;
		else
			ports = caps->num_sink_ports;

		caps->dpn_caps[i] = kzalloc((sizeof(*slv_dpn_cap) *
						ports), GFP_KERNEL);

		if (caps->dpn_caps[i] == NULL) {
			ret = -ENOMEM;
			goto error;
		}
	}

	for (i = 0; i < SDW_MAX_PORT_DIRECTIONS; i++) {

		if (i == SDW_DATA_DIR_OUT)
			ports = caps->num_src_ports;
		else
			ports = caps->num_sink_ports;

		for (j = 0; j < ports; j++) {

			dpn_cap = &cap->dpn_caps[i][j];
			slv_dpn_cap = &caps->dpn_caps[i][j];

			ret = sdw_update_dpn_caps(&caps->dpn_caps[i][j],
						&cap->dpn_caps[i][j]);
			if (ret < 0) {
				dev_err(&slave->mstr->dev, "Failed to update Slave caps ret = %d\n", ret);
				goto error;
			}
		}
	}

	slave->priv.slave_cap_updated = true;

	return 0;

error:
	sdw_unregister_slv_caps(slave, i);
	return ret;
}
EXPORT_SYMBOL_GPL(snd_sdw_slave_register_caps);

/**
 * snd_sdw_master_add: Registers the SoundWire Master interface. This needs
 *	to be called for each Master interface supported by SoC. This
 *	represents One clock and data line (Optionally multiple data lanes)
 *	of Master interface.
 *
 * @master: the Master to be added.
 */
int snd_sdw_master_add(struct sdw_master *master)
{
	int i, id, ret;
	struct sdw_bus *sdw_bus = NULL;

	/* Sanity checks */
	if (unlikely(master->name[0] == '\0')) {
		pr_err("sdw-core: Attempt to register a master with no name!\n");
		return -EINVAL;
	}

	mutex_lock(&snd_sdw_core.core_mutex);

	/* Always start bus with 0th Index */
	id = idr_alloc(&snd_sdw_core.idr, master, 0, 0, GFP_KERNEL);

	if (id < 0) {
		mutex_unlock(&snd_sdw_core.core_mutex);
		return id;
	}

	master->nr = id;

	/*
	 * Initialize the DeviceNumber in the Master structure. Each of
	 * these is assigned to the Slaves enumerating on this Master
	 * interface.
	 */
	for (i = 0; i <= SDW_MAX_DEVICES; i++)
		master->sdw_addr[i].dev_num = i;

	mutex_init(&master->lock);
	mutex_init(&master->msg_lock);
	INIT_LIST_HEAD(&master->slv_list);
	INIT_LIST_HEAD(&master->mstr_rt_list);

	sdw_bus = kzalloc(sizeof(*sdw_bus), GFP_KERNEL);
	if (!sdw_bus) {
		ret = -ENOMEM;
		goto alloc_failed;
	}

	sdw_bus->mstr = master;
	master->bus = sdw_bus;

	dev_set_name(&master->dev, "sdw-%d", master->nr);
	master->dev.bus = &sdw_bus_type;
	master->dev.type = &sdw_mstr_type;

	ret = device_register(&master->dev);
	if (ret < 0)
		goto dev_reg_failed;

	dev_dbg(&master->dev, "master [%s] registered\n", master->name);

	kthread_init_worker(&sdw_bus->kworker);
	sdw_bus->status_thread = kthread_run(kthread_worker_fn,
			&sdw_bus->kworker, "%s",
			dev_name(&master->dev));

	if (IS_ERR(sdw_bus->status_thread)) {
		dev_err(&master->dev, "error: failed to create status message task\n");
		ret = PTR_ERR(sdw_bus->status_thread);
		goto thread_create_failed;
	}

	kthread_init_work(&sdw_bus->kwork, sdw_handle_slv_status);
	INIT_LIST_HEAD(&sdw_bus->status_list);
	spin_lock_init(&sdw_bus->spinlock);

	/* Initialize bandwidth calculation data structures */
	sdw_init_bus_params(sdw_bus);

	/*
	 * Add bus to the list of buses inside core. This is list of Slave
	 * devices enumerated on this bus. Adding new devices at end. It can
	 * be added at any location in list.
	 */
	list_add_tail(&sdw_bus->bus_node, &snd_sdw_core.bus_list);
	mutex_unlock(&snd_sdw_core.core_mutex);

	return 0;

thread_create_failed:
	device_unregister(&master->dev);
dev_reg_failed:
	kfree(sdw_bus);
alloc_failed:
	idr_remove(&snd_sdw_core.idr, master->nr);
	mutex_unlock(&snd_sdw_core.core_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(snd_sdw_master_add);

static void sdw_unregister_slv(struct sdw_slave *sdw_slv)
{
	struct sdw_master *mstr;

	mstr = sdw_slave_to_master(sdw_slv);

	sdw_unregister_slv_caps(sdw_slv, SDW_MAX_PORT_DIRECTIONS);

	mutex_lock(&mstr->lock);
	list_del(&sdw_slv->priv.node);
	mutex_unlock(&mstr->lock);

	mstr->sdw_addr[sdw_slv->dev_num].assigned = false;

	device_unregister(&sdw_slv->dev);
	kfree(sdw_slv);
}

static int __unregister_slv(struct device *dev, void *dummy)
{
	struct sdw_slave *slave = sdw_slv_verify(dev);

	if (slave)
		sdw_unregister_slv(slave);

	return 0;
}

/**
 * snd_sdw_master_del - unregister SDW Master
 *
 * @master: the Master being unregistered
 */
void snd_sdw_master_del(struct sdw_master *master)
{
	struct sdw_master *found;

	/* First make sure that this Master was ever added */
	mutex_lock(&snd_sdw_core.core_mutex);
	found = idr_find(&snd_sdw_core.idr, master->nr);

	if (found != master) {
		pr_debug("sdw-core: attempting to delete unregistered master [%s]\n",
				master->name);
		mutex_unlock(&snd_sdw_core.core_mutex);
		return;
	}
	/*
	 * Detach any active Slaves. This can't fail, thus we do not check
	 * the returned value.
	 */
	device_for_each_child(&master->dev, NULL, __unregister_slv);

	/* device name is gone after device_unregister */
	dev_dbg(&master->dev, "master [%s] unregistered\n", master->name);

	/* wait until all references to the device are gone */
	init_completion(&master->slv_released_complete);
	device_unregister(&master->dev);
	wait_for_completion(&master->slv_released_complete);

	/* free bus id */
	idr_remove(&snd_sdw_core.idr, master->nr);
	mutex_unlock(&snd_sdw_core.core_mutex);

	/*
	 * Clear the device structure in case this Master is ever going to
	 * be added again
	 */
	memset(&master->dev, 0, sizeof(master->dev));
}
EXPORT_SYMBOL_GPL(snd_sdw_master_del);

static enum sdw_clk_stop_mode sdw_slv_get_clk_stp_mode(struct sdw_slave *slave)
{
	enum sdw_clk_stop_mode clock_stop_mode;
	struct sdw_slave_priv *slv_priv = &slave->priv;
	struct sdw_slave_caps *cap = &slv_priv->caps;

	/*
	 * Get the dynamic value of clock stop from Slave driver if
	 * supported, else use the static value from capabilities register.
	 * Update the capabilities also if we have new dynamic value.
	 */
	if (slv_priv->driver->get_dyn_clk_stp_mod) {
		clock_stop_mode = slv_priv->driver->get_dyn_clk_stp_mod(slave);
		if (clock_stop_mode == SDW_CLOCK_STOP_MODE_1)
			cap->clk_stp1_mode = true;
		else
			cap->clk_stp1_mode = false;
	} else
		clock_stop_mode = cap->clk_stp1_mode;

	return clock_stop_mode;
}

/**
 * sdw_acquire_mstr_lock: This function acquires Master lock for the
 *	Master(s) used by the given stream. The advantage of using Master
 *	lock over core lock is Master lock will lock only those Master(s)
 *	associated with given stream giving the advantage of simultaneous
 *	configuration of stream(s) running on different Master(s). On the
 *	other hand, core lock will not allow multiple stream configuration
 *	simultaneously.
 *
 * @stream_tag: Stream tag on which operations needs to be performed.
 */
static void sdw_acquire_mstr_lock(struct sdw_stream_tag *stream_tag)
{
	struct sdw_runtime *sdw_rt = stream_tag->sdw_rt;
	struct sdw_mstr_runtime *sdw_mstr_rt = NULL;
	struct sdw_master *sdw_mstr = NULL;

	/* Acquire core lock */
	mutex_lock(&snd_sdw_core.core_mutex);

	/* Iterate for all Master(s) in Master list */
	list_for_each_entry(sdw_mstr_rt, &sdw_rt->mstr_rt_list,
			mstr_strm_node) {

		/* Get Master structure */
		sdw_mstr = sdw_mstr_rt->mstr;

		/* Acquire Master lock */
		mutex_lock(&sdw_mstr->lock);
	}

	/* Release core lock */
	mutex_unlock(&snd_sdw_core.core_mutex);

}

/**
 * sdw_release_mstr_lock: This function releases Master lock for the
 *	Master(s) used by the given stream acquired in sdw_acquire_mstr_lock
 *	API.
 *
 * @stream_tag: Stream tag on which operations needs to be performed.
 *
 */
static void sdw_release_mstr_lock(struct sdw_stream_tag *stream_tag)
{
	struct sdw_runtime *sdw_rt = stream_tag->sdw_rt;
	struct sdw_mstr_runtime *sdw_mstr_rt = NULL;
	struct sdw_master *sdw_mstr = NULL;

	/* Acquire core lock */
	mutex_lock(&snd_sdw_core.core_mutex);

	/* Iterate for all Master(s) in Master list */
	list_for_each_entry(sdw_mstr_rt, &sdw_rt->mstr_rt_list,
			mstr_strm_node) {

		/* Get Master structure */
		sdw_mstr = sdw_mstr_rt->mstr;

		/* Release Master lock */
		mutex_unlock(&sdw_mstr->lock);
	}

	/* Release core lock */
	mutex_unlock(&snd_sdw_core.core_mutex);

}

/**
 * snd_sdw_release_stream_tag: Free the already assigned stream tag.
 *	Reverses effect of "sdw_alloc_stream_tag"
 *
 * @stream_tag: Stream tag to be freed.
 */
void snd_sdw_release_stream_tag(unsigned int stream_tag)
{
	int i;
	struct sdw_stream_tag *stream_tags = snd_sdw_core.stream_tags;

	/* Acquire core lock */
	mutex_lock(&snd_sdw_core.core_mutex);

	/* Get stream tag data structure */
	for (i = 0; i < SDW_NUM_STREAM_TAGS; i++) {
		if (stream_tag == stream_tags[i].stream_tag) {

			/* Reference count update */
			sdw_dec_ref_count(&stream_tags[i].ref_count);

			if (stream_tags[i].ref_count == 0)
				/* Free up resources */
				kfree(stream_tags[i].sdw_rt);
		}
	}

	/* Release core lock */
	mutex_unlock(&snd_sdw_core.core_mutex);
}
EXPORT_SYMBOL_GPL(snd_sdw_release_stream_tag);

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
int snd_sdw_alloc_stream_tag(unsigned int *stream_tag)
{
	int i;
	int ret = -EINVAL;
	struct sdw_runtime *sdw_rt;
	struct sdw_stream_tag *stream_tags = snd_sdw_core.stream_tags;

	/* Acquire core lock */
	mutex_lock(&snd_sdw_core.core_mutex);

	/* Allocate new stream tag and initialize resources */
	for (i = 0; i < SDW_NUM_STREAM_TAGS; i++) {
		if (!stream_tags[i].ref_count) {

			*stream_tag = stream_tags[i].stream_tag;

			/* Initialize stream lock */
			mutex_init(&stream_tags[i].stream_lock);

			/* Allocate resources for stream runtime handle */
			sdw_rt = kzalloc(sizeof(*sdw_rt), GFP_KERNEL);
			if (!sdw_rt) {
				ret = -ENOMEM;
				goto out;
			}

			/* Reference count update */
			sdw_inc_ref_count(&stream_tags[i].ref_count);

			/* Initialize Master and Slave list */
			INIT_LIST_HEAD(&sdw_rt->slv_rt_list);
			INIT_LIST_HEAD(&sdw_rt->mstr_rt_list);

			/* Change stream state to ALLOC */
			sdw_rt->stream_state = SDW_STATE_STRM_ALLOC;

			stream_tags[i].sdw_rt = sdw_rt;

			ret = 0;
			break;
		}
	}
out:
	/* Release core lock */
	mutex_unlock(&snd_sdw_core.core_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(snd_sdw_alloc_stream_tag);

/**
 * sdw_config_mstr_stream: Checks if master runtime handle already
 *	available, if not allocates and initialize Master runtime handle.
 *
 * @mstr: Master handle
 * @stream_config: Stream configuration for the SoundWire audio stream.
 * @sdw_rt: Stream runtime handle.
 *
 * Returns Master runtime handle.
 */
static struct sdw_mstr_runtime *sdw_config_mstr_stream(struct sdw_master *mstr,
				struct sdw_stream_config *stream_config,
				struct sdw_runtime *sdw_rt)
{
	struct sdw_mstr_runtime *mstr_rt = NULL;
	struct sdw_stream_params *str_p;

	/* Retrieve Master handle if already available */
	list_for_each_entry(mstr_rt, &sdw_rt->mstr_rt_list, mstr_strm_node) {
		if (mstr_rt->mstr == mstr)
			return mstr_rt;
	}

	/* Allocate resources for Master runtime handle */
	mstr_rt = kzalloc(sizeof(*mstr_rt), GFP_KERNEL);
	if (!mstr_rt)
		goto out;

	/* Initialization of Master runtime handle */
	INIT_LIST_HEAD(&mstr_rt->port_rt_list);
	INIT_LIST_HEAD(&mstr_rt->slv_rt_list);
	list_add_tail(&mstr_rt->mstr_strm_node, &sdw_rt->mstr_rt_list);
	list_add_tail(&mstr_rt->mstr_node, &mstr->mstr_rt_list);

	/* Update PCM parameters for Master */
	mstr_rt->direction = stream_config->direction;
	str_p = &mstr_rt->stream_params;
	str_p->rate = stream_config->frame_rate;
	str_p->channel_count = stream_config->channel_count;
	str_p->bps = stream_config->bps;

	/* Add reference for Master device handle */
	mstr_rt->mstr = mstr;

	/* Add reference for stream runtime handle */
	mstr_rt->sdw_rt = sdw_rt;

out:
	return mstr_rt;
}

/**
 * sdw_config_slave_stream: Allocate and initialize slave runtime handle.
 *
 * @slave: Slave handle
 * @stream_config: Stream configuration for the SoundWire audio stream.
 * @sdw_rt: Stream runtime handle.
 *
 * Returns Slave runtime handle.
 */
static struct sdw_slv_runtime *sdw_config_slv_stream(
				struct sdw_slave *slave,
				struct sdw_stream_config *stream_config,
				struct sdw_runtime *sdw_rt)
{
	struct sdw_slv_runtime *slv_rt = NULL;
	struct sdw_stream_params *str_p;

	/* Allocate resources for Slave runtime handle */
	slv_rt = kzalloc(sizeof(*slv_rt), GFP_KERNEL);
	if (!slv_rt)
		goto out;

	/* Initialization of Slave runtime handle */
	INIT_LIST_HEAD(&slv_rt->port_rt_list);

	/* Update PCM parameters for Slave */
	slv_rt->direction = stream_config->direction;
	str_p = &slv_rt->stream_params;
	str_p->rate = stream_config->frame_rate;
	str_p->channel_count = stream_config->channel_count;
	str_p->bps = stream_config->bps;

	/* Add reference for Slave device handle */
	slv_rt->slv = slave;

	/* Add reference for stream runtime handle */
	slv_rt->sdw_rt = sdw_rt;

out:
	return slv_rt;
}

/**
 * sdw_release_mstr_stream: Removes entry from master runtime list and free
 *	up resources.
 *
 * @mstr: Master handle.
 * @sdw_rt: Master runtime handle.
 */
static void sdw_release_mstr_stream(struct sdw_master *mstr,
			struct sdw_runtime *sdw_rt)
{
	struct sdw_mstr_runtime *mstr_rt, *__mstr_rt;

	/* Retrieve Master runtime handle */
	list_for_each_entry_safe(mstr_rt, __mstr_rt, &sdw_rt->mstr_rt_list,
			mstr_strm_node) {

		if (mstr_rt->mstr == mstr) {

			if (mstr_rt->direction == SDW_DATA_DIR_OUT)
				/* Reference count update */
				sdw_dec_ref_count(&sdw_rt->tx_ref_count);
			else
				/* Reference count update */
				sdw_dec_ref_count(&sdw_rt->rx_ref_count);

			/* Remove node from the list */
			list_del(&mstr_rt->mstr_strm_node);
			list_del(&mstr_rt->mstr_node);

			pm_runtime_mark_last_busy(&mstr->dev);
			pm_runtime_put_sync_autosuspend(&mstr->dev);

			/* Free up Master runtime handle resources */
			kfree(mstr_rt);
		}
	}
}

/**
 * sdw_release_slv_stream: Removes entry from slave runtime list and free up
 *	resources.
 *
 * @slave: Slave handle.
 * @sdw_rt: Stream runtime handle.
 */
static void sdw_release_slv_stream(struct sdw_slave *slave,
			struct sdw_runtime *sdw_rt)
{
	struct sdw_slv_runtime *slv_rt, *__slv_rt;

	/* Retrieve Slave runtime handle */
	list_for_each_entry_safe(slv_rt, __slv_rt, &sdw_rt->slv_rt_list,
			slave_strm_node) {

		if (slv_rt->slv == slave) {

			if (slv_rt->direction == SDW_DATA_DIR_OUT)
				/* Reference count update */
				sdw_dec_ref_count(&sdw_rt->tx_ref_count);
			else
				/* Reference count update */
				sdw_dec_ref_count(&sdw_rt->rx_ref_count);

			/* Remove node from the list */
			list_del(&slv_rt->slave_strm_node);

			pm_runtime_mark_last_busy(&slave->dev);
			pm_runtime_put_sync_autosuspend(&slave->dev);

			/* Free up Slave runtime handle resources */
			kfree(slv_rt);
		}
	}
}

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
int snd_sdw_release_stream(struct sdw_master *mstr,
		struct sdw_slave *slave,
		unsigned int stream_tag)
{
	int i;
	struct sdw_runtime *sdw_rt = NULL;
	struct sdw_stream_tag *stream_tags = snd_sdw_core.stream_tags;

	/* Retrieve master handle if called by Slave */
	if (!mstr)
		mstr = slave->mstr;

	/* Retrieve stream runtime handle */
	for (i = 0; i < SDW_NUM_STREAM_TAGS; i++) {
		if (stream_tags[i].stream_tag == stream_tag) {
			sdw_rt = stream_tags[i].sdw_rt;
			break;
		}
	}

	if (!sdw_rt) {
		dev_err(&mstr->dev, "Invalid stream tag\n");
		return -EINVAL;
	}

	/* Call release API of Master/Slave */
	if (!slave)
		sdw_release_mstr_stream(mstr, sdw_rt);
	else
		sdw_release_slv_stream(slave, sdw_rt);

	return 0;
}
EXPORT_SYMBOL_GPL(snd_sdw_release_stream);

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
int snd_sdw_config_stream(struct sdw_master *mstr,
		struct sdw_slave *slave,
		struct sdw_stream_config *stream_config,
		unsigned int stream_tag)
{
	int i;
	int ret = 0;
	struct sdw_runtime *sdw_rt = NULL;
	struct sdw_mstr_runtime *mstr_rt = NULL;
	struct sdw_slv_runtime *slv_rt = NULL;
	struct sdw_stream_tag *stream_tags = snd_sdw_core.stream_tags;
	struct sdw_stream_tag *stream = NULL;

	/* Retrieve master handle if called by Slave */
	if (!mstr)
		mstr = slave->mstr;

	/* Retrieve stream runtime handle */
	for (i = 0; i < SDW_NUM_STREAM_TAGS; i++) {
		if (stream_tags[i].stream_tag == stream_tag) {
			sdw_rt = stream_tags[i].sdw_rt;
			stream = &stream_tags[i];
			break;
		}
	}

	if (!sdw_rt) {
		dev_err(&mstr->dev, "Valid stream tag not found\n");
		ret = -EINVAL;
		goto out;
	}

	/* Acquire stream lock */
	mutex_lock(&stream->stream_lock);

	/* Get and Initialize Master runtime handle */
	mstr_rt = sdw_config_mstr_stream(mstr, stream_config, sdw_rt);
	if (!mstr_rt) {
		dev_err(&mstr->dev, "Master runtime configuration failed\n");
		ret = -EINVAL;
		goto error;
	}

	/* Initialize Slave runtime handle */
	if (slave) {
		slv_rt = sdw_config_slv_stream(slave, stream_config, sdw_rt);
		if (!slv_rt) {
			dev_err(&mstr->dev, "Slave runtime configuration failed\n");
			ret = -EINVAL;
			goto error;
		}
	}

	/*
	 * Stream params will be stored based on Tx only, since there can be
	 * only one Tx and multiple Rx, There can be multiple Tx if there is
	 * aggregation on Tx. That is handled by adding the channels to
	 * stream_params for each aggregated Tx slaves
	 */
	if (!sdw_rt->tx_ref_count && stream_config->direction ==
					SDW_DATA_DIR_OUT) {
		sdw_rt->stream_params.rate = stream_config->frame_rate;
		sdw_rt->stream_params.channel_count =
						stream_config->channel_count;
		sdw_rt->stream_params.bps = stream_config->bps;
		/* Reference count update */
		sdw_inc_ref_count(&sdw_rt->tx_ref_count);
	}

	/*
	 * Normally there will be only one Tx in system, multiple Tx can
	 * only be there if we support aggregation. In that case there may
	 * be multiple slave or masters handing different channels of same
	 * Tx stream.
	 */
	else if (sdw_rt->tx_ref_count && stream_config->direction ==
						SDW_DATA_DIR_OUT) {
		if (sdw_rt->stream_params.rate !=
			stream_config->frame_rate) {
			dev_err(&mstr->dev, "Frame rate for aggregated devices not matching\n");
			ret = -EINVAL;
			goto error;
		}

		if (sdw_rt->stream_params.bps != stream_config->bps) {
			dev_err(&mstr->dev, "bps for aggregated devices not matching\n");
			ret = -EINVAL;
			goto error;
		}

		/*
		 * Number of channels gets added, since both devices will be
		 * supporting different channels. Like one Codec supporting
		 * L and other supporting R channel.
		 */
		sdw_rt->stream_params.channel_count +=
			stream_config->channel_count;

		/* Reference count update */
		sdw_inc_ref_count(&sdw_rt->tx_ref_count);
	} else
		/* Reference count update */
		sdw_inc_ref_count(&sdw_rt->rx_ref_count);

	sdw_rt->type = stream_config->type;

	/* Change stream state to CONFIG */
	sdw_rt->stream_state = SDW_STATE_STRM_CONFIG;

	/*
	 * Slaves are added to two list, This is because bandwidth is
	 * calculated for two masters individually, while Ports are enabled
	 * of all the aggregated masters and slaves part of the same stream
	 * tag simultaneously.
	 */
	if (slave) {
		list_add_tail(&slv_rt->slave_strm_node, &sdw_rt->slv_rt_list);
		list_add_tail(&slv_rt->slave_mstr_node, &mstr_rt->slv_rt_list);
	}

	/* Release stream lock */
	mutex_unlock(&stream->stream_lock);

	if (slave)
		pm_runtime_get_sync(&slave->dev);
	else
		pm_runtime_get_sync(&mstr->dev);

	return ret;

error:
	mutex_unlock(&stream->stream_lock);
	kfree(mstr_rt);
	kfree(slv_rt);
out:
	return ret;

}
EXPORT_SYMBOL_GPL(snd_sdw_config_stream);

/**
 * sdw_check_dpn_caps: Check Master and Slave port capabilities. This performs
 *			PCM parameter check based on PCM
 *			parameters received in stream.
 * @dpn_cap: Capabilities of Master or Slave port.
 * @strm_params: Stream PCM parameters.
 */
static int sdw_check_dpn_caps(struct sdw_dpn_caps *dpn_cap,
		struct sdw_stream_params *strm_prms)
{
	struct sdw_port_aud_mode_prop *mode_prop =
		dpn_cap->mode_properties;
	int i, value;

	/* Check for sampling frequency */
	if (mode_prop->num_sample_rate_cfgs) {
		for (i = 0; i < mode_prop->num_sample_rate_cfgs; i++) {
			value = mode_prop->sample_rate_buf[i];
			if (strm_prms->rate == value)
				break;
		}

		if (i == mode_prop->num_sample_rate_cfgs)
			return -EINVAL;
	} else {

		if ((strm_prms->rate < mode_prop->min_sample_rate)
				|| (strm_prms->rate >
				mode_prop->max_sample_rate)) {
			return -EINVAL;
		}
	}

	/* Check for bit rate */
	if (dpn_cap->num_bps) {
		for (i = 0; i < dpn_cap->num_bps; i++) {
			value = dpn_cap->bps_buf[i];
			if (strm_prms->bps == value)
				break;
		}

		if (i == dpn_cap->num_bps)
			return -EINVAL;

	} else {

		if ((strm_prms->bps < dpn_cap->min_bps)
				|| (strm_prms->bps > dpn_cap->max_bps))
			return -EINVAL;
	}

	/* Check for number of channels */
	if (dpn_cap->num_ch_cnt) {
		for (i = 0; i < dpn_cap->num_ch_cnt; i++) {
			value = dpn_cap->ch_cnt_buf[i];
			if (strm_prms->bps == value)
				break;
		}

		if (i == dpn_cap->num_ch_cnt)
			return -EINVAL;

	} else {

		if ((strm_prms->channel_count < dpn_cap->min_ch_cnt) ||
			(strm_prms->channel_count > dpn_cap->max_ch_cnt))
			return -EINVAL;

	}

	return 0;
}

/**
 * sdw_mstr_port_configuration: Master Port configuration. This performs
 *	all the port related configuration including allocation port
 *	structure memory, assign PCM parameters and add port node in master
 *	runtime list.
 *
 * @mstr: Master handle.
 * @sdw_rt: Stream runtime information.
 * @ports_config: Port configuration for Slave.
 */
static int sdw_mstr_port_configuration(struct sdw_master *mstr,
			struct sdw_runtime *sdw_rt,
			struct sdw_ports_config *ports_config)
{
	struct sdw_mstr_runtime *mstr_rt = NULL;
	struct sdw_port_runtime *port_rt;
	int found = 0;
	int i;
	int ret = 0, pn = 0;
	struct sdw_dpn_caps *dpn_cap = mstr->caps.sdw_dpn_caps;

	/* Get Master device handle */
	list_for_each_entry(mstr_rt, &sdw_rt->mstr_rt_list, mstr_strm_node) {
		if (mstr_rt->mstr == mstr) {
			found = 1;
			break;
		}
	}

	if (!found) {
		dev_err(&mstr->dev, "Master not found for this port\n");
		return -EINVAL;
	}

	/* Allocate resources for port runtime handle */
	port_rt = kzalloc((sizeof(*port_rt) * ports_config->num_ports),
				GFP_KERNEL);
	if (!port_rt)
		return -ENOMEM;

	/* Check master capabilities */
	if (!dpn_cap)
		return -EINVAL;

	/* Iterate for number of ports to perform initialization */
	for (i = 0; i < ports_config->num_ports; i++) {
		port_rt[i].channel_mask = ports_config->port_config[i].ch_mask;
		port_rt[i].port_num = pn = ports_config->port_config[i].num;

		/* Perform capability check for master port */
		ret = sdw_check_dpn_caps(&dpn_cap[pn], &mstr_rt->stream_params);
		if (ret < 0) {
			dev_err(&mstr->dev, "Master capabilities check failed ret = %d\n", ret);
			goto error;
		}

		/* Add node to port runtime list */
		list_add_tail(&port_rt[i].port_node, &mstr_rt->port_rt_list);
	}

	return ret;

error:
	kfree(port_rt);
	return ret;
}

struct sdw_dpn_caps *sdw_get_slv_dpn_cap(struct sdw_slave_caps *slv_cap,
				enum sdw_data_direction direction,
				unsigned int port_num)
{
	int i;
	struct sdw_dpn_caps *dpn_cap;
	u8 num_ports;
	bool port_found = 0;

	if (direction == SDW_DATA_DIR_OUT)
		num_ports = slv_cap->num_src_ports;
	else
		num_ports = slv_cap->num_sink_ports;

	for (i = 0; i < num_ports; i++) {
		dpn_cap = &slv_cap->dpn_caps[direction][i];

		if (dpn_cap->port_number == port_num) {
			port_found = 1;
			break;
		}
	}

	if (!port_found)
		return NULL;

	return dpn_cap;

}

/**
 * sdw_config_slv_port: Slave Port configuration. This performs
 *	all the port related configuration including allocation port
 *	structure memory, assign PCM parameters and add port node in slave
 *	runtime list.
 * @slave: Slave handle.
 * @sdw_rt: Stream runtime information.
 * @ports_config: Port configuration for Slave.
 */
static int sdw_config_slv_port(struct sdw_slave *slave,
			struct sdw_runtime *sdw_rt,
			struct sdw_ports_config *ports_config)
{
	struct sdw_slv_runtime *slv_rt;
	struct sdw_port_runtime *port_rt;
	struct sdw_dpn_caps *dpn_cap;
	int found = 0, ret = 0;
	int i, pn;

	/* Get Slave device handle */
	list_for_each_entry(slv_rt, &sdw_rt->slv_rt_list, slave_strm_node) {
		if (slv_rt->slv == slave) {
			found = 1;
			break;
		}
	}

	if (!found) {
		dev_err(&slave->mstr->dev, "Slave not found for this port\n");
		return -EINVAL;
	}

	/* Check whether slave capabilities are valid or invalid */
	if (!slave->priv.slave_cap_updated) {
		dev_err(&slave->mstr->dev, "Slave capabilities not updated\n");
		return -EINVAL;
	}

	/* Allocate resources for port runtime handle */
	port_rt = kzalloc((sizeof(*port_rt) * ports_config->num_ports),
					GFP_KERNEL);
	if (!port_rt)
		return -ENOMEM;

	/* Assign PCM parameters */
	for (i = 0; i < ports_config->num_ports; i++) {
		port_rt[i].channel_mask = ports_config->port_config[i].ch_mask;
		port_rt[i].port_num = pn =
				ports_config->port_config[i].num;

		dpn_cap = sdw_get_slv_dpn_cap(&slave->priv.caps,
				slv_rt->direction,
				ports_config->port_config[i].num);
		if (!dpn_cap) {
			ret = -EINVAL;
			dev_err(&slave->mstr->dev, "Slave port capabilities not found ret = %d\n", ret);
			goto error;
		}

		/* Perform capability check for slave port */
		ret = sdw_check_dpn_caps(dpn_cap, &slv_rt->stream_params);
		if (ret < 0) {
			dev_err(&slave->mstr->dev, "Slave capabilities check failed ret = %d\n", ret);
			goto error;
		}

		/* Add node to port runtime list */
		list_add_tail(&port_rt[i].port_node, &slv_rt->port_rt_list);
	}

	return ret;

error:
	kfree(port_rt);
	return ret;
}

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
				unsigned int stream_tag)
{
	int ret;
	int i;
	struct sdw_stream_tag *stream_tags = snd_sdw_core.stream_tags;
	struct sdw_runtime *sdw_rt = NULL;
	struct sdw_stream_tag *stream = NULL;

	/* Retrieve master handle if called by Slave */
	if (!mstr)
		mstr = slave->mstr;

	/* Retrieve stream runtime handle */
	for (i = 0; i < SDW_NUM_STREAM_TAGS; i++) {
		if (stream_tags[i].stream_tag == stream_tag) {
			sdw_rt = stream_tags[i].sdw_rt;
			stream = &stream_tags[i];
			break;
		}
	}

	if (!sdw_rt) {
		dev_err(&mstr->dev, "Invalid stream tag\n");
		return -EINVAL;
	}

	/* Acquire stream lock */
	mutex_lock(&stream->stream_lock);

	/* Perform Master/Slave port configuration */
	if (!slave)
		ret = sdw_mstr_port_configuration(mstr, sdw_rt, ports_config);
	else
		ret = sdw_config_slv_port(slave, sdw_rt, ports_config);

	/* Release stream lock */
	mutex_unlock(&stream->stream_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(snd_sdw_config_ports);

/**
 * sdw_find_stream: Retrieves stream tag handle by matching stream tag.
 *
 * @stream_tag: Stream tag.
 */
static struct sdw_stream_tag *sdw_find_stream(int stream_tag)
{
	int i;
	struct sdw_stream_tag *stream_tags = snd_sdw_core.stream_tags;
	struct sdw_stream_tag *stream = NULL;

	/* Acquire core lock */
	mutex_lock(&snd_sdw_core.core_mutex);

	for (i = 0; i < SDW_NUM_STREAM_TAGS; i++) {
		if (stream_tag == stream_tags[i].stream_tag) {
			stream = &stream_tags[i];
			break;
		}
	}

	if (stream == NULL) {
		/* Release core lock */
		mutex_unlock(&snd_sdw_core.core_mutex);
		WARN_ON(1);
		return NULL;
	}

	/* Release core lock */
	mutex_unlock(&snd_sdw_core.core_mutex);

	return stream;
}

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
int snd_sdw_prepare_and_enable(unsigned int stream_tag)
{

	int ret;

	struct sdw_stream_tag *stream = NULL;

	stream = sdw_find_stream(stream_tag);
	if (!stream)
		return -EINVAL;

	/* Acquire Master lock */
	sdw_acquire_mstr_lock(stream);

	ret = sdw_prepare_and_enable_ops(stream);
	if (ret < 0)
		pr_err("Error: prepare/enable operation failed\n");

	/* Release Master lock */
	sdw_release_mstr_lock(stream);

	return ret;
}
EXPORT_SYMBOL_GPL(snd_sdw_prepare_and_enable);

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
int snd_sdw_disable_and_deprepare(unsigned int stream_tag)
{
	int ret;
	struct sdw_stream_tag *stream = NULL;

	stream = sdw_find_stream(stream_tag);
	if (!stream)
		return -EINVAL;

	/* Acquire Master lock */
	sdw_acquire_mstr_lock(stream);

	ret = sdw_disable_and_deprepare_ops(stream);
	if (ret < 0)
		pr_err("Error: disable/de-prepare operations failed\n");

	/* Release Master lock */
	sdw_release_mstr_lock(stream);

	return ret;
}
EXPORT_SYMBOL_GPL(snd_sdw_disable_and_deprepare);

/**
 * snd_sdw_master_stop_clock: Stop the clock. This function broadcasts the
 *	SCP_CTRL register with clock_stop_now bit set.
 *
 * @master: Master handle for which clock has to be stopped.
 */
int snd_sdw_master_stop_clock(struct sdw_master *master)
{
	int ret, i;
	struct sdw_msg msg;
	u8 buf = 0;
	enum sdw_clk_stop_mode mode;

	/*
	 * Send Broadcast message to the SCP_ctrl register with clock stop
	 * now. If none of the Slaves are attached, then there may not be
	 * ACK, flag the error about ACK not received but clock will be
	 * still stopped.
	 */

	buf |= 0x1 << SDW_SCP_CTRL_CLK_STP_NOW_SHIFT;
	ret = sdw_wr_msg_nopm(&msg, 0x0, SDW_SCP_CTRL, 1, &buf,
				SDW_SLAVE_BDCAST_ADDR, master,
				SDW_NUM_OF_MSG1_XFRD);
	if (ret != SDW_NUM_OF_MSG1_XFRD)
		dev_err(&master->dev, "ClockStopNow Broadcast message failed\n");

	/*
	 * Mark all Slaves as un-attached which are entering clock stop
	 * mode1
	 */
	for (i = 1; i <= SDW_MAX_DEVICES; i++) {

		if (!master->sdw_addr[i].assigned)
			continue;

		/* Get clock stop mode for all Slaves */
		mode = sdw_slv_get_clk_stp_mode(master->sdw_addr[i].slave);
		if (mode == SDW_CLOCK_STOP_MODE_0)
			continue;

		/* If clock stop mode 1, mark Slave as not present */
		master->sdw_addr[i].status = SDW_SLAVE_STAT_NOT_PRESENT;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(snd_sdw_master_stop_clock);

static struct sdw_slave *sdw_get_slv_status(struct sdw_master *mstr,
					int *slave_index)
{
	int i;

	for (i = *slave_index; i <= SDW_MAX_DEVICES; i++) {
		if (mstr->sdw_addr[i].assigned != true)
			continue;

		if (mstr->sdw_addr[i].status == SDW_SLAVE_STAT_NOT_PRESENT)
			continue;

		*slave_index = i + 1;
		return mstr->sdw_addr[i].slave;
	}
	return NULL;
}

/*
 * Wait till ClockStop prepared/De-prepared is finished, Broadcasts the read
 * message to read the SCP_STAT register. Wait till ClockStop_NotFinished bit
 * is set. Break loop after timeout.
 */
static void sdw_wait_for_clk_stp_prep(struct sdw_master *mstr, unsigned int
			prep_timeout)
{
	int ret;
	struct sdw_msg msg;
	u8 buf = 0;
	int count = 0;

	/* Create message to read clock stop status, its broadcast message. */
	sdw_create_rd_msg(&msg, 0x0, SDW_SCP_STAT, 1, &buf,
				SDW_SLAVE_BDCAST_ADDR);
	/*
	 * Once all the Slaves are written with prepare bit, broadcast the
	 * read message for the SCP_STAT register to read the
	 * ClockStopNotFinished bit. Read till we get this a 0. Currently
	 * we have timeout of 1sec before giving up. Even if its not read as
	 * 0 after timeout, controller can stop the clock after warning.
	 */
	do {
		ret = sdw_slv_transfer_nopm(mstr, &msg, SDW_NUM_OF_MSG1_XFRD);
		if (ret != SDW_NUM_OF_MSG1_XFRD) {
			WARN_ONCE(1, "Clock stop status read failed\n");
			break;
		}

		if (!(buf & SDW_SCP_STAT_CLK_STP_NF_MASK))
			break;

		/*
		 * Sleep in range of 1ms for the max number of millisecond
		 * of timeout
		 */
		usleep_range(1000, 1200);
		count++;

	} while (count != prep_timeout);

	if (!(buf & SDW_SCP_STAT_CLK_STP_NF_MASK))
		dev_info(&mstr->dev, "Clock stop prepare done\n");
	else
		WARN_ONCE(1, "Some Slaves prepare un-successful\n");
}

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
int snd_sdw_master_prepare_for_clk_stop(struct sdw_master *master)
{
	struct sdw_slave_caps *cap;
	enum sdw_clk_stop_mode clock_stop_mode;
	int ret;
	struct sdw_slave *slave = NULL;
	int slv_index = 1;
	unsigned int prep_timeout = 0;

	/*
	 * Get all the Slaves registered to the Master driver for preparing
	 * for clock stop. Start from Slave with logical address as 1.
	 */
	while ((slave = sdw_get_slv_status(master, &slv_index)) != NULL) {

		struct sdw_slave_priv *slv_priv = &slave->priv;

		cap = &slv_priv->caps;

		clock_stop_mode = sdw_slv_get_clk_stp_mode(slave);

		/*
		 * Call the pre clock stop prepare, if Slave requires.
		 */
		if (slv_priv->driver->pre_clk_stop_prep) {
			ret = slv_priv->driver->pre_clk_stop_prep(slave,
						clock_stop_mode, true);

			/* If it fails we still continue */
			if (ret < 0)
				dev_warn(&master->dev, "Pre prepare failed for Slave %d\n",
						slave->dev_num);
		}

		sdw_prepare_slv_for_clk_stp(master, slave, clock_stop_mode,
								true);

		if (prep_timeout > cap->clk_stp_prep_timeout)
			prep_timeout = cap->clk_stp_prep_timeout;
	}

	/* Wait till prepare for all Slaves is finished */
	sdw_wait_for_clk_stp_prep(master, prep_timeout);

	slv_index = 1;
	while ((slave = sdw_get_slv_status(master, &slv_index)) != NULL) {

		struct sdw_slave_priv *slv_priv = &slave->priv;

		cap = &slv_priv->caps;

		clock_stop_mode = sdw_slv_get_clk_stp_mode(slave);

		if (slv_priv->driver->post_clk_stop_prep) {
			ret = slv_priv->driver->post_clk_stop_prep(slave,
							clock_stop_mode,
							true);
			/*
			 * Even if Slave fails we continue with other
			 * Slaves. This should never happen ideally.
			 */
			if (ret < 0)
				dev_err(&master->dev, "Post prepare failed for Slave %d ret = %d\n",
							slave->dev_num, ret);

		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_sdw_master_prepare_for_clk_stop);

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
int snd_sdw_master_deprepare_after_clk_start(struct sdw_master *master)
{
	struct sdw_slave_caps *cap;
	enum sdw_clk_stop_mode clock_stop_mode;
	int ret = 0;
	struct sdw_slave *slave = NULL;
	bool stop = false;
	int slv_index = 1;
	unsigned int prep_timeout = 0;

	while ((slave = sdw_get_slv_status(master, &slv_index)) != NULL) {
		struct sdw_slave_priv *slv_priv = &slave->priv;

		cap = &slv_priv->caps;

		/* Get the clock stop mode from which Slave is exiting */
		clock_stop_mode = sdw_slv_get_clk_stp_mode(slave);

		/*
		 * Slave is exiting from Clock stop mode 1, De-prepare is
		 * optional based on capability, and it has to be done after
		 * Slave is enumerated. So nothing to be done here.
		 */
		if (clock_stop_mode == SDW_CLOCK_STOP_MODE_1)
			continue;
		/*
		 * Call the pre clock stop prepare, if Slave requires.
		 */
		if (slv_priv->driver->pre_clk_stop_prep)
			ret = slv_priv->driver->pre_clk_stop_prep(slave,
						clock_stop_mode, false);

		/* If it fails we still continue */
		if (ret < 0)
			dev_warn(&master->dev, "Pre de-prepare failed for Slave %d ret = %d\n",
						slave->dev_num, ret);

		sdw_prepare_slv_for_clk_stp(master, slave, clock_stop_mode,
						false);
		if (prep_timeout > cap->clk_stp_prep_timeout)
			prep_timeout = cap->clk_stp_prep_timeout;
	}

	/*
	 * Wait till de-prepare is finished for all the Slaves.
	 */
	sdw_wait_for_clk_stp_prep(master, prep_timeout);

	slv_index = 1;
	while ((slave = sdw_get_slv_status(master, &slv_index)) != NULL) {

		struct sdw_slave_priv *slv_priv = &slave->priv;

		cap = &slv_priv->caps;

		clock_stop_mode = sdw_slv_get_clk_stp_mode(slave);

		/*
		 * Slave is exiting from Clock stop mode 1, De-prepare is
		 * optional based on capability, and it has to be done after
		 * Slave is enumerated.
		 */
		if (clock_stop_mode == SDW_CLOCK_STOP_MODE_1)
			continue;

		if (slv_priv->driver->post_clk_stop_prep)
			ret = slv_priv->driver->post_clk_stop_prep(slave,
							clock_stop_mode,
							stop);
			/*
			 * Even if Slave fails we continue with other
			 * Slaves. This should never happen ideally.
			 */
			if (ret < 0)
				dev_err(&master->dev, "Post de-prepare failed for Slave %d ret = %d\n",
							slave->dev_num, ret);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_sdw_master_deprepare_after_clk_start);

/**
 * snd_sdw_master_get: Return the Master handle from Master number.
 *	Increments the reference count of the module. Similar to
 *	i2c_get_adapter.
 *
 * @nr: Master number.
 *
 * Returns Master handle on success, else NULL
 */
struct sdw_master *snd_sdw_master_get(int nr)
{
	struct sdw_master *master;

	mutex_lock(&snd_sdw_core.core_mutex);

	master = idr_find(&snd_sdw_core.idr, nr);
	if (master && !try_module_get(master->driver->driver.owner))
		master = NULL;

	mutex_unlock(&snd_sdw_core.core_mutex);

	return master;
}
EXPORT_SYMBOL_GPL(snd_sdw_master_get);

/**
 * snd_sdw_master_put: Reverses the effect of sdw_master_get
 *
 * @master: Master handle.
 */
void snd_sdw_master_put(struct sdw_master *master)
{
	if (master)
		module_put(master->driver->driver.owner);
}
EXPORT_SYMBOL_GPL(snd_sdw_master_put);

static void sdw_exit(void)
{
	bus_unregister(&sdw_bus_type);
}

static int sdw_init(void)
{
	int retval;

	mutex_init(&snd_sdw_core.core_mutex);
	INIT_LIST_HEAD(&snd_sdw_core.bus_list);
	idr_init(&snd_sdw_core.idr);
	retval = bus_register(&sdw_bus_type);

	if (retval)
		bus_unregister(&sdw_bus_type);

	/*
	 * Initialization of bandwidth and runtime stream
	 * management related operations required for bus driver.
	 * Currently pre-calculation of row-column combination is performed
	 * which is required to expedite computation of bus frame shape.
	 */
	sdw_create_row_col_pair();

	return retval;
}

subsys_initcall(sdw_init);
module_exit(sdw_exit);

MODULE_AUTHOR("Hardik Shah <hardik.t.shah@intel.com>");
MODULE_AUTHOR("Sanyog Kale <sanyog.r.kale@intel.com>");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("SoundWire bus driver");
MODULE_ALIAS("platform:soundwire");
