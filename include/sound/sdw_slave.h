/*
 * sdw_slave.h - Definition for SoundWire Slave interface. This file has
 *	all the definitions which are required for only SoundWire Slave
 *	driver. Some interfaces are common for both Slave and Master
 *	driver. Please refer to sdw_bus.h for common interfaces.
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

#ifndef _LINUX_SDW_SLAVE_H
#define _LINUX_SDW_SLAVE_H

#include <sound/sdw_bus.h>

/**
 * sdw_portn_intr_mask: Implementation defined interrupt mask for Slave
 *	Ports other than Data Port 0.
 *
 * @mask: Mask for the implementation defined interrupt.
 */
struct sdw_portn_intr_mask {
	u8 mask;
};

/**
 * sdw_impl_def_intr_mask: Implementation defined interrupt mask for Slave.
 *	Slave Ports can be sink or source or bidirectional. If Slave has
 *	bidirectional Ports, implementation defined interrupt mask should be
 *	provided for both directions. Bus driver programs the mask in the
 *	register before Port prepare based on Port configured as a sink or
 *	source.
 *
 * @portn_mask: Implementation defined mask for Slave Ports other than port0.
 *	Mask bits are exactly same as defined in MIPI Spec 1.1. Array size
 *	shall be same as number of Ports in Slave. For bidirectional ports,
 *	masks can be different for Source and Sink ports.
 *
 * @control_port_mask: Implementation defined interrupt mask for control
 *	Port. Mask Bits are exactly same as defined in MIPI Spec 1.1.
 *
 * @port0_mask: Implementation defined interrupt mask for Data Port 0.
 *	Mask bits are exactly same as defined in MIPI Spec 1.1.
 */
struct sdw_impl_def_intr_mask {
	struct sdw_portn_intr_mask *portn_mask[SDW_MAX_PORT_DIRECTIONS];
	u8 control_port_mask;
	u8 port0_mask;
};

/**
 * sdw_slave_bra_caps: Bulk Register Access (BRA) Capabilities of the Slave.
 *	This list all the fields listed in MIPI DisCo Spec.
 *
 * @max_bus_freq: Maximum bus frequency of this mode, in Hz
 * @min_bus_freq: Minimum bus frequency of this mode, in Hz when using
 *	min-max properties, all values in the defined range are allowed. Use
 *	the config list in the next field if only discrete values are
 *	supported.
 *
 * @num_bus_freq: Number of discrete bus frequency configurations. Use of
 *	max_ and min_ bus frequencies requires num_bus_freq to be zero.
 *
 * @bus_freq_buf: Array of bus frequency configs.
 * @max_data_per_frame: Maximum Data payload, in bytes per frame. Excludes
 *	header, CRC, footer. Maximum value defined by SoundWire protocol is
 *	470 bytes.
 *
 * @min_us_between_transactions: Amount of delay, in microseconds,
 *	required to be inserted between BRA transactions. Use if Slave needs
 *	idle time between BRA transactions.
 *
 * @max_bandwidth: Maximum bandwidth (in bytes/s) that can be written/read
 *	(header, CRCs, footer excluded).
 *
 * @mode_block_alignment: Size of basic block in bytes. The Data payload
 *	size shall be an integer multiple of this basic block size.
 *	padding/repeating of the same value is required for transactions
 *	smaller than this basic block size.
 */
struct sdw_slave_bra_caps {
	unsigned int max_bus_freq;
	unsigned int min_bus_freq;
	unsigned int num_bus_freq;
	unsigned int *bus_freq_buf;
	unsigned int max_data_per_frame;
	unsigned int min_us_between_transactions;
	unsigned int max_bandwidth;
	unsigned int mode_block_alignment;
};

/**
 * sdw_slave_dp0_caps: Capabilities of the Data Port 0 of Slave. All fields
 *	are listed in order of the SoundWire DisCo Spec. Name of the fields
 *	are altered to follow Linux convention. All the fields which are
 *	required by bus driver but not present in DisCo Spec are listed at
 *	the end of the structure.
 *
 * @max_bps: Maximum bits per sample supported by Port. This is same as word
 *	length in SoundWire Spec.
 *
 * @min_bps: Minimum bits per sample supported by Port. This is same as word
 *	length in SoundWire Spec.
 *
 * @num_bps: Array size of the buffer containing the supported bits per sample
 *	configuration. The use of max_ and min_ bps requires num_bps to be
 *	zero
 *
 * @bps_buf: Array containing supported sample sizes.
 * @bra_use_flow_control: Flow control is required or not for bra block
 *	transfer.
 *
 * @impl_def_response: If True (nonzero), implementation-defined response is
 *	supported. This information may be used by a device driver to
 *	request that a generic bus driver forwards the response to the
 *	client device driver.
 *
 * @bra_initiator: Slave BRA initiator role supported
 * @prepare_ch: Type of channel prepare scheme. Simplified or Normal channel
 *	prepare.
 *
 * @imp_def_intr_mask: Implementation defined interrupt mask for DP0 Port.
 *	These interrupts will be enabled by bus driver.
 *
 * @impl_def_bpt: If True (nonzero), implementation-defined. Payload Type is
 *	supported. This information is used to bypass the BRA protocol and
 *	may only be of interest when a device driver is aware of the
 *	Capabilities of the Master and Slave devices devices. This is not
 *	supported as of now.
 *
 * @bra_cap: BRA capabilities of the Slave. Currently only one mode is
 *	supported.
 */
struct sdw_slave_dp0_caps {
	unsigned int max_bps;
	unsigned int min_bps;
	unsigned int num_bps;
	unsigned int *bps_buf;
	bool bra_use_flow_control;
	bool impl_def_response;
	bool bra_initiator;
	enum sdw_ch_prepare_mode prepare_ch;
	unsigned int imp_def_intr_mask;
	bool impl_def_bpt;
	struct sdw_slave_bra_caps bra_cap;
};

/**
 * sdw_slave_caps: Capabilities of the SoundWire Slave. This is the
 *	structure for Slave driver to register its capabilities to bus
 *	driver. This structure is based on the MIPI DisCo Spec for
 *	registering Slave capabilities. It lists all the capabilities
 *	required by bus driver from DisCo Spec. It omits few of the
 *	capabilities from DisCo Spec, which bus driver doesn't require.
 *	Some of the DisCo Slave properties also provides information about
 *	Slave containing SoundWire controller and number of Master
 *	interfaces. This is not required by bus driver as Slave properties.
 *
 * @wake_up_unavailable: Slave is not capable of waking up the Master by
 *	driving the data line to High.
 *
 * @test_mode: Slave supports test modes.
 * @clk_stp1_mode: Clock stop 1 mode supported by this Slave. SoundWire
 *	specification requires all compliant Slaves to support Clock Stop
 *	mode 0.
 *
 * @simple_clk_stp_prep: Simplified clock stop prepare supported.
 * @clk_stp_prep_timeout: A slave-specific timeout in milliseconds. This
 *	value indicates the worst-case latency of the Clock Stop Prepare
 *	state machine transitions. After receiving a successful clock stop
 *	prepare/de-prepare command, the Slave should complete the operation
 *	before the expiration of the timeout. Software may poll for
 *	completion periodically or wait for this timeout value. If the
 *	requested action has not completed at the end of this timeout,
 *	software shall interpret this as an error condition.
 *
 * @clk_stp_prep_hard_reset_behavior: When set, the Slave keeps its prepare
 *	status after exit from clock stop mode1 and needs to be de-prepared
 *	by software. Otherwise, the slave does not need a de-prepare command
 *	upon resuming from clock stop mode1.
 *
 * @highphy_capable: Slave is highphy_capable or not? Currently highphy is
 *	not supported in bus driver.
 *
 * @paging: Paging registers supported for Slave?
 * @bank_delay_support: Bank switching delay for Slave
 * @port_15_read_behavior: Slave behavior when the Master attempts a Read to
 *	the Port15 alias
 *	0: Command_Ignored
 *	1: Command_OK, Data is OR of all registers
 *
 * @scp_impl_def_intr_mask: Implementation defined interrupt mask for Slave
 *	control port
 *
 * @lane_control_support: Lane control support for Slave
 * @dp0_present: DP0 is supported by Slave.
 * @dp0_caps: Data Port 0 Capabilities of the Slave.
 * @num_ports: Number of SoundWire Data ports present. The representation
 *	assumes contiguous Port numbers starting at 1.
 *
 * @dpn_caps: Capabilities of the SoundWire Slave ports. This includes
 *	both Source and Sink Ports.
 *
 * @num_src_ports: Number of Source ports present on Slave. If Slave has
 *	bidirectional ports, it is counted as both Source and Sink ports.
 *	E.g. if Slave has two bidirectional ports, num_src_ports in
 *	capabilities shall be 2. This is used by bus driver to get the port
 *	capabilities based on port configured as Source or Sink for the
 *	particular stream.
 *
 * @num_sink_ports: Number of Sink ports present on Slave. If Slave has
 *	bidirectional ports, it is counted as both Source and Sink ports.
 *	E.g. if Slave has two bidirectional ports, num_sink_ports in
 *	capabilities shall be 2. This is used by bus driver to get the port
 *	capabilities based on port configured as Source or Sink for the
 *	particular stream.
 *
 * @num_ports: Total number of ports on the Slave. E.g. If Slave has 2
 *	bidirectional ports and 2 source ports and 2 sink ports, total
 *	number of ports reported to bus driver shall be 6.
 */
struct sdw_slave_caps {
	bool wake_up_unavailable;
	bool test_mode;
	bool clk_stp1_mode;
	bool simple_clk_stp_prep;
	unsigned int clk_stp_prep_timeout;
	bool clk_stp_prep_hard_reset_behavior;
	bool highphy_capable;
	bool paging;
	bool bank_delay_support;
	unsigned int port_15_read_behavior;
	u8 scp_impl_def_intr_mask;
	bool lane_control_support;
	bool dp0_present;
	struct sdw_slave_dp0_caps *dp0_caps;
	unsigned int num_src_ports;
	unsigned int num_sink_ports;
	struct sdw_dpn_caps *dpn_caps[SDW_MAX_PORT_DIRECTIONS];
	unsigned int num_ports;
};

/**
 * sdw_portn_intr_stat: Implementation defined interrupt status for Slave
 *	Ports other than Data Port 0
 *
 * @num: Port number for which status is reported.
 * @status: status of the implementation defined interrupts.
 */
struct sdw_portn_intr_stat {
	unsigned int num;
	u8 status;
};

/**
 * sdw_impl_def_intr_stat: Implementation defined interrupt status for
 *	Slave.
 *
 * @num_ports: Number of ports in Slave other than Data Port 0.
 * @portn_stat: Implementation defined status for Slave ports other than
 *	port0. Mask bits are exactly same as defined in MIPI Spec 1.1. Array
 *	size is same as number of ports in Slave.
 *
 * @control_port_stat: Implementation defined interrupt status mask for
 *	control port. Mask Bits are exactly same as defined in MIPI Spec
 *	1.1.
 *
 * @port0_stat: Implementation defined interrupt status mask for Data
 *	Port 0. Mask bits are exactly same as defined in MIPI Spec 1.1.
 */
struct sdw_impl_def_intr_stat {
	unsigned int num_ports;
	struct sdw_portn_intr_stat *portn_stat;
	u8 control_port_stat;
	u8 port0_stat;
};

/**
 * sdw_slave_priv: Slave device private structure. This is used by bus
 *	driver and need not to be used by Slave driver.
 *
 * @name: Name of the driver to use with the device.
 * @addr: Slave logical and dev_id address structures. This is non-null
 *	for all the Slave which are enumerated.
 *
 * @driver: Slave's driver, pointer to access routine.
 * @node: Node to add the Slave to the list of Slave devices physically
 *	connected to and managed by same Master.
 *
 * @port_ready: Port ready completion flag for each Port of the Slave. This
 *	field is not used for simplified channel prepare.
 *
 * @caps: Slave capabilities.
 * @slave_cap_updated: Did Slave device driver updated Slave capabilities to
 *	bus. This is used by bus driver for Slave configurations like
 *	ClockStopMode etc.
 *
 * @dev_id: 6-byte unique device identification.
 */
struct sdw_slave_priv {
	char name[SOUNDWIRE_NAME_SIZE];
	struct sdw_slave_addr *addr;
	struct sdw_slave_driver *driver;
	struct list_head node;
	struct completion *port_ready;
	struct sdw_slave_caps caps;
	bool slave_cap_updated;
	unsigned int dev_id[SDW_NUM_DEV_ID_REGISTERS];
};

/**
 * sdw_slave: Represents SoundWire Slave device (similar to 'i2c_client' on
 *	I2C).
 *
 * @dev: Driver model representation of the device.
 * @mstr: SoundWire Master, instance physically connected to Slave.
 * @link_id: SoundWire Master link_id to which this Slave is connected. This
 *	is required by Slave driver to get the ACPI or any platform specific
 *	configuration based on dev_id and link_id to which Slave is
 *	connected.
 *
 * @dev_num: DeviceNumber of the Slave, assigned by bus driver.
 * @priv: Bus driver private data structure, Slave device driver should not
 *	use it.
 */
struct sdw_slave {
	struct device dev;
	struct sdw_master *mstr;
	unsigned int link_id;
	unsigned int dev_num;
	struct sdw_slave_priv priv;
};
#define to_sdw_slave(d) container_of(d, struct sdw_slave, dev)

/**
 * sdw_slave_driver: Manage SoundWire Slave device driver.
 *
 * @driver_type: To distinguish between Master and Slave driver. Driver
 *	should set this based on its controlling Master or Slave device.
 *
 * @probe: Binds this driver to a SoundWire Slave device (in Linux Device
 *	model sense).
 *
 * @remove: Unbinds this driver from the SoundWire Slave.
 * @shutdown: Standard shutdown callback used during power down/halt.
 * @suspend: Standard suspend callback used during system suspend.
 * @resume: Standard resume callback used during system resume.
 * @driver: Generic driver structure, according to driver model.
 * @slave_irq: When interrupts are enabled, the Master driver is notified of
 *	alert conditions through the PREQ mechanism and fetches the Slave
 *	status. Bus driver handles all interrupts specified in the
 *	SoundWire specification, such as bus clash, parity or Port test
 *	fail. Slave driver handles all implementation-defined interrupts,
 *	such as jack detect and pll-locked. If the slave_irq callback is
 *	defined, the bus driver invokes it and lets the Slave driver handle
 *	the status. Bus driver ACKs all interrupts including implementation
 *	defined interrupts once Slave handles it. This is mandatory if Slave
 *	is supporting implementation defined interrupts.
 *
 * @pre_bus_config: Slave callback function to let Slave configure
 *	implementation defined registers prior to any bus configuration
 *	changes. Bus configuration changes will be signaled by a bank switch
 *	initiated by the bus driver once all Slaves drivers have performed
 *	their imp-def configuration sequence (if any). If this callback is
 *	not implemented the bus driver will assume the Slave can tolerate
 *	bus configurations changes at any time. This is optional based on
 *	Slave implementation.
 *
 * @port_prep: Slave driver callback to allow Slave Port to be
 *	prepared/de-prepared by configuring impl defined register as part of
 *	Port prepare state machine. This fn is called before and after
 *	DPn_Prepare ctrl is written based on "sdw_port_prep_ops" ops. Post
 *	prepare is called after Port is prepared/de-prepared.
 *
 * @status_change_event: Slave device status change event to Slave driver.
 *	Bus driver calls this callback to Slave driver every time there is
 *	status change of the Slave. Slave device use this to make sure that
 *	its in attached state after resuming, before doing any register
 *	access. This is mandatory callback to be provided by Slave to update
 *	the status change events.
 *
 * @pre_clk_stop_prep: Common Slave driver callback to prepare for clock
 *	stop and deprepare after clock is resumed - depending upon flag
 *	value.  This is called before prepare bit is set for clock stop,
 *	while in resume case its called before prepare bit is reset. This is
 *	optional based on Slave implementation.
 *
 * @post_clk_stop_prep: Common Slave driver call back for Slave operations
 *	to be done after clock stop prepare is done for clock stop and after
 *	clock stop de-prepare is done after clock resume. This is called
 *	after prepare is done for clock stop and deprepare is done for clock
 *	resume. This is optional based on Slave implementation.
 *
 * @get_dyn_clk_stp_mod: Get the clock stop mode from Slave dynamically
 *	before preparing Slave for prepare. Slave registers the ClockStop
 *	capability as part of registering Slave capability. This API
 *	provides Slave with dynamically updating the ClockStop mode based on
 *	use case. If this is not defined by Slave, bus driver will use
 *	static capability for ClockStop Mode registered to bus driver as
 *	part of Slave capabilities. This is optional based on Slave
 *	implementation.
 *
 * @id_table: List of SoundWire Slaves supported by this driver. Multiple
 *	Slaves from the same vendor may be handled by the same Slave driver
 *	as long as hardware differences are handled within this Slave driver
 *	(same as for I2S codecs).
 */
struct sdw_slave_driver {
	enum sdw_driver_type driver_type;
	struct device_driver driver;
	int (*probe)(struct sdw_slave *slave, const struct sdw_slave_id *);
	int (*remove)(struct sdw_slave *slave);
	void (*shutdown)(struct sdw_slave *slave);
	int (*suspend)(struct sdw_slave *slave);
	int (*resume)(struct sdw_slave *slave);
	int (*slave_irq)(struct sdw_slave *slave,
		struct sdw_impl_def_intr_stat *intr_stat);
	int (*pre_bus_config)(struct sdw_slave *slave,
			struct sdw_bus_params *params);
	int (*port_prep)(struct sdw_slave *slave,
			struct sdw_prepare_ch *prepare_ch,
			enum sdw_port_prep_ops pre_ops);
	int (*status_change_event)(struct sdw_slave *slave,
			enum sdw_slave_status status);
	int (*pre_clk_stop_prep)(struct sdw_slave *slave,
			enum sdw_clk_stop_mode mode, bool prep);
	int (*post_clk_stop_prep)(struct sdw_slave *slave,
			enum sdw_clk_stop_mode mode, bool prep);
	enum sdw_clk_stop_mode (*get_dyn_clk_stp_mod)(struct sdw_slave *slave);
	const struct sdw_slave_id *id_table;
};
#define to_sdw_slave_driver(d) container_of(d, struct sdw_slave_driver, driver)

/**
 * snd_sdw_slave_driver_register: SoundWire Slave driver registration with
 *	bus. This API will register the Slave driver with the SoundWire bus.
 *	It is typically called from the driver's module-init function.
 *
 * @driver: Driver to be associated with Slave.
 * @owner: Module owner, generally THIS module.
 */
int snd_sdw_slave_driver_register(struct sdw_slave_driver *driver,
					struct module *owner);

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
			struct sdw_slave_caps *cap);

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
		struct sdw_impl_def_intr_mask *intr_mask);

/**
 * sdw_slave_unregister_driver: Undo effects of sdw_slave_driver_register.
 *
 * @drv: SoundWire Slave driver to be unregistered.
 */
static inline void sdw_slave_unregister_driver(struct sdw_slave_driver *drv)
{
	driver_unregister(&drv->driver);
}

static inline struct sdw_master *sdw_slave_to_master(struct sdw_slave *slv)
{
	return slv->mstr;
}

static inline void *sdw_slave_get_drvdata(const struct sdw_slave *slv)
{
	return dev_get_drvdata(&slv->dev);
}

static inline void sdw_slave_set_drvdata(struct sdw_slave *slv, void *data)
{
	dev_set_drvdata(&slv->dev, data);
}

#endif /*  _LINUX_SDW_SLAVE_H */
