/*
 * MMIO register bitfield-controlled multiplexer driver
 *
 * Copyright (C) 2017 Pengutronix, Philipp Zabel <kernel@pengutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/err.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mux/driver.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

static int mux_mmio_set(struct mux_control *mux, int state)
{
	struct regmap_field **fields = mux_chip_priv(mux->chip);
	int index = mux - mux->chip->mux;

	return regmap_field_write(fields[index], state);
}

static const struct mux_control_ops mux_mmio_ops = {
	.set = mux_mmio_set,
};

static const struct of_device_id mux_mmio_dt_ids[] = {
	{ .compatible = "mmio-mux", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mux_mmio_dt_ids);

static int mux_mmio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct regmap_field **fields;
	struct mux_chip *mux_chip;
	struct regmap *regmap;
	s32 *idle_states;
	u32 *reg_masks;
	int num_fields;
	int ret;
	int i;

	regmap = syscon_node_to_regmap(np->parent);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(dev, "failed to get syscon regmap: %d\n", ret);
		return ret;
	}

	ret = of_property_count_u32_elems(np, "mux-reg-masks");
	if (ret == 0 || ret % 2)
		ret = -EINVAL;
	if (ret > 0) {
		num_fields = ret / 2;

		reg_masks = devm_kmalloc(dev, ret * sizeof(u32), GFP_KERNEL);
		if (!reg_masks)
			return -ENOMEM;

		ret = of_property_read_u32_array(np, "mux-reg-masks",
						 reg_masks, ret);
	}
	if (ret < 0) {
		dev_err(dev, "mux-reg-masks property missing or invalid: %d\n",
			ret);
		return ret;
	}

	mux_chip = devm_mux_chip_alloc(dev, num_fields, num_fields *
				       sizeof(*fields));
	if (IS_ERR(mux_chip))
		return PTR_ERR(mux_chip);

	fields = mux_chip_priv(mux_chip);

	for (i = 0; i < num_fields; i++) {
		struct mux_control *mux = &mux_chip->mux[i];
		struct reg_field field;
		u32 *reg_mask = reg_masks + 2 * i;
		int bits;

		field.reg = reg_mask[0];
		field.msb = fls(reg_mask[1]) - 1;
		field.lsb = ffs(reg_mask[1]) - 1;
		bits = 1 + field.msb - field.lsb;

		fields[i] = devm_regmap_field_alloc(&pdev->dev, regmap, field);
		if (IS_ERR(fields[i])) {
			ret = PTR_ERR(fields[i]);
			dev_err(&pdev->dev, "failed to get bit-field %d: %d\n",
				i, ret);
			return ret;
		}

		mux->states = 1 << bits;
	}

	devm_kfree(dev, reg_masks);

	if (of_find_property(np, "idle-states", NULL)) {
		ret = of_property_count_u32_elems(np, "idle-states");
		if (ret == num_fields) {
			idle_states = devm_kmalloc(dev, ret * sizeof(s32),
						   GFP_KERNEL);
			if (!idle_states)
				return -ENOMEM;

			ret = of_property_read_u32_array(np, "idle-states",
							 idle_states, ret);
		} else {
			idle_states = NULL;
			if (ret >= 0)
				ret = -EINVAL;
		}
		if (ret < 0) {
			dev_err(dev, "idle-states property invalid: %d\n", ret);
			return ret;
		}

		for (i = 0; i < num_fields; i++) {
			struct mux_control *mux = &mux_chip->mux[i];

			if (idle_states[i] != MUX_IDLE_AS_IS) {
				if (idle_states[i] < 0 ||
				    idle_states[i] >= mux->states) {
					dev_err(dev, "invalid idle-state %u\n",
						idle_states[i]);
					return -EINVAL;
				}

				mux->idle_state = idle_states[i];
			}
		}

		devm_kfree(dev, idle_states);
	}

	mux_chip->ops = &mux_mmio_ops;

	return devm_mux_chip_register(dev, mux_chip);
}

static struct platform_driver mux_mmio_driver = {
	.driver = {
		.name = "mmio-mux",
		.of_match_table	= of_match_ptr(mux_mmio_dt_ids),
	},
	.probe = mux_mmio_probe,
};
module_platform_driver(mux_mmio_driver);

MODULE_DESCRIPTION("MMIO register bitfield-controlled multiplexer driver");
MODULE_AUTHOR("Philipp Zabel <p.zabel@pengutronix.de>");
MODULE_LICENSE("GPL v2");
