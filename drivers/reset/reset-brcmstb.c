// SPDX-License-Identifier: GPL-2.0
/*
 * Broadcom STB generic reset controller for SW_INIT style reset controller
 *
 * Copyright (C) 2018 Broadcom
 *
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/types.h>

struct brcmstb_reset {
	void __iomem *base;
	unsigned int n_words;
	struct device *dev;
	struct reset_controller_dev rcdev;
};

#define SW_INIT_SET		0x00
#define SW_INIT_CLEAR		0x04
#define SW_INIT_STATUS		0x08

#define SW_INIT_BIT(id)		BIT((id) & 0x1f)
#define SW_INIT_BANK(id)	(id >> 5)

#define SW_INIT_BANK_SIZE	0x18

static inline
struct brcmstb_reset *to_brcmstb(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct brcmstb_reset, rcdev);
}

static int brcmstb_reset_assert(struct reset_controller_dev *rcdev,
				unsigned long id)
{
	unsigned int off = SW_INIT_BANK(id) * SW_INIT_BANK_SIZE;
	struct brcmstb_reset *priv = to_brcmstb(rcdev);

	writel_relaxed(SW_INIT_BIT(id), priv->base + off + SW_INIT_SET);
	msleep(10);

	return 0;
}

static int brcmstb_reset_deassert(struct reset_controller_dev *rcdev,
				  unsigned long id)
{
	unsigned int off = SW_INIT_BANK(id) * SW_INIT_BANK_SIZE;
	struct brcmstb_reset *priv = to_brcmstb(rcdev);

	writel_relaxed(SW_INIT_BIT(id), priv->base + off + SW_INIT_CLEAR);
	msleep(10);

	return 0;
}

static int brcmstb_reset_status(struct reset_controller_dev *rcdev,
				unsigned long id)
{
	unsigned int off = SW_INIT_BANK(id) * SW_INIT_BANK_SIZE;
	struct brcmstb_reset *priv = to_brcmstb(rcdev);

	return readl_relaxed(priv->base + off + SW_INIT_STATUS);
}

static const struct reset_control_ops brcmstb_reset_ops = {
	.assert	= brcmstb_reset_assert,
	.deassert = brcmstb_reset_deassert,
	.status = brcmstb_reset_status,
};

static int brcmstb_reset_probe(struct platform_device *pdev)
{
	struct device *kdev = &pdev->dev;
	struct brcmstb_reset *priv;
	struct resource *res;

	priv = devm_kzalloc(kdev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->base = devm_ioremap_resource(kdev, res);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	dev_set_drvdata(kdev, priv);

	priv->rcdev.owner = THIS_MODULE;
	priv->rcdev.nr_resets = (resource_size(res) / SW_INIT_BANK_SIZE) * 32;
	priv->rcdev.ops = &brcmstb_reset_ops;
	priv->rcdev.of_node = kdev->of_node;
	/* Use defaults: 1 cell and simple xlate function */
	priv->dev = kdev;

	return devm_reset_controller_register(kdev, &priv->rcdev);
}

static const struct of_device_id brcmstb_reset_of_match[] = {
	{ .compatible = "brcm,brcmstb-reset" },
	{ /* sentinel */ }
};

static struct platform_driver brcmstb_reset_driver = {
	.probe	= brcmstb_reset_probe,
	.driver	= {
		.name = "brcmstb-reset",
		.of_match_table = brcmstb_reset_of_match,
	},
};
module_platform_driver(brcmstb_reset_driver);

MODULE_AUTHOR("Broadcom");
MODULE_DESCRIPTION("Broadcom STB reset controller");
MODULE_LICENSE("GPL");
