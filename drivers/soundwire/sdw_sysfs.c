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

#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/soundwire/soundwire.h>
#include "sdw_bus.h"

/*
 * The sysfs for properties reflects the MIPI description as given
 * in the MIPI DisCo spec
 *
 * Base file is:
 *		properties
 * 		|---- interface-revision
 *		|---- master-count
 *		|---- link-N
 *		      |---- clock-stop-modes
 *		      |---- max-clock-frequency
 *		      |---- clock-frequencies
 *		      |---- default-frame-rows
 *		      |---- default-frame-cols
 *		      |---- dynamic-frame-shape
 *		      |---- command-error-threshold
 */

struct sdw_master_sysfs {
	struct kobject kobj;
	struct sdw_bus *bus;
};

struct prop_attribute {
	struct attribute attr;
	ssize_t (*show)(struct sdw_bus *bus,
			struct prop_attribute *attr, char *buf);
};

static ssize_t prop_attr_show(struct kobject *kobj,
		struct attribute *attr, char *buf)
{
	struct prop_attribute *prop_attr =
		container_of(attr, struct prop_attribute, attr);
	struct sdw_master_sysfs *master =
		container_of(kobj, struct sdw_master_sysfs, kobj);

	if (!prop_attr->show)
		return -EIO;

	return prop_attr->show(master->bus, prop_attr, buf);
}

static const struct sysfs_ops prop_sysfs_ops = {
	.show	= prop_attr_show,
};

static void prop_release(struct kobject *kobj)
{
	struct sdw_master_sysfs *master =
		container_of(kobj, struct sdw_master_sysfs, kobj);

	kfree(master);
}

static struct kobj_type prop_ktype = {
	.release	= prop_release,
	.sysfs_ops	= &prop_sysfs_ops,
};


#define MASTER_ATTR(_name) \
	struct prop_attribute master_attr_##_name = __ATTR_RO(_name)

static ssize_t revision_show(struct sdw_bus *bus,
			struct prop_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%08x\n", bus->prop.revision);
}

static ssize_t clock_stop_modes_show(struct sdw_bus *bus,
			struct prop_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%08x\n", bus->prop.clk_stop_mode);
}

static ssize_t max_clock_frequency_show(struct sdw_bus *bus,
			struct prop_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%08x\n", bus->prop.max_freq);
}

static ssize_t clock_frequencies_show(struct sdw_bus *bus,
			struct prop_attribute *attr, char *buf)
{
	ssize_t size = 0;
	int i;

	for (i = 0; i < bus->prop.num_freq; i++)
		size += sprintf(buf + size, "%8d\n", bus->prop.freq[i]);

	return size;
}

static ssize_t clock_gears_show(struct sdw_bus *bus,
			struct prop_attribute *attr, char *buf)
{
	ssize_t size = 0;
	int i;

	for (i = 0; i < bus->prop.num_clk_gears; i++)
		size += sprintf(buf + size, "%8d\n", bus->prop.clk_gears[i]);

	return size;
}

static ssize_t default_frame_rows_show(struct sdw_bus *bus,
			struct prop_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%08x\n", bus->prop.default_rows);
}

static ssize_t default_frame_cols_show(struct sdw_bus *bus,
			struct prop_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%08x\n", bus->prop.default_col);
}

static ssize_t dynamic_frame_shape_show(struct sdw_bus *bus,
			struct prop_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%08x\n", bus->prop.dynamic_frame);
}

static ssize_t command_error_threshold_show(struct sdw_bus *bus,
			struct prop_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%08x\n", bus->prop.err_threshold);
}

static MASTER_ATTR(revision);
static MASTER_ATTR(clock_stop_modes);
static MASTER_ATTR(max_clock_frequency);
static MASTER_ATTR(clock_frequencies);
static MASTER_ATTR(clock_gears);
static MASTER_ATTR(default_frame_rows);
static MASTER_ATTR(default_frame_cols);
static MASTER_ATTR(dynamic_frame_shape);
static MASTER_ATTR(command_error_threshold);

static struct attribute *master_node_attrs[] = {
	&master_attr_revision.attr,
	&master_attr_clock_stop_modes.attr,
	&master_attr_max_clock_frequency.attr,
	&master_attr_clock_frequencies.attr,
	&master_attr_clock_gears.attr,
	&master_attr_default_frame_rows.attr,
	&master_attr_default_frame_cols.attr,
	&master_attr_dynamic_frame_shape.attr,
	&master_attr_command_error_threshold.attr,
	NULL,
};

static const struct attribute_group master_node_group = {
	.attrs = master_node_attrs,
};


static int sdw_sysfs_init(struct sdw_bus *bus)
{
	struct kset *sdw_bus_kset;
	struct sdw_master_sysfs *master;
	int err;

	sdw_bus_kset = bus_get_kset(&sdw_bus_type);

	master = bus->sysfs = kzalloc(sizeof(*master), GFP_KERNEL);
	if (!master)
		return -ENOMEM;

	err = kobject_init_and_add(&master->kobj, &prop_ktype,
			&sdw_bus_kset->kobj, "mipi-properties-link%d", bus->link_id);
	if (err < 0)
		return err;

	master->bus = bus;

	err = sysfs_create_group(&master->kobj, &master_node_group);
	if (err < 0) {
		kobject_put(&master->kobj);
		return err;
	}

	kobject_uevent(&master->kobj, KOBJ_CHANGE);
	return 0;
}

static void sdw_sysfs_free(struct sdw_bus *bus)
{
	struct sdw_master_sysfs *master = bus->sysfs;

	if (!master)
		return;

	kobject_put(&master->kobj);
	bus->sysfs = NULL;
}

int sdw_sysfs_bus_init(struct sdw_bus *bus)
{
	if (!bus->sysfs)
		sdw_sysfs_init(bus);

	return 0;
}

void sdw_sysfs_bus_exit(struct sdw_bus *bus)
{
	sdw_sysfs_free(bus);
}

/*
 * slave sysfs
 */

/*
 * The sysfs for slave reflects the MIPI description as given
 * in the MIPI DisCo spec
 *
 * Base file is device
 *	|---- mipi_revision
 * 	|---- wake_capable
 * 	|---- test_mode_capable
 *	|---- simple_clk_stop_capable
 *	|---- clk_stop_timeout
 *	|---- ch_prep_timeout
 *	|---- reset_behave
 *	|---- high_PHY_capable
 *	|---- paging_support
 *	|---- bank_delay_support
 *	|---- p15_behave
 *	|---- master_count
 *	|---- source_ports
 *	|---- sink_ports
 *	|---- dp0
 *		|---- max_word
 *		|---- min_word
 *		|---- words
 *		|---- flow_controlled
 *		|---- simple_ch_prep_sm
 *		|---- device_interrupts
 *	|---- dpN
 *		|---- max_word
 *		|---- min_word
 *		|---- words
 *		|---- type
 *		|---- max_grouping
 *		|---- simple_ch_prep_sm
 *		|---- ch_prep_timeout
 *		|---- device_interrupts
 *		|---- max_ch
 *		|---- min_ch
 *		|---- ch
 *		|---- ch_combinations
 *		|---- modes
 *		|---- max_async_buffer
 *		|---- block_pack_mode
 *		|---- port_encoding
 *		|---- bus_min_freq
 *		|---- bus_max_freq
 *		|---- bus_freq
 *		|---- max_freq
 *		|---- min_freq
 *		|---- freq
 *		|---- prep_ch_behave
 *		|---- glitchless
 *
 */
struct sdw_slave_sysfs {
	struct sdw_slave *slave;
};

#define SLAVE_ATTR(type)					\
static ssize_t type##_show(struct device *dev,			\
		struct device_attribute *attr, char *buf)	\
{								\
	struct sdw_slave *slave = dev_to_sdw_dev(dev);		\
	return sprintf(buf, "0x%x\n", slave->prop.type);	\
}								\
static DEVICE_ATTR_RO(type)

SLAVE_ATTR(mipi_revision);
SLAVE_ATTR(wake_capable);
SLAVE_ATTR(test_mode_capable);
SLAVE_ATTR(clk_stop_mode1);
SLAVE_ATTR(simple_clk_stop_capable);
SLAVE_ATTR(clk_stop_timeout);
SLAVE_ATTR(ch_prep_timeout);
SLAVE_ATTR(reset_behave);
SLAVE_ATTR(high_PHY_capable);
SLAVE_ATTR(paging_support);
SLAVE_ATTR(bank_delay_support);
SLAVE_ATTR(p15_behave);
SLAVE_ATTR(master_count);
SLAVE_ATTR(source_ports);

static ssize_t modalias_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct sdw_slave *slave = dev_to_sdw_dev(dev);

	return sprintf(buf, "sdw:m%08Xx%08x\n",
			slave->id.mfg_id, slave->id.part_id);
}
static DEVICE_ATTR_RO(modalias);

static struct attribute *slave_dev_attrs[] = {
	&dev_attr_mipi_revision.attr,
	&dev_attr_wake_capable.attr,
	&dev_attr_test_mode_capable.attr,
	&dev_attr_clk_stop_mode1.attr,
	&dev_attr_simple_clk_stop_capable.attr,
	&dev_attr_clk_stop_timeout.attr,
	&dev_attr_ch_prep_timeout.attr,
	&dev_attr_reset_behave.attr,
	&dev_attr_high_PHY_capable.attr,
	&dev_attr_paging_support.attr,
	&dev_attr_bank_delay_support.attr,
	&dev_attr_p15_behave.attr,
	&dev_attr_master_count.attr,
	&dev_attr_source_ports.attr,
	&dev_attr_modalias.attr,
	NULL,
};

static struct attribute_group slave_dev_attr_group = {
	.attrs	= slave_dev_attrs,
};

const struct attribute_group *slave_dev_attr_groups[] = {
	&slave_dev_attr_group,
	NULL
};

int sdw_sysfs_slave_init(struct sdw_slave *slave)
{
	return 0;
}

void sdw_sysfs_slave_exit(struct sdw_slave *slave)
{
}

