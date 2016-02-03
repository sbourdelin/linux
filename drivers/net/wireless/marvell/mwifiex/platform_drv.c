/* Marvell wireless LAN device driver: platform specific driver
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
#include "main.h"

struct platform_device *mwifiex_plt_dev;

struct mwifiex_wake_dev {
	struct device	*dev;
	int		irq_wifi;
	bool		wake_by_wifi;
};

static irqreturn_t mwifiex_wake_irq_wifi(int irq, void *priv)
{
	struct mwifiex_wake_dev *ctx = priv;

	if (ctx->irq_wifi >= 0) {
		ctx->wake_by_wifi = true;
		disable_irq_nosync(ctx->irq_wifi);
	}

	return IRQ_HANDLED;
}

static int mwifiex_plt_probe(struct platform_device *pdev)
{
	int ret;
	struct mwifiex_wake_dev *ctx;
	int gpio;

	mwifiex_plt_dev = pdev;

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = &pdev->dev;
	ctx->irq_wifi = platform_get_irq(pdev, 0);
	if (ctx->irq_wifi < 0)
		dev_err(&pdev->dev, "Failed to get irq_wifi\n");

	gpio = of_get_gpio(pdev->dev.of_node, 0);
	if (gpio_is_valid(gpio))
		gpio_direction_input(gpio);
	else
		dev_err(&pdev->dev, "gpio wifi is invalid\n");

	if (ctx->irq_wifi >= 0) {
		ret = devm_request_irq(&pdev->dev, ctx->irq_wifi,
				       mwifiex_wake_irq_wifi,
				       IRQF_TRIGGER_LOW,
				       "wifi_wake", ctx);
		if (ret) {
			dev_err(&pdev->dev,
				"Failed to request irq_wifi %d (%d)\n",
				ctx->irq_wifi, ret);
			return -EINVAL;
		}
		disable_irq(ctx->irq_wifi);
	}

	platform_set_drvdata(pdev, ctx);

	return 0;
}

static int mwifiex_plt_remove(struct platform_device *pdev)
{
	mwifiex_plt_dev = NULL;
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int mwifiex_plt_suspend(struct device *dev)
{
	struct mwifiex_wake_dev *ctx = dev_get_drvdata(dev);
	int ret;

	if (ctx->irq_wifi >= 0) {
		ctx->wake_by_wifi = false;
		enable_irq(ctx->irq_wifi);
		ret = enable_irq_wake(ctx->irq_wifi);
		if (ret)
			return ret;
	}

	return 0;
}

static int mwifiex_plt_resume(struct device *dev)
{
	struct mwifiex_wake_dev *ctx = dev_get_drvdata(dev);
	int ret;

	if (ctx->irq_wifi >= 0) {
		ret = disable_irq_wake(ctx->irq_wifi);
		if (!ctx->wake_by_wifi)
			disable_irq(ctx->irq_wifi);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct dev_pm_ops mwifiex_plt_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mwifiex_plt_suspend, mwifiex_plt_resume)
};
#endif /* CONFIG_PM_SLEEP */

static const struct of_device_id mwifiex_dt_match[] = {
	{
		.compatible = "marvell,mwifiex",
	},
	{},
};

MODULE_DEVICE_TABLE(of, mwifiex_dt_match);

static struct platform_driver mwifiex_platform_driver = {
	.probe		= mwifiex_plt_probe,
	.remove		= mwifiex_plt_remove,
	.driver = {
		.name	= "mwifiex_plt",
		.of_match_table = mwifiex_dt_match,
#ifdef CONFIG_PM_SLEEP
		.pm             = &mwifiex_plt_pm_ops,
#endif
	}
};

int mwifiex_platform_drv_init(void)
{
	return platform_driver_register(&mwifiex_platform_driver);
}

void mwifiex_platform_drv_exit(void)
{
	platform_driver_unregister(&mwifiex_platform_driver);
}
