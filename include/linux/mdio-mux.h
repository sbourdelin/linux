/*
 * MDIO bus multiplexer framwork.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2011, 2012 Cavium, Inc.
 */
#ifndef __LINUX_MDIO_MUX_H
#define __LINUX_MDIO_MUX_H
#include <linux/device.h>
#include <linux/phy.h>

/* mdio_mux_init() - Initialize a MDIO mux
 * @dev		The device owning the MDIO mux
 * @mux_node	The device node of the MDIO mux
 * @switch_fn	The function called for switching target MDIO child
 * mux_handle	A pointer to a (void *) used internaly by mdio-mux
 * @data	Private data used by switch_fn()
 * @mux_bus	An optional parent bus (Other case are to use parent_bus property)
 */
int mdio_mux_init(struct device *dev,
		  struct device_node *mux_node,
		  int (*switch_fn) (int cur, int desired, void *data),
		  void **mux_handle,
		  void *data,
		  struct mii_bus *mux_bus);

void mdio_mux_uninit(void *mux_handle);

#ifdef CONFIG_MDIO_BUS_MUX_REGMAP
/**
 * mdio_mux_regmap_init - control MDIO bus muxing using regmap constructs.
 * @dev: device with which regmap construct is associated.
 * @mux_node: mdio bus mux node that contains parent mdio bus phandle.
 *	      This node also contains sub nodes, where each subnode denotes
 *	      a child mdio bus. All the child mdio buses are muxed, i.e. at a
 *	      time only one of the child mdio buses can be used.
 * @data: to store the address of data allocated by this function
 */
int mdio_mux_regmap_init(struct device *dev,
			 struct device_node *mux_node,
			 void **data);

/**
 * mdio_mux_regmap_uninit - relinquish the control of MDIO bus muxing using
 *			    regmap constructs.
 * @data: address of data allocated by mdio_mux_regmap_init
 */
int mdio_mux_regmap_uninit(void *data);
#else /* CONFIG_MDIO_BUS_MUX_REGMAP */
static inline int mdio_mux_regmap_init(struct device *dev,
				       struct device_node *mux_node,
				       void **data)
{
	return -ENODEV;
}

static inline int mdio_mux_regmap_uninit(void *data)
{
	return 0;
}
#endif /* CONFIG_MDIO_BUS_MUX_REGMAP */

#endif /* __LINUX_MDIO_MUX_H */
