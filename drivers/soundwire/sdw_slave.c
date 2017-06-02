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
#include <linux/soundwire/soundwire.h>
#include "sdw_bus.h"

static void sdw_delete_slave(struct sdw_slave *slave)
{
	struct sdw_bus *bus = slave->bus;

	spin_lock(&bus->lock);
	if (list_empty(&bus->slaves))
		goto end;

	list_del(&slave->node);

end:
	spin_unlock(&bus->lock);
}

static void sdw_slave_release(struct device *dev)
{
	struct sdw_slave *slave = dev_to_sdw_dev(dev);

	sdw_delete_slave(slave);
}

/*
 * sdw_add_slave: add a slave to a sdw bus instance
 *
 * @bus: bus instance
 * @id: Address of sdw device
 *
 * This allocates the sdw_slave and registers it to the core so that driver
 * can load against it.
 * NOTE: device is marked SDW_SLAVE_NOT_PRESENT as this will be called from
 * ACPI/DT context when the device shows up, but needs to show up in bus by
 * marking SDW_SLAVE_PRESENT before bus will start using it
 */
static int sdw_add_slave(struct sdw_bus *bus,
		struct sdw_slave_id *id, struct fwnode_handle *fwnode)
{
	struct sdw_slave *slave = kzalloc(sizeof(*slave), GFP_KERNEL);
	char name[20];
	int ret;

	if (!slave)
		return -ENOMEM;

	/* Initialize data structure */
	memcpy(&slave->id, id, sizeof(*id));

	snprintf(name, sizeof(name), "sdw:%x:%x:%x:%x:%x",
			id->mfg_id, id->part_id, id->class_id, id->unique_id, id->link_id);

	device_initialize(&slave->dev);
	slave->dev.parent = bus->dev;
	slave->dev.fwnode = fwnode;
	dev_set_name(&slave->dev, "%s", name);
	slave->dev.release = sdw_slave_release;
	slave->dev.bus = &sdw_bus_type;
	slave->bus = bus;
	slave->status = SDW_SLAVE_NOT_PRESENT;
	slave->addr = 0;

	spin_lock(&bus->lock);
	list_add_tail(&slave->node, &bus->slaves);
	spin_unlock(&bus->lock);

	ret = device_add(&slave->dev);
	if (ret) {
		dev_err(bus->dev, "Failed to add slave\n");
		return ret;
	}

	/* device is added so init the properties */
	if (slave->ops && slave->ops->read_prop)
		slave->ops->read_prop(slave);

	return 0;
}

int sdw_acpi_find_slaves(struct sdw_bus *bus)
{
	struct acpi_device *adev, *parent;

	parent = ACPI_COMPANION(bus->dev);
	if (!parent) {
		dev_err(bus->dev, "Can't find parent for acpi bind\n");
		return -ENODEV;
	}

	list_for_each_entry(adev, &parent->children, node) {
		struct sdw_slave_id id;
		unsigned long long addr;
		acpi_status status;
		unsigned int link_id;

		status = acpi_evaluate_integer(adev->handle, METHOD_NAME__ADR,
				NULL, &addr);

		if (ACPI_FAILURE(status))
			continue;

		/* check if this has same link_id as master */
		link_id = (addr & GENMASK(51, 48)) >> 48;

		if (link_id != bus->link_id)
			continue;

		dev_err(bus->dev, "Found SDW slave: %llx\n", addr);

		id.class_id = addr & GENMASK(7, 0);
		id.part_id = (addr & GENMASK(23, 8)) >> 8;
		id.mfg_id = (addr & GENMASK(39, 24)) >> 16;
		id.unique_id = (addr & GENMASK(43, 40)) >> 40;
		id.sdw_version = (addr & GENMASK(47, 44)) >> 44;
		id.link_id = link_id;

		dev_err(bus->dev,
			"Found class_id %x, part_id %x, mfg_id %x, unique_id %x, version %x\n",
			id.class_id, id.part_id, id.mfg_id,
			id.unique_id, id.sdw_version);

		sdw_add_slave(bus, &id, acpi_fwnode_handle(adev));
	}

	return 0;
}
