/*
 * SYSCON regmap reset driver
 *
 * Copyright (C) 2015-2016 Texas Instruments Incorporated - http://www.ti.com/
 *	Andrew F. Davis <afd@ti.com>
 *	Suman Anna <afd@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/idr.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>

/**
 * struct syscon_reset_control - reset control structure
 * @offset: reset control register offset from syscon base
 * @reset_bit: reset bit in the reset control register
 * @assert_high: flag to indicate if setting the bit high asserts the reset
 * @status_offset: reset status register offset from syscon base
 * @status_reset_bit: reset status bit in the reset status register
 * @status_assert_high: flag to indicate if a set bit represents asserted state
 * @toggle: flag to indicate this reset has no readable status register
 */
struct syscon_reset_control {
	unsigned int offset;
	unsigned int reset_bit;
	bool assert_high;
	unsigned int status_offset;
	unsigned int status_reset_bit;
	bool status_assert_high;
	bool toggle;
};

/**
 * struct syscon_reset_data - reset controller information structure
 * @rcdev: reset controller entity
 * @dev: reset controller device pointer
 * @regmap: regmap handle containing the memory-mapped reset registers
 * @idr: idr structure for mapping ids to reset control structures
 */
struct syscon_reset_data {
	struct reset_controller_dev rcdev;
	struct device *dev;
	struct regmap *regmap;
	struct idr idr;
};

#define to_syscon_reset_data(rcdev)	\
	container_of(rcdev, struct syscon_reset_data, rcdev)

/**
 * syscon_reset_set() - program a device's reset
 * @rcdev: reset controller entity
 * @id: ID of the reset to toggle
 * @assert: boolean flag to indicate assert or deassert
 *
 * This is a common internal function used to assert or deassert a device's
 * reset using the regmap API. The device's reset is asserted if the @assert
 * argument is true, or deasserted if the @assert argument is false.
 *
 * Return: 0 for successful request, else a corresponding error value
 */
static int syscon_reset_set(struct reset_controller_dev *rcdev,
			    unsigned long id, bool assert)
{
	struct syscon_reset_data *data = to_syscon_reset_data(rcdev);
	struct syscon_reset_control *control;
	unsigned int mask, value;

	control = idr_find(&data->idr, id);
	if (!control)
		return -EINVAL;

	mask = BIT(control->reset_bit);
	value = (assert == control->assert_high) ? mask : 0x0;

	return regmap_update_bits(data->regmap, control->offset, mask, value);
}

/**
 * syscon_reset_assert() - assert device reset
 * @rcdev: reset controller entity
 * @id: ID of the reset to be asserted
 *
 * This function implements the reset driver op to assert a device's reset.
 * This invokes the function syscon_reset_set() with the corresponding
 * parameters as passed in, but with the @assert argument set to true for
 * asserting the reset.
 *
 * Return: 0 for successful request, else a corresponding error value
 */
static int syscon_reset_assert(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	return syscon_reset_set(rcdev, id, true);
}

/**
 * syscon_reset_deassert() - deassert device reset
 * @rcdev: reset controller entity
 * @id: ID of reset to be deasserted
 *
 * This function implements the reset driver op to deassert a device's reset.
 * This invokes the function syscon_reset_set() with the corresponding
 * parameters as passed in, but with the @assert argument set to false for
 * deasserting the reset.
 *
 * Return: 0 for successful request, else a corresponding error value
 */
static int syscon_reset_deassert(struct reset_controller_dev *rcdev,
				 unsigned long id)
{
	return syscon_reset_set(rcdev, id, false);
}

/**
 * syscon_reset_status() - check device reset status
 * @rcdev: reset controller entity
 * @id: ID of the reset for which the status is being requested
 *
 * This function implements the reset driver op to return the status of a
 * device's reset.
 *
 * Return: 0 if reset is deasserted, true if reset is asserted, else a
 * corresponding error value
 */
static int syscon_reset_status(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	struct syscon_reset_data *data = to_syscon_reset_data(rcdev);
	struct syscon_reset_control *control;
	unsigned int reset_state;
	int ret;

	control = idr_find(&data->idr, id);
	if (!control)
		return -EINVAL;

	if (control->toggle)
		return -ENOSYS; /* status not supported for this reset */

	ret = regmap_read(data->regmap, control->status_offset, &reset_state);
	if (ret)
		return ret;

	return (reset_state & BIT(control->status_reset_bit)) ==
			control->status_assert_high;
}

static struct reset_control_ops syscon_reset_ops = {
	.assert		= syscon_reset_assert,
	.deassert	= syscon_reset_deassert,
	.status		= syscon_reset_status,
};

static int syscon_reset_of_xlate(struct reset_controller_dev *rcdev,
			  const struct of_phandle_args *reset_spec)
{
	struct syscon_reset_data *data = to_syscon_reset_data(rcdev);
	struct syscon_reset_control *control;
	phandle phandle = reset_spec->args[0];
	struct device_node *node;
	const __be32 *list;
	int size;

	control = devm_kzalloc(data->dev, sizeof(*control), GFP_KERNEL);
	if (!control)
		return -ENOMEM;

	node = of_find_node_by_phandle(phandle);
	if (!node) {
		pr_err("could not find reset node by phandle %#x\n", phandle);
		devm_kfree(data->dev, control);
		return -ENOENT;
	}

	list = of_get_property(node, "reset-control", &size);
	if (!list || size != (3 * sizeof(*list))) {
		of_node_put(node);
		devm_kfree(data->dev, control);
		return -EINVAL;
	}
	control->offset = be32_to_cpup(list++);
	control->reset_bit = be32_to_cpup(list++);
	control->assert_high = be32_to_cpup(list) == 1;

	if (of_find_property(node, "reset-toggle", NULL)) {
		control->toggle = true;
		goto done;
	}
	control->toggle = false;

	list = of_get_property(node, "reset-status", &size);
	if (!list) {
		/* use control register values */
		control->status_offset = control->offset;
		control->status_reset_bit = control->reset_bit;
		control->status_assert_high = control->assert_high;
		goto done;
	}
	if (size != (3 * sizeof(*list))) {
		of_node_put(node);
		devm_kfree(data->dev, control);
		return -EINVAL;
	}
	control->status_offset = be32_to_cpup(list++);
	control->status_reset_bit = be32_to_cpup(list++);
	control->status_assert_high = be32_to_cpup(list) == 1;

done:
	of_node_put(node);
	return idr_alloc(&data->idr, control, 0, 0, GFP_KERNEL);
}

static int syscon_reset_probe(struct platform_device *pdev)
{
	struct syscon_reset_data *data;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct regmap *regmap;

	if (!np)
		return -ENODEV;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	regmap = syscon_node_to_regmap(np->parent);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	data->rcdev.ops = &syscon_reset_ops;
	data->rcdev.owner = THIS_MODULE;
	data->rcdev.of_node = np;
	data->rcdev.of_reset_n_cells = 1;
	data->rcdev.of_xlate = syscon_reset_of_xlate;
	data->dev = dev;
	data->regmap = regmap;
	idr_init(&data->idr);

	platform_set_drvdata(pdev, data);

	return reset_controller_register(&data->rcdev);
}

static int syscon_reset_remove(struct platform_device *pdev)
{
	struct syscon_reset_data *data = platform_get_drvdata(pdev);

	reset_controller_unregister(&data->rcdev);

	idr_destroy(&data->idr);

	return 0;
}

static const struct of_device_id syscon_reset_of_match[] = {
	{ .compatible = "syscon-reset", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, syscon_reset_of_match);

static struct platform_driver syscon_reset_driver = {
	.probe = syscon_reset_probe,
	.remove = syscon_reset_remove,
	.driver = {
		.name = "syscon-reset",
		.of_match_table = syscon_reset_of_match,
	},
};
module_platform_driver(syscon_reset_driver);

MODULE_AUTHOR("Andrew F. Davis <afd@ti.com>");
MODULE_AUTHOR("Suman Anna <s-anna@ti.com>");
MODULE_DESCRIPTION("SYSCON Regmap Reset Driver");
MODULE_LICENSE("GPL v2");
