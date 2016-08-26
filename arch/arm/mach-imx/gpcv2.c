/*
 * Copyright 2016 Freescale Semiconductor, Inc.
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>

#include "common.h"

#define GPC_CPU_PGC_SW_PUP_REQ	0xf0
#define GPC_CPU_PGC_SW_PDN_REQ	0xfc
#define GPC_PGC_C1		0x840

#define BM_CPU_PGC_SW_PDN_PUP_REQ_CORE1_A7	0x2
#define BM_GPC_PGC_PCG				0x1

static void __iomem *gpcv2_base;

static void imx_gpcv2_set_m_core_pgc(bool enable, u32 offset)
{
	u32 val = readl_relaxed(gpcv2_base + offset) & (~BM_GPC_PGC_PCG);

	if (enable)
		val |= BM_GPC_PGC_PCG;

	writel_relaxed(val, gpcv2_base + offset);
}

void imx_gpcv2_set_core1_pdn_pup_by_software(bool pdn)
{
	u32 val = readl_relaxed(gpcv2_base + (pdn ?
		GPC_CPU_PGC_SW_PDN_REQ : GPC_CPU_PGC_SW_PUP_REQ));

	imx_gpcv2_set_m_core_pgc(true, GPC_PGC_C1);
	val |= BM_CPU_PGC_SW_PDN_PUP_REQ_CORE1_A7;
	writel_relaxed(val, gpcv2_base + (pdn ?
		GPC_CPU_PGC_SW_PDN_REQ : GPC_CPU_PGC_SW_PUP_REQ));

	while ((readl_relaxed(gpcv2_base + (pdn ?
		GPC_CPU_PGC_SW_PDN_REQ : GPC_CPU_PGC_SW_PUP_REQ)) &
		BM_CPU_PGC_SW_PDN_PUP_REQ_CORE1_A7) != 0)
		;
	imx_gpcv2_set_m_core_pgc(false, GPC_PGC_C1);
}

void __init imx_gpcv2_check_dt(void)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "fsl,imx7d-gpc");
	if (WARN_ON(!np))
		return;

	gpcv2_base = of_iomap(np, 0);
	WARN_ON(!gpcv2_base);
}
