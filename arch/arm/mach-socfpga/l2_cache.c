/*
 * Copyright Altera Corporation (C) 2016. All rights reserved.
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
#include <linux/io.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>

#include "core.h"

/* A10 System Manager ECC interrupt mask control registers */
#define A10_L2_ECC_CTRL_OFST            0x0

#define A10_SYSMGR_ECC_INTMASK_CLR_OFST 0x98
#define A10_L2_ECC_INT_CLR_OFST         0xA8

#define A10_MPU_CTRL_L2_ECC_EN          BIT(0)
#define A10_ECC_INTMASK_CLR_EN          BIT(0)
#define A10_ECC_INT_CLR                 (BIT(31) | BIT(15))

void socfpga_init_l2_ecc(void)
{
	struct device_node *np;
	void __iomem *mapped_l2_edac_addr;
	const char *compat = "altr,socfpga-l2-ecc";

	if (of_machine_is_compatible("altr,socfpga-arria10"))
		compat = "altr,socfpga-a10-l2-ecc";

	/* Find the L2 EDAC device tree node */
	np = of_find_compatible_node(NULL, NULL, compat);
	if (!np) {
		pr_err("Unable to find %s in dtb\n", compat);
		return;
	}

	mapped_l2_edac_addr = of_iomap(np, 0);
	of_node_put(np);
	if (!mapped_l2_edac_addr) {
		pr_err("Unable to find L2 ECC mapping in dtb\n");
		return;
	}

	if (of_machine_is_compatible("altr,socfpga-arria10")) {
		if (!sys_manager_base_addr) {
			pr_err("System Mananger not mapped for L2 ECC\n");
			goto exit;
		}
		/* Clear any pending IRQs */
		writel(A10_ECC_INT_CLR, (sys_manager_base_addr +
					 A10_L2_ECC_INT_CLR_OFST));
		/* Enable ECC */
		writel(A10_ECC_INTMASK_CLR_EN, sys_manager_base_addr +
		       A10_SYSMGR_ECC_INTMASK_CLR_OFST);
		writel(A10_MPU_CTRL_L2_ECC_EN, mapped_l2_edac_addr +
		       A10_L2_ECC_CTRL_OFST);
	} else {
		/* Enable ECC */
		writel(0x01, mapped_l2_edac_addr);
	}

exit:
	iounmap(mapped_l2_edac_addr);
}
