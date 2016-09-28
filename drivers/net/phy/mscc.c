/*
 * Driver for Microsemi VSC85xx PHYs
 *
 * Author: Nagaraju Lakkaraju
 * License: Dual MIT/GPL
 * Copyright (c) 2016 Microsemi Corporation
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mdio.h>
#include <linux/mii.h>
#include <linux/phy.h>
#include <linux/netdevice.h>

enum rgmii_rx_clock_delay {
	RGMII_RX_CLK_DELAY_0_2_NS = 0,
	RGMII_RX_CLK_DELAY_0_8_NS = 1,
	RGMII_RX_CLK_DELAY_1_1_NS = 2,
	RGMII_RX_CLK_DELAY_1_7_NS = 3,
	RGMII_RX_CLK_DELAY_2_0_NS = 4,
	RGMII_RX_CLK_DELAY_2_3_NS = 5,
	RGMII_RX_CLK_DELAY_2_6_NS = 6,
	RGMII_RX_CLK_DELAY_3_4_NS = 7
};

/* Microsemi VSC85xx PHY registers */
/* IEEE 802. Std Registers */
#define MSCC_PHY_BYPASS_CONTROL		  18
#define DISABLE_HP_AUTO_MDIX_MASK	  0x0080
#define DISABLE_PAIR_SWAP_CORR_MASK	  0x0020
#define DISABLE_POLARITY_CORR_MASK	  0x0010

#define MSCC_PHY_EXT_PHY_CNTL_1           23
#define MAC_IF_SELECTION_MASK             0x1800
#define MAC_IF_SELECTION_GMII             0
#define MAC_IF_SELECTION_RMII             1
#define MAC_IF_SELECTION_RGMII            2
#define MAC_IF_SELECTION_POS              11
#define FAR_END_LOOPBACK_MODE_MASK        0x0008

#define MII_VSC85XX_INT_MASK		  25
#define MII_VSC85XX_INT_MASK_MASK	  0xa000
#define MII_VSC85XX_INT_MASK_WOL	  0x0040
#define MII_VSC85XX_INT_STATUS		  26

#define MSCC_EXT_PAGE_ACCESS		  31
#define MSCC_PHY_PAGE_STANDARD		  0x0000 /* Standard registers */
#define MSCC_PHY_PAGE_EXTENDED		  0x0001 /* Extended registers */
#define MSCC_PHY_PAGE_EXTENDED_2	  0x0002 /* Extended reg - page 2 */

/* Extended Page 1 Registers */
#define MSCC_PHY_EXT_MODE_CNTL		  19
#define FORCE_MDI_CROSSOVER_MASK	  0x000C
#define FORCE_MDI_CROSSOVER_MDIX	  0x000C
#define FORCE_MDI_CROSSOVER_MDI		  0x0008
#define FORCE_MDI_CROSSOVER_NORMAL	  0x0000

/* Extended Page 2 Registers */
#define MSCC_PHY_RGMII_CNTL		  20
#define RGMII_RX_CLK_DELAY_MASK		  0x0070
#define RGMII_RX_CLK_DELAY_POS		  4

#define MSCC_PHY_WOL_LOWER_MAC_ADDR	  21
#define MSCC_PHY_WOL_MID_MAC_ADDR	  22
#define MSCC_PHY_WOL_UPPER_MAC_ADDR	  23
#define MSCC_PHY_WOL_LOWER_PASSWD	  24
#define MSCC_PHY_WOL_MID_PASSWD		  25
#define MSCC_PHY_WOL_UPPER_PASSWD	  26

#define MSCC_PHY_WOL_MAC_CONTROL	  27
#define EDGE_RATE_CNTL_POS		  5
#define EDGE_RATE_CNTL_MASK		  0x00E0
#define SECURE_ON_ENABLE		  0x8000
#define SECURE_ON_PASSWD_LEN_4		  0x4000

/* Microsemi PHY ID's */
#define PHY_ID_VSC8531			  0x00070570
#define PHY_ID_VSC8541			  0x00070770

static int vsc85xx_phy_page_set(struct phy_device *phydev, u8 page)
{
	int rc;

	rc = phy_write(phydev, MSCC_EXT_PAGE_ACCESS, page);
	return rc;
}

static int vsc85xx_mdix_set(struct phy_device *phydev,
			    u8 mdix)
{
	int rc;
	u16 reg_val;

	reg_val = phy_read(phydev, MSCC_PHY_BYPASS_CONTROL);
	if ((mdix == ETH_TP_MDI) || (mdix == ETH_TP_MDI_X)) {
		reg_val |= (DISABLE_PAIR_SWAP_CORR_MASK |
			    DISABLE_POLARITY_CORR_MASK  |
			    DISABLE_HP_AUTO_MDIX_MASK);
	} else {
		reg_val &= ~(DISABLE_PAIR_SWAP_CORR_MASK |
			     DISABLE_POLARITY_CORR_MASK  |
			     DISABLE_HP_AUTO_MDIX_MASK);
	}
	rc = phy_write(phydev, MSCC_PHY_BYPASS_CONTROL, reg_val);
	if (rc != 0)
		goto out_unlock;

	rc = vsc85xx_phy_page_set(phydev, MSCC_PHY_PAGE_EXTENDED);
	if (rc != 0)
		goto out_unlock;

	reg_val = phy_read(phydev, MSCC_PHY_EXT_MODE_CNTL);
	reg_val &= ~(FORCE_MDI_CROSSOVER_MASK);
	if (mdix == ETH_TP_MDI)
		reg_val |= FORCE_MDI_CROSSOVER_MDI;
	else if (mdix == ETH_TP_MDI_X)
		reg_val |= FORCE_MDI_CROSSOVER_MDIX;
	rc = phy_write(phydev, MSCC_PHY_EXT_MODE_CNTL, reg_val);
	if (rc != 0)
		goto out_unlock;

	rc = vsc85xx_phy_page_set(phydev, MSCC_PHY_PAGE_STANDARD);

out_unlock:

	return rc;
}

static int vsc85xx_wol_set(struct phy_device *phydev,
			   struct ethtool_wolinfo *wol)
{
	int rc;
	u16 reg_val;
	struct ethtool_wolinfo *wol_conf = wol;

	mutex_lock(&phydev->lock);
	rc = vsc85xx_phy_page_set(phydev, MSCC_PHY_PAGE_EXTENDED_2);
	if (rc != 0)
		goto out_unlock;

	if (wol->wolopts & WAKE_MAGIC) {
		/* Store the device address for the magic packet */
		reg_val = phydev->attached_dev->dev_addr[4] << 8;
		reg_val |= phydev->attached_dev->dev_addr[5];
		phy_write(phydev, MSCC_PHY_WOL_LOWER_MAC_ADDR, reg_val);
		reg_val = phydev->attached_dev->dev_addr[2] << 8;
		reg_val |= phydev->attached_dev->dev_addr[3];
		phy_write(phydev, MSCC_PHY_WOL_MID_MAC_ADDR, reg_val);
		reg_val = phydev->attached_dev->dev_addr[0] << 8;
		reg_val |= phydev->attached_dev->dev_addr[1];
		phy_write(phydev, MSCC_PHY_WOL_UPPER_MAC_ADDR, reg_val);
	} else {
		phy_write(phydev, MSCC_PHY_WOL_LOWER_MAC_ADDR, 0);
		phy_write(phydev, MSCC_PHY_WOL_MID_MAC_ADDR, 0);
		phy_write(phydev, MSCC_PHY_WOL_UPPER_MAC_ADDR, 0);
	}

	reg_val = phy_read(phydev, MSCC_PHY_WOL_MAC_CONTROL);
	if (wol_conf->wolopts & WAKE_MAGICSECURE)
		reg_val |= SECURE_ON_ENABLE;
	else
		reg_val &= ~SECURE_ON_ENABLE;
	phy_write(phydev, MSCC_PHY_WOL_MAC_CONTROL, reg_val);

	if (wol_conf->wolopts & WAKE_MAGICSECURE) {
		reg_val = wol_conf->sopass[4] << 8;
		reg_val |= wol_conf->sopass[5];
		phy_write(phydev, MSCC_PHY_WOL_LOWER_PASSWD, reg_val);
		reg_val = wol_conf->sopass[2] << 8;
		reg_val |= wol_conf->sopass[3];
		phy_write(phydev, MSCC_PHY_WOL_MID_PASSWD, reg_val);
		reg_val = wol_conf->sopass[0] << 8;
		reg_val |= wol_conf->sopass[1];
		phy_write(phydev, MSCC_PHY_WOL_UPPER_PASSWD, reg_val);
	} else {
		phy_write(phydev, MSCC_PHY_WOL_LOWER_PASSWD, 0);
		phy_write(phydev, MSCC_PHY_WOL_MID_PASSWD, 0);
		phy_write(phydev, MSCC_PHY_WOL_UPPER_PASSWD, 0);
	}

	rc = vsc85xx_phy_page_set(phydev, MSCC_PHY_PAGE_STANDARD);
	if (rc != 0)
		goto out_unlock;

	if (wol->wolopts & WAKE_MAGIC) {
		/* Enable the WOL interrupt */
		reg_val = phy_read(phydev, MII_VSC85XX_INT_MASK);
		reg_val |= MII_VSC85XX_INT_MASK_WOL;
		rc = phy_write(phydev, MII_VSC85XX_INT_MASK, reg_val);
		if (rc != 0)
			goto out_unlock;
	} else {
		/* Disable the WOL interrupt */
		reg_val = phy_read(phydev, MII_VSC85XX_INT_MASK);
		reg_val &= (~MII_VSC85XX_INT_MASK_WOL);
		rc = phy_write(phydev, MII_VSC85XX_INT_MASK, reg_val);
		if (rc != 0)
			goto out_unlock;
	}
	/* Clear WOL iterrupt status */
	reg_val = phy_read(phydev, MII_VSC85XX_INT_STATUS);

out_unlock:
	mutex_unlock(&phydev->lock);

	return rc;
}

static void vsc85xx_wol_get(struct phy_device *phydev,
			    struct ethtool_wolinfo *wol)
{
	int rc;
	u16 reg_val;
	struct ethtool_wolinfo *wol_conf = wol;

	mutex_lock(&phydev->lock);
	rc = vsc85xx_phy_page_set(phydev, MSCC_PHY_PAGE_EXTENDED_2);
	if (rc != 0)
		goto out_unlock;

	reg_val = phy_read(phydev, MSCC_PHY_WOL_MAC_CONTROL);
	if (reg_val & SECURE_ON_ENABLE)
		wol_conf->wolopts |= WAKE_MAGICSECURE;
	if (wol_conf->wolopts & WAKE_MAGICSECURE) {
		reg_val = phy_read(phydev, MSCC_PHY_WOL_LOWER_PASSWD);
		wol_conf->sopass[5] = reg_val & 0x00ff;
		wol_conf->sopass[4] = (reg_val & 0xff00) >> 8;
		reg_val = phy_read(phydev, MSCC_PHY_WOL_MID_PASSWD);
		wol_conf->sopass[3] = reg_val & 0x00ff;
		wol_conf->sopass[2] = (reg_val & 0xff00) >> 8;
		reg_val = phy_read(phydev, MSCC_PHY_WOL_UPPER_PASSWD);
		wol_conf->sopass[1] = reg_val & 0x00ff;
		wol_conf->sopass[0] = (reg_val & 0xff00) >> 8;
	}

	rc = vsc85xx_phy_page_set(phydev, MSCC_PHY_PAGE_STANDARD);

out_unlock:
	mutex_unlock(&phydev->lock);
}

static int vsc85xx_mac_if_set(struct phy_device *phydev,
			      phy_interface_t interface)
{
	int rc;
	u16 reg_val;

	mutex_lock(&phydev->lock);
	reg_val = phy_read(phydev, MSCC_PHY_EXT_PHY_CNTL_1);
	reg_val &= ~(MAC_IF_SELECTION_MASK);
	switch (interface) {
	case PHY_INTERFACE_MODE_RGMII:
		reg_val |= (MAC_IF_SELECTION_RGMII << MAC_IF_SELECTION_POS);
		break;
	case PHY_INTERFACE_MODE_RMII:
		reg_val |= (MAC_IF_SELECTION_RMII << MAC_IF_SELECTION_POS);
		break;
	case PHY_INTERFACE_MODE_MII:
	case PHY_INTERFACE_MODE_GMII:
		reg_val |= (MAC_IF_SELECTION_GMII << MAC_IF_SELECTION_POS);
		break;
	default:
		rc = -EINVAL;
		goto out_unlock;
	}
	rc = phy_write(phydev, MSCC_PHY_EXT_PHY_CNTL_1, reg_val);
	if (rc != 0)
		goto out_unlock;

	rc = genphy_soft_reset(phydev);

out_unlock:
	mutex_unlock(&phydev->lock);

	return rc;
}

static int vsc85xx_default_config(struct phy_device *phydev)
{
	int rc;
	u16 reg_val;

	phydev->mdix = ETH_TP_MDI_AUTO;
	mutex_lock(&phydev->lock);
	rc = vsc85xx_phy_page_set(phydev, MSCC_PHY_PAGE_EXTENDED_2);
	if (rc != 0)
		goto out_unlock;

	reg_val = phy_read(phydev, MSCC_PHY_RGMII_CNTL);
	reg_val &= ~(RGMII_RX_CLK_DELAY_MASK);
	reg_val |= (RGMII_RX_CLK_DELAY_1_1_NS << RGMII_RX_CLK_DELAY_POS);
	phy_write(phydev, MSCC_PHY_RGMII_CNTL, reg_val);
	rc = vsc85xx_phy_page_set(phydev, MSCC_PHY_PAGE_STANDARD);

out_unlock:
	mutex_unlock(&phydev->lock);

	return rc;
}

static int vsc85xx_config_init(struct phy_device *phydev)
{
	int rc;

	rc = vsc85xx_default_config(phydev);
	if (rc)
		return rc;

	rc = vsc85xx_mac_if_set(phydev, phydev->interface);
	if (rc)
		return rc;

	rc = genphy_config_init(phydev);

	return rc;
}

static int vsc85xx_ack_interrupt(struct phy_device *phydev)
{
	int rc = 0;

	if (phydev->interrupts == PHY_INTERRUPT_ENABLED)
		rc = phy_read(phydev, MII_VSC85XX_INT_STATUS);

	return (rc < 0) ? rc : 0;
}

static int vsc85xx_config_intr(struct phy_device *phydev)
{
	int rc;

	if (phydev->interrupts == PHY_INTERRUPT_ENABLED) {
		rc = phy_write(phydev, MII_VSC85XX_INT_MASK,
			       MII_VSC85XX_INT_MASK_MASK);
	} else {
		rc = phy_write(phydev, MII_VSC85XX_INT_MASK, 0);
		if (rc < 0)
			return rc;
		rc = phy_read(phydev, MII_VSC85XX_INT_STATUS);
	}

	return rc;
}

static int vsc85xx_config_aneg(struct phy_device *phydev)
{
	int rc;

	rc = vsc85xx_mdix_set(phydev, phydev->mdix);
	if (rc < 0)
		return rc;

	rc = genphy_config_aneg(phydev);

	return rc;
}

/* Microsemi VSC85xx PHYs */
static struct phy_driver vsc85xx_driver[] = {
{
	.phy_id		= PHY_ID_VSC8531,
	.name		= "Microsemi VSC8531",
	.phy_id_mask    = 0xfffffff0,
	.features	= PHY_GBIT_FEATURES,
	.flags		= PHY_HAS_INTERRUPT,
	.soft_reset	= &genphy_soft_reset,
	.config_init    = &vsc85xx_config_init,
	.config_aneg    = &vsc85xx_config_aneg,
	.aneg_done	= &genphy_aneg_done,
	.read_status    = &genphy_read_status,
	.ack_interrupt  = &vsc85xx_ack_interrupt,
	.config_intr    = &vsc85xx_config_intr,
	.suspend	= &genphy_suspend,
	.resume		= &genphy_resume,
	.set_wol        = &vsc85xx_wol_set,
	.get_wol        = &vsc85xx_wol_get,
},
{
	.phy_id		= PHY_ID_VSC8541,
	.name		= "Microsemi VSC8541 SyncE",
	.phy_id_mask    = 0xfffffff0,
	.features	= PHY_GBIT_FEATURES,
	.flags		= PHY_HAS_INTERRUPT,
	.soft_reset	= &genphy_soft_reset,
	.config_init    = &vsc85xx_config_init,
	.config_aneg    = &vsc85xx_config_aneg,
	.aneg_done	= &genphy_aneg_done,
	.read_status    = &genphy_read_status,
	.ack_interrupt  = &vsc85xx_ack_interrupt,
	.config_intr    = &vsc85xx_config_intr,
	.suspend	= &genphy_suspend,
	.resume		= &genphy_resume,
	.set_wol        = &vsc85xx_wol_set,
	.get_wol        = &vsc85xx_wol_get,
}

};

module_phy_driver(vsc85xx_driver);

static struct mdio_device_id __maybe_unused vsc85xx_tbl[] = {
	{ PHY_ID_VSC8531, 0xfffffff0, },
	{ PHY_ID_VSC8541, 0xfffffff0, },
	{ }
};

MODULE_DEVICE_TABLE(mdio, vsc85xx_tbl);

MODULE_DESCRIPTION("Microsemi VSC85xx PHY driver");
MODULE_AUTHOR("Nagaraju Lakkaraju");
MODULE_LICENSE("Dual MIT/GPL");
