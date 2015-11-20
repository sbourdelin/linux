/*
 * Copyright (c) 2015 Marvell Technology Group Ltd.
 *
 * Author: Jisheng Zhang <jszhang@marvell.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/clk-provider.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/spinlock_types.h>

#include "clk.h"

static DEFINE_SPINLOCK(berlin_gateclk_lock);

void __init berlin_gateclk_setup(struct device_node *np,
				 const struct gateclk_desc *descs,
				 struct clk_onecell_data *clk_data,
				 int n)
{
	int i, ret;
	void __iomem *base;
	struct clk **clks;

	clks = kcalloc(n, sizeof(struct clk *), GFP_KERNEL);
	if (!clks)
		return;

	base = of_iomap(np, 0);
	if (WARN_ON(!base))
		goto err_iomap;

	for (i = 0; i < n; i++) {
		struct clk *clk;

		clk = clk_register_gate(NULL, descs[i].name,
				descs[i].parent_name,
				descs[i].flags, base,
				descs[i].bit_idx, 0,
				&berlin_gateclk_lock);
		if (WARN_ON(IS_ERR(clk)))
			goto err_clk_register;
		clks[i] = clk;
	}

	clk_data->clks = clks;
	clk_data->clk_num = i;

	ret = of_clk_add_provider(np, of_clk_src_onecell_get, clk_data);
	if (WARN_ON(ret))
		goto err_clk_register;
	return;

err_clk_register:
	for (i = 0; i < n; i++)
		clk_unregister(clks[i]);
	iounmap(base);
err_iomap:
	kfree(clks);
}
