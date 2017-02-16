/*
 * Renesas WDT Reset Driver
 *
 * Copyright (C) 2017 Renesas Electronics America, Inc.
 * Copyright (C) 2017 Chris Brandt
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * based on rmobile-reset.c
 *
 */

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/reboot.h>
#include <linux/delay.h>

/* Watchdog Timer Registers */
#define WTCSR 0
#define WTCSR_MAGIC 0xA500
#define WTSCR_WT (1<<6)
#define WTSCR_TME (1<<5)

#define WTCNT 2
#define WTCNT_MAGIC 0x5A00

#define WRCSR 4
#define WRCSR_MAGIC 0x5A00
#define WRCSR_RSTE (1<<6)
#define WRCSR_CLEAR_WOVF 0xA500 /* special value */

static void __iomem *base;

static int wdt_reset_handler(struct notifier_block *this,
			     unsigned long mode, void *cmd)
{
	pr_debug("%s %lu\n", __func__, mode);

	/* Dummy read (must read WRCSR:WOVF at least once before clearing) */
	readb(base + WRCSR);

	writew(WRCSR_CLEAR_WOVF, base + WRCSR);		/* Clear WOVF */
	writew(WRCSR_MAGIC | WRCSR_RSTE, base + WRCSR);	/* Reset Enable */
	writew(WTCNT_MAGIC, base + WTCNT);		/* Counter to 00 */

	/* Start timer */
	writew(WTCSR_MAGIC | WTSCR_WT | WTSCR_TME, base + WTCSR);

	/* Wait for WDT overflow (reset) */
	while (1)
		msleep(1);

	return NOTIFY_DONE;
}

static struct notifier_block wdt_reset_nb = {
	.notifier_call = wdt_reset_handler,
	.priority = 192,
};

static int wdt_reset_probe(struct platform_device *pdev)
{
	int error;

	base = of_iomap(pdev->dev.of_node, 0);
	if (!base)
		return -ENODEV;

	error = register_restart_handler(&wdt_reset_nb);
	if (error) {
		dev_err(&pdev->dev,
			"cannot register restart handler (err=%d)\n", error);
		goto fail_unmap;
	}

	return 0;

fail_unmap:
	iounmap(base);
	return error;
}

static int wdt_reset_remove(struct platform_device *pdev)
{
	unregister_restart_handler(&wdt_reset_nb);
	iounmap(base);
	return 0;
}

static const struct of_device_id wdt_reset_of_match[] = {
	{ .compatible = "renesas,rza-wdt", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, wdt_reset_of_match);

static struct platform_driver wdt_reset_driver = {
	.probe = wdt_reset_probe,
	.remove = wdt_reset_remove,
	.driver = {
		.name = "wdt_reset",
		.of_match_table = wdt_reset_of_match,
	},
};

module_platform_driver(wdt_reset_driver);

MODULE_DESCRIPTION("Renesas WDT Reset Driver");
MODULE_AUTHOR("Chris Brandt <chris.brandt@renesas.com>");
MODULE_LICENSE("GPL v2");
