// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2018 ROHM Semiconductors
// ROHM BD70528MWV watchdog driver

#include <linux/bcd.h>
#include <linux/kernel.h>
#include <linux/mfd/rohm-bd70528.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/watchdog.h>

struct wdtbd70528 {
	struct device *dev;
	struct regmap *regmap;
	struct mutex *rtc_lock;
	struct watchdog_device wdt;
};

static int bd70528_wdt_set_locked(struct wdtbd70528 *w, int enable)
{
	struct bd70528 *bd70528;

	bd70528 = container_of(w->rtc_lock, struct bd70528, rtc_timer_lock);
	return bd70528->wdt_set(bd70528, enable, NULL);
}
static int bd70528_wdt_set(struct wdtbd70528 *w, int enable)
{
	int ret;

	mutex_lock(w->rtc_lock);
	ret = bd70528_wdt_set_locked(w, enable);
	mutex_unlock(w->rtc_lock);

	return ret;
}

static int bd70528_wdt_start(struct watchdog_device *wdt)
{
	struct wdtbd70528 *w = watchdog_get_drvdata(wdt);

	dev_dbg(w->dev, "WDT ping...\n");
	return bd70528_wdt_set(w, 1);
}

static int bd70528_wdt_stop(struct watchdog_device *wdt)
{
	struct wdtbd70528 *w = watchdog_get_drvdata(wdt);

	dev_dbg(w->dev, "WDT stopping...\n");
	return bd70528_wdt_set(w, 0);
}

static int bd70528_wdt_set_timeout(struct watchdog_device *wdt,
				    unsigned int timeout)
{
	unsigned int hours;
	unsigned int minutes;
	unsigned int seconds;
	int ret;
	struct wdtbd70528 *w = watchdog_get_drvdata(wdt);

	seconds = timeout;
	hours = timeout / (60 * 60);
	/* Maximum timeout is 1h 59m 59s => hours is 1 or 0 */
	if (hours)
		seconds -= (60 * 60);
	minutes = seconds / 60;
	seconds = seconds % 60;

	mutex_lock(w->rtc_lock);

	ret = bd70528_wdt_set_locked(w, 0);
	if (ret)
		goto out_unlock;

	ret = regmap_update_bits(w->regmap, BD70528_REG_WDT_HOUR,
				 BD70528_MASK_WDT_HOUR, hours);
	if (ret) {
		dev_err(w->dev, "Failed to set WDT hours\n");
		goto out_en_unlock;
	}
	ret = regmap_update_bits(w->regmap, BD70528_REG_WDT_MINUTE,
				 BD70528_MASK_WDT_MINUTE, bin2bcd(minutes));
	if (ret) {
		dev_err(w->dev, "Failed to set WDT minutes\n");
		goto out_en_unlock;
	}
	ret = regmap_update_bits(w->regmap, BD70528_REG_WDT_SEC,
				 BD70528_MASK_WDT_SEC, bin2bcd(seconds));
	if (ret)
		dev_err(w->dev, "Failed to set WDT seconds\n");
	else
		dev_dbg(w->dev, "WDT tmo set to %u\n", timeout);

out_en_unlock:
	ret = bd70528_wdt_set_locked(w, 1);
out_unlock:
	mutex_unlock(w->rtc_lock);

	return ret;
}

static const struct watchdog_info bd70528_wdt_info = {
	.identity = "bd70528-wdt",
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
};

static const struct watchdog_ops bd70528_wdt_ops = {
	.start		= bd70528_wdt_start,
	.stop		= bd70528_wdt_stop,
	.set_timeout	= bd70528_wdt_set_timeout,
};

/* Max time we can set is 1 hour, 59 minutes and 59 seconds */
#define WDT_MAX_MS ((2 * 60 * 60 - 1) * 1000)
/* Minimum time is 1 second */
#define WDT_MIN_MS 1000
#define DEFAULT_TIMEOUT 60

static int bd70528_wdt_probe(struct platform_device *pdev)
{
	struct bd70528 *bd70528;
	struct wdtbd70528 *w;
	int ret;
	unsigned int reg;

	bd70528 = dev_get_drvdata(pdev->dev.parent);
	if (!bd70528) {
		dev_err(&pdev->dev, "No MFD driver data\n");
		return -EINVAL;
	}
	w = devm_kzalloc(&pdev->dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;

	w->regmap = bd70528->chip.regmap;
	w->rtc_lock = &bd70528->rtc_timer_lock;
	w->dev = &pdev->dev;

	w->wdt.info = &bd70528_wdt_info;
	w->wdt.ops =  &bd70528_wdt_ops;
	w->wdt.min_hw_heartbeat_ms = WDT_MIN_MS;
	w->wdt.max_hw_heartbeat_ms = WDT_MAX_MS;
	w->wdt.parent = pdev->dev.parent;
	w->wdt.timeout = DEFAULT_TIMEOUT;
	watchdog_set_drvdata(&w->wdt, w);
	watchdog_init_timeout(&w->wdt, 0, pdev->dev.parent);

	ret = bd70528_wdt_set_timeout(&w->wdt, w->wdt.timeout);
	if (ret) {
		dev_err(&pdev->dev, "Failed to set the watchdog timeout\n");
		return ret;
	}

	mutex_lock(w->rtc_lock);
	ret = regmap_read(w->regmap, BD70528_REG_WDT_CTRL, &reg);
	mutex_unlock(w->rtc_lock);

	if (ret) {
		dev_err(&pdev->dev, "Failed to get the watchdog state\n");
		return ret;
	}
	if (reg & BD70528_MASK_WDT_EN) {
		dev_dbg(&pdev->dev, "watchdog was running during probe\n");
		set_bit(WDOG_HW_RUNNING, &w->wdt.status);
	}

	ret = devm_watchdog_register_device(&pdev->dev, &w->wdt);
	if (ret < 0)
		dev_err(&pdev->dev, "watchdog registration failed: %d\n", ret);

	return ret;
}
static struct platform_driver bd70528_wdt = {
	.driver = {
		.name = "bd70528-wdt"
	},
	.probe = bd70528_wdt_probe,
};

module_platform_driver(bd70528_wdt);

MODULE_AUTHOR("Matti Vaittinen <matti.vaittinen@fi.rohmeurope.com>");
MODULE_DESCRIPTION("BD70528 watchdog driver");
MODULE_LICENSE("GPL");
