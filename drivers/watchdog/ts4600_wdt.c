/*
 * Watchdog driver for TS-4600 based boards
 *
 * Copyright (c) 2016 - Savoir-faire Linux
 * Author: Sebastien Bourdelin <sebastien.bourdelin@savoirfairelinux.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 *
 * The watchdog on the TS-4600 based boards is in an FPGA and can only be
 * accessed using a GPIO bit-banged bus called the NBUS by Technologic Systems.
 * The logic for the watchdog is the same then for the TS-4800 SoM, only the way
 * to access it change, therefore this driver is heavely based on the ts4800_wdt
 * driver from Damien Riegel <damien.riegel@savoirfairelinux.com>.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/ts-nbus.h>
#include <linux/watchdog.h>

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout,
	"Watchdog cannot be stopped once started (default="
	__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

/* possible feed values */
#define TS4600_WDT_FEED_2S       0x1
#define TS4600_WDT_FEED_10S      0x2
#define TS4600_WDT_DISABLE       0x3

struct ts4600_wdt {
	struct watchdog_device  wdd;
	struct ts_nbus		*ts_nbus;
	u32                     feed_offset;
	u32                     feed_val;
};

/*
 * TS-4600 supports the following timeout values:
 *
 *   value desc
 *   ---------------------
 *     0    feed for 338ms
 *     1    feed for 2.706s
 *     2    feed for 10.824s
 *     3    disable watchdog
 *
 * Keep the regmap/timeout map ordered by timeout
 */
static const struct {
	const int timeout;
	const int regval;
} ts4600_wdt_map[] = {
	{ 2,  TS4600_WDT_FEED_2S },
	{ 10, TS4600_WDT_FEED_10S },
};

#define MAX_TIMEOUT_INDEX       (ARRAY_SIZE(ts4600_wdt_map) - 1)

static void ts4600_write_feed(struct ts4600_wdt *wdt, u32 val)
{
	ts_nbus_write(wdt->ts_nbus, wdt->feed_offset, val);
}

static int ts4600_wdt_start(struct watchdog_device *wdd)
{
	struct ts4600_wdt *wdt = watchdog_get_drvdata(wdd);

	ts4600_write_feed(wdt, wdt->feed_val);

	return 0;
}

static int ts4600_wdt_stop(struct watchdog_device *wdd)
{
	struct ts4600_wdt *wdt = watchdog_get_drvdata(wdd);

	ts4600_write_feed(wdt, TS4600_WDT_DISABLE);
	return 0;
}

static int ts4600_wdt_set_timeout(struct watchdog_device *wdd,
				  unsigned int timeout)
{
	struct ts4600_wdt *wdt = watchdog_get_drvdata(wdd);
	int i = 0;

	if (ts4600_wdt_map[i].timeout < timeout)
		i = MAX_TIMEOUT_INDEX;

	wdd->timeout = ts4600_wdt_map[i].timeout;
	wdt->feed_val = ts4600_wdt_map[i].regval;

	return 0;
}

static const struct watchdog_ops ts4600_wdt_ops = {
	.owner = THIS_MODULE,
	.start = ts4600_wdt_start,
	.stop = ts4600_wdt_stop,
	.set_timeout = ts4600_wdt_set_timeout,
};

static const struct watchdog_info ts4600_wdt_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_MAGICCLOSE | WDIOF_KEEPALIVEPING,
	.identity = "TS-4600 Watchdog",
};

static int ts4600_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct ts_nbus *ts_nbus;
	struct watchdog_device *wdd;
	struct ts4600_wdt *wdt;
	u32 reg;
	int ret;

	ret = of_property_read_u32(np, "reg", &reg);
	if (ret < 0) {
		dev_err(&pdev->dev, "missing reg property\n");
		return ret;
	}

	/* allocate memory for watchdog struct */
	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	/* set offset to know where to write */
	wdt->feed_offset = reg;

	/* keep a pointer to the ts_nbus instanciated by the parent node */
	ts_nbus = dev_get_drvdata(dev->parent);
	if (!ts_nbus) {
		dev_err(dev, "missing ts-nbus compatible parent node\n");
		return PTR_ERR(ts_nbus);
	}
	wdt->ts_nbus = ts_nbus;

	/* Initialize struct watchdog_device */
	wdd = &wdt->wdd;
	wdd->parent = dev;
	wdd->info = &ts4600_wdt_info;
	wdd->ops = &ts4600_wdt_ops;
	wdd->min_timeout = ts4600_wdt_map[0].timeout;
	wdd->max_hw_heartbeat_ms =
		ts4600_wdt_map[MAX_TIMEOUT_INDEX].timeout * 1000;

	watchdog_set_drvdata(wdd, wdt);
	watchdog_set_nowayout(wdd, nowayout);
	watchdog_init_timeout(wdd, 0, dev);

	/*
	 * As this watchdog supports only a few values, ts4600_wdt_set_timeout
	 * must be called to initialize timeout and feed_val with valid values.
	 * Default to maximum timeout if none, or an invalid one, is provided in
	 * device tree.
	 */
	if (!wdd->timeout)
		wdd->timeout = wdd->max_timeout;
	ts4600_wdt_set_timeout(wdd, wdd->timeout);

	/*
	 * The feed register is write-only, so it is not possible to determine
	 * watchdog's state. Disable it to be in a known state.
	 */
	ts4600_wdt_stop(wdd);

	ret = watchdog_register_device(wdd);
	if (ret) {
		dev_err(dev, "failed to register watchdog device\n");
		return ret;
	}

	platform_set_drvdata(pdev, wdt);

	dev_info(dev, "initialized (timeout = %d sec, nowayout = %d)\n",
		 wdd->timeout, nowayout);

	return 0;
}

static int ts4600_wdt_remove(struct platform_device *pdev)
{
	struct ts4600_wdt *wdt = platform_get_drvdata(pdev);

	watchdog_unregister_device(&wdt->wdd);

	return 0;
}

static const struct of_device_id ts4600_wdt_of_match[] = {
	{ .compatible = "technologic,ts4600-wdt", },
	{ },
};
MODULE_DEVICE_TABLE(of, ts4600_wdt_of_match);

static struct platform_driver ts4600_wdt_driver = {
	.probe		= ts4600_wdt_probe,
	.remove		= ts4600_wdt_remove,
	.driver		= {
		.name	= "ts4600_wdt",
		.of_match_table = ts4600_wdt_of_match,
	},
};

module_platform_driver(ts4600_wdt_driver);

MODULE_AUTHOR("Sebastien Bourdelin <sebastien.bourdelin@savoirfairelinux.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:ts4600_wdt");
