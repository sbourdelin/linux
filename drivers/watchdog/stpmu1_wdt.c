// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) STMicroelectronics 2018 - All Rights Reserved
 * Author: Philippe Peurichard <philippe.peurichard@st.com>,
 * Pascal Paillet <p.paillet@st.com> for STMicroelectronics.
 */

#include <linux/kernel.h>
#include <linux/mfd/stpmu1.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/watchdog.h>

/* WATCHDOG CONTROL REGISTER bit */
#define WDT_START		BIT(0)
#define WDT_PING		BIT(1)
#define WDT_START_MASK		BIT(0)
#define WDT_PING_MASK		BIT(1)

#define PMIC_WDT_MIN_TIMEOUT 1
#define PMIC_WDT_MAX_TIMEOUT 256

struct stpmu1_wdt {
	struct stpmu1_dev *pmic;
	struct watchdog_device wdtdev;
	struct notifier_block restart_handler;
};

static int pmic_wdt_start(struct watchdog_device *wdd)
{
	struct stpmu1_wdt *wdt = watchdog_get_drvdata(wdd);

	return regmap_update_bits(wdt->pmic->regmap,
				  WCHDG_CR, WDT_START_MASK, WDT_START);
}

static int pmic_wdt_stop(struct watchdog_device *wdd)
{
	struct stpmu1_wdt *wdt = watchdog_get_drvdata(wdd);

	return regmap_update_bits(wdt->pmic->regmap,
				  WCHDG_CR, WDT_START_MASK, ~WDT_START);
}

static int pmic_wdt_ping(struct watchdog_device *wdd)
{
	struct stpmu1_wdt *wdt = watchdog_get_drvdata(wdd);
	int ret;

	return regmap_update_bits(wdt->pmic->regmap,
				  WCHDG_CR, WDT_PING_MASK, WDT_PING);
	return ret;
}

static int pmic_wdt_set_timeout(struct watchdog_device *wdd,
				unsigned int timeout)
{
	struct stpmu1_wdt *wdt = watchdog_get_drvdata(wdd);
	int ret;

	ret = regmap_write(wdt->pmic->regmap, WCHDG_TIMER_CR, timeout);
	if (ret)
		dev_err(wdt->pmic->dev,
			"Failed to set watchdog timeout (err = %d)\n", ret);
	else
		wdd->timeout = PMIC_WDT_MAX_TIMEOUT;

	return ret;
}

static int pmic_wdt_restart_handler(struct notifier_block *this,
				    unsigned long mode, void *cmd)
{
	struct stpmu1_wdt *wdt = container_of(this,
						   struct stpmu1_wdt,
						   restart_handler);

	dev_info(wdt->pmic->dev,
		 "PMIC Watchdog Elapsed (timeout %d), shutdown of PMIC initiated\n",
		 wdt->wdtdev.timeout);

	return NOTIFY_DONE;
}

static const struct watchdog_info pmic_watchdog_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING,
	.identity = "STPMU1 PMIC Watchdog",
};

static const struct watchdog_ops pmic_watchdog_ops = {
	.owner = THIS_MODULE,
	.start = pmic_wdt_start,
	.stop = pmic_wdt_stop,
	.ping = pmic_wdt_ping,
	.set_timeout = pmic_wdt_set_timeout,
};

static int pmic_wdt_probe(struct platform_device *pdev)
{
	int ret;
	struct stpmu1_dev *pmic;
	struct stpmu1_wdt *wdt;

	if (!pdev->dev.parent)
		return -EINVAL;

	pmic = dev_get_drvdata(pdev->dev.parent);
	if (!pmic)
		return -EINVAL;

	wdt = devm_kzalloc(&pdev->dev, sizeof(struct stpmu1_wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	wdt->pmic = pmic;

	wdt->wdtdev.info = &pmic_watchdog_info;
	wdt->wdtdev.ops = &pmic_watchdog_ops;
	wdt->wdtdev.min_timeout = PMIC_WDT_MIN_TIMEOUT;
	wdt->wdtdev.max_timeout = PMIC_WDT_MAX_TIMEOUT;
	wdt->wdtdev.timeout = PMIC_WDT_MAX_TIMEOUT;

	wdt->wdtdev.status = WATCHDOG_NOWAYOUT_INIT_STATUS;

	watchdog_set_drvdata(&wdt->wdtdev, wdt);
	dev_set_drvdata(&pdev->dev, wdt);

	ret = watchdog_register_device(&wdt->wdtdev);
	if (ret)
		return ret;

	wdt->restart_handler.notifier_call = pmic_wdt_restart_handler;
	wdt->restart_handler.priority = 128;
	ret = register_restart_handler(&wdt->restart_handler);
	if (ret) {
		dev_err(wdt->pmic->dev, "failed to register restart handler\n");
		return ret;
	}

	dev_dbg(wdt->pmic->dev, "PMIC Watchdog driver probed\n");
	return 0;
}

static int pmic_wdt_remove(struct platform_device *pdev)
{
	struct stpmu1_wdt *wdt = dev_get_drvdata(&pdev->dev);

	unregister_restart_handler(&wdt->restart_handler);
	watchdog_unregister_device(&wdt->wdtdev);

	return 0;
}

static const struct of_device_id of_pmic_wdt_match[] = {
	{ .compatible = "st,stpmu1-wdt" },
	{ },
};

MODULE_DEVICE_TABLE(of, of_pmic_wdt_match);

static struct platform_driver stpmu1_wdt_driver = {
	.probe = pmic_wdt_probe,
	.remove = pmic_wdt_remove,
	.driver = {
		.name = "stpmu1-wdt",
		.of_match_table = of_pmic_wdt_match,
	},
};
module_platform_driver(stpmu1_wdt_driver);

MODULE_AUTHOR("philippe.peurichard@st.com>");
MODULE_DESCRIPTION("Watchdog driver for STPMU1 device");
MODULE_LICENSE("GPL");
