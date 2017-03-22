/*
 * Driver for STM32 Independent Watchdog
 *
 * Copyright (C) Yannick Fertre 2017
 * Author: Yannick Fertre <yannick.fertre@st.com>
 *
 * This driver is based on tegra_wdt.c
 *
 * License terms:  GNU General Public License (GPL), version 2
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

/* minimum watchdog trigger timeout, in seconds */
#define MIN_WDT_TIMEOUT	1

/* IWDG registers */
#define IWDG_KR		0x00 /* Key register */
#define IWDG_PR		0x04 /* Prescaler Register */
#define IWDG_RLR	0x08 /* ReLoad Register */
#define IWDG_SR		0x0C /* Status Register */
#define IWDG_WINR	0x10 /* Windows Register */

/* IWDG_KR register bit mask */
#define KR_KEY_RELOAD	0xAAAA /* reload counter enable */
#define KR_KEY_ENABLE	0xCCCC /* peripheral enable */
#define KR_KEY_EWA	0x5555 /* write access enable */
#define KR_KEY_DWA	0x0000 /* write access disable */

/* IWDG_PR register bit values */
#define PR_4    0x00 /* prescaler set to 4 */
#define PR_8    0x01 /* prescaler set to 8 */
#define PR_16   0x02 /* prescaler set to 16 */
#define PR_32   0x03 /* prescaler set to 32 */
#define PR_64   0x04 /* prescaler set to 64 */
#define PR_128  0x05 /* prescaler set to 128 */
#define PR_256	0x06 /* prescaler set to 256 */

/* IWDG_RLR register values */
#define RLR_MAX		0xFFF /* max value supported by reload register */

/* IWDG_SR register bit mask */
#define FLAG_PVU	BIT(0) /* Watchdog prescaler value update */
#define FLAG_RVU	BIT(1) /* Watchdog counter reload value update */

/* set timeout to 100000 us */
#define TIMEOUT_US	100000
#define SLEEP_US	1000

struct stm32_iwdg {
	struct watchdog_device	wdd;
	struct device		*dev;
	void __iomem		*regs;
	struct clk		*clk;
	unsigned int		rate;
};

static int heartbeat = MIN_WDT_TIMEOUT;
module_param(heartbeat, int, 0x0);
MODULE_PARM_DESC(heartbeat,
		 "Watchdog heartbeats in seconds. (default = "
		 __MODULE_STRING(WDT_HEARTBEAT) ")");

static inline u32 reg_read(void __iomem *base, u32 reg)
{
	return readl_relaxed(base + reg);
}

static inline void reg_write(void __iomem *base, u32 reg, u32 val)
{
	writel_relaxed(val, base + reg);
}

static int stm32_iwdg_start(struct watchdog_device *wdd)
{
	struct stm32_iwdg *wdt = watchdog_get_drvdata(wdd);
	u32 val = FLAG_PVU | FLAG_RVU;
	u32 reload;
	int ret;

	dev_dbg(wdt->dev, "%s\n", __func__);

	/* prescaler fixed to 256 */
	reload = (wdd->timeout * wdt->rate) / 256;
	if (reload > RLR_MAX + 1) {
		dev_err(wdt->dev,
			"Watchdog doesn't support reload value: %d\n", reload);
		return -EINVAL;
	}

	/* enable watchdog */
	reg_write(wdt->regs, IWDG_KR, KR_KEY_ENABLE);

	/* set prescaler & reload registers */
	reg_write(wdt->regs, IWDG_KR, KR_KEY_EWA);
	reg_write(wdt->regs, IWDG_PR, PR_256); /* prescaler fix to 256 */
	reg_write(wdt->regs, IWDG_RLR, reload - 1);

	/* wait for the registers to be updated (max 100ms) */
	ret = readl_relaxed_poll_timeout(wdt->regs + IWDG_SR, val,
					 !(val & (FLAG_PVU | FLAG_RVU)),
					 SLEEP_US, TIMEOUT_US);
	if (ret) {
		dev_err(wdt->dev,
			"Fail to set prescaler or reload registers\n");
		return -EINVAL;
	}

	/* reload watchdog */
	reg_write(wdt->regs, IWDG_KR, KR_KEY_RELOAD);

	return 0;
}

static int stm32_iwdg_stop(struct watchdog_device *wdd)
{
	struct stm32_iwdg *wdt = watchdog_get_drvdata(wdd);

	if (watchdog_active(wdd)) {
		dev_err(wdt->dev,
			"Watchdog can't be stopped once started(no way out)\n");
		return -EPERM;
	}

	return 0;
}

static int stm32_iwdg_ping(struct watchdog_device *wdd)
{
	struct stm32_iwdg *wdt = watchdog_get_drvdata(wdd);

	dev_dbg(wdt->dev, "%s\n", __func__);

	reg_write(wdt->regs, IWDG_KR, KR_KEY_RELOAD);

	return 0;
}

static int stm32_iwdg_set_timeout(struct watchdog_device *wdd,
				  unsigned int timeout)
{
	struct stm32_iwdg *wdt = watchdog_get_drvdata(wdd);

	dev_dbg(wdt->dev, "%s timeout: %d sec\n", __func__, timeout);

	wdd->timeout = timeout;

	if (watchdog_active(wdd))
		return stm32_iwdg_start(wdd);

	return 0;
}

static const struct watchdog_info stm32_iwdg_info = {
	.options	= WDIOF_SETTIMEOUT |
			  WDIOF_MAGICCLOSE |
			  WDIOF_KEEPALIVEPING,
	.identity	= "STM32 Independent Watchdog",
};

static struct watchdog_ops stm32_iwdg_ops = {
	.owner		= THIS_MODULE,
	.start		= stm32_iwdg_start,
	.stop		= stm32_iwdg_stop,
	.ping		= stm32_iwdg_ping,
	.set_timeout	= stm32_iwdg_set_timeout,
};

static int stm32_iwdg_probe(struct platform_device *pdev)
{
	struct watchdog_device *wdd;
	struct stm32_iwdg *wdt;
	struct resource *res;
	void __iomem *regs;
	struct clk *clk;
	int max_wdt_timeout;
	int ret;

	/* This is the timer base. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(regs)) {
		dev_err(&pdev->dev, "Could not get resource\n");
		return PTR_ERR(regs);
	}

	clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(clk)) {
		dev_err(&pdev->dev, "Unable to get clock\n");
		return PTR_ERR(clk);
	}

	ret = clk_prepare_enable(clk);
	if (ret) {
		dev_err(&pdev->dev, "Unable to prepare clock %p\n", clk);
		return ret;
	}

	/*
	 * Allocate our watchdog driver data, which has the
	 * struct watchdog_device nested within it.
	 */
	wdt = devm_kzalloc(&pdev->dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt) {
		ret = -ENOMEM;
		goto err;
	}

	/* Initialize struct stm32_iwdg. */
	wdt->regs = regs;
	wdt->dev = &pdev->dev;
	wdt->clk = clk;
	/*
	 * iwdg is clocked by its own dedicated low-speed clock (LSI)
	 * at 32khz.
	 */
	wdt->rate = 32 * 1024;

	/* get max timeout & set heartbeat */
	max_wdt_timeout = ((RLR_MAX + 1) * 256) / wdt->rate;
	heartbeat = max_wdt_timeout;

	/* Initialize struct watchdog_device. */
	wdd = &wdt->wdd;
	wdd->timeout = heartbeat;
	wdd->info = &stm32_iwdg_info;
	wdd->ops = &stm32_iwdg_ops;
	wdd->min_timeout = MIN_WDT_TIMEOUT;
	wdd->max_timeout = max_wdt_timeout;
	watchdog_set_drvdata(wdd, wdt);
	watchdog_set_nowayout(wdd, true);

	ret = watchdog_register_device(wdd);
	if (ret) {
		dev_err(&pdev->dev,
			"failed to register watchdog device\n");
		goto err;
	}

	platform_set_drvdata(pdev, wdt);

	dev_info(&pdev->dev,
		 "initialized (heartbeat = %d sec)\n", heartbeat);
	return 0;

err:
	clk_disable_unprepare(clk);
	return ret;
}

static int stm32_iwdg_remove(struct platform_device *pdev)
{
	struct stm32_iwdg *wdt = platform_get_drvdata(pdev);

	watchdog_unregister_device(&wdt->wdd);
	clk_disable_unprepare(wdt->clk);

	dev_info(&pdev->dev, "removed watchdog device\n");
	return 0;
}

static const struct of_device_id stm32_iwdg_of_match[] = {
	{ .compatible = "st,stm32-iwdg" },
	{ /* end node */ }
};
MODULE_DEVICE_TABLE(of, stm32_iwdg_of_match);

static struct platform_driver stm32_iwdg_driver = {
	.probe		= stm32_iwdg_probe,
	.remove		= stm32_iwdg_remove,
	.driver = {
		.name	= "iwdg",
		.of_match_table = stm32_iwdg_of_match,
	},
};
module_platform_driver(stm32_iwdg_driver);

MODULE_AUTHOR("Yannick Fertre <yannick.fertre@st.com>");
MODULE_DESCRIPTION("STMicroelectronics STM32 Independent Watchdog Driver");
MODULE_LICENSE("GPL v2");
