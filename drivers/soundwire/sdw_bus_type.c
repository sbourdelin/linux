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

/*
 * First written by Hardik T Shah
 * Rewrite by Vinod
 */

#include <linux/init.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/acpi.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/soundwire/soundwire.h>

static const struct sdw_device_id *
sdw_get_device_id(struct sdw_slave *sdw, struct sdw_driver *sdrv)
{
	const struct sdw_device_id *id = sdrv->id_table;
	if (id) {
		while (id->mfg_id) {
			if (sdw->id.mfg_id == id->mfg_id &&
					sdw->id.part_id == id->part_id)
				return id;
			id++;
		}
	}

	return NULL;
}

static int sdw_bus_match(struct device *dev, struct device_driver *drv)
{
	struct sdw_slave *sdw = dev_to_sdw_dev(dev);
	struct sdw_driver *sdrv = drv_to_sdw_driver(drv);
	const struct sdw_device_id *id;

	id = sdw_get_device_id(sdw, sdrv);
	if (id)
		return 1;
	else
		return 0;

}

static int sdw_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct sdw_slave *sdw = dev_to_sdw_dev(dev);
	char modalias[32];
	int rc;

	rc = acpi_device_uevent_modalias(dev, env);
	if (rc != -ENODEV)
		return rc;

	snprintf(modalias, 32, "sdw:m%08Xx%08x\n", sdw->id.mfg_id, sdw->id.part_id);
	if (add_uevent_var(env, "MODALIAS=%s", modalias))
		return -ENOMEM;

	return 0;
}

struct bus_type sdw_bus_type = {
	.name = "soundwire",
	.match = sdw_bus_match,
	.uevent = sdw_uevent,
};
EXPORT_SYMBOL(sdw_bus_type);

static int sdw_drv_probe(struct device *dev)
{
	struct sdw_slave *sdw = dev_to_sdw_dev(dev);
	struct sdw_driver *sdrv = drv_to_sdw_driver(dev->driver);
	const struct sdw_device_id *id;
	int ret;

	if (!sdrv->probe)
		return -ENODEV;

	id = sdw_get_device_id(sdw, sdrv);
	if (!id)
		return -ENODEV;

	ret = dev_pm_domain_attach(dev, false);
	if (ret) {
		dev_err(dev, "Failed to attach PM domain: %d\n", ret);
		return ret;
	}
	sdw->ops = sdrv->ops;

	/*
	 * Unbound SDW functions are always suspended. During probe, the
	 * function is set active and the usage count is incremented. If
	 * the driver supports runtime PM, it should call
	 * pm_runtime_put_noidle() in its probe routine and
	 * pm_runtime_get_noresume() in its remove routine.
	 */
	ret = pm_runtime_get_sync(dev);
	if (ret) {
		dev_err(dev, "Failed to do runtime_get_sync: %d\n", ret);
		goto pm_disable;
	}

	ret = sdrv->probe(sdw, id);
	if (ret) {
		dev_err(dev, "Probe of %s failed: %d\n", sdrv->name, ret);
		goto pm_disable;
	}

	return 0;

pm_disable:
	pm_runtime_put_noidle(dev);
	dev_pm_domain_detach(dev, false);
	return ret;
}

static int sdw_drv_remove(struct device *dev)
{
	struct sdw_slave *sdw = dev_to_sdw_dev(dev);
	struct sdw_driver *sdrv = drv_to_sdw_driver(dev->driver);

	/* Make sure card is powered before invoking ->remove() */
	pm_runtime_get_sync(dev);

	if (sdrv->remove)
		sdrv->remove(sdw);

	/* undo the increment done above */
	pm_runtime_put_noidle(dev);
	pm_runtime_put_sync(dev);

	dev_pm_domain_detach(dev, false);

	return 0;
}

static void sdw_drv_shutdown(struct device *dev)
{
	struct sdw_slave *sdw = dev_to_sdw_dev(dev);
	struct sdw_driver *sdrv = drv_to_sdw_driver(dev->driver);

	if (sdrv->shutdown)
		sdrv->shutdown(sdw);
}

/**
 * sdw_register_driver - register a SoundWire driver
 *
 * @drv: the driver to register
 * @owner: owner module of the driver to register
 *
 * Return: zero on success, else a negative error code.
 */
int sdw_register_driver(struct sdw_driver *drv, struct module *owner)
{
	drv->driver.owner = owner;
	drv->driver.bus = &sdw_bus_type;

	if (drv->probe)
		drv->driver.probe = sdw_drv_probe;
	if (drv->remove)
		drv->driver.remove = sdw_drv_remove;
	if (drv->shutdown)
		drv->driver.shutdown = sdw_drv_shutdown;

	return driver_register(&drv->driver);
}
EXPORT_SYMBOL(sdw_register_driver);

/*
 * sdw_unregister_driver: unregisters the SoundWire driver
 *
 * @drv: already registered driver to unregister
 *
 * Return: zero on success, else a negative error code.
 */
void sdw_unregister_driver(struct sdw_driver *drv)
{
	        driver_unregister(&drv->driver);
}
EXPORT_SYMBOL(sdw_unregister_driver);


static int __init sdw_bus_init(void)
{
	return bus_register(&sdw_bus_type);
}

static void __exit sdw_bus_exit(void)
{
	bus_unregister(&sdw_bus_type);
}

subsys_initcall(sdw_bus_init);
module_exit(sdw_bus_exit);

MODULE_DESCRIPTION("Soundwire bus");
MODULE_LICENSE("Dual BSD/GPL");
