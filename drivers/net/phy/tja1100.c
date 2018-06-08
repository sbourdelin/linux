// SPDX-License-Identifier: GPL-2.0
/* tja1100.c: TJA1100 BoardR-REACH PHY driver.
 *
 * Copyright (c) 2017 Kirill Kranke <kirill.kranke@gmail.com>
 * Author: Kirill Kranke <kirill.kranke@gmail.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/phy.h>

/* TJA1100 specific registers */
#define TJA1100_ECTRL	0x11	/* Extended control register */
#define TJA1100_CFG1	0x12	/* Configuration register 1 */
#define TJA1100_CFG2	0x13	/* Configuration register 2 */
#define TJA1100_SERRCNT	0x14	/* Symbol error counter register 2 */
#define TJA1100_INTST	0x15	/* Interrupt status register */
#define TJA1100_INTEN	0x16	/* Interrupt enable register */
#define TJA1100_COMST	0x17	/* Communication status register */
#define TJA1100_GST	0x18	/* General status register */
#define TJA1100_EXTST	0x19	/* External status register */
#define TJA1100_LFCNT	0x1a	/* Link fail counter register */

/* Extended control register */
#define ECTRL_LC	0x8000	/* link control enable */
#define ECTRL_PM	0x7800	/* operating mode select */
#define ECTRL_PM_NOCNG	0x0000	/* PM == 0000: no change */
#define ECTRL_PM_NORMAL	0x1800	/* PM == 0011: Normal mode */
#define ECTRL_PM_STANBY	0x6000	/* PM == 1100: Standby mode */
#define ECTRL_PM_SREQ	0x5800	/* PM == 1011: Sleep Request mode */
#define ECTRL_SJ_TST	0x0400	/* enable/disable Slave jitter test */
#define ECTRL_TR_RST	0x0200	/* Autonegotiation process restart */
#define ECTRL_TST_MODE	0x01c0	/* test mode selection */
#define ECTRL_C_TST	0x0020	/* TDR-based cable test */
#define ECTRL_LOOPBACK	0x0018	/* loopback mode select */
#define ECTRL_CFGEN	0x0004	/* configuration register access */
#define ECTRL_CFGINH	0x0002	/* INH configuration */
#define ECTRL_WAKE_REQ	0x0001	/* wake-up request configuration */

/* Configuration register 1 */
#define CFG1_MS		0x8000	/* PHY Master/Slave configuration */
#define CFG1_AUTO_OP	0x4000	/* managed/autonomous operation */
#define CFG1_LINKLEN	0x2000	/* cable length: 0 < 15 m; 1 > 15 m */
#define CFG1_TXAMP	0x0c00	/* nominal transmit amplitude */
#define CFG1_TXAMP_050	0x0000	/* TXAMP == 00: 500 mV */
#define CFG1_TXAMP_075	0x0200	/* TXAMP == 01: 750 mV */
#define CFG1_TXAMP_100	0x0400	/* TXAMP == 10: 1000 mV */
#define CFG1_TXAMP_125	0x0c00	/* TXAMP == 11: 1250 mV */
#define CFG1_MODE	0x0300	/* MII/RMII mode */
#define CFG1_DRIVER	0x0080	/* MII output driver strength */
#define CFG1_SC		0x0040	/* sleep confirmation setting */
#define CFG1_LED_MODE	0x0030	/* LED mode */
#define CFG1_LED_EN	0x0008	/* LED enable */
#define CFG1_CFG_WAKE	0x0004	/* local wake configuration */
#define CFG1_APWD	0x0002	/* autonomous power down */
#define CFG1_LPS	0x0001	/* LPS code group reception */

/* Configuration register 2 */
#define CFG2_PHYAD_4_0	0xf800	/* PHY address used for the SMI addr */
#define CFG2_SNR_AVG	0x0600	/* signal-to-noise ratio averaging */
#define CFG2_SNR_WLIM	0x01c0	/* signal-to-noise ratio warning limit */
#define CFG2_SNR_FLIM	0x0038	/* signal-to-noise ratio fail limit */
#define CFG2_JUMBO_EN	0x0004	/* Jumbo packet support */
#define CFG2_SRTO	0x0003	/* sleep request time-out */
#define CFG2_SRTO_04	0x0000	/* SRTO == 00: 0.4 ms */
#define CFG2_SRTO_1	0x0001	/* SRTO == 01: 1 ms */
#define CFG2_SRTO_4	0x0002	/* SRTO == 10: 4 ms */
#define CFG2_SRTO_16	0x0003	/* SRTO == 11: 16 ms */

/* Symbol error counter register 2 */
#define SERRCNT_SEC	0xffff	/* The symbol error counter */

/* Interrupt status register */
#define INTST_PWON	0x8000	/* power-on detected */
#define INTST_WAKEUP	0x4000	/* local or remote wake-up detected */
#define INTST_WUR	0x2000	/* dedicated wake-up request detected */
#define INTST_LPS	0x1000	/* LPS code groups received */
#define INTST_PIF	0x0800	/* PHY initialization error detected */
#define INTST_LINK_FAIL	0x0400	/* link status changed to ‘link fail’ */
#define INTST_LINK_UP	0x0200	/* link status changed to ‘link up’ */
#define INTST_SYM_ERR	0x0100	/* symbol error detected */
#define INTST_TF	0x0080	/* training phase failure detected */
#define INTST_SNRW	0x0040	/* SNR value above warning limit */
#define INTST_CTRL_ERR	0x0020	/* SMI control error detected */
#define INTST_TXENC	0x0010	/* TXEN clamping detected */
#define INTST_UV_ERR	0x0008	/* undervoltage detected */
#define INTST_UVR	0x0004	/* undervoltage recovery detected */
#define INTST_TEMP_ERR	0x0002	/* overtemperature error detected */
#define INTST_SA	0x0001	/* transition to Normal on timer expiring */

/* Interrupt enable register */
#define INTEN_PWON	0x8000	/* PWON interrupt enable */
#define INTEN_WAKEUP	0x4000	/* WAKEUP interrupt enable */
#define INTEN_WUR	0x2000	/* WUR_RECEIVED interrupt enable */
#define INTEN_LPS	0x1000	/* LPS_RECEIVED interrupt enable */
#define INTEN_PIF	0x0800	/* PHY_INIT_FAIL interrupt enable */
#define INTEN_LINK_FAIL	0x0400	/* LINK_STATUS_FAIL interrupt enable */
#define INTEN_LINK_UP	0x0200	/* LINK_STATUS_UP interrupt enable */
#define INTEN_SYM_ERR	0x0100	/* SYM_ERR interrupt enable */
#define INTEN_TF	0x0080	/* TRAINING_FAILED interrupt enable */
#define INTEN_SNRW	0x0040	/* SNR_WARNING interrupt enable */
#define INTEN_CTRL_ERR	0x0020	/* CONTROL_ERR interrupt enable */
#define INTEN_TXENC	0x0010	/* TXEN_CLAMPED interrupt enable */
#define INTEN_UV_ERR	0x0008	/* UV_ERR interrupt enable */
#define INTEN_UVR	0x0004	/* UV_RECOVERY interrupt enable */
#define INTEN_TEMP_ERR	0x0002	/* TEMP_ERR interrupt enable */
#define INTEN_SA	0x0001	/* SLEEP_ABORT interrupt enable */

/* Communication status register */
#define COMST_LINK_UP	0x8000	/* link OK */
#define COMST_TXM	0x6000	/* transmitter mode */
#define COMST_TXM_DIS	0x0000	/* TXM == 00: transmitter disabled */
#define COMST_TXM_DIS	0x0000	/* TXM == 01: transmitter in SEND_N mode */
#define COMST_TXM_DIS	0x0000	/* TXM == 10: transmitter in SEND_I mode */
#define COMST_TXM_DIS	0x0000	/* TXM == 11: transmitter in SEND_Z mode */
#define COMST_LR	0x1000	/* local receiver OK */
#define COMST_RR	0x0800	/* remote receiver OK */
#define COMST_SCRL	0x0400	/* descrambler locked */
#define COMST_SSD_ERR	0x0200	/* SSD error detected */
#define COMST_ESD_ERR	0x0100	/* ESD error detected */
#define COMST_SNR	0x00e0	/* SNR link status */
#define COMST_RX_ERR	0x0010	/* receive error detected since last read */
#define COMST_TX_ERR	0x0080	/* transmit error detected since last read */
#define COMST_PS	0x0007	/* PHY state */

/* General status register */
#define GST_INTP	0x8000	/* unmasked interrupt pending */
#define GST_PLL_LOCKED	0x4000	/* PLL stable and locked */
#define GST_LWU		0x2000	/* local wake-up detected */
#define GST_RWU		0x1000	/* remote wake-up detected */
#define GST_DDWU	0x0800	/* data detected at MDI in Sleep Request mode */
#define GST_EN		0x0400	/* EN switched LOW since last read */
#define GST_RST		0x0200	/* hardware reset detected since last read */
#define GST_LF_CNT	0x00f8	/* number of link fails since last read */

/* External status register */
#define EXTST_UVDDA_3V3	0x4000	/* undervoltage detected on pin VDDA(3V3) */
#define EXTST_UVDDD_1V8	0x2000	/* undervoltage detected on pin VDDD(1V8) */
#define EXTST_UVDDA_1V8	0x1000	/* undervoltage detected on pin VDDA(1V8) */
#define EXTST_UVDDIO	0x0800	/* undervoltage detected on pin VDD(IO) */
#define EXTST_TH	0x0400	/* temperature above high level */
#define EXTST_TW	0x0200	/* temperature above warning level */
#define EXTST_SD	0x0100	/* short circuit detected since last read */
#define EXTST_OD	0x0080	/* open circuit detected since last read */
#define EXTST_INTDET	0x0040	/* interleave order detection */

/* Link fail counter register */
#define LFCNT_LRC	0xff00	/* incremented when local receiver is NOT_OK */
#define LFCNT_RRC	0x00ff	/* incremented when remote receiver is NOT_OK */

static int tja1100_phy_config_init(struct phy_device *phydev)
{
	u32 features;

	/* TJA1100 has only 100BASE-BroadR-REACH ability specified at
	 * MII_ESTATUS register. Standard modes are not supported. Therefore
	 * BroadR-REACH allow only 100Mbps full duplex without autoneg.
	 */
	features = SUPPORTED_MII;
	features |= SUPPORTED_100baseT_Full;

	phydev->supported &= features;
	phydev->advertising &= features;
	phydev->autoneg = AUTONEG_DISABLE;
	phydev->speed = SPEED_100;
	phydev->duplex = DUPLEX_FULL;

	return 0;
}

static int tja1100_phy_config_aneg(struct phy_device *phydev)
{
	if (phydev->autoneg == AUTONEG_ENABLE) {
		pr_err("TJA1100: autonegotiation is not supported\n");
		return -1;
	}

	if (phydev->speed != SPEED_100 || phydev->duplex != DUPLEX_FULL) {
		pr_err("TJA1100: only 100MBps Full Duplex allowed\n");
		return -2;
	}

	return 0;
}

static struct phy_driver tja1100_phy_driver[] = {
	{
		.phy_id = 0x0180dc48,
		.phy_id_mask = 0xfffffff0,
		.name = "NXP TJA1100",

		.features = SUPPORTED_100baseT_Full | SUPPORTED_MII,

		.config_aneg = tja1100_phy_config_aneg,
		.read_status = genphy_read_status,
		.config_init = tja1100_phy_config_init,
		.soft_reset = genphy_soft_reset,

		.suspend = genphy_suspend,
		.resume = genphy_resume,
	}
};

module_phy_driver(tja1100_phy_driver);

MODULE_DESCRIPTION("NXP TJA1100 driver");
MODULE_AUTHOR("Kirill Kranke <kkranke@topcon.com>");
MODULE_LICENSE("GPL");

static struct mdio_device_id __maybe_unused nxp_tbl[] = {
	{ 0x0180dc48, 0xfffffff0 },
	{}
};

MODULE_DEVICE_TABLE(mdio, nxp_tbl);
