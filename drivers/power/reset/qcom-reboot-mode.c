/* Copyright (c) 2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include "reboot-mode.h"

struct qcom_reboot_mode {
	struct regmap *map;
	struct reboot_mode_driver reboot;
	u32 offset;
	u32 mask;
};

static int qcom_reboot_mode_write(struct reboot_mode_driver *reboot,
				    unsigned int magic)
{
	struct qcom_reboot_mode *qrm;
	int ret;

	qrm = container_of(reboot, struct qcom_reboot_mode, reboot);

	/* update reboot magic */
	ret = regmap_update_bits(qrm->map, qrm->offset, qrm->mask, magic);
	if (ret < 0) {
		dev_err(reboot->dev, "Failed to update reboot mode bits\n");
		return ret;
	}

	return 0;
}

static int qcom_reboot_mode_probe(struct platform_device *pdev)
{
	struct qcom_reboot_mode *qrm;
	struct device *dev = &pdev->dev;
	int ret;

	qrm = devm_kzalloc(&pdev->dev, sizeof(*qrm), GFP_KERNEL);
	if (!qrm)
		return -ENOMEM;

	qrm->reboot.dev = dev;
	qrm->reboot.write = qcom_reboot_mode_write;
	qrm->mask = 0xffffffff;

	qrm->map = dev_get_regmap(dev->parent, NULL);
	if (IS_ERR_OR_NULL(qrm->map))
		return -EINVAL;

	if (of_property_read_u32(dev->of_node, "offset", &qrm->offset))
		return -EINVAL;

	of_property_read_u32(dev->of_node, "mask", &qrm->mask);

	ret = reboot_mode_register(&qrm->reboot);
	if (ret)
		dev_err(dev, "Failed to register reboot mode\n");

	dev_set_drvdata(dev, qrm);
	return ret;
}

static int qcom_reboot_mode_remove(struct platform_device *pdev)
{
	struct qcom_reboot_mode *qrm;

	qrm = dev_get_drvdata(&pdev->dev);
	return reboot_mode_unregister(&qrm->reboot);
}

static const struct of_device_id of_qcom_reboot_mode_match[] = {
	{ .compatible = "qcom,reboot-mode" },
	{}
};
MODULE_DEVICE_TABLE(of, of_qcom_reboot_mode_match);

static struct platform_driver qcom_reboot_mode_driver = {
	.probe = qcom_reboot_mode_probe,
	.remove = qcom_reboot_mode_remove,
	.driver = {
		.name = "qcom-reboot-mode",
		.of_match_table = of_match_ptr(of_qcom_reboot_mode_match),
	},
};

static int __init qcom_reboot_mode_init(void)
{
	return platform_driver_register(&qcom_reboot_mode_driver);
}
device_initcall(qcom_reboot_mode_init);

MODULE_DESCRIPTION("QCOM Reboot Mode Driver");
MODULE_LICENSE("GPL v2");
