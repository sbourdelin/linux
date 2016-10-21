/*
 * Copyright (C) 2016 Freescale Semiconductor, Inc.
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>

static void __iomem *ocotp_base;

void __init imx_ocotp_init(const char *compat)
{
	struct device_node *ocotp_np;

	ocotp_np = of_find_compatible_node(NULL, NULL, compat);
	if (!ocotp_np) {
		pr_warn("failed to find ocotp node\n");
		return;
	}

	ocotp_base = of_iomap(ocotp_np, 0);
	if (!ocotp_base)
		pr_warn("failed to map ocotp\n");

	of_node_put(ocotp_np);
}

u32 imx_ocotp_read(u32 offset)
{
	if (WARN_ON(!ocotp_base))
		return 0;

	return readl_relaxed(ocotp_base + offset);
}
