/*
 * Spreadtrum clock set/clear gate driver
 *
 * Copyright (C) 2015~2017 spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/spinlock.h>
#include <linux/hwspinlock.h>

DEFINE_SPINLOCK(gate_lock);

#define to_clk_gate(_hw) container_of(_hw, struct clk_gate, hw)
#define CLK_GATE_HWSPINLOCK		BIT(7)
#define GLB_CLK_HWSPINLOCK_TIMEOUT	5000

static struct hwspinlock		*glb_clk_hw_lock;

static void sprd_clk_lock(struct clk_gate *gate,
			unsigned long flags)
{
	if (gate->lock)
		spin_lock_irqsave(gate->lock, flags);
	else
		__acquire(gate->lock);
}

static void sprd_clk_unlock(struct clk_gate *gate,
			unsigned long flags)
{
	if (gate->lock)
		spin_unlock_irqrestore(gate->lock, flags);
	else
		__release(gate->lock);
}

static void sprd_clk_hw_lock(struct clk_gate *gate,
				unsigned long *flags)
{
	int ret = 0;

	if (glb_clk_hw_lock && (gate->flags & CLK_GATE_HWSPINLOCK)) {
		ret = hwspin_lock_timeout_irqsave(glb_clk_hw_lock,
					  GLB_CLK_HWSPINLOCK_TIMEOUT,
					  flags);
		if (ret)
			pr_err("glb_clk:%s lock the hwlock failed.\n",
			       __clk_get_name(gate->hw.clk));
		return;
	}

	sprd_clk_lock(gate, *flags);
}

static void sprd_clk_hw_unlock(struct clk_gate *gate,
				unsigned long *flags)
{
	if (glb_clk_hw_lock && (gate->flags & CLK_GATE_HWSPINLOCK)) {
		hwspin_unlock_irqrestore(glb_clk_hw_lock, flags);
		return;
	}

	sprd_clk_unlock(gate, *flags);
}

static void sprd_clk_gate_endisable(struct clk_hw *hw, int enable)
{
	struct clk_gate *gate = to_clk_gate(hw);
	int set = gate->flags & CLK_GATE_SET_TO_DISABLE ? 1 : 0;
	unsigned long flags = 0;
	u32 reg;

	set ^= enable;

	sprd_clk_hw_lock(gate, &flags);

	reg = clk_readl(gate->reg);

	if (set)
		reg |= BIT(gate->bit_idx);
	else
		reg &= ~BIT(gate->bit_idx);

	clk_writel(reg, gate->reg);

	sprd_clk_hw_unlock(gate, &flags);
}

static int sprd_clk_gate_enable(struct clk_hw *hw)
{
	sprd_clk_gate_endisable(hw, 1);

	return 0;
}

static void sprd_clk_gate_disable(struct clk_hw *hw)
{
	sprd_clk_gate_endisable(hw, 0);
}

static int sprd_clk_gate_is_enabled(struct clk_hw *hw)
{
	u32 reg;
	struct clk_gate *gate = to_clk_gate(hw);

	reg = clk_readl(gate->reg);

	if (gate->flags & CLK_GATE_SET_TO_DISABLE)
		reg ^= BIT(gate->bit_idx);

	reg &= BIT(gate->bit_idx);

	return reg ? 1 : 0;
}

const struct clk_ops sprd_clk_gate_ops = {
	.enable = sprd_clk_gate_enable,
	.disable = sprd_clk_gate_disable,
	.is_enabled = sprd_clk_gate_is_enabled,
};

static void sprd_clk_sc_gate_endisable(struct clk_hw *hw, int enable,
				       unsigned int offset)
{
	struct clk_gate *gate = to_clk_gate(hw);
	int set = gate->flags & CLK_GATE_SET_TO_DISABLE ? 1 : 0;
	unsigned long flags = 0;
	void __iomem *reg;

	set ^= enable;

	sprd_clk_lock(gate, flags);

	/*
	 * Each gate clock has three registers:
	 * gate->reg			- base register
	 * gate->reg + offset		- set register
	 * gate->reg + 2 * offset	- clear register
	 */
	reg = set ? gate->reg + offset : gate->reg + 2 * offset;
	clk_writel(BIT(gate->bit_idx), reg);

	sprd_clk_unlock(gate, flags);

}

static int sprd_clk_sc100_gate_enable(struct clk_hw *hw)
{
	sprd_clk_sc_gate_endisable(hw, 1, 0x100);

	return 0;
}

static void sprd_clk_sc100_gate_disable(struct clk_hw *hw)
{
	sprd_clk_sc_gate_endisable(hw, 0, 0x100);
}

static int sprd_clk_sc1000_gate_enable(struct clk_hw *hw)
{
	sprd_clk_sc_gate_endisable(hw, 1, 0x1000);

	return 0;
}

static void sprd_clk_sc1000_gate_disable(struct clk_hw *hw)
{
	sprd_clk_sc_gate_endisable(hw, 0, 0x1000);
}

const struct clk_ops sprd_clk_sc100_gate_ops = {
	.enable = sprd_clk_sc100_gate_enable,
	.disable = sprd_clk_sc100_gate_disable,
	.is_enabled = sprd_clk_gate_is_enabled,
};

const struct clk_ops sprd_clk_sc1000_gate_ops = {
	.enable = sprd_clk_sc1000_gate_enable,
	.disable = sprd_clk_sc1000_gate_disable,
	.is_enabled = sprd_clk_gate_is_enabled,
};

static struct clk *sprd_clk_register_gate(struct device *dev,
		const char *name, const char *parent_name,
		unsigned long flags, void __iomem *reg,
		u8 bit_idx, u8 clk_gate_flags,
		spinlock_t *lock, const struct clk_ops *ops)
{
	struct clk_gate *gate;
	struct clk *clk;
	struct clk_init_data init;

	gate = kzalloc(sizeof(struct clk_gate), GFP_KERNEL);
	if (!gate)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = ops;
	init.flags = flags | CLK_IS_BASIC;
	init.parent_names = parent_name ? &parent_name : NULL;
	init.num_parents = parent_name ? 1 : 0;

	gate->reg = reg;
	gate->bit_idx = bit_idx;
	gate->flags = clk_gate_flags;
	gate->lock = lock;
	gate->hw.init = &init;

	clk = clk_register(dev, &gate->hw);

	if (IS_ERR(clk))
		kfree(gate);

	return clk;
}

static void __init sprd_clk_gates_setup(struct device_node *node,
					   const struct clk_ops *ops)
{
	const char *clk_name = NULL;
	void __iomem *reg;
	const char *parent_name;
	unsigned long flags = CLK_IGNORE_UNUSED;
	u8 gate_flags = 0;
	u32 index;
	int number, i = 0;
	struct resource res;
	struct clk_onecell_data *clk_data;
	struct property *prop;
	const __be32 *p;

	if (of_address_to_resource(node, 0, &res)) {
		pr_err("%s: no DT registers found for %s\n",
		       __func__, node->full_name);
		return;
	}

	/*
	 * bit[1:0] represents the gate flags, but bit[1] is not used
	 * for the time being.
	 */
	if (res.start & 0x3) {
		res.start &= ~0x3;
		gate_flags |= CLK_GATE_SET_TO_DISABLE;
	}
	reg = ioremap(res.start, resource_size(&res));
	if (!reg) {
		pr_err("%s: gates clock[%s] ioremap failed!\n",
		       __func__, node->full_name);
		return;
	}

	parent_name = of_clk_get_parent_name(node, 0);

	clk_data = kmalloc(sizeof(struct clk_onecell_data), GFP_KERNEL);
	if (!clk_data)
		goto iounmap_reg;

	number = of_property_count_u32_elems(node, "clock-indices");
	if (number > 0) {
		of_property_read_u32_index(node, "clock-indices",
					   number - 1, &number);
		number += 1;
	} else {
		number = of_property_count_strings(node, "clock-output-names");
	}

	clk_data->clks = kcalloc(number, sizeof(struct clk *),
				 GFP_KERNEL);
	if (!clk_data->clks)
		goto kfree_clk_data;

	/*
	 * If the identifying number for the clocks in the node is not
	 * linear from zero, then we use clock-indices mapping
	 * identifiers into the clock-output-names array in DT.
	 */
	if (of_property_count_u32_elems(node, "clock-indices") > 0) {
		of_property_for_each_u32(node, "clock-indices",
					 prop, p, index) {
			of_property_read_string_index(node,
						      "clock-output-names",
						      i++, &clk_name);

			clk_data->clks[index] = sprd_clk_register_gate(NULL,
						clk_name, parent_name, flags,
						reg, index, gate_flags,
						&gate_lock, ops);
			WARN_ON(IS_ERR(clk_data->clks[index]));

			clk_register_clkdev(clk_data->clks[index], clk_name,
					    NULL);
		}
	} else {
		of_property_for_each_string(node, "clock-output-names",
					    prop, clk_name) {
			clk_data->clks[i] = sprd_clk_register_gate(NULL,
						clk_name, parent_name, flags,
						reg, i,	gate_flags,
						&gate_lock, ops);
			WARN_ON(IS_ERR(clk_data->clks[i]));

			clk_register_clkdev(clk_data->clks[i], clk_name, NULL);
			i++;
		}
	}

	clk_data->clk_num = number;
	if (number == 1)
		of_clk_add_provider(node, of_clk_src_simple_get, clk_data);
	else
		of_clk_add_provider(node, of_clk_src_onecell_get, clk_data);
	return;

kfree_clk_data:
	kfree(clk_data);

iounmap_reg:
	iounmap(reg);
}

static void __init sprd_sc100_clk_gates_setup(struct device_node *node)
{
	sprd_clk_gates_setup(node, &sprd_clk_sc100_gate_ops);
}

static void __init sprd_sc1000_clk_gates_setup(struct device_node *node)
{
	sprd_clk_gates_setup(node, &sprd_clk_sc1000_gate_ops);
}

static void __init sprd_trad_clk_gates_setup(struct device_node *node)
{
	sprd_clk_gates_setup(node, &sprd_clk_gate_ops);
}

CLK_OF_DECLARE(gates_clock, "sprd,gates-clock",
		sprd_trad_clk_gates_setup);
CLK_OF_DECLARE(sc100_gates_clock, "sprd,sc100-gates-clock",
	       sprd_sc100_clk_gates_setup);
CLK_OF_DECLARE(sc1000_gates_clock, "sprd,sc1000-gates-clock",
	       sprd_sc1000_clk_gates_setup);

#ifdef CONFIG_SPRD_HWSPINLOCK
static int __init sprd_clk_hwspinlock_init(void)
{
	/*
	 * glb_clk belongs to the global registers, so it can use the
	 * same hwspinlock
	 */
	glb_clk_hw_lock = hwspin_lock_get_used(1);
	if (!glb_clk_hw_lock) {
		pr_err("%s: Can't get the hardware spinlock.\n", __func__);
		return -ENXIO;
	}

	return 0;
}
subsys_initcall_sync(sprd_clk_hwspinlock_init);
#endif
