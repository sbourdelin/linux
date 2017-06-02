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
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/soundwire/soundwire.h>
#include "sdw_bus.h"

/**
 * sdw_add_bus_master: add a bus master instance
 *
 * @bus: the bus instance
 *
 * This initializes the bus instance and adds and starts reading the
 * properties for child devices and creates them
 */
int sdw_add_bus_master(struct sdw_bus *bus)
{
	if (!bus->dev) {
		pr_err("Soundwire bus w/o a device\n");
		return -ENODEV;
	}

	spin_lock_init(&bus->lock);
	INIT_LIST_HEAD(&bus->slaves);

	/*
	 * SDW is an enumerable bus, but devices can be powered off, so they
	 * won't be able to report as present.
	 *
	 * So do a device creation now, probe the driver and wait for them
	 * to report as present before using them
	 *
	 * here we need to find the slaves described in the respective
	 * firmware (ACPI/DT)
	 */

	/* ACPI check first */
	sdw_acpi_find_slaves(bus);

	return 0;
}
EXPORT_SYMBOL(sdw_add_bus_master);

void sdw_delete_bus_master(struct sdw_bus *bus)
{
	WARN_ON(!list_empty(&bus->slaves));
}
EXPORT_SYMBOL(sdw_delete_bus_master);
