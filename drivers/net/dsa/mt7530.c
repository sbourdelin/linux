/*
 * Mediatek MT7530 DSA Switch driver
 * Copyright (C) 2017 Sean Wang <sean.wang@mediatek.com>
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
#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/mdio.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of_gpio.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/phy.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <net/dsa.h>
#include <net/switchdev.h>

#include "mt7530.h"

/* String, offset, and register size in bytes if different from 4 bytes */
static const struct mt7530_mib_desc mt7530_mib[] = {
	MIB_DESC(1, 0x00, "TxDrop"),
	MIB_DESC(1, 0x04, "TxCrcErr"),
	MIB_DESC(1, 0x08, "TxUnicast"),
	MIB_DESC(1, 0x0c, "TxMulticast"),
	MIB_DESC(1, 0x10, "TxBroadcast"),
	MIB_DESC(1, 0x14, "TxCollision"),
	MIB_DESC(1, 0x18, "TxSingleCollision"),
	MIB_DESC(1, 0x1c, "TxMultipleCollision"),
	MIB_DESC(1, 0x20, "TxDeferred"),
	MIB_DESC(1, 0x24, "TxLateCollision"),
	MIB_DESC(1, 0x28, "TxExcessiveCollistion"),
	MIB_DESC(1, 0x2c, "TxPause"),
	MIB_DESC(1, 0x30, "TxPktSz64"),
	MIB_DESC(1, 0x34, "TxPktSz65To127"),
	MIB_DESC(1, 0x38, "TxPktSz128To255"),
	MIB_DESC(1, 0x3c, "TxPktSz256To511"),
	MIB_DESC(1, 0x40, "TxPktSz512To1023"),
	MIB_DESC(1, 0x44, "Tx1024ToMax"),
	MIB_DESC(2, 0x48, "TxBytes"),
	MIB_DESC(1, 0x60, "RxDrop"),
	MIB_DESC(1, 0x64, "RxFiltering"),
	MIB_DESC(1, 0x6c, "RxMulticast"),
	MIB_DESC(1, 0x70, "RxBroadcast"),
	MIB_DESC(1, 0x74, "RxAlignErr"),
	MIB_DESC(1, 0x78, "RxCrcErr"),
	MIB_DESC(1, 0x7c, "RxUnderSizeErr"),
	MIB_DESC(1, 0x80, "RxFragErr"),
	MIB_DESC(1, 0x84, "RxOverSzErr"),
	MIB_DESC(1, 0x88, "RxJabberErr"),
	MIB_DESC(1, 0x8c, "RxPause"),
	MIB_DESC(1, 0x90, "RxPktSz64"),
	MIB_DESC(1, 0x94, "RxPktSz65To127"),
	MIB_DESC(1, 0x98, "RxPktSz128To255"),
	MIB_DESC(1, 0x9c, "RxPktSz256To511"),
	MIB_DESC(1, 0xa0, "RxPktSz512To1023"),
	MIB_DESC(1, 0xa4, "RxPktSz1024ToMax"),
	MIB_DESC(2, 0xa8, "RxBytes"),
	MIB_DESC(1, 0xb0, "RxCtrlDrop"),
	MIB_DESC(1, 0xb4, "RxIngressDrop"),
	MIB_DESC(1, 0xb8, "RxArlDrop"),
};

static int
mt7623_trgmii_write(struct mt7530_priv *priv,  u32 reg, u32 val)
{
	int ret;

	ret =  regmap_write(priv->ethernet, TRGMII_BASE(reg), val);
	if (ret < 0)
		dev_err(priv->dev,
			"failed to priv write register\n");
	return ret;
}

static u32
mt7623_trgmii_read(struct mt7530_priv *priv, u32 reg)
{
	int ret;
	u32 val;

	ret = regmap_read(priv->ethernet, TRGMII_BASE(reg), &val);
	if (ret < 0) {
		dev_err(priv->dev,
			"failed to priv read register\n");
		return ret;
	}

	return val;
}

static void
mt7623_trgmii_rmw(struct mt7530_priv *priv, u32 reg,
		  u32 mask, u32 set)
{
	u32 val;

	val = mt7623_trgmii_read(priv, reg);
	val &= ~mask;
	val |= set;
	mt7623_trgmii_write(priv, reg, val);
}

static void
mt7623_trgmii_set(struct mt7530_priv *priv, u32 reg, u32 val)
{
	mt7623_trgmii_rmw(priv, reg, 0, val);
}

static void
mt7623_trgmii_clear(struct mt7530_priv *priv, u32 reg, u32 val)
{
	mt7623_trgmii_rmw(priv, reg, val, 0);
}

static int
core_read_mmd_indirect(struct mt7530_priv *priv, int prtad, int devad)
{
	struct mii_bus *bus = priv->bus;
	int value, ret;

	/* Write the desired MMD Devad */
	ret = bus->write(bus, 0, MII_MMD_CTRL, devad);
	if (ret < 0)
		goto err;

	/* Write the desired MMD register address */
	ret = bus->write(bus, 0, MII_MMD_DATA, prtad);
	if (ret < 0)
		goto err;

	/* Select the Function : DATA with no post increment */
	ret = bus->write(bus, 0, MII_MMD_CTRL, (devad | MII_MMD_CTRL_NOINCR));
	if (ret < 0)
		goto err;

	/* Read the content of the MMD's selected register */
	value = bus->read(bus, 0, MII_MMD_DATA);

	return value;
err:
	dev_err(&bus->dev,  "failed to read mmd register\n");

	return ret;
}

static int
core_write_mmd_indirect(struct mt7530_priv *priv, int prtad,
			int devad, u32 data)
{
	struct mii_bus *bus = priv->bus;
	int ret;

	/* Write the desired MMD Devad */
	ret = bus->write(bus, 0, MII_MMD_CTRL, devad);
	if (ret < 0)
		goto err;

	/* Write the desired MMD register address */
	ret = bus->write(bus, 0, MII_MMD_DATA, prtad);
	if (ret < 0)
		goto err;

	/* Select the Function : DATA with no post increment */
	ret = bus->write(bus, 0, MII_MMD_CTRL, (devad | MII_MMD_CTRL_NOINCR));
	if (ret < 0)
		goto err;

	/* Write the data into MMD's selected register */
	ret = bus->write(bus, 0, MII_MMD_DATA, data);
err:
	if (ret < 0)
		dev_err(&bus->dev,
			"failed to write mmd register\n");
	return ret;
}

static void
core_write(struct mt7530_priv *priv, u32 reg, u32 val)
{
	struct mii_bus *bus = priv->bus;

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	core_write_mmd_indirect(priv, reg, MDIO_MMD_VEND2, val);

	mutex_unlock(&bus->mdio_lock);
}

static void
core_rmw(struct mt7530_priv *priv, u32 reg, u32 mask, u32 set)
{
	struct mii_bus *bus = priv->bus;
	u32 val;

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	val = core_read_mmd_indirect(priv, reg, MDIO_MMD_VEND2);
	val &= ~mask;
	val |= set;
	core_write_mmd_indirect(priv, reg, MDIO_MMD_VEND2, val);

	mutex_unlock(&bus->mdio_lock);
}

static void
core_set(struct mt7530_priv *priv, u32 reg, u32 val)
{
	core_rmw(priv, reg, 0, val);
}

static void
core_clear(struct mt7530_priv *priv, u32 reg, u32 val)
{
	core_rmw(priv, reg, val, 0);
}

static int
mt7530_mii_write(struct mt7530_priv *priv, u32 reg, u32 val)
{
	struct mii_bus *bus = priv->bus;
	u16 page, r, lo, hi;
	int ret;

	page = (reg >> 6) & 0x3ff;
	r  = (reg >> 2) & 0xf;
	lo = val & 0xffff;
	hi = val >> 16;

	/* MT7530 uses 31 as the pseudo port */
	ret = bus->write(bus, 0x1f, 0x1f, page);
	if (ret < 0)
		goto err;

	ret = bus->write(bus, 0x1f, r,  lo);
	if (ret < 0)
		goto err;

	ret = bus->write(bus, 0x1f, 0x10, hi);
err:
	if (ret < 0)
		dev_err(&bus->dev,
			"failed to write mt7530 register\n");
	return ret;
}

static u32
mt7530_mii_read(struct mt7530_priv *priv, u32 reg)
{
	struct mii_bus *bus = priv->bus;
	u16 page, r, lo, hi;
	int ret;

	page = (reg >> 6) & 0x3ff;
	r = (reg >> 2) & 0xf;

	/* MT7530 uses 31 as the pseudo port */
	ret = bus->write(bus, 0x1f, 0x1f, page);
	if (ret < 0) {
		dev_err(&bus->dev,
			"failed to read mt7530 register\n");
		return ret;
	}

	lo = bus->read(bus, 0x1f, r);
	hi = bus->read(bus, 0x1f, 0x10);

	return (hi << 16) | (lo & 0xffff);
}

static void
mt7530_write(struct mt7530_priv *priv, u32 reg, u32 val)
{
	struct mii_bus *bus = priv->bus;

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	mt7530_mii_write(priv, reg, val);

	mutex_unlock(&bus->mdio_lock);
}

static u32
mt7530_read(struct mt7530_priv *priv, u32 reg)
{
	struct mii_bus *bus = priv->bus;
	u32 val;

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	val = mt7530_mii_read(priv, reg);

	mutex_unlock(&bus->mdio_lock);

	return val;
}

static void
mt7530_rmw(struct mt7530_priv *priv, u32 reg,
	   u32 mask, u32 set)
{
	struct mii_bus *bus = priv->bus;
	u32 val;

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	val = mt7530_mii_read(priv, reg);
	val &= ~mask;
	val |= set;
	mt7530_mii_write(priv, reg, val);

	mutex_unlock(&bus->mdio_lock);
}

static void
mt7530_set(struct mt7530_priv *priv, u32 reg, u32 val)
{
	mt7530_rmw(priv, reg, 0, val);
}

static void
mt7530_clear(struct mt7530_priv *priv, u32 reg, u32 val)
{
	mt7530_rmw(priv, reg, val, 0);
}

static int
mt7530_regmap_read(void *ctx, uint32_t reg, uint32_t *val)
{
	struct mt7530_priv *priv = (struct mt7530_priv *)ctx;

	/* BIT(15) is used as indication for pseudo registers
	 * which would be translated into the general MDIO
	 * access to leverage the unique regmap sys interface.
	 */
	if (reg & BIT(15))
		*val = mdiobus_read_nested(priv->bus,
					   (reg & 0xf00) >> 8,
					   (reg & 0xff) >> 2);
	else
		*val = mt7530_read(priv, reg);

	return 0;
}

static int
mt7530_regmap_write(void *ctx, uint32_t reg, uint32_t val)
{
	struct mt7530_priv *priv = (struct mt7530_priv *)ctx;

	if (reg & BIT(15))
		mdiobus_write_nested(priv->bus,
				     (reg & 0xf00) >> 8,
				     (reg & 0xff) >> 2, val);
	else
		mt7530_write(priv, reg, val);

	return 0;
}

static const struct regmap_range mt7530_readable_ranges[] = {
	regmap_reg_range(0x0000, 0x00ac), /* Global control */
	regmap_reg_range(0x2000, 0x202c), /* Port Control - P0 */
	regmap_reg_range(0x2100, 0x212c), /* Port Control - P1 */
	regmap_reg_range(0x2200, 0x222c), /* Port Control - P2 */
	regmap_reg_range(0x2300, 0x232c), /* Port Control - P3 */
	regmap_reg_range(0x2400, 0x242c), /* Port Control - P4 */
	regmap_reg_range(0x2500, 0x252c), /* Port Control - P5 */
	regmap_reg_range(0x2600, 0x262c), /* Port Control - P6 */
	regmap_reg_range(0x30e0, 0x30f8), /* Port MAC - SYS */
	regmap_reg_range(0x3000, 0x3014), /* Port MAC - P0 */
	regmap_reg_range(0x3100, 0x3114), /* Port MAC - P1 */
	regmap_reg_range(0x3200, 0x3214), /* Port MAC - P2*/
	regmap_reg_range(0x3300, 0x3314), /* Port MAC - P3*/
	regmap_reg_range(0x3400, 0x3414), /* Port MAC - P4 */
	regmap_reg_range(0x3500, 0x3514), /* Port MAC - P5 */
	regmap_reg_range(0x3600, 0x3614), /* Port MAC - P6 */
	regmap_reg_range(0x4000, 0x40d4), /* MIB - P0 */
	regmap_reg_range(0x4100, 0x41d4), /* MIB - P1 */
	regmap_reg_range(0x4200, 0x42d4), /* MIB - P2 */
	regmap_reg_range(0x4300, 0x43d4), /* MIB - P3 */
	regmap_reg_range(0x4400, 0x44d4), /* MIB - P4 */
	regmap_reg_range(0x4500, 0x45d4), /* MIB - P5 */
	regmap_reg_range(0x4600, 0x46d4), /* MIB - P6 */
	regmap_reg_range(0x4fe0, 0x4ff4), /* SYS */
	regmap_reg_range(0x7000, 0x700c), /* SYS 2 */
	regmap_reg_range(0x7018, 0x7028), /* SYS 3 */
	regmap_reg_range(0x7800, 0x7830), /* SYS 4 */
	regmap_reg_range(0x7a00, 0x7a7c), /* TRGMII */
	regmap_reg_range(0x8000, 0x8078), /* Psedo address for Phy - P0 */
	regmap_reg_range(0x8100, 0x8178), /* Psedo address for Phy - P1 */
	regmap_reg_range(0x8200, 0x8278), /* Psedo address for Phy - P2 */
	regmap_reg_range(0x8300, 0x8378), /* Psedo address for Phy - P3 */
	regmap_reg_range(0x8400, 0x8478), /* Psedo address for Phy - P4 */
};

static const struct regmap_access_table mt7530_readable_table = {
	.yes_ranges = mt7530_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(mt7530_readable_ranges),
};

static struct regmap_config mt7530_regmap_config = {
	.reg_bits = 16,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0x8478,
	.reg_read = mt7530_regmap_read,
	.reg_write = mt7530_regmap_write,
	.rd_table = &mt7530_readable_table,
};

static int
mt7530_fdb_cmd(struct mt7530_priv *priv, enum mt7530_fdb_cmd cmd, u32 *rsp)
{
	u32 reg;
	int ret;

	/* Set the command operating upon the MAC address entries */
	reg = ATC_BUSY | ATC_MAT(0) | cmd;
	mt7530_write(priv, MT7530_ATC, reg);

	/* Wait for completion */
	ret = wait_condition_timeout(
		!(mt7530_read(priv, MT7530_ATC) & ATC_BUSY), 20);
	if (ret < 0) {
		dev_err(priv->dev, "cmd = %x timeout\n", cmd);
		return -EIO;
	}

	/* Additional sanity for read command if the specified
	 * entry is invalid
	 */
	reg = mt7530_read(priv, MT7530_ATC);
	if ((cmd == MT7530_FDB_READ) && (reg & ATC_INVALID))
		return -EINVAL;

	if (rsp)
		*rsp = reg;

	return 0;
}

static void
mt7530_fdb_read(struct mt7530_priv *priv, struct mt7530_fdb *fdb)
{
	u32 reg[3];
	int i;

	/* Read from ARL table into an array */
	for (i = 0; i < 3; i++) {
		reg[i] = mt7530_read(priv, MT7530_TSRA1 + (i * 4));

		dev_dbg(priv->dev, "%s(%d) reg[%d]=0x%x\n",
			__func__, __LINE__, i, reg[i]);
	}

	/* vid - 11:0 on reg[1] */
	fdb->vid = (reg[1] >> 0) & 0xfff;
	/* aging - 31:24 on reg[2] */
	fdb->aging = (reg[2] >> 24) & 0xff;
	/* portmask - 11:4 on reg[2] */
	fdb->port_mask = (reg[2] >> 4) & 0xff;
	/* mac - 31:0 on reg[0] and 31:16 on reg[1] */
	fdb->mac[0] = (reg[0] >> 24) & 0xff;
	fdb->mac[1] = (reg[0] >> 16) & 0xff;
	fdb->mac[2] = (reg[0] >>  8) & 0xff;
	fdb->mac[3] = (reg[0] >>  0) & 0xff;
	fdb->mac[4] = (reg[1] >> 24) & 0xff;
	fdb->mac[5] = (reg[1] >> 16) & 0xff;
	/* noarp - 3:2 on reg[2] */
	fdb->noarp = ((reg[2] >> 2) & 0x3) == STATIC_ENT;
}

static void
mt7530_fdb_write(struct mt7530_priv *priv, u16 vid,
		 u8 port_mask, const u8 *mac,
		 u8 aging, u8 type)
{
	u32 reg[3] = { 0 };
	int i;

	/* vid - 11:0 on reg[1] */
	reg[1] |= (vid & 0xfff) << 0;
	/* aging - 31:25 on reg[2] */
	reg[2] |= (aging & 0xff) << 24;
	/* portmask - 11:4 on reg[2] */
	reg[2] |= (port_mask & 0xff) << 4;
	/* type - 3 indicate that entry is static wouldn't
	 * be aged out and 0 specified as erasing an entry
	 */
	reg[2] |= (type & 0x3) << 2;
	/* mac - 31:0 on reg[0] and 31:16 on reg[1] */
	reg[1] |= mac[5] << 16;
	reg[1] |= mac[4] << 24;
	reg[0] |= mac[3] << 0;
	reg[0] |= mac[2] << 8;
	reg[0] |= mac[1] << 16;
	reg[0] |= mac[0] << 24;

	/* Wrirte array into the ARL table */
	for (i = 0; i < 3; i++)
		mt7530_write(priv, MT7530_ATA1 + (i * 4), reg[i]);
}

static int
mt7530_pad_clk_setup(struct dsa_switch *ds, int mode)
{
	struct mt7530_priv *priv = ds->priv;
	u32 ncpo1, ssc_delta, trgint, i;

	switch (mode) {
	case PHY_INTERFACE_MODE_RGMII:
		trgint = 0;
		ncpo1 = 0x0c80;
		ssc_delta = 0x87;
		break;
	case PHY_INTERFACE_MODE_TRGMII:
		trgint = 1;
		ncpo1 = 0x1400;
		ssc_delta = 0x57;
		break;
	default:
		pr_err("xMII mode %d not supported\n", mode);
		return -EINVAL;
	}

	mt7530_rmw(priv, MT7530_P6ECR, P6_INTF_MODE_MASK,
		   P6_INTF_MODE(trgint));

	/* Lower Tx Driving */
	for (i = 0 ; i < 6 ; i++)
		mt7530_write(priv, MT7530_TRGMII_TD_ODT(i),
			     TD_DM_DRVP(8) | TD_DM_DRVN(8));

	/* Setup MT7530 core clock */
	if (!trgint) {
		/* Disable MT7530 core clock */
		core_clear(priv, CORE_TRGMII_GSW_CLK_CG, REG_GSWCK_EN);

		/* Disable MT7530 PLL, since phy_device has not yet been
		 * created when this function is called. So we provide
		 * core_write_mmd_indirect to complete this function
		 */
		core_write_mmd_indirect(priv,
					CORE_GSWPLL_GRP1,
					MDIO_MMD_VEND2,
					0);

		/* Setup MT7530 core clock into 500Mhz */
		core_write(priv, CORE_GSWPLL_GRP2,
			   RG_GSWPLL_POSDIV_500M(1) |
			   RG_GSWPLL_FBKDIV_500M(25));

		/* Enable MT7530 PLL */
		core_write(priv, CORE_GSWPLL_GRP1,
			   RG_GSWPLL_EN_PRE |
			   RG_GSWPLL_POSDIV_200M(2) |
			   RG_GSWPLL_FBKDIV_200M(32));

		/* Enable MT7530 core clock */
		core_set(priv, CORE_TRGMII_GSW_CLK_CG, REG_GSWCK_EN);
	}

	/* Setup the MT7530 TRGMII Tx Clock */
	core_set(priv, CORE_TRGMII_GSW_CLK_CG, REG_GSWCK_EN);
	core_write(priv, CORE_PLL_GROUP5, RG_LCDDS_PCW_NCPO1(ncpo1));
	core_write(priv, CORE_PLL_GROUP6, RG_LCDDS_PCW_NCPO0(0));
	core_write(priv, CORE_PLL_GROUP10, RG_LCDDS_SSC_DELTA(ssc_delta));
	core_write(priv, CORE_PLL_GROUP11, RG_LCDDS_SSC_DELTA1(ssc_delta));
	core_write(priv, CORE_PLL_GROUP4,
		   RG_SYSPLL_DDSFBK_EN | RG_SYSPLL_BIAS_EN |
		   RG_SYSPLL_BIAS_LPF_EN);
	core_write(priv, CORE_PLL_GROUP2,
		   RG_SYSPLL_EN_NORMAL | RG_SYSPLL_VODEN |
		   RG_SYSPLL_POSDIV(1));
	core_write(priv, CORE_PLL_GROUP7,
		   RG_LCDDS_PCW_NCPO_CHG | RG_LCCDS_C(3) |
		   RG_LCDDS_PWDB | RG_LCDDS_ISO_EN);
	core_set(priv, CORE_TRGMII_GSW_CLK_CG,
		 REG_GSWCK_EN | REG_TRGMIICK_EN);

	if (!trgint)
		for (i = 0 ; i < 5 ; i++)
			mt7530_rmw(priv, MT7530_TRGMII_RD(i),
				   RD_TAP_MASK, RD_TAP(16));
	else
		mt7623_trgmii_set(priv, GSW_INTF_MODE, INTF_MODE_TRGMII);

	return 0;
}

static int
mt7623_pad_clk_setup(struct dsa_switch *ds)
{
	struct mt7530_priv *priv = ds->priv;
	int i;

	for (i = 0 ; i < 6; i++)
		mt7623_trgmii_write(priv, GSW_TRGMII_TD_ODT(i),
				    TD_DM_DRVP(8) | TD_DM_DRVN(8));

	mt7623_trgmii_set(priv, GSW_TRGMII_RCK_CTRL, RX_RST | RXC_DQSISEL);
	mt7623_trgmii_clear(priv, GSW_TRGMII_RCK_CTRL, RX_RST);

	return 0;
}

static void
mt7530_mib_reset(struct dsa_switch *ds)
{
	struct mt7530_priv *priv = ds->priv;

	mt7530_write(priv, MT7530_MIB_CCR, CCR_MIB_FLUSH);
	mt7530_write(priv, MT7530_MIB_CCR, CCR_MIB_ACTIVATE);
}

static void
mt7530_port_set_status(struct dsa_switch *ds, int port, int enable)
{
	struct mt7530_priv *priv = ds->priv;
	u32 mask = PMCR_TX_EN | PMCR_RX_EN;

	mutex_lock(&priv->reg_mutex);
	if (enable)
		mt7530_set(priv, MT7530_PMCR_P(port), mask);
	else
		mt7530_clear(priv, MT7530_PMCR_P(port), mask);
	mutex_unlock(&priv->reg_mutex);
}

static int
mt7530_setup(struct dsa_switch *ds)
{
	struct mt7530_priv *priv = ds->priv;
	int ret, i, phy_mode;
	u8  cpup_mask = 0;
	u32 id, val;
	struct regmap *regmap;
	struct device_node *dn;

	/* Make sure that cpu port specfied on the dt is appropriate */
	if (!dsa_is_cpu_port(ds, MT7530_CPU_PORT)) {
		dev_err(priv->dev, "port not matched with the CPU port\n");
		return -EINVAL;
	}

	/* The parent node of master_netdev which holds the common system
	 * controller also is the container for two GMACs nodes representing
	 * as two netdev instances.
	 */
	dn = ds->master_netdev->dev.of_node->parent;
	priv->ethernet = syscon_node_to_regmap(dn);
	if (IS_ERR(priv->ethernet))
		return PTR_ERR(priv->ethernet);

	regmap = devm_regmap_init(ds->dev, NULL, priv,
				  &mt7530_regmap_config);
	if (IS_ERR(regmap))
		dev_warn(priv->dev, "phy regmap initialization failed");

	phy_mode = of_get_phy_mode(ds->ports[ds->dst->cpu_port].dn);
	if (phy_mode < 0) {
		dev_err(priv->dev, "Can't find phy-mode for master device\n");
		return phy_mode;
	}
	dev_info(priv->dev, "phy-mode for master device = %x\n", phy_mode);

	regulator_set_voltage(priv->core_pwr, 1000000, 1000000);
	ret = regulator_enable(priv->core_pwr);
	if (ret < 0) {
		dev_err(priv->dev,
			"Failed to enable core power: %d\n", ret);
		return ret;
	}

	regulator_set_voltage(priv->io_pwr, 3300000, 3300000);
	ret = regulator_enable(priv->io_pwr);
	if (ret < 0) {
		dev_err(priv->dev, "Failed to enable io pwr: %d\n",
			ret);
		return ret;
	}

	/* Reset whole chip through gpio pin or memory-mapped registers for
	 * different type of hardware
	 */
	if (priv->mcm) {
		reset_control_assert(priv->rstc);
		usleep_range(1000, 1100);
		reset_control_deassert(priv->rstc);
	} else {
		gpiod_set_value_cansleep(priv->reset, 0);
		usleep_range(1000, 1100);
		gpiod_set_value_cansleep(priv->reset, 1);
	}

	/* Wait until the reset completion */
	ret = wait_condition_timeout(mt7530_read(priv, MT7530_HWTRAP) != 0,
				     1000);
	if (ret < 0) {
		dev_err(priv->dev, "reset timeout\n");
		return ret;
	}

	id = mt7530_read(priv, MT7530_CREV);
	id >>= CHIP_NAME_SHIFT;
	if (id != MT7530_ID)
		return -ENODEV;

	/* Reset the switch through internal reset */
	mt7530_write(priv, MT7530_SYS_CTRL,
		     SYS_CTRL_PHY_RST | SYS_CTRL_SW_RST |
		     SYS_CTRL_REG_RST);

	/* Enable Port 6 only, P5 as GMAC5 which currently is not supported */
	val = mt7530_read(priv, MT7530_MHWTRAP);
	val &= ~MHWTRAP_P6_DIS & ~MHWTRAP_PHY_ACCESS;
	val |= MHWTRAP_MANUAL;
	mt7530_write(priv, MT7530_MHWTRAP, val);

	ret = mt7530_pad_clk_setup(ds, phy_mode);
	if (ret < 0)
		return ret;

	/* Enable and reset MIB counters */
	mt7530_mib_reset(ds);

	/* Disable forwarding by default on all ports */
	for (i = 0; i < MT7530_NUM_PORTS; i++)
		mt7530_write(priv, MT7530_PCR_P(i), PCR_MATRIX_INIT);

	mt7530_clear(priv, MT7530_MFC, UNU_FFP_MASK);

	/* Fabric setup for the cpu port */
	for (i = 0; i < MT7530_NUM_PORTS; i++)
		if (dsa_is_cpu_port(ds, i)) {
			/* Enable Mediatek header mode on the cpu port */
			mt7530_write(priv, MT7530_PVC_P(i),
				     PORT_SPEC_TAG);

			/* Setup the MAC by default for the cpu port */
			mt7530_write(priv, MT7530_PMCR_P(i), PMCR_CPUP_LINK);

			/* Disable auto learning on the cpu port */
			mt7530_set(priv, MT7530_PSC_P(i), SA_DIS);

			/* Unknown unicast frame fordwarding to the cpu port */
			mt7530_set(priv, MT7530_MFC, UNU_FFP(BIT(i)));

			/* CPU port gets connected to all user ports of
			 * the switch
			 */
			mt7530_write(priv, MT7530_PCR_P(i),
				     PCR_MATRIX(ds->enabled_port_mask));

			cpup_mask |= BIT(i);
		}

	/* Fabric setup for the all user ports */
	for (i = 0; i < MT7530_NUM_PORTS; i++)
		if (ds->enabled_port_mask & BIT(i)) {
			/* Setup the MAC by default for all user ports */
			mt7530_write(priv, MT7530_PMCR_P(i),
				     PMCR_USERP_LINK);

			/* The user port gets connected to the cpu port only */
			mt7530_write(priv, MT7530_PCR_P(i),
				     PCR_MATRIX(cpup_mask));
		}

	/* Flush the FDB table */
	ret = mt7530_fdb_cmd(priv, MT7530_FDB_FLUSH, 0);
	if (ret < 0)
		return ret;

	/* Setup RX circuit, relevant PAD and driving on the host which must
	 * be placed after the setup on the device side is all finished.
	 */
	ret = mt7623_pad_clk_setup(ds);
	if (ret < 0)
		return ret;

	return 0;
}

static int mt7530_phy_read(struct dsa_switch *ds, int port, int regnum)
{
	struct mt7530_priv *priv = ds->priv;

	return mdiobus_read_nested(priv->bus, port, regnum);
}

int mt7530_phy_write(struct dsa_switch *ds, int port, int regnum, u16 val)
{
	struct mt7530_priv *priv = ds->priv;

	return mdiobus_write_nested(priv->bus, port, regnum, val);
}

static void
mt7530_get_strings(struct dsa_switch *ds, int port, uint8_t *data)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mt7530_mib); i++)
		strncpy(data + i * ETH_GSTRING_LEN, mt7530_mib[i].name,
			ETH_GSTRING_LEN);
}

static void
mt7530_get_ethtool_stats(struct dsa_switch *ds, int port,
			 uint64_t *data)
{
	struct mt7530_priv *priv = ds->priv;
	const struct mt7530_mib_desc *mib;
	u32 reg, i;
	u64 hi;

	for (i = 0; i < ARRAY_SIZE(mt7530_mib); i++) {
		mib = &mt7530_mib[i];
		reg = MT7530_PORT_MIB_COUNTER(port) + mib->offset;

		data[i] = mt7530_read(priv, reg);
		if (mib->size == 2) {
			hi = mt7530_read(priv, reg + 4);
			data[i] |= hi << 32;
		}
	}
}

static int
mt7530_get_sset_count(struct dsa_switch *ds)
{
	return ARRAY_SIZE(mt7530_mib);
}

static int
mt7530_port_enable(struct dsa_switch *ds, int port,
		   struct phy_device *phy)
{
	mt7530_port_set_status(ds, port, 1);

	return 0;
}

static void
mt7530_port_disable(struct dsa_switch *ds, int port,
		    struct phy_device *phy)
{
	mt7530_port_set_status(ds, port, 0);
}

static void
mt7530_stp_state_set(struct dsa_switch *ds, int port, u8 state)
{
	struct mt7530_priv *priv = ds->priv;
	u32 stp_state;

	switch (state) {
	case BR_STATE_DISABLED:
		stp_state = MT7530_STP_DISABLED;
		break;
	case BR_STATE_BLOCKING:
		stp_state = MT7530_STP_BLOCKING;
		break;
	case BR_STATE_LISTENING:
		stp_state = MT7530_STP_LISTENING;
		break;
	case BR_STATE_LEARNING:
		stp_state = MT7530_STP_LEARNING;
		break;
	case BR_STATE_FORWARDING:
	default:
		stp_state = MT7530_STP_FORWARDING;
		break;
	}

	mt7530_rmw(priv, MT7530_SSP_P(port), FID_PST_MASK, stp_state);
}

static int
mt7530_port_bridge_join(struct dsa_switch *ds, int port,
			struct net_device *bridge)
{
	struct mt7530_priv *priv = ds->priv;
	int port_bitmap = BIT(MT7530_CPU_PORT);
	int i;

	mutex_lock(&priv->reg_mutex);

	for (i = 0; i < MT7530_NUM_PORTS; i++)
		if (ds->enabled_port_mask & BIT(i)) {
			if (ds->ports[i].bridge_dev != bridge)
				continue;
			/* Add this port to the port maxtrix of the
			 * other ports in the bridge
			 */
			mt7530_set(priv, MT7530_PCR_P(i),
				   PCR_MATRIX(BIT(port)));

			if (i != port)
				port_bitmap |= BIT(i);
		}

	/* Add all other ports to this port matrix */
	mt7530_rmw(priv, MT7530_PCR_P(port),
		   PCR_MATRIX_MASK, PCR_MATRIX(port_bitmap));

	mutex_unlock(&priv->reg_mutex);

	return 0;
}

static void
mt7530_port_bridge_leave(struct dsa_switch *ds, int port,
			 struct net_device *bridge)
{
	struct mt7530_priv *priv = ds->priv;
	int i;

	mutex_lock(&priv->reg_mutex);

	for (i = 0; i < MT7530_NUM_PORTS; i++)
		if (ds->enabled_port_mask & BIT(i)) {
			if (ds->ports[i].bridge_dev != bridge)
				continue;
			/* Remove this port from the port maxtrix
			 * of the other ports in the bridge.
			 */
			mt7530_clear(priv, MT7530_PCR_P(i),
				     PCR_MATRIX(BIT(port)));
		}

	/* Set the cpu port to be the only one in the port matrix of
	 * this port.
	 */
	mt7530_rmw(priv, MT7530_PCR_P(port),
		   PCR_MATRIX_MASK,
		   PCR_MATRIX(BIT(MT7530_CPU_PORT)));

	mutex_unlock(&priv->reg_mutex);
}

static int
mt7530_port_fdb_prepare(struct dsa_switch *ds, int port,
			const struct switchdev_obj_port_fdb *fdb,
			struct switchdev_trans *trans)
{
	struct mt7530_priv *priv = ds->priv;
	int ret;

	/* Because auto-learned entries shares the same FDB table.
	 * an entry is reserved with no port_mask to make sure fdb_add
	 * is called while the entry is still available.
	 */
	mutex_lock(&priv->reg_mutex);
	mt7530_fdb_write(priv, fdb->vid, 0, fdb->addr, -1, STATIC_ENT);
	ret = mt7530_fdb_cmd(priv, MT7530_FDB_WRITE, 0);
	mutex_unlock(&priv->reg_mutex);

	return ret;
}

static void
mt7530_port_fdb_add(struct dsa_switch *ds, int port,
		    const struct switchdev_obj_port_fdb *fdb,
		    struct switchdev_trans *trans)
{
	struct mt7530_priv *priv = ds->priv;
	u8 port_mask = BIT(port);

	mutex_lock(&priv->reg_mutex);
	mt7530_fdb_write(priv, fdb->vid, port_mask, fdb->addr, -1, STATIC_ENT);
	mt7530_fdb_cmd(priv, MT7530_FDB_WRITE, 0);
	mutex_unlock(&priv->reg_mutex);
}

static int
mt7530_port_fdb_del(struct dsa_switch *ds, int port,
		    const struct switchdev_obj_port_fdb *fdb)
{
	struct mt7530_priv *priv = ds->priv;
	int ret;
	u8 port_mask = BIT(port);

	mutex_lock(&priv->reg_mutex);
	mt7530_fdb_write(priv, fdb->vid, port_mask, fdb->addr, -1, STATIC_EMP);
	ret = mt7530_fdb_cmd(priv, MT7530_FDB_WRITE, 0);
	mutex_unlock(&priv->reg_mutex);

	return ret;
}

static int
mt7530_port_fdb_dump(struct dsa_switch *ds, int port,
		     struct switchdev_obj_port_fdb *fdb,
		     int (*cb)(struct switchdev_obj *obj))
{
	struct mt7530_priv *priv = ds->priv;
	struct mt7530_fdb _fdb = { 0 };
	int cnt = MT7530_NUM_FDB_RECORDS;
	int ret = 0;
	u32 rsp = 0;

	mutex_lock(&priv->reg_mutex);

	ret = mt7530_fdb_cmd(priv, MT7530_FDB_START, &rsp);
	if (ret < 0)
		goto err;

	do {
		if (rsp & ATC_SRCH_HIT) {
			mt7530_fdb_read(priv, &_fdb);
			if (_fdb.port_mask & BIT(port)) {
				ether_addr_copy(fdb->addr, _fdb.mac);
				fdb->vid = _fdb.vid;
				fdb->ndm_state = _fdb.noarp ?
						NUD_NOARP : NUD_REACHABLE;
				ret = cb(&fdb->obj);
				if (ret < 0)
					break;
			}
		}
	} while (--cnt &&
		 !(rsp & ATC_SRCH_END) &&
		 !mt7530_fdb_cmd(priv, MT7530_FDB_NEXT, &rsp));
err:
	mutex_unlock(&priv->reg_mutex);

	return 0;
}

static enum dsa_tag_protocol
mtk_get_tag_protocol(struct dsa_switch *ds)
{
	return DSA_TAG_PROTO_MTK;
}

static struct dsa_switch_ops mt7530_switch_ops = {
	.get_tag_protocol	= mtk_get_tag_protocol,
	.setup			= mt7530_setup,
	.get_strings		= mt7530_get_strings,
	.phy_read		= mt7530_phy_read,
	.phy_write		= mt7530_phy_write,
	.get_ethtool_stats	= mt7530_get_ethtool_stats,
	.get_sset_count		= mt7530_get_sset_count,
	.port_enable		= mt7530_port_enable,
	.port_disable		= mt7530_port_disable,
	.port_stp_state_set	= mt7530_stp_state_set,
	.port_bridge_join	= mt7530_port_bridge_join,
	.port_bridge_leave	= mt7530_port_bridge_leave,
	.port_fdb_prepare	= mt7530_port_fdb_prepare,
	.port_fdb_add		= mt7530_port_fdb_add,
	.port_fdb_del		= mt7530_port_fdb_del,
	.port_fdb_dump		= mt7530_port_fdb_dump,
};

static int
mt7530_probe(struct mdio_device *mdiodev)
{
	struct mt7530_priv *priv;
	struct device_node *dn;

	dn = mdiodev->dev.of_node;

	priv = devm_kzalloc(&mdiodev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->ds = dsa_switch_alloc(&mdiodev->dev, DSA_MAX_PORTS);
	if (!priv->ds)
		return -ENOMEM;

	/* Use medatek,mcm property to distinguish hardware type that would
	 * casues a little bit differences on power-on sequence.
	 */
	priv->mcm = of_property_read_bool(dn, "mediatek,mcm");
	if (priv->mcm) {
		dev_info(&mdiodev->dev, "MT7530 adapts as multi-chip module\n");

		priv->rstc = devm_reset_control_get(&mdiodev->dev, "mcm");
		if (IS_ERR(priv->rstc)) {
			dev_err(&mdiodev->dev, "Couldn't get our reset line\n");
			return PTR_ERR(priv->rstc);
		}
	}

	priv->core_pwr = devm_regulator_get(&mdiodev->dev, "core");
	if (IS_ERR(priv->core_pwr))
		return PTR_ERR(priv->core_pwr);

	priv->io_pwr = devm_regulator_get(&mdiodev->dev, "io");
	if (IS_ERR(priv->io_pwr))
		return PTR_ERR(priv->io_pwr);

	/* Not MCM that indicates switch works as the remote standalone
	 * integrated circuit so the GPIO pin would be used to complete
	 * the reset, otherwise memory-mapped register accessing used
	 * through syscon provides in the case of MCM.
	 */
	if (!priv->mcm) {
		priv->reset = devm_gpiod_get_optional(&mdiodev->dev, "reset",
						      GPIOD_OUT_LOW);
		if (IS_ERR(priv->reset)) {
			dev_err(&mdiodev->dev, "Couldn't get our reset line\n");
			return PTR_ERR(priv->reset);
		}
	}

	priv->bus = mdiodev->bus;
	priv->dev = &mdiodev->dev;
	priv->ds->priv = priv;
	priv->ds->ops = &mt7530_switch_ops;
	mutex_init(&priv->reg_mutex);
	dev_set_drvdata(&mdiodev->dev, priv);

	return dsa_register_switch(priv->ds, &mdiodev->dev);
}

static void
mt7530_remove(struct mdio_device *mdiodev)
{
	struct mt7530_priv *priv = dev_get_drvdata(&mdiodev->dev);
	int ret = 0;

	ret = regulator_disable(priv->core_pwr);
	if (ret < 0)
		dev_err(priv->dev,
			"Failed to disable core power: %d\n", ret);

	ret = regulator_disable(priv->io_pwr);
	if (ret < 0)
		dev_err(priv->dev, "Failed to disable io pwr: %d\n",
			ret);

	dsa_unregister_switch(priv->ds);
	mutex_destroy(&priv->reg_mutex);
}

static const struct of_device_id mt7530_of_match[] = {
	{ .compatible = "mediatek,mt7530" },
	{ /* sentinel */ },
};

static struct mdio_driver mt7530_mdio_driver = {
	.probe  = mt7530_probe,
	.remove = mt7530_remove,
	.mdiodrv.driver = {
		.name = "mt7530",
		.of_match_table = mt7530_of_match,
	},
};

mdio_module_driver(mt7530_mdio_driver);

MODULE_AUTHOR("Sean Wang <sean.wang@mediatek.com>");
MODULE_DESCRIPTION("Driver for Mediatek MT7530 Switch");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:mediatek-mt7530");
