/*
 * Copyright (c) 2016 Yang Ling <gnaygnil@gmail.com>
 *
 * This program is free software; you can redistribute	it and/or modify it
 * under  the terms of	the GNU General	 Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/module.h>
#include <linux/watchdog.h>
#include <linux/platform_device.h>
#include <linux/clk.h>

#include <loongson1.h>

#define MIN_HEARTBEAT		1
#define MAX_HEARTBEAT		30
#define DEFAULT_HEARTBEAT	10

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);

static unsigned int heartbeat = DEFAULT_HEARTBEAT;
module_param(heartbeat, uint, 0);

struct ls1x_wdt_drvdata {
	struct watchdog_device wdt;
	void __iomem *base;
	unsigned int count;
	struct clk *clk;
};

static int ls1x_wdt_ping(struct watchdog_device *wdt_dev)
{
	struct ls1x_wdt_drvdata *drvdata = watchdog_get_drvdata(wdt_dev);

	writel(0x1, drvdata->base + WDT_EN);
	writel(drvdata->count, drvdata->base + WDT_TIMER);
	writel(0x1, drvdata->base + WDT_SET);

	return 0;
}

static int ls1x_wdt_set_timeout(struct watchdog_device *wdt_dev,
				unsigned int new_timeout)
{
	struct ls1x_wdt_drvdata *drvdata = watchdog_get_drvdata(wdt_dev);
	int counts_per_second;

	if (watchdog_init_timeout(wdt_dev, new_timeout, NULL))
		return -EINVAL;

	counts_per_second = clk_get_rate(drvdata->clk);
	drvdata->count = counts_per_second * wdt_dev->timeout;

	ls1x_wdt_ping(wdt_dev);

	return 0;
}

static int ls1x_wdt_start(struct watchdog_device *wdt_dev)
{
	ls1x_wdt_set_timeout(wdt_dev, wdt_dev->timeout);

	return 0;
}

static int ls1x_wdt_stop(struct watchdog_device *wdt_dev)
{
	struct ls1x_wdt_drvdata *drvdata = watchdog_get_drvdata(wdt_dev);

	writel(0x0, drvdata->base + WDT_EN);

	return 0;
}

static const struct watchdog_info ls1x_wdt_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
	.identity = "Loongson1 Watchdog",
};

static const struct watchdog_ops ls1x_wdt_ops = {
	.owner = THIS_MODULE,
	.start = ls1x_wdt_start,
	.stop = ls1x_wdt_stop,
	.ping = ls1x_wdt_ping,
	.set_timeout = ls1x_wdt_set_timeout,
};

static int ls1x_wdt_probe(struct platform_device *pdev)
{
	struct ls1x_wdt_drvdata *drvdata;
	struct watchdog_device *ls1x_wdt;
	struct resource *res;
	int ret;

	drvdata = devm_kzalloc(&pdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	drvdata->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(drvdata->base))
		return PTR_ERR(drvdata->base);

	drvdata->clk = devm_clk_get(&pdev->dev, pdev->name);
	if (IS_ERR(drvdata->clk)) {
		dev_err(&pdev->dev, "failed to get %s clock\n", pdev->name);
		return PTR_ERR(drvdata->clk);
	}
	clk_prepare_enable(drvdata->clk);

	ls1x_wdt = &drvdata->wdt;
	ls1x_wdt->info = &ls1x_wdt_info;
	ls1x_wdt->ops = &ls1x_wdt_ops;
	ls1x_wdt->timeout = heartbeat;
	ls1x_wdt->min_timeout = MIN_HEARTBEAT;
	ls1x_wdt->max_timeout = MAX_HEARTBEAT;
	ls1x_wdt->parent = &pdev->dev;
	watchdog_set_nowayout(ls1x_wdt, nowayout);
	watchdog_set_drvdata(ls1x_wdt, drvdata);

	ret = watchdog_register_device(&drvdata->wdt);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register watchdog device\n");
		return ret;
	}

	platform_set_drvdata(pdev, drvdata);

	return 0;
}

static int ls1x_wdt_remove(struct platform_device *pdev)
{
	struct ls1x_wdt_drvdata *drvdata = platform_get_drvdata(pdev);

	ls1x_wdt_stop(&drvdata->wdt);
	watchdog_unregister_device(&drvdata->wdt);
	clk_disable_unprepare(drvdata->clk);

	return 0;
}

static struct platform_driver ls1x_wdt_driver = {
	.probe = ls1x_wdt_probe,
	.remove = ls1x_wdt_remove,
	.driver = {
		.name = "ls1x-wdt",
	},
};

module_platform_driver(ls1x_wdt_driver);

MODULE_AUTHOR("Yang Ling <gnaygnil@gmail.com>");
MODULE_DESCRIPTION("Loongson1 Watchdog Driver");
MODULE_LICENSE("GPL");
