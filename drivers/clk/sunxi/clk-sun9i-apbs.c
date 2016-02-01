/*
 * Copyright (C) 2016 Chen-Yu Tsai
 * Author: Chen-Yu Tsai <wens@csie.org>
 *
 * Allwinner A80 APBS clock driver
 *
 * License Terms: GNU General Public License v2
 *
 * Based on clk-sun6i-apbs.c
 * Allwinner A31 APB0 clock driver
 *
 * Copyright (C) 2014 Free Electrons
 * Author: Boris BREZILLON <boris.brezillon@free-electrons.com>
 *
 */

#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

static void sun9i_apbs_setup(struct device_node *node)
{
	const char *name = node->name;
	const char *parent;
	struct resource res;
	struct clk *clk;
	void __iomem *reg;
	int ret;

	reg = of_io_request_and_map(node, 0, of_node_full_name(node));
	if (IS_ERR(reg)) {
		pr_err("Could not get registers for a80-apbs-clk\n");
		return;
	}

	parent = of_clk_get_parent_name(node, 0);
	if (!parent)
		return;

	of_property_read_string(node, "clock-output-names", &name);

	/* The A80 APBS clock is a standard 2 bit wide divider clock */
	clk = clk_register_divider(NULL, name, parent, 0, reg, 0, 2, 0, NULL);
	if (IS_ERR(clk)) {
		pr_err("failed to register a80-apbs-clk: %ld\n", PTR_ERR(clk));
		goto err_unmap;
	}

	ret = of_clk_add_provider(node, of_clk_src_simple_get, clk);
	if (ret)
		goto err_unregister;

	return;

err_unregister:
	clk_unregister_divider(clk);
err_unmap:
	iounmap(reg);
	of_address_to_resource(node, 0, &res);
	release_mem_region(res.start, resource_size(&res));
}
CLK_OF_DECLARE(sun9i_apbs, "allwinner,sun9i-a80-apbs-clk", sun9i_apbs_setup);
