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
#include <linux/pm_runtime.h>
#include <linux/pm_domain.h>
#include <sound/sdw_bus.h>
#include <sound/sdw_master.h>
#include <sound/sdw_slave.h>

#include "sdw_priv.h"

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

/**
 * snd_sdw_master_register_driver: SoundWire Master driver registration with
 *	bus. This API will register the Master driver with the SoundWire
 *	bus. It is typically called from the driver's module-init function.
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

	/*
	 * Add bus to the list of buses inside core. This is list of Slave
	 * devices enumerated on this bus. Adding new devices at end. It can
	 * be added at any location in list.
	 */
	list_add_tail(&sdw_bus->bus_node, &snd_sdw_core.bus_list);
	mutex_unlock(&snd_sdw_core.core_mutex);

	return 0;

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
	return retval;
}

subsys_initcall(sdw_init);
module_exit(sdw_exit);

MODULE_AUTHOR("Hardik Shah <hardik.t.shah@intel.com>");
MODULE_AUTHOR("Sanyog Kale <sanyog.r.kale@intel.com>");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("SoundWire bus driver");
MODULE_ALIAS("platform:soundwire");
