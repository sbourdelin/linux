/*
 * Copyright (c) 2015 Mediatek, Shunli Wang <shunli.wang@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pm_domain.h>
#include <linux/soc/mediatek/infracfg.h>
#include <dt-bindings/power/mt2701-power.h>

#include "mtk-scpsys.h"

#define SPM_VDE_PWR_CON			0x0210
#define SPM_MFG_PWR_CON			0x0214
#define SPM_ISP_PWR_CON			0x0238
#define SPM_DIS_PWR_CON			0x023C
#define SPM_CONN_PWR_CON		0x0280
#define SPM_BDP_PWR_CON			0x029C
#define SPM_ETH_PWR_CON			0x02A0
#define SPM_HIF_PWR_CON			0x02A4
#define SPM_IFR_MSC_PWR_CON		0x02A8
#define SPM_PWR_STATUS			0x060c
#define SPM_PWR_STATUS_2ND		0x0610

#define CONN_PWR_STA_MASK		BIT(1)
#define DIS_PWR_STA_MASK		BIT(3)
#define MFG_PWR_STA_MASK		BIT(4)
#define ISP_PWR_STA_MASK		BIT(5)
#define VDE_PWR_STA_MASK		BIT(7)
#define BDP_PWR_STA_MASK		BIT(14)
#define ETH_PWR_STA_MASK		BIT(15)
#define HIF_PWR_STA_MASK		BIT(16)
#define IFR_MSC_PWR_STA_MASK		BIT(17)

#define MT2701_TOP_AXI_PROT_EN_CONN	0x0104
#define MT2701_TOP_AXI_PROT_EN_DISP	0x0002

static const struct scp_domain_data scp_domain_data[] = {
	[MT2701_POWER_DOMAIN_CONN] = {
		.name = "conn",
		.sta_mask = CONN_PWR_STA_MASK,
		.ctl_offs = SPM_CONN_PWR_CON,
		.bus_prot_mask = MT2701_TOP_AXI_PROT_EN_CONN,
		.active_wakeup = true,
	},
	[MT2701_POWER_DOMAIN_DISP] = {
		.name = "disp",
		.sta_mask = DIS_PWR_STA_MASK,
		.ctl_offs = SPM_DIS_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.clk_id = {CLK_MM},
		.bus_prot_mask = MT2701_TOP_AXI_PROT_EN_DISP,
		.active_wakeup = true,
	},
	[MT2701_POWER_DOMAIN_MFG] = {
		.name = "mfg",
		.sta_mask = MFG_PWR_STA_MASK,
		.ctl_offs = SPM_MFG_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.active_wakeup = true,
	},
	[MT2701_POWER_DOMAIN_VDEC] = {
		.name = "vdec",
		.sta_mask = VDE_PWR_STA_MASK,
		.ctl_offs = SPM_VDE_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_MM},
		.active_wakeup = true,
	},
	[MT2701_POWER_DOMAIN_ISP] = {
		.name = "isp",
		.sta_mask = ISP_PWR_STA_MASK,
		.ctl_offs = SPM_ISP_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(13, 12),
		.active_wakeup = true,
	},
	[MT2701_POWER_DOMAIN_BDP] = {
		.name = "bdp",
		.sta_mask = BDP_PWR_STA_MASK,
		.ctl_offs = SPM_BDP_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.active_wakeup = true,
	},
	[MT2701_POWER_DOMAIN_ETH] = {
		.name = "eth",
		.sta_mask = ETH_PWR_STA_MASK,
		.ctl_offs = SPM_ETH_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.active_wakeup = true,
	},
	[MT2701_POWER_DOMAIN_HIF] = {
		.name = "hif",
		.sta_mask = HIF_PWR_STA_MASK,
		.ctl_offs = SPM_HIF_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.active_wakeup = true,
	},
	[MT2701_POWER_DOMAIN_IFR_MSC] = {
		.name = "ifr_msc",
		.sta_mask = IFR_MSC_PWR_STA_MASK,
		.ctl_offs = SPM_IFR_MSC_PWR_CON,
		.active_wakeup = true,
	},
};

#define NUM_DOMAINS	ARRAY_SIZE(scp_domain_data)

static int __init scpsys_probe(struct platform_device *pdev)
{
	struct scp *scp;

	scp = init_scp(pdev, scp_domain_data, NUM_DOMAINS);
	if (IS_ERR(scp))
		return PTR_ERR(scp);

	mtk_register_power_domains(pdev, scp, NUM_DOMAINS);

	return 0;
}

static const struct of_device_id of_scpsys_match_tbl[] = {
	{
		.compatible = "mediatek,mt2701-scpsys",
	}, {
		/* sentinel */
	}
};

static struct platform_driver scpsys_drv = {
	.driver = {
		.name = "mtk-scpsys-mt2701",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(of_scpsys_match_tbl),
	},
};

static int __init scpsys_drv_init(void)
{
	return platform_driver_probe(&scpsys_drv, scpsys_probe);
}

/*
 * There are some Mediatek drivers which depend on the power domain driver need
 * to probe in earlier initcall levels. So scpsys driver also need to probe
 * earlier.
 *
 * IOMMU(M4U) and SMI drivers for example. SMI is a bridge between IOMMU and
 * multimedia HW. IOMMU depends on SMI, and SMI is a power domain consumer,
 * so the proper probe sequence should be scpsys -> SMI -> IOMMU driver.
 * IOMMU drivers are initialized during subsys_init by default, so we need to
 * move SMI and scpsys drivers to subsys_init or earlier init levels.
 */
subsys_initcall(scpsys_drv_init);

MODULE_DESCRIPTION("MediaTek MT2701 scpsys driver");
MODULE_LICENSE("GPL v2");
