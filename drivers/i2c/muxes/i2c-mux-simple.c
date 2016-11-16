/*
 * Generic simple I2C multiplexer
 *
 * Peter Rosin <peda@axentia.se>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/i2c.h>
#include <linux/i2c-mux.h>
#include <linux/module.h>
#include <linux/mux.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

struct mux {
	struct mux_control *control;

	int parent;
	int n_values;
	u32 *values;
};

static int i2c_mux_select(struct i2c_mux_core *muxc, u32 chan)
{
	struct mux *mux = i2c_mux_priv(muxc);

	return mux_control_select(mux->control, chan);
}

static int i2c_mux_deselect(struct i2c_mux_core *muxc, u32 chan)
{
	struct mux *mux = i2c_mux_priv(muxc);

	return mux_control_deselect(mux->control);
}

static int i2c_mux_probe_dt(struct mux *mux,
			    struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct device_node *adapter_np, *child;
	struct i2c_adapter *adapter;
	int i = 0;

	if (!np)
		return -ENODEV;

	adapter_np = of_parse_phandle(np, "i2c-parent", 0);
	if (!adapter_np) {
		dev_err(dev, "Cannot parse i2c-parent\n");
		return -ENODEV;
	}
	adapter = of_find_i2c_adapter_by_node(adapter_np);
	of_node_put(adapter_np);
	if (!adapter)
		return -EPROBE_DEFER;

	mux->parent = i2c_adapter_id(adapter);
	put_device(&adapter->dev);

	mux->control = devm_mux_control_get(dev, "mux");
	if (IS_ERR(mux->control)) {
		if (PTR_ERR(mux->control) != -EPROBE_DEFER)
			dev_err(dev, "failed to get control-mux\n");
		return PTR_ERR(mux->control);
	}

	mux->n_values = of_get_child_count(np);

	mux->values = devm_kzalloc(dev,
				   sizeof(*mux->values) * mux->n_values,
				   GFP_KERNEL);
	if (!mux->values)
		return -ENOMEM;

	for_each_child_of_node(np, child) {
		of_property_read_u32(child, "reg", mux->values + i);
		i++;
	}

	return 0;
}

static const struct of_device_id i2c_mux_of_match[] = {
	{ .compatible = "i2c-mux-simple,parent-locked",
	  .data = (void *)0, },
	{ .compatible = "i2c-mux-simple,mux-locked",
	  .data = (void *)1, },
	{},
};
MODULE_DEVICE_TABLE(of, i2c_mux_of_match);

static int i2c_mux_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct of_device_id *match;
	struct i2c_mux_core *muxc;
	struct mux *mux;
	struct i2c_adapter *parent;
	int i, ret;

	mux = devm_kzalloc(dev, sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return -ENOMEM;

	ret = i2c_mux_probe_dt(mux, dev);
	if (ret < 0)
		return ret;

	parent = i2c_get_adapter(mux->parent);
	if (!parent)
		return -EPROBE_DEFER;

	muxc = i2c_mux_alloc(parent, dev, mux->n_values, 0, 0,
			     i2c_mux_select, i2c_mux_deselect);
	if (!muxc) {
		ret = -ENOMEM;
		goto alloc_failed;
	}
	muxc->priv = mux;

	platform_set_drvdata(pdev, muxc);

	match = of_match_device(of_match_ptr(i2c_mux_of_match), dev);
	if (match)
		muxc->mux_locked = !!of_device_get_match_data(dev);

	for (i = 0; i < mux->n_values; i++) {
		ret = i2c_mux_add_adapter(muxc, 0, mux->values[i], 0);
		if (ret)
			goto add_adapter_failed;
	}

	dev_info(dev, "%d port mux on %s adapter\n",
		 mux->n_values, parent->name);

	return 0;

add_adapter_failed:
	i2c_mux_del_adapters(muxc);
alloc_failed:
	i2c_put_adapter(parent);

	return ret;
}

static int i2c_mux_remove(struct platform_device *pdev)
{
	struct i2c_mux_core *muxc = platform_get_drvdata(pdev);

	i2c_mux_del_adapters(muxc);
	i2c_put_adapter(muxc->parent);

	return 0;
}

static struct platform_driver i2c_mux_driver = {
	.probe	= i2c_mux_probe,
	.remove	= i2c_mux_remove,
	.driver	= {
		.name	= "i2c-mux-simple",
		.of_match_table = i2c_mux_of_match,
	},
};
module_platform_driver(i2c_mux_driver);

MODULE_DESCRIPTION("Simple I2C multiplexer driver");
MODULE_AUTHOR("Peter Rosin <peda@axentia.se>");
MODULE_LICENSE("GPL v2");
