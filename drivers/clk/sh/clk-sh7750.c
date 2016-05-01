/*
 * Renesas SH7750/51 clock driver
 *
 * Copyright 2016 Yoshinori Sato <ysato@users.sourceforge.jp>
 */

#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/io.h>

struct clk *sh_div_clk_register(struct device *dev, const char *name,
				const char *parent_name,
				void __iomem *reg, u8 shift, u8 width,
				const struct clk_div_table *table,
				spinlock_t *lock);

static DEFINE_SPINLOCK(clklock);

static struct clk_div_table pdiv_table[] = {
	{ .val = 0, .div = 2, },
	{ .val = 1, .div = 3, },
	{ .val = 2, .div = 4, },
	{ .val = 3, .div = 6, },
	{ .val = 4, .div = 8, },
	{ .val = 0, .div = 0, },
};

static struct clk_div_table div_table[] = {
	{ .val = 0, .div = 1, },
	{ .val = 1, .div = 2, },
	{ .val = 2, .div = 3, },
	{ .val = 3, .div = 4, },
	{ .val = 4, .div = 6, },
	{ .val = 5, .div = 8, },
	{ .val = 0, .div = 0, },
};

struct pll_clock {
	struct clk_hw hw;
	void __iomem *freqcr;
	void __iomem *wdt;
	int mult;
};

#define to_pll_clock(_hw) container_of(_hw, struct pll_clock, hw)

static unsigned long pll_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct pll_clock *pll_clock = to_pll_clock(hw);

	if ((ioread16(pll_clock->freqcr) >> 9) & 1)
		return parent_rate * pll_clock->mult;
	else
		return parent_rate;
}

static long pll_round_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long *prate)
{
	struct pll_clock *pll_clock = to_pll_clock(hw);
	int mul;

	mul = rate / *prate;
	mul = (pll_clock->mult / 2 < mul)?pll_clock->mult:1;
	return *prate * mul;
}

static int pll_set_rate(struct clk_hw *hw, unsigned long rate,
			unsigned long parent_rate)
{
	int mult;
	unsigned char val;
	unsigned long flags;
	struct pll_clock *pll_clock = to_pll_clock(hw);

	mult = rate / parent_rate;
	if (mult > 1) {
		/* PLL enable */
		/* required stable time */
		spin_lock_irqsave(&clklock, flags);
		iowrite16(0x5a00, pll_clock->wdt);
		iowrite16(0xa503, pll_clock->wdt + 2);
		val = ioread16(pll_clock->freqcr);
		val |= 0x0200;
		iowrite16(val, pll_clock->freqcr);
		spin_unlock_irqrestore(&clklock, flags);
	} else {
		/* PLL disable */
		/* not required stable time */
		val = ioread16(pll_clock->freqcr);
		val &= ~0x0200;
		iowrite16(val, pll_clock->freqcr);
	}
	return 0;
}

static const struct clk_ops pll_ops = {
	.recalc_rate = pll_recalc_rate,
	.round_rate = pll_round_rate,
	.set_rate = pll_set_rate,
};

static void __init sh7750_pll_clk_setup(struct device_node *node)
{
	unsigned int num_parents;
	struct clk *clk;
	const char *clk_name = node->name;
	const char *parent_name;
	struct pll_clock *pll_clock;
	struct clk_init_data init;

	num_parents = of_clk_get_parent_count(node);
	if (num_parents < 1) {
		pr_err("%s: no parent found", clk_name);
		return;
	}

	pll_clock = kzalloc(sizeof(struct pll_clock), GFP_KERNEL);
	if (!pll_clock) {
		pr_err("%s: failed to alloc memory", clk_name);
		return;
	}

	pll_clock->freqcr = of_iomap(node, 0);
	if (pll_clock->freqcr == NULL) {
		pr_err("%s: failed to map frequenct control register",
		       clk_name);
		goto free_clock;
	}

	pll_clock->wdt = of_iomap(node, 1);
	if (pll_clock->wdt == NULL) {
		pr_err("%s: failed to map watchdog register", clk_name);
		goto unmap_freqcr;
	}

	of_property_read_u32_index(node, "renesas,mult", 0, &pll_clock->mult);

	parent_name = of_clk_get_parent_name(node, 0);
	init.name = clk_name;
	init.ops = &pll_ops;
	init.flags = CLK_IS_BASIC;
	init.parent_names = &parent_name;
	init.num_parents = 1;
	pll_clock->hw.init = &init;

	clk = clk_register(NULL, &pll_clock->hw);
	if (IS_ERR(clk)) {
		pr_err("%s: failed to register %s pll clock (%ld)\n",
		       __func__, clk_name, PTR_ERR(clk));
		goto unmap_wdt;
	}

	of_clk_add_provider(node, of_clk_src_simple_get, clk);
	return;

unmap_wdt:
	iounmap(pll_clock->wdt);
unmap_freqcr:
	iounmap(pll_clock->freqcr);
free_clock:
	kfree(pll_clock);
}

static void __init sh7750_div_clk_setup(struct device_node *node)
{
	unsigned int num_parents;
	struct clk *clk;
	const char *clk_name = node->name;
	const char *parent_name;
	void __iomem *freqcr = NULL;
	int i;
	int num_clks;
	int offset;

	num_parents = of_clk_get_parent_count(node);
	if (num_parents < 1) {
		pr_err("%s: no parent found", clk_name);
		return;
	}

	num_clks = of_property_count_strings(node, "clock-output-names");
	if (num_clks < 0) {
		pr_err("%s: failed to count clocks", clk_name);
		return;
	}

	freqcr = of_iomap(node, 0);
	if (freqcr == NULL) {
		pr_err("%s: failed to map divide register", clk_name);
		goto error;
	}
	of_property_read_u32_index(node, "renesas,offset", 0, &offset);

	parent_name = of_clk_get_parent_name(node, 0);
	for (i = 0; i < num_clks; i++) {
		of_property_read_string_index(node, "clock-output-names", i,
					      &clk_name);
		clk = sh_div_clk_register(NULL, clk_name, parent_name,
					  freqcr,
					  offset, 3,
					  (offset == 0)?pdiv_table:div_table,
					  &clklock);
		if (IS_ERR(clk))
			pr_err("%s: failed to register %s div clock (%ld)\n",
			       __func__, clk_name, PTR_ERR(clk));
		else
			of_clk_add_provider(node, of_clk_src_simple_get, clk);
	}
error:
	if (freqcr)
		iounmap(freqcr);
}

CLK_OF_DECLARE(sh7750_div_clk, "renesas,sh7750-div-clock",
	       sh7750_div_clk_setup);
CLK_OF_DECLARE(sh7750_pll_clk, "renesas,sh7750-pll-clock",
	       sh7750_pll_clk_setup);
