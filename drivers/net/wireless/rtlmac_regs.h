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
 * Register definitions taken from original Realttek rtl8723au driver
 */

/* 0x0000 ~ 0x00FF System Configuration */
#define REG_SYS_ISO_CTRL		0x0000
#define REG_SYS_FUNC_EN			0x0002
#define REG_APS_FSMCO			0x0004
#define REG_SYS_CLKR			0x0008
#define REG_9346CR			0x000A
#define REG_EE_VPD			0x000C
#define REG_AFE_MISC			0x0010
#define REG_SPS0_CTRL			0x0011
#define REG_SPS_OCP_CFG			0x0018
#define REG_RSV_CTRL			0x001C
#define REG_RF_CTRL			0x001F
#define REG_LDOA15_CTRL			0x0020
#define REG_LDOV12D_CTRL		0x0021
#define REG_LDOHCI12_CTRL		0x0022
#define REG_LPLDO_CTRL			0x0023
#define REG_AFE_XTAL_CTRL		0x0024
#define REG_AFE_PLL_CTRL		0x0028
#define REG_MAC_PHY_CTRL		0x002c
#define REG_EFUSE_CTRL			0x0030
#define REG_EFUSE_TEST			0x0034
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
/*  RTL8723 WIFI/BT/GPS Multi-Function control source. */
#define REG_MULTI_FUNC_CTRL		0x0068
#define REG_MCUFWDL			0x0080
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
#define REG_GPIO_OUTSTS			0x00F4	/*  For RTL8723 only. */

/* 0x0100h ~ 0x01FFh MACTOP General Configuration */
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
#define REG_BB_ACCEESS_CTRL		0x01E8
#define REG_BB_ACCESS_DATA		0x01EC
