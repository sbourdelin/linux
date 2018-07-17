// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015 Pengutronix, Sascha Hauer <kernel@pengutronix.de>
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

#include <linux/export.h>
#include <linux/jiffies.h>
#include <linux/regmap.h>
#include <linux/soc/mediatek/infracfg.h>
#include <linux/soc/mediatek/scpsys-ext.h>
#include <asm/processor.h>

#define MTK_POLL_DELAY_US   10
#define MTK_POLL_TIMEOUT    (jiffies_to_usecs(HZ))

#define INFRA_TOPAXI_PROTECTEN		0x0220
#define INFRA_TOPAXI_PROTECTSTA1	0x0228
#define INFRA_TOPAXI_PROTECTEN_SET	0x0260
#define INFRA_TOPAXI_PROTECTEN_CLR	0x0264

/**
 * mtk_generic_set_cmd - enable bus protection with set register
 * @regmap: The bus protect regmap
 * @set_ofs: The set register offset to set corresponding bit to 1.
 * @sta_ofs: The status register offset to show bus protect enable/disable.
 * @mask: The mask containing the protection bits to be disabled.
 *
 * This function enables the bus protection bits for disabled power
 * domains so that the system does not hang when some unit accesses the
 * bus while in power down.
 */
int mtk_generic_set_cmd(struct regmap *regmap, u32 set_ofs,
			u32 sta_ofs, u32 mask)
{
	u32 val;
	int ret;

	regmap_write(regmap, set_ofs, mask);

	ret = regmap_read_poll_timeout(regmap, sta_ofs, val,
				       (val & mask) == mask,
				       MTK_POLL_DELAY_US,
				       MTK_POLL_TIMEOUT);

	return ret;
}

/**
 * mtk_generic_clr_cmd - disable bus protection  with clr register
 * @regmap: The bus protect regmap
 * @clr_ofs: The clr register offset to clear corresponding bit to 0.
 * @sta_ofs: The status register offset to show bus protect enable/disable.
 * @mask: The mask containing the protection bits to be disabled.
 *
 * This function disables the bus protection bits previously enabled with
 * mtk_set_bus_protection.
 */

int mtk_generic_clr_cmd(struct regmap *regmap, u32 clr_ofs,
			u32 sta_ofs, u32 mask)
{
	int ret;
	u32 val;

	regmap_write(regmap, clr_ofs, mask);

	ret = regmap_read_poll_timeout(regmap, sta_ofs, val,
				       !(val & mask),
				       MTK_POLL_DELAY_US,
				       MTK_POLL_TIMEOUT);
	return ret;
}

/**
 * mtk_generic_enable_cmd - enable bus protection with upd register
 * @regmap: The bus protect regmap
 * @upd_ofs: The update register offset to directly rewrite value to
 *              corresponding bit.
 * @sta_ofs: The status register offset to show bus protect enable/disable.
 * @mask: The mask containing the protection bits to be disabled.
 *
 * This function enables the bus protection bits for disabled power
 * domains so that the system does not hang when some unit accesses the
 * bus while in power down.
 */
int mtk_generic_enable_cmd(struct regmap *regmap, u32 upd_ofs,
			   u32 sta_ofs, u32 mask)
{
	u32 val;
	int ret;

	regmap_update_bits(regmap, upd_ofs, mask, mask);

	ret = regmap_read_poll_timeout(regmap, sta_ofs, val,
				       (val & mask) == mask,
				       MTK_POLL_DELAY_US,
				       MTK_POLL_TIMEOUT);
	return ret;
}

/**
 * mtk_generic_disable_cmd - disable bus protection with updd register
 * @regmap: The bus protect regmap
 * @upd_ofs: The update register offset to directly rewrite value to
 *              corresponding bit.
 * @sta_ofs: The status register offset to show bus protect enable/disable.
 * @mask: The mask containing the protection bits to be disabled.
 *
 * This function disables the bus protection bits previously enabled with
 * mtk_set_bus_protection.
 */

int mtk_generic_disable_cmd(struct regmap *regmap, u32 upd_ofs,
			    u32 sta_ofs, u32 mask)
{
	int ret;
	u32 val;

	regmap_update_bits(regmap, upd_ofs, mask, 0);

	ret = regmap_read_poll_timeout(regmap, sta_ofs,
				       val, !(val & mask),
				       MTK_POLL_DELAY_US,
				       MTK_POLL_TIMEOUT);
	return ret;
}

/**
 * mtk_set_bus_protection - enable bus protection
 * @infracfg: The bus protect regmap, default use infracfg
 * @mask: The mask containing the protection bits to be enabled.
 *
 * This function enables the bus protection bits for disabled power
 * domains so that the system does not hang when some unit accesses the
 * bus while in power down.
 */
int mtk_infracfg_set_bus_protection(struct regmap *infracfg, u32 mask)
{
	return mtk_generic_set_cmd(infracfg,
				   INFRA_TOPAXI_PROTECTEN_SET,
				   INFRA_TOPAXI_PROTECTSTA1,
				   mask);
}

/**
 * mtk_clear_bus_protection - disable bus protection
 * @infracfg: The bus protect regmap, default use infracfg
 * @mask: The mask containing the protection bits to be disabled.
 *
 * This function disables the bus protection bits previously enabled with
 * mtk_set_bus_protection.
 */

int mtk_infracfg_clear_bus_protection(struct regmap *infracfg, u32 mask)
{
	return mtk_generic_clr_cmd(infracfg,
				   INFRA_TOPAXI_PROTECTEN_CLR,
				   INFRA_TOPAXI_PROTECTSTA1,
				   mask);
}

/**
 * mtk_infracfg_enable_bus_protection - enable bus protection
 * @infracfg: The bus protect regmap, default use infracfg
 * @mask: The mask containing the protection bits to be disabled.
 *
 * This function enables the bus protection bits for disabled power
 * domains so that the system does not hang when some unit accesses the
 * bus while in power down.
 */
int mtk_infracfg_enable_bus_protection(struct regmap *infracfg, u32 mask)
{
	return mtk_generic_enable_cmd(infracfg,
				      INFRA_TOPAXI_PROTECTEN,
				      INFRA_TOPAXI_PROTECTSTA1,
				      mask);
}

/**
 * mtk_infracfg_disable_bus_protection - disable bus protection
 * @infracfg: The bus protect regmap, default use infracfg
 * @mask: The mask containing the protection bits to be disabled.
 *
 * This function disables the bus protection bits previously enabled with
 * mtk_infracfg_set_bus_protection.
 */

int mtk_infracfg_disable_bus_protection(struct regmap *infracfg, u32 mask)
{
	return mtk_generic_disable_cmd(infracfg,
				       INFRA_TOPAXI_PROTECTEN,
				       INFRA_TOPAXI_PROTECTSTA1,
				       mask);
}
