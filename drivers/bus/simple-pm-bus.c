/*
 * Simple Power-Managed Bus Driver
 *
 * Author: Geert Uytterhoeven <geert+renesas@glider.be>
 *
 * Copyright (C) 2014-2015 Glider bvba
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/init.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>


static int simple_pm_bus_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;

	dev_dbg(&pdev->dev, "%s\n", __func__);

	pm_runtime_enable(&pdev->dev);

	if (np)
		of_platform_populate(np, NULL, NULL, &pdev->dev);

	return 0;
}

static const struct of_device_id simple_pm_bus_of_match[] = {
	{ .compatible = "simple-pm-bus", },
	{ /* sentinel */ }
};

static struct platform_driver simple_pm_bus_driver = {
	.probe = simple_pm_bus_probe,
	.driver = {
		.name = "simple-pm-bus",
		.of_match_table = simple_pm_bus_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(simple_pm_bus_driver);
