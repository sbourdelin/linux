/*
 * Copyright 2016 Yassin Jaffer
 *
 * Yassin Jaffer <yassinjaffer@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk-provider.h>
#include <linux/of_address.h>
#include <linux/reset-controller.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

static DEFINE_SPINLOCK(sun4i_csi_lock);

#define SUN4I_CSI_PARENTS       5
#define SUN4I_CSI_GATE_BIT      31
#define SUN4I_CSI_RESET_BIT     30
#define SUN4I_CSI_MUX_SHIFT     24
#define SUN4I_CSI_DIV_WIDTH     5
#define SUN4I_CSI_DIV_SHIFT     0

static u32 sun4i_csi_mux_table[SUN4I_CSI_PARENTS] = {
	0x0,
	0x1,
	0x2,
	0x5,
	0x6,
};

struct csi_reset_data {
	void __iomem			*reg;
	spinlock_t			*lock; /* lock for reset handling */
	struct reset_controller_dev	rcdev;
};

static int sun4i_csi_assert(struct reset_controller_dev *rcdev,
			    unsigned long id)
{
	struct csi_reset_data *data = container_of(rcdev,
						  struct csi_reset_data,
						  rcdev);
	unsigned long flags;
	u32 reg;

	spin_lock_irqsave(data->lock, flags);

	reg = readl(data->reg);
	writel(reg & ~BIT(SUN4I_CSI_RESET_BIT), data->reg);

	spin_unlock_irqrestore(data->lock, flags);

	return 0;
}

static int sun4i_csi_deassert(struct reset_controller_dev *rcdev,
			      unsigned long id)
{
	struct csi_reset_data *data = container_of(rcdev,
						  struct csi_reset_data,
						  rcdev);
	unsigned long flags;
	u32 reg;

	spin_lock_irqsave(data->lock, flags);

	reg = readl(data->reg);
	writel(reg | BIT(SUN4I_CSI_RESET_BIT), data->reg);

	spin_unlock_irqrestore(data->lock, flags);

	return 0;
}

static int sun4i_csi_of_xlate(struct reset_controller_dev *rcdev,
			      const struct of_phandle_args *reset_spec)
{
	if (WARN_ON(reset_spec->args_count != 0))
		return -EINVAL;

	return 0;
}

static struct reset_control_ops sun4i_csi_reset_ops = {
	.assert		= sun4i_csi_assert,
	.deassert	= sun4i_csi_deassert,
};

static void __init sun4i_csi_clk_setup(struct device_node *node)
{
	const char *parents[SUN4I_CSI_PARENTS];
	const char *clk_name = node->name;
	struct csi_reset_data *reset_data;
	struct clk_divider *div;
	struct clk_gate *gate;
	struct clk_mux *mux;
	void __iomem *reg;
	struct clk *clk;
	int i = 0;

	reg = of_io_request_and_map(node, 0, of_node_full_name(node));
	if (IS_ERR(reg))
		return;

	of_property_read_string(node, "clock-output-names", &clk_name);

	i = of_clk_parent_fill(node, parents, SUN4I_CSI_PARENTS);

	mux = kzalloc(sizeof(*mux), GFP_KERNEL);
	if (!mux)
		goto err_unmap;

	mux->reg = reg;
	mux->shift = SUN4I_CSI_MUX_SHIFT;
	mux->table = sun4i_csi_mux_table;
	mux->lock = &sun4i_csi_lock;

	gate = kzalloc(sizeof(*gate), GFP_KERNEL);
	if (!gate)
		goto err_free_mux;

	gate->reg = reg;
	gate->bit_idx = SUN4I_CSI_GATE_BIT;
	gate->lock = &sun4i_csi_lock;

	div = kzalloc(sizeof(*div), GFP_KERNEL);
	if (!div)
		goto err_free_gate;

	div->reg = reg;
	div->shift = SUN4I_CSI_DIV_SHIFT;
	div->width = SUN4I_CSI_DIV_WIDTH;
	div->lock = &sun4i_csi_lock;

	clk = clk_register_composite(NULL, clk_name,
				     parents, i,
				     &mux->hw, &clk_mux_ops,
				     &div->hw, &clk_divider_ops,
				     &gate->hw, &clk_gate_ops,
				     CLK_SET_RATE_PARENT);
	if (IS_ERR(clk))
		goto err_free_div;

	of_clk_add_provider(node, of_clk_src_simple_get, clk);

	reset_data = kzalloc(sizeof(*reset_data), GFP_KERNEL);
	if (!reset_data)
		goto err_free_clk;

	reset_data->reg = reg;
	reset_data->lock = &sun4i_csi_lock;
	reset_data->rcdev.nr_resets = 1;
	reset_data->rcdev.ops = &sun4i_csi_reset_ops;
	reset_data->rcdev.of_node = node;
	reset_data->rcdev.of_xlate = sun4i_csi_of_xlate;
	reset_data->rcdev.of_reset_n_cells = 0;

	if (reset_controller_register(&reset_data->rcdev))
		goto err_free_reset;

	return;

err_free_reset:
	kfree(reset_data);
err_free_clk:
	clk_unregister(clk);
err_free_div:
	kfree(div);
err_free_gate:
	kfree(gate);
err_free_mux:
	kfree(mux);
err_unmap:
	iounmap(reg);
}

CLK_OF_DECLARE(sun4i_csi, "allwinner,sun4i-a10-csi-clk",
	       sun4i_csi_clk_setup);

