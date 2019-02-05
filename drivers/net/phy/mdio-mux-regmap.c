// SPDX-License-Identifier: GPL-2.0+

/* Simple regmap based MDIO MUX driver
 *
 * Copyright 2018-2019 NXP
 *
 * Based on mdio-mux-mmioreg.c by Timur Tabi
 *
 * Author:
 *     Pankaj Bansal <pankaj.bansal@nxp.com>
 */

#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/of_mdio.h>
#include <linux/module.h>
#include <linux/phy.h>
#include <linux/mdio-mux.h>
#include <linux/regmap.h>

struct mdio_mux_regmap_state {
	void		*mux_handle;
	struct device	*dev;
	struct regmap	*regmap;
	u32		mux_reg;
	u32		mask;
};

/**
 * mdio_mux_regmap_switch_fn - This function is called by the mdio-mux layer
 *			       when it thinks the mdio bus multiplexer needs
 *			       to switch.
 * @current_child:  current value of the mux register (masked via s->mask).
 * @desired_child: value of the 'reg' property of the target child MDIO node.
 * @data: Private data used by this switch_fn passed to mdio_mux_init function
 *	  via mdio_mux_init(.., .., .., .., data, ..).
 *
 * The first time this function is called, current_child == -1.
 * If current_child == desired_child, then the mux is already set to the
 * correct bus.
 */
static int mdio_mux_regmap_switch_fn(int current_child, int desired_child,
				     void *data)
{
	struct mdio_mux_regmap_state *s = data;
	bool change;
	int ret;

	ret = regmap_update_bits_check(s->regmap,
				       s->mux_reg,
				       s->mask,
				       desired_child,
				       &change);

	if (ret)
		return ret;
	if (change)
		dev_dbg(s->dev, "%s %d -> %d\n", __func__, current_child,
			desired_child);
	return ret;
}

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
			 void **data)
{
	struct device_node *child;
	struct mdio_mux_regmap_state *s;
	int ret;
	u32 val;

	dev_dbg(dev, "probing node %pOF\n", mux_node);

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	s->regmap = dev_get_regmap(dev, NULL);
	if (IS_ERR(s->regmap)) {
		dev_err(dev, "Failed to get parent regmap\n");
		return PTR_ERR(s->regmap);
	}

	ret = of_property_read_u32(mux_node, "reg", &s->mux_reg);
	if (ret) {
		dev_err(dev, "missing or invalid reg property\n");
		return -ENODEV;
	}

	/* Test Register read write */
	ret = regmap_read(s->regmap, s->mux_reg, &val);
	if (ret) {
		dev_err(dev, "error while reading reg\n");
		return ret;
	}

	ret = regmap_write(s->regmap, s->mux_reg, val);
	if (ret) {
		dev_err(dev, "error while writing reg\n");
		return ret;
	}

	ret = of_property_read_u32(mux_node, "mux-mask", &s->mask);
	if (ret) {
		dev_err(dev, "missing or invalid mux-mask property\n");
		return -ENODEV;
	}

	/* Verify that the 'reg' property of each child MDIO bus does not
	 * set any bits outside of the 'mask'.
	 */
	for_each_available_child_of_node(mux_node, child) {
		ret = of_property_read_u32(child, "reg", &val);
		if (ret) {
			dev_err(dev, "%pOF is missing a 'reg' property\n",
				child);
			of_node_put(child);
			return -ENODEV;
		}
		if (val & ~s->mask) {
			dev_err(dev,
				"%pOF has a 'reg' value with unmasked bits\n",
				child);
			of_node_put(child);
			return -ENODEV;
		}
	}

	ret = mdio_mux_init(dev, mux_node, mdio_mux_regmap_switch_fn,
			    &s->mux_handle, s, NULL);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to register mdio-mux bus %pOF\n",
				mux_node);
		return ret;
	}

	*data = s;

	return 0;
}
EXPORT_SYMBOL_GPL(mdio_mux_regmap_init);

/**
 * mdio_mux_regmap_uninit - relinquish the control of MDIO bus muxing using
 *			    regmap constructs.
 * @data: address of data allocated by mdio_mux_regmap_init
 */
void mdio_mux_regmap_uninit(void *data)
{
	struct mdio_mux_regmap_state *s = data;

	mdio_mux_uninit(s->mux_handle);
}
EXPORT_SYMBOL_GPL(mdio_mux_regmap_uninit);

MODULE_AUTHOR("Pankaj Bansal <pankaj.bansal@nxp.com>");
MODULE_DESCRIPTION("regmap based MDIO MUX driver");
MODULE_LICENSE("GPL");

