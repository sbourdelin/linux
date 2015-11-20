/**
 * timer-ti-32k.c - OMAP2 32k Timer Support
 *
 * Copyright (C) 2009 Nokia Corporation
 *
 * Update to use new clocksource/clockevent layers
 * Author: Kevin Hilman, MontaVista Software, Inc. <source@mvista.com>
 * Copyright (C) 2007 MontaVista Software, Inc.
 *
 * Original driver:
 * Copyright (C) 2005 Nokia Corporation
 * Author: Paul Mundt <paul.mundt@nokia.com>
 *         Juha Yrjölä <juha.yrjola@nokia.com>
 * OMAP Dual-mode timer framework support by Timo Teras
 *
 * Some parts based off of TI's 24xx code:
 *
 * Copyright (C) 2004-2009 Texas Instruments, Inc.
 *
 * Roughly modelled after the OMAP1 MPU timer code.
 * Added OMAP4 support - Santosh Shilimkar <santosh.shilimkar@ti.com>
 *
 * Copyright (C) 2015 Texas Instruments Incorporated - http://www.ti.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2  of
 * the License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/init.h>
#include <linux/time.h>
#include <linux/sched_clock.h>
#include <linux/clocksource.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

/*
 * 32KHz clocksource ... always available, on pretty most chips except
 * OMAP 730 and 1510.  Other timers could be used as clocksources, with
 * higher resolution in free-running counter modes (e.g. 12 MHz xtal),
 * but systems won't necessarily want to spend resources that way.
 */

#define OMAP2_32KSYNCNT_REV_OFF		0x0
#define OMAP2_32KSYNCNT_REV_SCHEME	(0x3 << 30)
#define OMAP2_32KSYNCNT_CR_OFF_LOW	0x10
#define OMAP2_32KSYNCNT_CR_OFF_HIGH	0x30

struct ti_32k {
	void __iomem		*base;
	void __iomem		*counter;
	struct clocksource	cs;
};

static inline struct ti_32k *to_ti_32k(struct clocksource *cs)
{
	return container_of(cs, struct ti_32k, cs);
}

static cycle_t ti_32k_read_cycles(struct clocksource *cs)
{
	struct ti_32k *ti = to_ti_32k(cs);

	return (cycle_t)readl_relaxed(ti->counter);
}

static struct ti_32k ti_32k_timer = {
	.cs = {
		.name		= "32k_counter",
		.rating		= 250,
		.read		= ti_32k_read_cycles,
		.mask		= CLOCKSOURCE_MASK(32),
		.flags		= CLOCK_SOURCE_IS_CONTINUOUS |
				CLOCK_SOURCE_SUSPEND_NONSTOP,
	},
};

static u64 notrace omap_32k_read_sched_clock(void)
{
	return ti_32k_read_cycles(&ti_32k_timer.cs);
}

static const struct of_device_id ti_32k_of_table[] = {
	{ .compatible = "ti,omap-counter32k" },
	{ }
};
MODULE_DEVICE_TABLE(of, ti_32k_of_table);

static int __init ti_32k_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	int ret;

	/* Static mapping, never released */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ti_32k_timer.base = devm_ioremap_resource(dev, res);
	if (IS_ERR(ti_32k_timer.base))
		return PTR_ERR(ti_32k_timer.base);

	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0)
		goto probe_err;

	ti_32k_timer.counter = ti_32k_timer.base;

	/*
	 * 32k sync Counter IP register offsets vary between the highlander
	 * version and the legacy ones.
	 *
	 * The 'SCHEME' bits(30-31) of the revision register is used to identify
	 * the version.
	 */
	if (readl_relaxed(ti_32k_timer.base + OMAP2_32KSYNCNT_REV_OFF) &
			OMAP2_32KSYNCNT_REV_SCHEME)
		ti_32k_timer.counter += OMAP2_32KSYNCNT_CR_OFF_HIGH;
	else
		ti_32k_timer.counter += OMAP2_32KSYNCNT_CR_OFF_LOW;

	ret = clocksource_register_hz(&ti_32k_timer.cs, 32768);
	if (ret) {
		pr_err("32k_counter: can't register clocksource\n");
		goto probe_err;
	}

	sched_clock_register(omap_32k_read_sched_clock, 32, 32768);
	pr_info("OMAP clocksource: 32k_counter at 32768 Hz\n");
	return 0;

probe_err:
	pm_runtime_put_noidle(dev);
	return ret;
};

static struct platform_driver ti_32k_driver __initdata = {
	.probe		= ti_32k_probe,
	.driver		= {
		.name	= "ti_32k_timer",
		.of_match_table = of_match_ptr(ti_32k_of_table),
	}
};

static int __init ti_32k_init(void)
{
	return platform_driver_register(&ti_32k_driver);
}

subsys_initcall(ti_32k_init);

MODULE_AUTHOR("Paul Mundt");
MODULE_AUTHOR("Juha Yrjölä");
MODULE_DESCRIPTION("OMAP2 32k Timer");
MODULE_ALIAS("platform:ti_32k_timer");
MODULE_LICENSE("GPL v2");
