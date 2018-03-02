// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 The Linux Foundation. All rights reserved.
 */

#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/regmap.h>
#include <linux/of_device.h>
#include <dt-bindings/reset/qcom,aoss-sdm845.h>

struct qcom_aoss_reset_map {
	unsigned int reg;
	u8 bit;
};

struct qcom_aoss_desc {
	const struct regmap_config *config;
	const struct qcom_aoss_reset_map *resets;
	int delay;
	size_t num_resets;
};

struct qcom_aoss_reset_data {
	struct reset_controller_dev rcdev;
	struct regmap *regmap;
	const struct qcom_aoss_desc *desc;
};

static const struct regmap_config aoss_sdm845_regmap_config = {
	.name		= "aoss-reset",
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= 0x20000,
	.fast_io	= true,
};

static const struct qcom_aoss_reset_map aoss_sdm845_resets[] = {
	[AOSS_CC_MSS_RESTART] = { 0x0, 0 },
	[AOSS_CC_CAMSS_RESTART] = { 0x1000, 0 },
	[AOSS_CC_VENUS_RESTART] = { 0x2000, 0 },
	[AOSS_CC_GPU_RESTART] = { 0x3000, 0 },
	[AOSS_CC_DISPSS_RESTART] = { 0x4000, 0 },
	[AOSS_CC_WCSS_RESTART] = { 0x10000, 0 },
	[AOSS_CC_LPASS_RESTART] = { 0x20000, 0 },
};

static const struct qcom_aoss_desc aoss_sdm845_desc = {
	.config = &aoss_sdm845_regmap_config,
	.resets = aoss_sdm845_resets,
	/* Wait 6 32kHz sleep cycles for reset */
	.delay = 200,
	.num_resets = ARRAY_SIZE(aoss_sdm845_resets),
};

static struct qcom_aoss_reset_data *to_qcom_aoss_reset_data(
				struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct qcom_aoss_reset_data, rcdev);
}

static int qcom_aoss_control_assert(struct reset_controller_dev *rcdev,
					unsigned long idx)
{
	struct qcom_aoss_reset_data *data = to_qcom_aoss_reset_data(rcdev);
	const struct qcom_aoss_reset_map *map = &data->desc->resets[idx];

	if (idx >= rcdev->nr_resets)
		return -EINVAL;

	return regmap_update_bits(data->regmap, map->reg,
					BIT(map->bit), BIT(map->bit));
}

static int qcom_aoss_control_deassert(struct reset_controller_dev *rcdev,
					unsigned long idx)
{
	struct qcom_aoss_reset_data *data = to_qcom_aoss_reset_data(rcdev);
	const struct qcom_aoss_reset_map *map = &data->desc->resets[idx];

	if (idx >= rcdev->nr_resets)
		return -EINVAL;

	return regmap_update_bits(data->regmap, map->reg, BIT(map->bit), 0);
}

static int qcom_aoss_control_reset(struct reset_controller_dev *rcdev,
					unsigned long idx)
{
	struct qcom_aoss_reset_data *data = to_qcom_aoss_reset_data(rcdev);
	int ret;

	ret = rcdev->ops->assert(rcdev, idx);
	if (ret)
		return ret;

	udelay(data->desc->delay);

	ret = rcdev->ops->deassert(rcdev, idx);
	if (ret)
		return ret;

	return 0;
}

static const struct reset_control_ops qcom_aoss_reset_ops = {
	.reset = qcom_aoss_control_reset,
	.assert = qcom_aoss_control_assert,
	.deassert = qcom_aoss_control_deassert,
};

static int qcom_aoss_reset_probe(struct platform_device *pdev)
{
	struct qcom_aoss_reset_data *data;
	struct device *dev = &pdev->dev;
	const struct qcom_aoss_desc *desc;

	desc = of_device_get_match_data(&pdev->dev);
	if (!desc)
		return -EINVAL;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->desc = desc;
	data->regmap = syscon_node_to_regmap(dev->of_node);
	if (IS_ERR(data->regmap)) {
		dev_err(dev, "Unable to get aoss-reset regmap");
		return PTR_ERR(data->regmap);
	}
	regmap_attach_dev(dev, data->regmap, desc->config);

	data->rcdev.owner = THIS_MODULE;
	data->rcdev.ops = &qcom_aoss_reset_ops;
	data->rcdev.nr_resets = desc->num_resets;
	data->rcdev.of_node = pdev->dev.of_node;

	return devm_reset_controller_register(&pdev->dev, &data->rcdev);
}

static const struct of_device_id qcom_aoss_reset_of_match[] = {
	{ .compatible = "qcom,aoss-reset-sdm845", .data = &aoss_sdm845_desc},
	{}
};

static struct platform_driver qcom_aoss_reset_driver = {
	.probe = qcom_aoss_reset_probe,
	.driver  = {
		.name  = "qcom_aoss_reset",
		.of_match_table = qcom_aoss_reset_of_match,
	},
};

builtin_platform_driver(qcom_aoss_reset_driver);

MODULE_DESCRIPTION("Qualcomm AOSS Reset Driver");
MODULE_LICENSE("GPL v2");
