/* Marvell Bluetooth driver: platform specific driver
 *
 * Copyright (C) 2015, Marvell International Ltd.
 *
 * This software file (the "File") is distributed by Marvell International
 * Ltd. under the terms of the GNU General Public License Version 2, June 1991
 * (the "License").  You may use, redistribute and/or modify this File in
 * accordance with the terms and conditions of the License, a copy of which
 * is available on the worldwide web at
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt.
 *
 * THE FILE IS DISTRIBUTED AS-IS, WITHOUT WARRANTY OF ANY KIND, AND THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE
 * ARE EXPRESSLY DISCLAIMED.  The License provides additional details about
 * this warranty disclaimer.
 */

#include "btmrvl_drv.h"

struct platform_device *btmrvl_plt_dev;

struct btmrvl_wake_dev {
	struct device	*dev;
	int		irq_bt;
	bool		wake_by_bt;
};

static irqreturn_t btmrvl_wake_irq_bt(int irq, void *priv)
{
	struct btmrvl_wake_dev *ctx = priv;

	if (ctx->irq_bt >= 0) {
		ctx->wake_by_bt = true;
		disable_irq_nosync(ctx->irq_bt);
	}

	return IRQ_HANDLED;
}

static int btmrvl_plt_probe(struct platform_device *pdev)
{
	int ret;
	struct btmrvl_wake_dev *ctx;
	int gpio;

	btmrvl_plt_dev = pdev;

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = &pdev->dev;
	ctx->irq_bt = platform_get_irq(pdev, 0);
	if (ctx->irq_bt < 0)
		dev_err(&pdev->dev, "Failed to get irq_bt\n");

	gpio = of_get_gpio(pdev->dev.of_node, 0);
	if (gpio_is_valid(gpio))
		gpio_direction_input(gpio);
	else
		dev_err(&pdev->dev, "gpio bt is invalid\n");

	if (ctx->irq_bt >= 0) {
		ret = devm_request_irq(&pdev->dev, ctx->irq_bt,
				       btmrvl_wake_irq_bt,
				       IRQF_TRIGGER_LOW,
				       "bt_wake", ctx);
		if (ret) {
			dev_err(&pdev->dev,
				"Failed to request irq_bt %d (%d)\n",
				ctx->irq_bt, ret);
			return -EINVAL;
		}
		disable_irq(ctx->irq_bt);
	}

	platform_set_drvdata(pdev, ctx);

	return 0;
}

static int btmrvl_plt_remove(struct platform_device *pdev)
{
	btmrvl_plt_dev = NULL;
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int btmrvl_plt_suspend(struct device *dev)
{
	struct btmrvl_wake_dev *ctx = dev_get_drvdata(dev);
	int ret;

	if (ctx->irq_bt >= 0) {
		ctx->wake_by_bt = false;
		enable_irq(ctx->irq_bt);
		ret = enable_irq_wake(ctx->irq_bt);
		if (ret)
			return ret;
	}

	return 0;
}

static int btmrvl_plt_resume(struct device *dev)
{
	struct btmrvl_wake_dev *ctx = dev_get_drvdata(dev);
	int ret;

	if (ctx->irq_bt >= 0) {
		ret = disable_irq_wake(ctx->irq_bt);
		if (!ctx->wake_by_bt)
			disable_irq(ctx->irq_bt);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct dev_pm_ops btmrvl_plt_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(btmrvl_plt_suspend, btmrvl_plt_resume)
};
#endif /* CONFIG_PM_SLEEP */

static const struct of_device_id btmrvl_dt_match[] = {
	{
		.compatible = "marvell,btmrvl",
	},
	{},
};

MODULE_DEVICE_TABLE(of, btmrvl_dt_match);

static struct platform_driver btmrvl_platform_driver = {
	.probe		= btmrvl_plt_probe,
	.remove		= btmrvl_plt_remove,
	.driver = {
		.name	= "btmrvl_plt",
		.of_match_table = btmrvl_dt_match,
#ifdef CONFIG_PM_SLEEP
		.pm             = &btmrvl_plt_pm_ops,
#endif
	}
};

int btmrvl_platform_drv_init(void)
{
	return platform_driver_register(&btmrvl_platform_driver);
}

void btmrvl_platform_drv_exit(void)
{
	platform_driver_unregister(&btmrvl_platform_driver);
}
