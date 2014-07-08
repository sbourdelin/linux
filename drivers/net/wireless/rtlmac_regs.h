/*
 * Copyright (c) 2014 Jes Sorensen <Jes.Sorensen@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * Register definitions taken from original Realtek rtl8723au driver
 */

/* 0x0000 ~ 0x00FF	System Configuration */
#define REG_SYS_ISO_CTRL		0x0000
#define  SYS_ISO_PWC_EV25V		BIT(14)
#define  SYS_ISO_PWC_EV12V		BIT(15)

#define REG_SYS_FUNC			0x0002
#define  SYS_FUNC_BBRSTB		BIT(0)
#define  SYS_FUNC_BB_GLB_RSTN		BIT(1)
#define  SYS_FUNC_USBA			BIT(2)
#define  SYS_FUNC_UPLL			BIT(3)
#define  SYS_FUNC_USBD			BIT(4)
#define  SYS_FUNC_DIO_PCIE		BIT(5)
#define  SYS_FUNC_PCIEA			BIT(6)
#define  SYS_FUNC_PPLL			BIT(7)
#define  SYS_FUNC_PCIED			BIT(8)
#define  SYS_FUNC_DIOE			BIT(9)
#define  SYS_FUNC_CPU_ENABLE		BIT(10)
#define  SYS_FUNC_DCORE			BIT(11)
#define  SYS_FUNC_ELDR			BIT(12)
#define  SYS_FUNC_DIO_RF		BIT(13)
#define  SYS_FUNC_HWPDN			BIT(14)
#define  SYS_FUNC_MREGEN		BIT(15)

#define REG_APS_FSMCO			0x0004

#define REG_SYS_CLKR			0x0008
#define  SYS_CLK_ANAD16V_ENABLE		BIT(0)
#define  SYS_CLK_ANA8M			BIT(1)
#define  SYS_CLK_MACSLP			BIT(4)
#define  SYS_CLK_LOADER_ENABLE		BIT(5)
#define  SYS_CLK_80M_SSC_DISABLE	BIT(7)
#define  SYS_CLK_80M_SSC_ENABLE_HO	BIT(8)
#define  SYS_CLK_PHY_SSC_RSTB		BIT(9)
#define  SYS_CLK_SEC_CLK_ENABLE		BIT(10)
#define  SYS_CLK_MAC_CLK_ENABLE		BIT(11)
#define  SYS_CLK_ENABLE			BIT(12)
#define  SYS_CLK_RING_CLK_ENABLE	BIT(13)

#define REG_9346CR			0x000A
#define	 EEPROM_BOOT			BIT(4)
#define	 EEPROM_ENABLE			BIT(5)

#define REG_EE_VPD			0x000C
#define REG_AFE_MISC			0x0010
#define REG_SPS0_CTRL			0x0011
#define REG_SPS_OCP_CFG			0x0018
#define REG_RSV_CTRL			0x001C

#define REG_RF_CTRL			0x001F
#define  RF_ENABLE			BIT(0)
#define  RF_RSTB			BIT(1)
#define  RF_SDMRSTB			BIT(2)

#define REG_LDOA15_CTRL			0x0020
#define  LDOA15_ENABLE			BIT(0)
#define  LDOA15_STANDBY			BIT(1)
#define  LDOA15_OBUF			BIT(2)
#define  LDOA15_REG_VOS			BIT(3)
#define  LDOA15_VOADJ_SHIFT		4

#define REG_LDOV12D_CTRL		0x0021
#define  LDOV12D_ENABLE			BIT(0)
#define  LDOV12D_STANDBY		BIT(1)
#define  LDOV12D_VADJ_SHIFT		4

#define REG_LDOHCI12_CTRL		0x0022

#define REG_LPLDO_CTRL			0x0023
#define  LPLDO_HSM			BIT(2)
#define  LPLDO_LSM_DIS			BIT(3)

#define REG_AFE_XTAL_CTRL		0x0024
#define  AFE_XTAL_ENABLE		BIT(0)
#define  AFE_XTAL_B_SELECT		BIT(1)
#define  AFE_XTAL_RF_GATE		BIT(14)
#define  AFE_XTAL_BT_GATE		BIT(20)

#define REG_AFE_PLL_CTRL		0x0028
#define  AFE_PLL_ENABLE			BIT(0)
#define  AFE_PLL_320_ENABLE		BIT(1)
#define  APE_PLL_FREF_SELECT		BIT(2)
#define  AFE_PLL_EDGE_SELECT		BIT(3)
#define  AFE_PLL_WDOGB			BIT(4)
#define  AFE_PLL_LPF_ENABLE		BIT(5)

#define REG_MAC_PHY_CTRL		0x002c

#define REG_EFUSE_CTRL			0x0030
#define REG_EFUSE_TEST			0x0034
#define  EFUSE_TRPT			BIT(7)
	/*  00: Wifi Efuse, 01: BT Efuse0, 10: BT Efuse1, 11: BT Efuse2 */
#define  EFUSE_CELL_SEL			(BIT(8)|BIT(9))
#define  EFUSE_LDOE25_ENABLE		BIT(31)
#define  EFUSE_SELECT_MASK		0x0300
#define  EFUSE_WIFI_SELECT		0x0000
#define  EFUSE_BT0_SELECT		0x0100
#define  EFUSE_BT1_SELECT		0x0200
#define  EFUSE_BT2_SELECT		0x0300

#define  EFUSE_ACCESS_ENABLE		0x69	/* RTL8723 only */
#define  EFUSE_ACCESS_DISABLE		0x00	/* RTL8723 only */

#define REG_PWR_DATA			0x0038
#define REG_CAL_TIMER			0x003C
#define REG_ACLK_MON			0x003E
#define REG_GPIO_MUXCFG			0x0040
#define REG_GPIO_IO_SEL			0x0042
#define REG_MAC_PINMUX_CFG		0x0043
#define REG_GPIO_PIN_CTRL		0x0044
#define REG_GPIO_INTM			0x0048
#define REG_LEDCFG0			0x004C
#define REG_LEDCFG1			0x004D
#define REG_LEDCFG2			0x004E
#define REG_LEDCFG3			0x004F
#define REG_LEDCFG			REG_LEDCFG2
#define REG_FSIMR			0x0050
#define REG_FSISR			0x0054
#define REG_HSIMR			0x0058
#define REG_HSISR			0x005c
/*  RTL8723 WIFI/BT/GPS Multi-Function GPIO Pin Control. */
#define REG_GPIO_PIN_CTRL_2		0x0060
/*  RTL8723 WIFI/BT/GPS Multi-Function GPIO Select. */
#define REG_GPIO_IO_SEL_2		0x0062

/*  RTL8723 only WIFI/BT/GPS Multi-Function control source. */
#define REG_MULTI_FUNC_CTRL		0x0068

#define	 MULTI_FN_WIFI_HW_PWRDOWN_EN	BIT(0)	/* Enable GPIO[9] as WiFi HW
						   powerdown source */
#define	 MULTI_FN_WIFI_HW_PWRDOWN_SL	BIT(1)	/* WiFi HW powerdown polarity
						   control */
#define	 MULTI_WIFI_FUNC_EN		BIT(2)	/* WiFi function enable */

#define	 MULTI_WIFI_HW_ROF_EN		BIT(3)	/* Enable GPIO[9] as WiFi RF HW
						   powerdown source */
#define	 MULTI_BT_HW_PWRDOWN_EN		BIT(16)	/* Enable GPIO[11] as BT HW
						   powerdown source */
#define	 MULTI_BT_HW_PWRDOWN_SL		BIT(17)	/* BT HW powerdown polarity
						   control */
#define	 MULTI_BT_FUNC_EN		BIT(18)	/* BT function enable */
#define	 MULTI_BT_HW_ROF_EN		BIT(19)	/* Enable GPIO[11] as BT/GPS
						   RF HW powerdown source */
#define	 MULTI_GPS_HW_PWRDOWN_EN	BIT(20)	/* Enable GPIO[10] as GPS HW
						   powerdown source */
#define	 MULTI_GPS_HW_PWRDOWN_SL	BIT(21)	/* GPS HW powerdown polarity
						   control */
#define	 MULTI_GPS_FUNC_EN		BIT(22)	/* GPS function enable */


#define REG_MCU_FW_DL			0x0080
#define  MCU_FW_DL_ENABLE		BIT(0)
#define  MCU_FW_DL_READY		BIT(1)
#define  MCU_FW_DL_CSUM_REPORT		BIT(2)
#define  MCU_MAC_INIT_READY		BIT(3)
#define  MCU_BB_INIT_READY		BIT(4)
#define  MCU_RF_INIT_READY		BIT(5)
#define  MCU_WINT_INIT_READY		BIT(6)
#define  MCU_FW_RAM_SEL			BIT(7)	/* 1: RAM, 0:ROM */
#define  MCU_CP_RESET			BIT(23)

#define REG_HMEBOX_EXT_0		0x0088
#define REG_HMEBOX_EXT_1		0x008A
#define REG_HMEBOX_EXT_2		0x008C
#define REG_HMEBOX_EXT_3		0x008E
/*  Host suspend counter on FPGA platform */
#define REG_HOST_SUSP_CNT		0x00BC
/*  Efuse access protection for RTL8723 */
#define REG_EFUSE_ACCESS		0x00CF
#define REG_BIST_SCAN			0x00D0
#define REG_BIST_RPT			0x00D4
#define REG_BIST_ROM_RPT		0x00D8
#define REG_USB_SIE_INTF		0x00E0
#define REG_PCIE_MIO_INTF		0x00E4
#define REG_PCIE_MIO_INTD		0x00E8
#define REG_HPON_FSM			0x00EC
#define REG_SYS_CFG			0x00F0

#define XCLK_VLD			BIT(0)
#define ACLK_VLD			BIT(1)
#define UCLK_VLD			BIT(2)
#define PCLK_VLD			BIT(3)
#define PCIRSTB				BIT(4)
#define V15_VLD				BIT(5)
#define TRP_B15V_EN			BIT(7)
#define SIC_IDLE			BIT(8)
#define BD_MAC2				BIT(9)
#define BD_MAC1				BIT(10)
#define IC_MACPHY_MODE			BIT(11)
#define CHIP_VER			(BIT(12)|BIT(13)|BIT(14)|BIT(15))
#define BT_FUNC				BIT(16)
#define VENDOR_ID			BIT(19)
#define PAD_HWPD_IDN			BIT(22)
#define TRP_VAUX_EN			BIT(23)
#define TRP_BT_EN			BIT(24)
#define BD_PKG_SEL			BIT(25)
#define BD_HCI_SEL			BIT(26)
#define TYPE_ID				BIT(27)

#define  SYS_CFG_XCLK_VLD		BIT(0)
#define  SYS_CFG_ACLK_VLD		BIT(1)
#define  SYS_CFG_UCLK_VLD		BIT(2)
#define  SYS_CFG_PCLK_VLD		BIT(3)
#define  SYS_CFG_PCIRSTB		BIT(4)
#define  SYS_CFG_V15_VLD		BIT(5)
#define  SYS_CFG_TRP_B15V_EN		BIT(7)
#define  SYS_CFG_SIC_IDLE		BIT(8)
#define  SYS_CFG_BD_MAC2		BIT(9)
#define  SYS_CFG_BD_MAC1		BIT(10)
#define  SYS_CFG_IC_MACPHY_MODE		BIT(11)
#define  SYS_CFG_CHIP_VER		(BIT(12)|BIT(13)|BIT(14)|BIT(15))
#define  SYS_CFG_BT_FUNC		BIT(16)
#define  SYS_CFG_VENDOR_ID		BIT(19)
#define  SYS_CFG_PAD_HWPD_IDN		BIT(22)
#define  SYS_CFG_TRP_VAUX_EN		BIT(23)
#define  SYS_CFG_TRP_BT_EN		BIT(24)
#define  SYS_CFG_BD_PKG_SEL		BIT(25)
#define  SYS_CFG_BD_HCI_SEL		BIT(26)
#define  SYS_CFG_TYPE_ID		BIT(27)
#define  SYS_CFG_RTL_ID			BIT(23) /*  TestChip ID,
						    1:Test(RLE); 0:MP(RL) */
#define  SYS_CFG_SPS_SEL		BIT(24) /*  1:LDO regulator mode;
						    0:Switching regulator mode*/
#define  SYS_CFG_CHIP_VERSION_MASK	0xF000	/* Bit 12 - 15 */
#define  SYS_CFG_CHIP_VERSION_SHIFT	12


#define REG_GPIO_OUTSTS			0x00F4	/*  For RTL8723 only. */
#define	 GPIO_EFS_HCI_SEL		(BIT(0)|BIT(1))
#define	 GPIO_PAD_HCI_SEL		(BIT(2)|BIT(3))
#define	 GPIO_HCI_SEL			(BIT(4)|BIT(5))
#define	 GPIO_PKG_SEL_HCI		BIT(6)
#define	 GPIO_FEN_GPS			BIT(7)
#define	 GPIO_FEN_BT			BIT(8)
#define	 GPIO_FEN_WL			BIT(9)
#define	 GPIO_FEN_PCI			BIT(10)
#define	 GPIO_FEN_USB			BIT(11)
#define	 GPIO_BTRF_HWPDN_N		BIT(12)
#define	 GPIO_WLRF_HWPDN_N		BIT(13)
#define	 GPIO_PDN_BT_N			BIT(14)
#define	 GPIO_PDN_GPS_N			BIT(15)
#define	 GPIO_BT_CTL_HWPDN		BIT(16)
#define	 GPIO_GPS_CTL_HWPDN		BIT(17)
#define	 GPIO_PPHY_SUSB			BIT(20)
#define	 GPIO_UPHY_SUSB			BIT(21)
#define	 GPIO_PCI_SUSEN			BIT(22)
#define	 GPIO_USB_SUSEN			BIT(23)
#define	 GPIO_RF_RL_ID			(BIT(31)|BIT(30)|BIT(29)|BIT(28))

/* 0x0100 ~ 0x01FF	MACTOP General Configuration */
#define REG_CR				0x0100
#define  HCI_TXDMA_EN			BIT(0)
#define  HCI_RXDMA_EN			BIT(1)
#define  TXDMA_EN			BIT(2)
#define  RXDMA_EN			BIT(3)
#define  PROTOCOL_EN			BIT(4)
#define  SCHEDULE_EN			BIT(5)
#define  MACTXEN			BIT(6)
#define  MACRXEN			BIT(7)
#define  ENSWBCN			BIT(8)
#define  ENSEC				BIT(9)
#define  CALTMR_EN			BIT(10)

#define REG_PBP				0x0104
#define REG_TRXDMA_CTRL			0x010C
#define REG_TRXFF_BNDY			0x0114
#define REG_TRXFF_STATUS		0x0118
#define REG_RXFF_PTR			0x011C
#define REG_HIMR			0x0120
#define REG_HISR			0x0124
#define REG_HIMRE			0x0128
#define REG_HISRE			0x012C
#define REG_CPWM			0x012F
#define REG_FWIMR			0x0130
#define REG_FWISR			0x0134
#define REG_PKTBUF_DBG_CTRL		0x0140
#define REG_PKTBUF_DBG_DATA_L		0x0144
#define REG_PKTBUF_DBG_DATA_H		0x0148

#define REG_TC0_CTRL			0x0150
#define REG_TC1_CTRL			0x0154
#define REG_TC2_CTRL			0x0158
#define REG_TC3_CTRL			0x015C
#define REG_TC4_CTRL			0x0160
#define REG_TCUNIT_BASE			0x0164
#define REG_MBIST_START			0x0174
#define REG_MBIST_DONE			0x0178
#define REG_MBIST_FAIL			0x017C
#define REG_C2HEVT_MSG_NORMAL		0x01A0
#define REG_C2HEVT_CLEAR		0x01AF
#define REG_C2HEVT_MSG_TEST		0x01B8
#define REG_MCUTST_1			0x01c0
#define REG_FMETHR			0x01C8
#define REG_HMETFR			0x01CC
#define REG_HMEBOX_0			0x01D0
#define REG_HMEBOX_1			0x01D4
#define REG_HMEBOX_2			0x01D8
#define REG_HMEBOX_3			0x01DC

#define REG_LLT_INIT			0x01E0
#define  LLT_OP_INACTIVE		0x0
#define  LLT_OP_WRITE			(0x1 << 30)
#define  LLT_OP_READ			(0x2 << 30)
#define  LLT_OP_MASK			(0x3 << 30)

#define REG_BB_ACCEESS_CTRL		0x01E8
#define REG_BB_ACCESS_DATA		0x01EC

/* 0x0200 ~ 0x027F	TXDMA Configuration */
#define REG_RQPN			0x0200
#define REG_FIFOPAGE			0x0204
#define REG_TDECTRL			0x0208
#define REG_TXDMA_OFFSET_CHK		0x020C
#define REG_TXDMA_STATUS		0x0210
#define REG_RQPN_NPQ			0x0214

/* 0x0280 ~ 0x02FF	RXDMA Configuration */
#define REG_RXDMA_AGG_PG_TH		0x0280
#define REG_RXPKT_NUM			0x0284
#define REG_RXDMA_STATUS		0x0288

#define REG_RF_BB_CMD_ADDR		0x02c0
#define REG_RF_BB_CMD_DATA		0x02c4

/*  spec version 11 */
/* 0x0400 ~ 0x047F	Protocol Configuration */
#define REG_VOQ_INFORMATION		0x0400
#define REG_VIQ_INFORMATION		0x0404
#define REG_BEQ_INFORMATION		0x0408
#define REG_BKQ_INFORMATION		0x040C
#define REG_MGQ_INFORMATION		0x0410
#define REG_HGQ_INFORMATION		0x0414
#define REG_BCNQ_INFORMATION		0x0418


#define REG_CPU_MGQ_INFORMATION		0x041C
#define REG_FWHW_TXQ_CTRL		0x0420
#define REG_HWSEQ_CTRL			0x0423
#define REG_TXPKTBUF_BCNQ_BDNY		0x0424
#define REG_TXPKTBUF_MGQ_BDNY		0x0425
#define REG_LIFETIME_EN			0x0426
#define REG_MULTI_BCNQ_OFFSET		0x0427
#define REG_SPEC_SIFS			0x0428
#define REG_RL				0x042A
#define REG_DARFRC			0x0430
#define REG_RARFRC			0x0438
#define REG_RRSR			0x0440
#define REG_ARFR0			0x0444
#define REG_ARFR1			0x0448
#define REG_ARFR2			0x044C
#define REG_ARFR3			0x0450
#define REG_AGGLEN_LMT			0x0458
#define REG_AMPDU_MIN_SPACE		0x045C
#define REG_TXPKTBUF_WMAC_LBK_BF_HD	0x045D
#define REG_FAST_EDCA_CTRL		0x0460
#define REG_RD_RESP_PKT_TH		0x0463
#define REG_INIRTS_RATE_SEL		0x0480
#define REG_INIDATA_RATE_SEL		0x0484


#define REG_POWER_STATUS		0x04A4
#define REG_POWER_STAGE1		0x04B4
#define REG_POWER_STAGE2		0x04B8
#define REG_PKT_VO_VI_LIFE_TIME		0x04C0
#define REG_PKT_BE_BK_LIFE_TIME		0x04C2
#define REG_STBC_SETTING		0x04C4
#define REG_PROT_MODE_CTRL		0x04C8
#define REG_MAX_AGGR_NUM		0x04CA
#define REG_RTS_MAX_AGGR_NUM		0x04CB
#define REG_BAR_MODE_CTRL		0x04CC
#define REG_RA_TRY_RATE_AGG_LMT		0x04CF
#define REG_NQOS_SEQ			0x04DC
#define REG_QOS_SEQ			0x04DE
#define REG_NEED_CPU_HANDLE		0x04E0
#define REG_PKT_LOSE_RPT		0x04E1
#define REG_PTCL_ERR_STATUS		0x04E2
#define REG_DUMMY			0x04FC



/* 0x0500 ~ 0x05FF	EDCA Configuration */
#define REG_EDCA_VO_PARAM		0x0500
#define REG_EDCA_VI_PARAM		0x0504
#define REG_EDCA_BE_PARAM		0x0508
#define REG_EDCA_BK_PARAM		0x050C
#define REG_BCNTCFG			0x0510
#define REG_PIFS			0x0512
#define REG_RDG_PIFS			0x0513
#define REG_SIFS_CCK			0x0514
#define REG_SIFS_OFDM			0x0516
#define REG_SIFS_CTX			0x0514
#define REG_SIFS_TRX			0x0516
#define REG_TSFTR_SYN_OFFSET		0x0518
#define REG_AGGR_BREAK_TIME		0x051A
#define REG_SLOT			0x051B
#define REG_TX_PTCL_CTRL		0x0520
#define REG_TXPAUSE			0x0522
#define REG_DIS_TXREQ_CLR		0x0523
#define REG_RD_CTRL			0x0524
#define REG_TBTT_PROHIBIT		0x0540
#define REG_RD_NAV_NXT			0x0544
#define REG_NAV_PROT_LEN		0x0546
#define REG_BCN_CTRL			0x0550
#define REG_BCN_CTRL_1			0x0551
#define REG_MBID_NUM			0x0552
#define REG_DUAL_TSF_RST		0x0553
	/*  The same as REG_MBSSID_BCN_SPACE */
#define REG_BCN_INTERVAL		0x0554
#define REG_MBSSID_BCN_SPACE		0x0554
#define REG_DRVERLYINT			0x0558
#define REG_BCNDMATIM			0x0559
#define REG_ATIMWND			0x055A
#define REG_BCN_MAX_ERR			0x055D
#define REG_RXTSF_OFFSET_CCK		0x055E
#define REG_RXTSF_OFFSET_OFDM		0x055F
#define REG_TSFTR			0x0560
#define REG_TSFTR1			0x0568
#define REG_INIT_TSFTR			0x0564
#define REG_ATIMWND_1			0x0570
#define REG_PSTIMER			0x0580
#define REG_TIMER0			0x0584
#define REG_TIMER1			0x0588
#define REG_ACMHWCTRL			0x05C0
#define REG_ACMRSTCTRL			0x05C1
#define REG_ACMAVG			0x05C2
#define REG_VO_ADMTIME			0x05C4
#define REG_VI_ADMTIME			0x05C6
#define REG_BE_ADMTIME			0x05C8
#define REG_EDCA_RANDOM_GEN		0x05CC
#define REG_SCH_TXCMD			0x05D0

/* define REG_FW_TSF_SYNC_CNT		0x04A0 */
#define REG_FW_RESET_TSF_CNT_1		0x05FC
#define REG_FW_RESET_TSF_CNT_0		0x05FD
#define REG_FW_BCN_DIS_CNT		0x05FE

/* 0x0600 ~ 0x07FF  WMAC Configuration */
#define REG_APSD_CTRL			0x0600
#define REG_BWOPMODE			0x0603
#define REG_TCR				0x0604
#define REG_RCR				0x0608
#define REG_RX_PKT_LIMIT		0x060C
#define REG_RX_DLK_TIME			0x060D
#define REG_RX_DRVINFO_SZ		0x060F

#define REG_MACID			0x0610
#define REG_BSSID			0x0618
#define REG_MAR				0x0620
#define REG_MBIDCAMCFG			0x0628

#define REG_USTIME_EDCA			0x0638
#define REG_MAC_SPEC_SIFS		0x063A

/*  20100719 Joseph: Hardware register definition change. (HW datasheet v54) */
	/*  [15:8]SIFS_R2T_OFDM, [7:0]SIFS_R2T_CCK */
#define REG_R2T_SIFS			0x063C
	/*  [15:8]SIFS_T2T_OFDM, [7:0]SIFS_T2T_CCK */
#define REG_T2T_SIFS			0x063E
#define REG_ACKTO			0x0640
#define REG_CTS2TO			0x0641
#define REG_EIFS			0x0642

/* WMA, BA, CCX */
#define REG_NAV_CTRL			0x0650
#define REG_BACAMCMD			0x0654
#define REG_BACAMCONTENT		0x0658
#define REG_LBDLY			0x0660
#define REG_FWDLY			0x0661
#define REG_RXERR_RPT			0x0664
#define REG_WMAC_TRXPTCL_CTL		0x0668


/*  Security */
#define REG_CAMCMD			0x0670
#define REG_CAMWRITE			0x0674
#define REG_CAMREAD			0x0678
#define REG_CAMDBG			0x067C
#define REG_SECCFG			0x0680

/*  Power */
#define REG_WOW_CTRL			0x0690
#define REG_PSSTATUS			0x0691
#define REG_PS_RX_INFO			0x0692
#define REG_LPNAV_CTRL			0x0694
#define REG_WKFMCAM_CMD			0x0698
#define REG_WKFMCAM_RWD			0x069C
#define REG_RXFLTMAP0			0x06A0
#define REG_RXFLTMAP1			0x06A2
#define REG_RXFLTMAP2			0x06A4
#define REG_BCN_PSR_RPT			0x06A8
#define REG_CALB32K_CTRL		0x06AC
#define REG_PKT_MON_CTRL		0x06B4
#define REG_BT_COEX_TABLE		0x06C0
#define REG_WMAC_RESP_TXINFO		0x06D8

#define REG_MACID1			0x0700
#define REG_BSSID1			0x0708

#define REG_FPGA0_RF_MODE		0x0800
#define REG_FPGA0_TXINFO		0x0804
#define REG_FPGA0_PSD_FUNC		0x0808
#define REG_FPGA0_TX_GAIN		0x080c
#define REG_FPGA0_XA_HSSI_PARM1		0x0820	/* RF 3 wire register */
#define REG_FPGA0_XA_HSSI_PARM2		0x0824
#define REG_FPGA0_XB_HSSI_PARM1		0x0828
#define REG_FPGA0_XB_HSSI_PARM2		0x082c
#define  FPGA0_HSSI_3WIRE_DATA_LEN	0x800
#define  FPGA0_HSSI_3WIRE_ADDR_LEN	0x400

#define REG_FPGA0_XA_RF_INT_OE		0x0860	/* RF Channel switch */
#define REG_FPGA0_XB_RF_INT_OE		0x0864
#define REG_FPGA0_XAB_RF_SW_CTRL	0x0870
#define REG_FPGA0_XA_RF_SW_CTRL		0x0870	/* 16 bit */
#define REG_FPGA0_XB_RF_SW_CTRL		0x0872	/* 16 bit */
#define REG_FPGA0_XCD_RF_SW_CTRL	0x0874
#define REG_FPGA0_XC_RF_SW_CTRL		0x0874	/* 16 bit */
#define REG_FPGA0_XD_RF_SW_CTRL		0x0876	/* 16 bit */
#define  FPGA0_RF_3WIRE_DATA		0x1
#define  FPGA0_RF_3WIRE_CLOC		0x2
#define  FPGA0_RF_3WIRE_LOAD		0x4
#define  FPGA0_RF_3WIRE_RW		0x8
#define  FPGA0_RF_3WIRE_MASK		0xf
#define  FPGA0_RF_RFENV			0x10
#define  FPGA0_RF_TRSW			0x20	/* Useless now */
#define  FPGA0_RF_TRSWB			0x40
#define  FPGA0_RF_ANTSW			0x100
#define  FPGA0_RF_ANTSWB		0x200
#define  FPGA0_RF_PAPE			0x400
#define  FPGA0_RF_PAPE5G		0x800

#define REG_8723A_FW_START_ADDRESS	0x1000
