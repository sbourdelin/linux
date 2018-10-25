// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017  Mentor
 *
 * Contact: Jiada Wang <jiada_wang@mentor.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * avb Common Clock Framework support
 */

#include <linux/clk-provider.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/slab.h>

struct clk_avb_data {
	void __iomem *base;

	struct clk_onecell_data clk_data;
	/* lock reg access */
	spinlock_t lock;
};

struct clk_avb {
	struct clk_hw hw;
	unsigned int idx;
	struct clk_avb_data *data;
};

#define to_clk_avb(_hw) container_of(_hw, struct clk_avb, hw)

#define AVB_DIV_MASK	0x3ffff
#define AVB_MAX_DIV	0x3ffc0
#define AVB_COUNTER_MAX_FREQ	25000000
#define AVB_COUNTER_NUM		8
#define AVB_CLK_NAME_SIZE	10
#define AVB_ID_TO_DIV(id)	((id) * 4)

#define AVB_CLK_CONFIG		0x20
#define AVB_DIV_EN_COM		BIT(31)
#define AVB_CLK_NAME		"avb"
#define ADG_CLK_NAME		"adg"

static int clk_avb_is_enabled(struct clk_hw *hw)
{
	struct clk_avb *avb = to_clk_avb(hw);

	return (clk_readl(avb->data->base + AVB_CLK_CONFIG) & BIT(avb->idx));
}

static int clk_avb_enabledisable(struct clk_hw *hw, int enable)
{
	struct clk_avb *avb = to_clk_avb(hw);
	u32 val;

	spin_lock(&avb->data->lock);

	val = clk_readl(avb->data->base + AVB_CLK_CONFIG);
	if (enable)
		val |= BIT(avb->idx);
	else
		val &= ~BIT(avb->idx);
	clk_writel(val, avb->data->base + AVB_CLK_CONFIG);

	spin_unlock(&avb->data->lock);

	return 0;
}

static int clk_avb_enable(struct clk_hw *hw)
{
	return clk_avb_enabledisable(hw, 1);
}

static void clk_avb_disable(struct clk_hw *hw)
{
	clk_avb_enabledisable(hw, 0);
}

static unsigned long clk_avb_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct clk_avb *avb = to_clk_avb(hw);
	u32 div;

	div = clk_readl(avb->data->base + AVB_ID_TO_DIV(avb->idx)) &
			AVB_DIV_MASK;
	if (!div)
		return parent_rate;

	return parent_rate * 32 / div;
}

static unsigned int clk_avb_calc_div(unsigned long rate,
				     unsigned long parent_rate)
{
	unsigned int div;

	if (!rate)
		rate = 1;

	if (rate > AVB_COUNTER_MAX_FREQ)
		rate = AVB_COUNTER_MAX_FREQ;

	div = DIV_ROUND_CLOSEST(parent_rate * 32, rate);

	if (div > AVB_MAX_DIV)
		div = AVB_MAX_DIV;

	return div;
}

static long clk_avb_round_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long *parent_rate)
{
	unsigned int div = clk_avb_calc_div(rate, *parent_rate);

	return (*parent_rate * 32) / div;
}

static int clk_avb_set_rate(struct clk_hw *hw, unsigned long rate,
			    unsigned long parent_rate)
{
	struct clk_avb *avb = to_clk_avb(hw);
	unsigned int div = clk_avb_calc_div(rate, parent_rate);
	u32 val;

	val = clk_readl(avb->data->base + AVB_ID_TO_DIV(avb->idx)) &
			~AVB_DIV_MASK;
	clk_writel(val | div, avb->data->base + AVB_ID_TO_DIV(avb->idx));

	return 0;
}

static const struct clk_ops clk_avb_ops = {
	.enable = clk_avb_enable,
	.disable = clk_avb_disable,
	.is_enabled = clk_avb_is_enabled,
	.recalc_rate = clk_avb_recalc_rate,
	.round_rate = clk_avb_round_rate,
	.set_rate = clk_avb_set_rate,
};

static struct clk *clk_register_avb(struct clk_avb_data *data,
				    unsigned int id)
{
	struct clk_init_data init;
	struct clk_avb *avb;
	struct clk *clk;
	char name[AVB_CLK_NAME_SIZE];
	const char *parent_name = ADG_CLK_NAME;

	avb = kzalloc(sizeof(*avb), GFP_KERNEL);
	if (!avb)
		return ERR_PTR(-ENOMEM);

	snprintf(name, AVB_CLK_NAME_SIZE, "%s.%u", AVB_CLK_NAME, id);

	avb->idx = id;
	avb->data = data;

	/* Register the clock. */
	init.name = name;
	init.ops = &clk_avb_ops;
	init.flags = CLK_IS_BASIC;
	init.parent_names = &parent_name;
	init.num_parents = 1;

	avb->hw.init = &init;

	/* init DIV to a valid state */
	writel(AVB_MAX_DIV, data->base + AVB_ID_TO_DIV(avb->idx));

	clk = clk_register(NULL, &avb->hw);
	if (IS_ERR(clk))
		kfree(avb);

	return clk;
}

static void clk_unregister_avb(struct clk *clk)
{
	struct clk_avb *avb;
	struct clk_hw *hw;

	if (IS_ERR(clk))
		return;

	hw = __clk_get_hw(clk);
	if (!hw)
		return;

	avb = to_clk_avb(hw);

	clk_unregister(clk);
	kfree(avb);
}

static void __init clk_avb_setup(struct device_node *node)
{
	struct clk_avb_data *data;
	struct clk_onecell_data *clk_data;
	int ret, i;
	struct resource res;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return;

	data->base = of_io_request_and_map(node, 0, of_node_full_name(node));
	if (IS_ERR(data->base))
		goto err_data;

	spin_lock_init(&data->lock);

	clk_data = &data->clk_data;
	clk_data->clk_num = AVB_COUNTER_NUM;
	clk_data->clks = kmalloc_array(AVB_COUNTER_NUM,
				       sizeof(struct clk *),
				       GFP_KERNEL);
	if (!clk_data->clks)
		goto err_unmap;

	for (i = 0; i < AVB_COUNTER_NUM; i++) {
		clk_data->clks[i] = clk_register_avb(data, i);
		if (IS_ERR(clk_data->clks[i])) {
			pr_err("failed to register clock %s.%d\n",
			       AVB_CLK_NAME, i);
			goto err_clks;
		}
	}

	ret = of_clk_add_provider(node, of_clk_src_onecell_get, clk_data);
	if (ret) {
		pr_err("failed to register clock provider\n");
		goto err_clks;
	}

	writel(AVB_DIV_EN_COM, data->base + AVB_CLK_CONFIG);

	return;

err_clks:
	for (i = 0; i < AVB_COUNTER_NUM; i++)
		clk_unregister_avb(clk_data->clks[i]);
err_unmap:
	iounmap(data->base);
	of_address_to_resource(node, 0, &res);
	release_mem_region(res.start, resource_size(&res));
err_data:
	kfree(data);
}

CLK_OF_DECLARE(avb, "renesas,clk-avb", clk_avb_setup);
