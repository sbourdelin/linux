/*
 * Copyright (C) 2017 Microchip Technology
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */
#ifndef _LAN743X_H
#define _LAN743X_H

/* Register Definitions */
#define ID_REV					(0x00)
#define ID_REV_CHIP_ID_MASK_			(0xFFFF0000)
#define ID_REV_CHIP_REV_MASK_			(0x0000FFFF)
#define ID_REV_CHIP_ID_7430_			(0x7430)

#define FPGA_REV				(0x04)
#define FPGA_REV_MINOR_MASK_			(0x0000FF00)
#define FPGA_REV_MAJOR_MASK_			(0x000000FF)

#define HW_CFG					(0x010)
#define HW_CFG_INVERT_LED3_POLARITY		BIT(31)
#define HW_CFG_INVERT_LED2_POLARITY		BIT(30)
#define HW_CFG_INVERT_LED1_POLARITY		BIT(29)
#define HW_CFG_INVERT_LED0_POLARITY		BIT(28)
#define HW_CFG_CLK125_EN_			BIT(25)
#define HW_CFG_REFCLK25_EN_			BIT(24)
#define HW_CFG_LED3_EN_				BIT(23)
#define HW_CFG_LED2_EN_				BIT(22)
#define HW_CFG_LED1_EN_				BIT(21)
#define HW_CFG_LED0_EN_				BIT(20)
#define HW_CFG_EEE_PHY_LUSU_			BIT(17)
#define HW_CFG_EEE_TSU_				BIT(16)
#define HW_CFG_RST_PROTECT_			BIT(12)
#define HW_CFG_RL_TYPE_EEPROM_UIT_CSR_		BIT(11)
#define HW_CFG_RL_TYPE_EEPROM_UIT_PCIE_		BIT(10)
#define HW_CFG_RL_TYPE_LED_CONFIG_		BIT(9)
#define HW_CFG_RL_TYPE_MAC_CONFIG_		BIT(8)
#define HW_CFG_RL_TYPE_PCI_CONFIG_		BIT(7)
#define HW_CFG_RL_TYPE_MAC_ADDR_		BIT(6)
#define HW_CFG_EE_OTP_DL_			BIT(5)
#define HW_CFG_EE_OTP_RELOAD_			BIT(4)
#define HW_CFG_ETC_				BIT(3)
#define HW_CFG_EEP_GPIO_LED_PIN_DIS_		BIT(2)
#define HW_CFG_LRST_				BIT(1)
#define HW_CFG_SRST_				BIT(0)

#define PMT_CTL					(0x014)
#define PMT_CTL_ETH_PHY_D3_COLD_OVR_		BIT(27)
#define PMT_CTL_MAC_D3_TX_CLK_OVR_		BIT(26)
#define PMT_CTL_MAC_D3_RX_CLK_OVR_		BIT(25)
#define PMT_CTL_ETH_PHY_EDPD_PLL_CTL_		BIT(24)
#define PMT_CTL_ETH_PHY_D3_OVR_			BIT(23)
#define PMT_CTL_INT_D3_CLK_OVR_			BIT(22)
#define PMT_CTL_DMAC_D3_CLK_OVR_		BIT(21)
#define PMT_CTL_1588_D3_CLK_OVR_		BIT(20)
#define PMT_CTL_MAC_D3_CLK_OVR_			BIT(19)
#define PMT_CTL_RX_FCT_RFE_D3_CLK_OVR_		BIT(18)
#define PMT_CTL_TX_FCT_LSO_D3_CLK_OVR_		BIT(17)
#define PMT_CTL_OTP_EEPROM_D3_CLK_OVR_		BIT(16)
#define PMT_CTL_GPIO_WAKEUP_EN_			BIT(15)
#define PMT_CTL_GPIO_WUPS_			BIT(14)
#define PMT_CTL_EEE_WAKEUP_EN_			BIT(13)
#define PMT_CTL_EEE_WUPS_			BIT(12)
#define PMT_CTL_RES_CLR_WKP_MASK_		(0x00000300)
#define PMT_CTL_RES_CLR_WKP_STS_		BIT(9)
#define PMT_CTL_RES_CLR_WKP_EN_			BIT(8)
#define PMT_CTL_READY_				BIT(7)
#define PMT_CTL_EXT_PHY_RDY_EN_			BIT(5)
#define PMT_CTL_ETH_PHY_RST_			BIT(4)
#define PMT_CTL_WOL_EN_				BIT(3)
#define PMT_CTL_ETH_PHY_WAKE_EN_		BIT(2)
#define PMT_CTL_WUPS_MASK_			(0x00000003)
#define PMT_CTL_WUPS_MLT_			(0x00000003)
#define PMT_CTL_WUPS_MAC_			(0x00000002)
#define PMT_CTL_WUPS_PHY_			(0x00000001)

#define DP_SEL				(0x024)
#define DP_SEL_DPRDY_			BIT(31)
#define DP_SEL_MASK_			(0x0000001F)
#define DP_SEL_PCIE_DRCV_RAM		(0x00000016)
#define DP_SEL_PCIE_HRCV_RAM		(0x00000015)
#define DP_SEL_PCIE_SOT_RAM		(0x00000014)
#define DP_SEL_PCIE_RETRY_RAM		(0x00000013)
#define DP_SEL_DMAC_TX_RAM_0		(0x0000000F)
#define DP_SEL_DMAC_RX_RAM_3		(0x0000000E)
#define DP_SEL_DMAC_RX_RAM_2		(0x0000000D)
#define DP_SEL_DMAC_RX_RAM_1		(0x0000000C)
#define DP_SEL_DMAC_RX_RAM_0		(0x0000000B)
#define DP_SEL_DMAC_REORDER_BUFFER	(0x0000000A)
#define DP_SEL_FCT_TX_RAM_0		(0x00000006)
#define DP_SEL_FCT_RX_RAM_3		(0x00000005)
#define DP_SEL_FCT_RX_RAM_2		(0x00000004)
#define DP_SEL_FCT_RX_RAM_1		(0x00000003)
#define DP_SEL_FCT_RX_RAM_0		(0x00000002)
#define DP_SEL_RFE_RAM			(0x00000001)
#define DP_SEL_LSO_RAM			(0x00000000)

#define DP_SEL_VHF_HASH_LEN		(16)
#define DP_SEL_VHF_VLAN_LEN		(128)

#define DP_CMD				(0x028)
#define DP_CMD_WRITE_			(0x00000001)
#define DP_CMD_READ_			(0x00000000)

#define DP_ADDR				(0x02C)
#define DP_ADDR_MASK_			(0x00003FFF)

#define DP_DATA_0			(0x030)

#define DP_DATA_1			(0x034)

#define DP_DATA_2			(0x038)

#define DP_DATA_3			(0x03C)

#define GPIO_CFG0			(0x050)
#define GPIO_CFG0_GPIO_DIR_MASK_	(0x0FFF0000)
#define GPIO_CFG0_GPIO_DIR_(bit)	BIT(16 + (bit))
#define GPIO_CFG0_GPIO_DATA_MASK_	(0x00000FFF)
#define GPIO_CFG0_GPIO_DATA_(bit)	BIT(0 + (bit))

#define GPIO_CFG1			(0x054)
#define GPIO_CFG1_GPIOEN_MASK_		(0x0FFF0000)
#define GPIO_CFG1_GPIOEN_(bit)		BIT(16 + (bit))
#define GPIO_CFG1_GPIOBUF_MASK_		(0x00000FFF)
#define GPIO_CFG1_GPIOBUF_(bit)		BIT(0 + (bit))

#define GPIO_CFG2			(0x058)
#define GPIO_CFG2_1588_POL_MASK_	(0x00000FFF)
#define GPIO_CFG2_1588_POL_(bit)	BIT(0 + (bit))

#define GPIO_CFG3			(0x05C)
#define GPIO_CFG3_1588_CH_SEL_MASK_	(0x0FFF0000)
#define GPIO_CFG3_1588_CH_SEL_(bit)	BIT(16 + (bit))
#define GPIO_CFG3_1588_OE_MASK_		(0x00000FFF)
#define GPIO_CFG3_1588_OE_(bit)		BIT(0 + (bit))

#define GPIO_WAKE			(0x060)
#define GPIO_WAKE_GPIOPOL_MASK_		(0x0FFF0000)
#define GPIO_WAKE_GPIOPOL_(bit)		BIT(16 + (bit))
#define GPIO_WAKE_GPIOWK_MASK_		(0x00000FFF)
#define GPIO_WAKE_GPIOWK_(bit)		BIT(0 + (bit))

#define GPIO_INT_STS			(0x64)
#define GPIO_INT_EN_SET			(0x68)
#define GPIO_INT_EN_CLR			(0x6C)
#define GPIO_INT_BIT_(bit)		BIT(0 + (bit))

#define FCT_INT_STS			(0xA0)
#define FCT_INT_EN_SET			(0xA4)
#define FCT_INT_EN_CLR			(0xA8)
#define FCT_INT_MASK_RDFPA_		(0xF0000000)
#define FCT_INT_BIT_RDFPA_3_		BIT(31)
#define FCT_INT_BIT_RDFPA_2_		BIT(30)
#define FCT_INT_BIT_RDFPA_1_		BIT(29)
#define FCT_INT_BIT_RDFPA_0_		BIT(28)
#define FCT_INT_MASK_RDFO_		(0x0F000000)
#define FCT_INT_BIT_RDFO_3_		BIT(27)
#define FCT_INT_BIT_RDFO_2_		BIT(26)
#define FCT_INT_BIT_RDFO_1_		BIT(25)
#define FCT_INT_BIT_RDFO_0_		BIT(24)
#define FCT_INT_MASK_RXDF_		(0x00F00000)
#define FCT_INT_BIT_RXDF_3_		BIT(23)
#define FCT_INT_BIT_RXDF_2_		BIT(22)
#define FCT_INT_BIT_RXDF_1_		BIT(21)
#define FCT_INT_BIT_RXDF_0_		BIT(20)
#define FCT_INT_BIT_TXE_		BIT(16)
#define FCT_INT_BIT_TDFO_		BIT(12)
#define FCT_INT_BIT_TDFU_		BIT(8)
#define FCT_INT_BIT_RX_DIS_3_		BIT(7)
#define FCT_INT_BIT_RX_DIS_2_		BIT(6)
#define FCT_INT_BIT_RX_DIS_1_		BIT(5)
#define FCT_INT_BIT_RX_DIS_0_		BIT(4)
#define FCT_INT_BIT_TX_DIS_		BIT(0)
#define FCT_INT_MASK_ERRORS_	\
	(FCT_INT_MASK_RDFO_ |	\
	FCT_INT_MASK_RXDF_ |	\
	FCT_INT_BIT_TXE_ |	\
	FCT_INT_BIT_TDFO_ |	\
	FCT_INT_BIT_TDFU_)

#define FCT_RX_CTL			(0xAC)
#define FCT_RX_CTL_EN_(channel)		BIT(28 + (channel))
#define FCT_RX_CTL_DIS_(channel)	BIT(24 + (channel))
#define FCT_RX_CTL_RESET_(channel)	BIT(20 + (channel))

#define FCT_RX_FIFO_END			(0xB0)
#define FCT_RX_FIFO_END_3_		(0x3F000000)
#define FCT_RX_FIFO_END_2_		(0x003F0000)
#define FCT_RX_FIFO_END_1_		(0x00003F00)
#define FCT_RX_FIFO_END_0_		(0x0000003F)

#define FCT_RX_USED_0			(0xB4)
#define FCT_RX_USED_1			(0xB8)
#define FCT_RX_USED_2			(0xBC)
#define FCT_RX_USED_3			(0xC0)
#define FCT_RX_USED_MASK_		(0x0000FFFF)

#define FCT_TX_CTL			(0xC4)
#define FCT_TX_CTL_EN_(channel)		BIT(28 + (channel))
#define FCT_TX_CTL_DIS_(channel)	BIT(24 + (channel))
#define FCT_TX_CTL_RESET_(channel)	BIT(20 + (channel))

#define FCT_TX_FIFO_END			(0xC8)
#define FCT_TX_FIFO_END_0_		(0x0000003F)

#define FCT_TX_USED_0			(0xCC)
#define FCT_TX_USED_0_MASK_		(0x0000FFFF)

#define FCT_CFG					(0xDC)
#define FCT_CFG_ENABLE_OTHER_ROUTING_HEADERS_	BIT(4)
#define FCT_CFG_STORE_BAD_FRAMES_		BIT(0)

#define FCT_FLOW(rx_channel)			(0xE0 + ((rx_channel) << 2))
#define FCT_FLOW_CTL_OFF_THRESHOLD_		(0x00007F00)
#define FCT_FLOW_CTL_OFF_THRESHOLD_SET_(value) \
	((value << 8) & FCT_FLOW_CTL_OFF_THRESHOLD_)
#define FCT_FLOW_CTL_REQ_EN_			BIT(7)
#define FCT_FLOW_CTL_ON_THRESHOLD_		(0x0000007F)
#define FCT_FLOW_CTL_ON_THRESHOLD_SET_(value) \
	((value << 0) & FCT_FLOW_CTL_ON_THRESHOLD_)

#define MAC_CR				(0x100)
#define MAC_CR_MII_EN_			BIT(19)
#define MAC_CR_EEE_TX_CLK_STOP_EN_	BIT(18)
#define MAC_CR_EEE_EN_			BIT(17)
#define MAC_CR_EEE_TLAR_EN_		BIT(16)
#define MAC_CR_ADP_			BIT(13)
#define MAC_CR_ADD_			BIT(12)
#define MAC_CR_ASD_			BIT(11)
#define MAC_CR_INT_LOOP_		BIT(10)
#define MAC_CR_BOLMT_MASK_		(0x000000C0)
#define MAC_CR_CNTR_RST_		BIT(5)
#define MAC_CR_CNTR_WEN_		BIT(4)
#define MAC_CR_DPX_			BIT(3)
#define MAC_CR_SPEED_MASK_		(0x00000006)
#define MAC_CR_SPEED_1000_		(0x00000004)
#define MAC_CR_SPEED_100_		(0x00000002)
#define MAC_CR_SPEED_10_		(0x00000000)
#define MAC_CR_RST_			BIT(0)

#define MAC_RX				(0x104)
#define MAC_RX_MAX_SIZE_SHIFT_		(16)
#define MAC_RX_MAX_SIZE_MASK_		(0x3FFF0000)
#define MAC_RX_LEN_FLD_LT_CHK_		BIT(6)
#define MAC_RX_WTL_			BIT(5)
#define MAC_RX_FCS_STRIP_		BIT(4)
#define MAC_RX_LFCD_			BIT(3)
#define MAC_RX_VLAN_FSE_		BIT(2)
#define MAC_RX_RXD_			BIT(1)
#define MAC_RX_RXEN_			BIT(0)

#define MAC_TX				(0x108)
#define MAC_TX_BAD_FCS_			BIT(2)
#define MAC_TX_TXD_			BIT(1)
#define MAC_TX_TXEN_			BIT(0)

#define MAC_FLOW			(0x10C)
#define MAC_FLOW_CR_FORCE_FC_		BIT(31)
#define MAC_FLOW_CR_TX_FCEN_		BIT(30)
#define MAC_FLOW_CR_RX_FCEN_		BIT(29)
#define MAC_FLOW_CR_FPF_		BIT(28)
#define MAC_FLOW_CR_FCPT_MASK_		(0x0000FFFF)

#define MAC_RAND_SEED			(0x110)
#define MAC_RAND_SEED_MASK_		(0x0000FFFF)

#define MAC_ERR_STS			(0x114)
#define MAC_ERR_STS_RESERVED_		(0xFFFFF803)
#define MAC_ERR_STS_LEN_ERR_		BIT(10)
#define MAC_ERR_STS_RXERR_		BIT(9)
#define MAC_ERR_STS_FERR_		BIT(8)
#define MAC_ERR_STS_LFERR_		BIT(7)
#define MAC_ERR_STS_RFERR_		BIT(6)
#define MAC_ERR_STS_RWTERR_		BIT(5)
#define MAC_ERR_STS_ECERR_		BIT(4)
#define MAC_ERR_STS_ALERR_		BIT(3)
#define MAC_ERR_STS_URERR_		BIT(2)

#define MAC_RX_ADDRH			(0x118)
#define MAC_RX_ADDRH_MASK_		(0x0000FFFF)

#define MAC_RX_ADDRL			(0x11C)
#define MAC_RX_ADDRL_MASK_		(0xFFFFFFFF)

#define MAC_MII_ACC			(0x120)
#define MAC_MII_ACC_PHY_ADDR_SHIFT_	(11)
#define MAC_MII_ACC_PHY_ADDR_MASK_	(0x0000F800)
#define MAC_MII_ACC_MIIRINDA_SHIFT_	(6)
#define MAC_MII_ACC_MIIRINDA_MASK_	(0x000007C0)
#define MAC_MII_ACC_MII_READ_		(0x00000000)
#define MAC_MII_ACC_MII_WRITE_		(0x00000002)
#define MAC_MII_ACC_MII_BUSY_		BIT(0)

#define MAC_MII_DATA			(0x124)
#define MAC_MII_DATA_MASK_		(0x0000FFFF)

#define MAC_RGMII_ID			(0x128)
#define MAC_RGMII_ID_TXC_DELAY_EN_	BIT(1)
#define MAC_RGMII_ID_RXC_DELAY_EN_	BIT(0)

#define MAC_EEE_TX_LPI_REQ_DLY_CNT		(0x130)
#define MAC_EEE_TX_LPI_REQ_DLY_CNT_MASK_	(0xFFFFFFFF)

#define MAC_EEE_TW_TX_SYS			(0x134)
#define MAC_EEE_TW_TX_SYS_CNT1G_MASK_		(0xFFFF0000)
#define MAC_EEE_TW_TX_SYS_CNT100M_MASK_		(0x0000FFFF)

#define MAC_EEE_TX_LPI_AUTO_REM_DLY		(0x138)
#define MAC_EEE_TX_LPI_AUTO_REM_DLY_CNT_	(0x00FFFFFF)

#define MAC_WUCSR				(0x140)
#define MAC_WUCSR_TESTMODE_			BIT(31)
#define MAC_WUCSR_IGNORE_WU_			BIT(20)
#define MAC_WUCSR_IGNORE_WU_TIME_		(0x000F0000)
#define MAC_WUCSR_DISCARD_FRAMES_D0A_		BIT(15)
#define MAC_WUCSR_RFE_WAKE_EN_			BIT(14)
#define MAC_WUCSR_EEE_TX_WAKE_			BIT(13)
#define MAC_WUCSR_EEE_TX_WAKE_EN_		BIT(12)
#define MAC_WUCSR_EEE_RX_WAKE_			BIT(11)
#define MAC_WUCSR_EEE_RX_WAKE_EN_		BIT(10)
#define MAC_WUCSR_RFE_WAKE_FR_			BIT(9)
#define MAC_WUCSR_STORE_WAKE_			BIT(8)
#define MAC_WUCSR_PFDA_FR_			BIT(7)
#define MAC_WUCSR_WUFR_				BIT(6)
#define MAC_WUCSR_MPR_				BIT(5)
#define MAC_WUCSR_BCST_FR_			BIT(4)
#define MAC_WUCSR_PFDA_EN_			BIT(3)
#define MAC_WUCSR_WAKE_EN_			BIT(2)
#define MAC_WUCSR_MPEN_				BIT(1)
#define MAC_WUCSR_BCST_EN_			BIT(0)

#define MAC_WK_SRC				(0x144)
#define MAC_WK_SRC_GPIOX_INT_WK_SHIFT_		(20)
#define MAC_WK_SRC_GPIOX_INT_WK_MASK_		(0xFFF00000)
#define MAC_WK_SRC_ETH_PHY_WK_			BIT(17)
#define MAC_WK_SRC_IPV6_TCPSYN_RCD_WK_		BIT(16)
#define MAC_WK_SRC_IPV4_TCPSYN_RCD_WK_		BIT(15)
#define MAC_WK_SRC_EEE_TX_WK_			BIT(14)
#define MAC_WK_SRC_EEE_RX_WK_			BIT(13)
#define MAC_WK_SRC_RFE_FR_WK_			BIT(12)
#define MAC_WK_SRC_PFDA_FR_WK_			BIT(11)
#define MAC_WK_SRC_MP_FR_WK_			BIT(10)
#define MAC_WK_SRC_BCAST_FR_WK_			BIT(9)
#define MAC_WK_SRC_WU_FR_WK_			BIT(8)
#define MAC_WK_SRC_WK_FR_SAVED_			BIT(7)
#define MAC_WK_SRC_WK_FR_SAVE_RX_CH_		(0x00000060)
#define MAC_WK_SRC_WUFF_MATCH_MASK_		(0x0000001F)

#define MAC_WUF_CFG0			(0x150)
#define MAC_NUM_OF_WUF_CFG		(32)
#define MAC_WUF_CFG_BEGIN		(WUF_CFG0)
#define MAC_WUF_CFG(index)		(WUF_CFG_BEGIN + (4 * (index)))
#define MAC_WUF_CFG_EN_			BIT(31)
#define MAC_WUF_CFG_TYPE_MASK_		(0x03000000)
#define MAC_WUF_CFG_TYPE_MCAST_		(0x02000000)
#define MAC_WUF_CFG_TYPE_ALL_		(0x01000000)
#define MAC_WUF_CFG_TYPE_UCAST_		(0x00000000)
#define MAC_WUF_CFG_OFFSET_SHIFT_	(16)
#define MAC_WUF_CFG_OFFSET_MASK_	(0x00FF0000)
#define MAC_WUF_CFG_CRC16_MASK_		(0x0000FFFF)

#define MAC_WUF_MASK0_0			(0x200)
#define MAC_WUF_MASK0_1			(0x204)
#define MAC_WUF_MASK0_2			(0x208)
#define MAC_WUF_MASK0_3			(0x20C)
#define MAC_NUM_OF_WUF_MASK		(32)
#define MAC_WUF_MASK0_BEGIN		(MAC_WUF_MASK0_0)
#define MAC_WUF_MASK1_BEGIN		(MAC_WUF_MASK0_1)
#define MAC_WUF_MASK2_BEGIN		(MAC_WUF_MASK0_2)
#define MAC_WUF_MASK3_BEGIN		(MAC_WUF_MASK0_3)
#define MAC_WUF_MASK0(index)		(MAC_WUF_MASK0_BEGIN + (0x10 * (index)))
#define MAC_WUF_MASK1(index)		(MAC_WUF_MASK1_BEGIN + (0x10 * (index)))
#define MAC_WUF_MASK2(index)		(MAC_WUF_MASK2_BEGIN + (0x10 * (index)))
#define MAC_WUF_MASK3(index)		(MAC_WUF_MASK3_BEGIN + (0x10 * (index)))

/* offset 0x400 - 0x500, x may range from 0 to 32, for a total of 33 entries */
#define RFE_ADDR_FILT_HI(x)		(0x400 + (8 * (x)))
#define RFE_ADDR_FILT_HI_VALID_		BIT(31)
#define RFE_ADDR_FILT_HI_TYPE_MASK_	(0x40000000)
#define RFE_ADDR_FILT_HI_TYPE_SRC_	(0x40000000)
#define RFE_ADDR_FILT_HI_TYPE_DST_	(0x00000000)
#define RFE_ADDR_FILT_HI_PRI_FRM_	BIT(20)
#define RFE_ADDR_FILT_HI_RSS_EN_	BIT(19)
#define RFE_ADDR_FILT_HI_CH_EN_		BIT(18)
#define RFE_ADDR_FILT_HI_CH_NUM_MASK_	(0x00030000)
#define RFE_ADDR_FILT_HI_ADDR_MASK_	(0x0000FFFF)

/* offset 0x404 - 0x504, x may range from 0 to 32, for a total of 33 entries */
#define RFE_ADDR_FILT_LO(x)		(0x404 + (8 * (x)))
#define RFE_ADDR_FILT_LO_ADDR_MASK_	(0xFFFFFFFF)

#define RFE_CTL				(0x508)
#define RFE_CTL_EN_OTHER_RT_HEADER_	BIT(18)
#define RFE_CTL_DEFAULT_RX_CH_0_	(0x00000000)
#define RFE_CTL_DEFAULT_RX_CH_1_	(0x00010000)
#define RFE_CTL_DEFAULT_RX_CH_2_	(0x00020000)
#define RFE_CTL_DEFAULT_RX_CH_3_	(0x00030000)
#define RFE_CTL_DEFAULT_RX_CH_MASK_	(0x00030000)
#define RFE_CTL_PASS_WKP_		BIT(15)
#define RFE_CTL_IGMP_COE_		BIT(14)
#define RFE_CTL_ICMP_COE_		BIT(13)
#define RFE_CTL_TCPUDP_COE_		BIT(12)
#define RFE_CTL_IP_COE_			BIT(11)
#define RFE_CTL_AB_			BIT(10)
#define RFE_CTL_AM_			BIT(9)
#define RFE_CTL_AU_			BIT(8)
#define RFE_CTL_VLAN_STRIP_		BIT(7)
#define RFE_CTL_DISCARD_UNTAGGED_	BIT(6)
#define RFE_CTL_VLAN_FILTER_		BIT(5)
#define RFE_CTL_SA_FILTER_		BIT(4)
#define RFE_CTL_MCAST_HASH_		BIT(3)
#define RFE_CTL_DA_HASH_		BIT(2)
#define RFE_CTL_DA_PERFECT_		BIT(1)
#define RFE_CTL_RST_			BIT(0)

#define RFE_PRI_SEL			(0x50C)
#define RFE_PRI_SEL_CH_NUM_PRI_7_	(0xC0000000)
#define RFE_PRI_SEL_CH_NUM_PRI_6_	(0x30000000)
#define RFE_PRI_SEL_CH_NUM_PRI_5_	(0x0C000000)
#define RFE_PRI_SEL_CH_NUM_PRI_4_	(0x03000000)
#define RFE_PRI_SEL_CH_NUM_PRI_3_	(0x00C00000)
#define RFE_PRI_SEL_CH_NUM_PRI_2_	(0x00300000)
#define RFE_PRI_SEL_CH_NUM_PRI_1_	(0x000C0000)
#define RFE_PRI_SEL_CH_NUM_PRI_0_	(0x00030000)
#define RFE_PRI_SEL_RSS_EN_PRI_7_	BIT(15)
#define RFE_PRI_SEL_RSS_EN_PRI_6_	BIT(14)
#define RFE_PRI_SEL_RSS_EN_PRI_5_	BIT(13)
#define RFE_PRI_SEL_RSS_EN_PRI_4_	BIT(12)
#define RFE_PRI_SEL_RSS_EN_PRI_3_	BIT(11)
#define RFE_PRI_SEL_RSS_EN_PRI_2_	BIT(10)
#define RFE_PRI_SEL_RSS_EN_PRI_1_	BIT(9)
#define RFE_PRI_SEL_RSS_EN_PRI_0_	BIT(8)
#define RFE_PRI_SEL_FM_PRI_EN_		BIT(7)
#define RFE_PRI_SEL_FM_PRI_THRESH_	(0x00000070)
#define RFE_PRI_SEL_USE_PRECEDENCE_	BIT(3)
#define RFE_PRI_SEL_USE_IP_		BIT(2)
#define RFE_PRI_SEL_USE_TAG_		BIT(1)
#define RFE_PRI_SEL_VL_HIGHER_PRI_	BIT(0)

#define RFE_DIFFSERV0			(0x510)
#define RFE_DIFFSERV1			(0x514)
#define RFE_DIFFSERV2			(0x518)
#define RFE_DIFFSERV3			(0x51C)
#define RFE_DIFFSERV4			(0x520)
#define RFE_DIFFSERV5			(0x524)
#define RFE_DIFFSERV6			(0x528)
#define RFE_DIFFSERV7			(0x52C)

#define RFE_RSS_CFG			(0x554)
#define RFE_RSS_CFG_UDP_IPV6_EX_	BIT(16)
#define RFE_RSS_CFG_TCP_IPV6_EX_	BIT(15)
#define RFE_RSS_CFG_IPV6_EX_		BIT(14)
#define RFE_RSS_CFG_UDP_IPV6_		BIT(13)
#define RFE_RSS_CFG_TCP_IPV6_		BIT(12)
#define RFE_RSS_CFG_IPV6_		BIT(11)
#define RFE_RSS_CFG_UDP_IPV4_		BIT(10)
#define RFE_RSS_CFG_TCP_IPV4_		BIT(9)
#define RFE_RSS_CFG_IPV4_		BIT(8)
#define RFE_RSS_CFG_VALID_HASH_BITS_	(0x000000E0)
#define RFE_RSS_CFG_RSS_QUEUE_ENABLE_	BIT(2)
#define RFE_RSS_CFG_RSS_HASH_STORE_	BIT(1)
#define RFE_RSS_CFG_RSS_ENABLE_		BIT(0)

#define RFE_HASH_KEY0			(0x558)
#define RFE_HASH_KEY1			(0x55C)
#define RFE_HASH_KEY2			(0x560)
#define RFE_HASH_KEY3			(0x564)
#define RFE_HASH_KEY4			(0x568)
#define RFE_HASH_KEY5			(0x56C)
#define RFE_HASH_KEY6			(0x570)
#define RFE_HASH_KEY7			(0x574)
#define RFE_HASH_KEY8			(0x578)
#define RFE_HASH_KEY9			(0x57C)

#define MAC_WUCSR2			(0x600)
#define MAC_WUCSR2_CSUM_DISABLE_	BIT(31)
#define MAC_WUCSR2_EN_OTHER_RT_HDRS_	BIT(30)
#define MAC_WUCSR2_FARP_FR_		BIT(10)
#define MAC_WUCSR2_FNS_FR_		BIT(9)
#define MAC_WUCSR2_NA_SA_SEL_		BIT(8)
#define MAC_WUCSR2_NS_RCD_		BIT(7)
#define MAC_WUCSR2_ARP_RCD_		BIT(6)
#define MAC_WUCSR2_IPV6_TCPSYN_RCD_	BIT(5)
#define MAC_WUCSR2_IPV4_TCPSYN_RCD_	BIT(4)
#define MAC_WUCSR2_NS_OFFLOAD_EN_	BIT(3)
#define MAC_WUCSR2_ARP_OFFLOAD_EN_	BIT(2)
#define MAC_WUCSR2_IPV6_TCPSYN_WAKE_EN_ BIT(1)
#define MAC_WUCSR2_IPV4_TCPSYN_WAKE_EN_ BIT(0)

#define MAC_INT_STS			(0x604)
#define MAC_INT_EN_SET			(0x608)
#define MAC_INT_EN_CLR			(0x60C)
#define MAC_INT_BIT_EEE_START_TX_LPI_	BIT(26)
#define MAC_INT_BIT_EEE_STOP_TX_LPI_	BIT(25)
#define MAC_INT_BIT_EEE_RX_LPI_		BIT(24)
#define MAC_INT_BIT_MACRTO_		BIT(23)
#define MAC_INT_BIT_MAC_TX_DIS_		BIT(19)
#define MAC_INT_BIT_MAC_RX_DIS_		BIT(18)
#define MAC_INT_BIT_MAC_ERR_		BIT(15)
#define MAC_INT_BIT_MAC_RX_CNT_ROLL_	BIT(14)
#define MAC_INT_BIT_MAC_TX_CNT_ROLL_	BIT(13)

#define INT_STS				(0x780)
#define INT_BIT_RESERVED_		(0xF0FEF000)
#define INT_BIT_DMA_RX_(channel)	BIT(24 + (channel))
#define INT_BIT_ALL_RX_			(0x0F000000)
#define INT_BIT_DMA_TX_(channel)	BIT(16 + (channel))
#define INT_BIT_ALL_TX_			(0x000F0000)
#define INT_BIT_GPIO_			BIT(11)
#define INT_BIT_DMA_GEN_		BIT(10)
#define INT_BIT_SW_GP_			BIT(9)
#define INT_BIT_PCIE_			BIT(8)
#define INT_BIT_1588_			BIT(7)
#define INT_BIT_OTP_RDY_		BIT(6)
#define INT_BIT_PHY_			BIT(5)
#define INT_BIT_DP_			BIT(4)
#define INT_BIT_MAC_			BIT(3)
#define INT_BIT_FCT_			BIT(2)
#define INT_BIT_GPT_			BIT(1)
#define INT_BIT_ALL_OTHER_		(0x00000FFE)
#define INT_BIT_MAS_			BIT(0)

#define INT_SET				(0x784)

#define INT_EN_SET			(0x788)

#define INT_EN_CLR			(0x78C)

#define INT_VEC_EN_SET			(0x794)
#define INT_VEC_EN_CLR			(0x798)
#define INT_VEC_EN_(vector_index)	BIT(0 + vector_index)

#define INT_VEC_MAP0			(0x7A0)
#define INT_VMAP0_DMA_RX3_VEC_MASK_	(0x0000F000)
#define INT_VMAP0_DMA_RX2_VEC_MASK_	(0x00000F00)
#define INT_VMAP0_DMA_RX1_VEC_MASK_	(0x000000F0)
#define INT_VMAP0_DMA_RX0_VEC_MASK_	(0x0000000F)

#define INT_VEC_MAP1			(0x7A4)
#define INT_VMAP1_DMA_TX0_VEC_MASK_	(0x0000000F)

#define INT_VEC_MAP2			(0x7A8)
#define INT_VMAP2_FCT_VEC_MASK_		(0x00F00000)
#define INT_VMAP2_DMA_GEN_VEC_MASK_	(0x000F0000)
#define INT_VMAP2_SW_GP_VEC_MASK_	(0x0000F000)
#define INT_VMAP2_1588_VEC_MASK_	(0x00000F00)
#define INT_VMAP2_GPT_VEC_MASK_		(0x000000F0)
#define INT_VMAP2_OTHER_VEC_MASK_	(0x0000000F)

#define INT_MOD_MAP0			(0x7B0)
#define INT_MMAP0_DMA_RX3_MASK_		(0x0000F000)
#define INT_MMAP0_DMA_RX2_MASK_		(0x00000F00)
#define INT_MMAP0_DMA_RX1_MASK_		(0x000000F0)
#define INT_MMAP0_DMA_RX0_MASK_		(0x0000000F)

#define INT_MOD_MAP1			(0x7B4)
#define INT_MMAP1_DMA_TX0_MASK_		(0x0000000F)

#define INT_MOD_MAP2			(0x7B8)
#define INT_MMAP2_FCT_MOD_MASK_		(0x00F00000)
#define INT_MMAP2_DMA_GEN_MASK_		(0x000F0000)
#define INT_MMAP2_SW_GP_MASK_		(0x0000F000)
#define INT_MMAP2_1588_MASK_		(0x00000F00)
#define INT_MMAP2_GPT_MASK_		(0x000000F0)
#define INT_MMAP2_OTHER_MASK_		(0x0000000F)

#define INT_MOD_CFG0			(0x7C0)
#define INT_MOD_CFG1			(0x7C4)
#define INT_MOD_CFG2			(0x7C8)
#define INT_MOD_CFG3			(0x7CC)
#define INT_MOD_CFG4			(0x7D0)
#define INT_MOD_CFG5			(0x7D4)
#define INT_MOD_CFG6			(0x7C8)
#define INT_MOD_CFG7			(0x7DC)
#define INT_MOD_CFG_STATUS_		BIT(18)
#define INT_MOD_CFG_START_		BIT(17)
#define INT_MOD_CFG_TMODE_MASK_		(0x00010000)
#define INT_MOD_CFG_TMODE_ABS_		(0x00000000)
#define INT_MOD_CFG_TMODE_CREDIT_	(0x00010000)
#define INT_MOD_CFG_INTERVAL_MASK_	(0x00001FFF)

#define PTP_CMD_CTL					(0x0A00)
#define PTP_CMD_CTL_PTP_CLOCK_TARGET_READ_		BIT(13)
#define PTP_CMD_CTL_PTP_MANUAL_CAPTURE_SEL_MASK_	(0x00001E00)
#define PTP_CMD_CTL_PTP_MANUAL_CAPTURE_			BIT(8)
#define PTP_CMD_CTL_PTP_CLOCK_TEMP_RATE_		BIT(7)
#define PTP_CMD_CTL_PTP_CLK_STP_NSEC_			BIT(6)
#define PTP_CMD_CTL_PTP_CLOCK_STEP_SEC_			BIT(5)
#define PTP_CMD_CTL_PTP_CLOCK_LOAD_			BIT(4)
#define PTP_CMD_CTL_PTP_CLOCK_READ_			BIT(3)
#define PTP_CMD_CTL_PTP_ENABLE_				BIT(2)
#define PTP_CMD_CTL_PTP_DISABLE_			BIT(1)
#define PTP_CMD_CTL_PTP_RESET_				BIT(0)
#define PTP_GENERAL_CONFIG				(0x0A04)
#define PTP_GENERAL_CONFIG_TSU_ENABLE_			BIT(31)
#define PTP_GENERAL_CONFIG_GPIO_FECR_			BIT(25)
#define PTP_GENERAL_CONFIG_GPIO_RECR_			BIT(24)
#define PTP_GENERAL_CONFIG_GPIO_PTP_TIMER_INT_X_CLEAR_EN_(channel) \
	(BIT(12 + ((channel) << 3)))
#define PTP_GENERAL_CONFIG_GPIO_PTP_TIMER_INT_X_CLEAR_SEL_SET_(channel, value) \
	(((value) & 0xF) << (8 + ((channel) << 3)))
#define PTP_GENERAL_CONFIG_CLOCK_EVENT_X_MASK_(channel) \
	(0x7 << (1 + ((channel) << 2)))
#define PTP_GENERAL_CONFIG_CLOCK_EVENT_100NS_	(0)
#define PTP_GENERAL_CONFIG_CLOCK_EVENT_10US_	(1)
#define PTP_GENERAL_CONFIG_CLOCK_EVENT_100US_	(2)
#define PTP_GENERAL_CONFIG_CLOCK_EVENT_1MS_	(3)
#define PTP_GENERAL_CONFIG_CLOCK_EVENT_10MS_	(4)
#define PTP_GENERAL_CONFIG_CLOCK_EVENT_200MS_	(5)
#define PTP_GENERAL_CONFIG_CLOCK_EVENT_TOGGLE_	(6)
#define PTP_GENERAL_CONFIG_CLOCK_EVENT_INT_	(7)
#define PTP_GENERAL_CONFIG_CLOCK_EVENT_X_SET_(channel, value) \
	(((value) & 0x7) << (1 + ((channel) << 2)))
#define PTP_GENERAL_CONFIG_RELOAD_ADD_X_(channel)	(BIT((channel) << 2))

#define PTP_INT_STS				(0x0A08)
#define PTP_INT_EN_SET				(0x0A0C)
#define PTP_INT_EN_CLR				(0x0A10)
#define PTP_INT_BIT_GPIO_FE(gpio_num)		BIT(24 + (gpio_num))
#define PTP_INT_BIT_GPIO_RE(gpio_num)		BIT(16 + (gpio_num))
#define PTP_INT_BIT_TX_SWTS_ERR_		BIT(13)
#define PTP_INT_BIT_TX_TS_			BIT(12)
#define PTP_INT_BIT_RX_TS_			BIT(8)
#define PTP_INT_BIT_TIMER_B_			BIT(1)
#define PTP_INT_BIT_TIMER_A_			BIT(0)
#define PTP_INT_BIT_TIMER_(channel)		BIT(channel)

#define PTP_CLOCK_SEC				(0x0A14)
#define PTP_CLOCK_NS				(0x0A18)
#define PTP_CLOCK_SUBNS				(0x0A1C)
#define PTP_CLOCK_RATE_ADJ			(0x0A20)
#define PTP_CLOCK_RATE_ADJ_DIR_			BIT(31)
#define PTP_CLOCK_RATE_ADJ_VALUE_MASK_		(0x3FFFFFFF)
#define PTP_CLOCK_TEMP_RATE_ADJ			(0x0A24)
#define PTP_CLOCK_TEMP_RATE_DURATION		(0x0A28)
#define PTP_CLOCK_STEP_ADJ			(0x0A2C)
#define PTP_CLOCK_STEP_ADJ_DIR_			BIT(31)
#define PTP_CLOCK_STEP_ADJ_VALUE_MASK_		(0x3FFFFFFF)
#define PTP_CLOCK_TARGET_SEC_X(channel)		(0x0A30 + ((channel) << 4))
#define PTP_CLOCK_TARGET_NS_X(channel)		(0x0A34 + ((channel) << 4))
#define PTP_CLOCK_TARGET_RELOAD_SEC_X(channel)	(0x0A38 + ((channel) << 4))
#define PTP_CLOCK_TARGET_RELOAD_NS_X(channel)	(0x0A3C + ((channel) << 4))
#define PTP_USER_MAC_HI				(0x0A50)
#define PTP_USER_MAC_LO				(0x0A54)
#define PTP_GPIO_SEL				(0x0A58)
#define PTP_LATENCY				(0x0A5C)
#define PTP_CAP_INFO				(0x0A60)
#define PTP_CAP_INFO_TX_TS_CNT_GET_(reg_val)	((reg_val & 0x00000070) >> 4)
#define PTP_RX_PARSE_CONFIG			(0x0A64)
#define PTP_RX_TIMESTAMP_CONFIG			(0x0A68)

#define PTP_RX_INGRESS_SEC			(0x0A78)
#define PTP_RX_INGRESS_NS			(0x0A7C)
#define PTP_RX_MSG_HEADER			(0x0A80)
#define PTP_TX_PARSE_CONFIG			(0x0A9C)
#define PTP_TX_TIMESTAMP_CONFIG			(0x0AA0)
#define PTP_TX_MOD				(0x0AA4)
#define PTP_TX_MOD2				(0x0AA8)
#define PTP_TX_EGRESS_SEC			(0x0AAC)
#define PTP_TX_EGRESS_NS			(0x0AB0)
#define PTP_TX_EGRESS_NS_CAPTURE_CAUSE_MASK_	(0xC0000000)
#define PTP_TX_EGRESS_NS_CAPTURE_CAUSE_AUTO_	(0x00000000)
#define PTP_TX_EGRESS_NS_CAPTURE_CAUSE_SW_	(0x40000000)
#define PTP_TX_EGRESS_NS_TS_NS_MASK_		(0x3FFFFFFF)
#define PTP_TX_MSG_HEADER			(0x0AB4)
#define PTP_TX_ONE_STEP_SYNC_SEC		(0x0AC0)
#define PTP_GPIO_CAP_CONFIG			(0x0AC4)
#define PTP_GPIO_RE_CLOCK_SEC_CAP		(0x0AC8)
#define PTP_GPIO_RE_CLOCK_NS_CAP		(0x0ACC)
#define PTP_GPIO_FE_CLOCK_SEC_CAP		(0x0AD0)
#define PTP_GPIO_FE_CLOCK_NS_CAP		(0x0AD4)

#define DMAC_CFG				(0xC00)
#define DMAC_CFG_INTR_DSCR_RD_EN_		BIT(18)
#define DMAC_CFG_INTR_DSCR_WR_EN_		BIT(17)
#define DMAC_CFG_COAL_EN_			BIT(16)
#define DMAC_CFG_CMPL_RETRY_CNT_MASK_		(0x00006000)
#define DMAC_CFG_CMPL_RETRY_EN_			BIT(12)
#define DMAC_CFG_CH_ARB_SEL_MASK_		(0x00000C00)
#define DMAC_CFG_CH_ARB_SEL_RX_HIGH_		(0x00000000)
#define DMAC_CFG_CH_ARB_SEL_CH_ORDER_		BIT(10)
#define DMAC_CFG_CH_ARB_SEL_RX_HIGH_RR_		BIT(11)
#define DMAC_CFG_CH_ARB_SEL_RR_			(0x00000C00)
#define DMAC_CFG_MAX_READ_REQ_MASK_		(0x00000070)
#define DMAC_CFG_MAX_READ_REQ_SET_(val) \
	((((u32)(val)) << 4) & DMAC_CFG_MAX_READ_REQ_MASK_)
#define DMAC_CFG_MAX_DSPACE_MASK_		(0x00000003)
#define DMAC_CFG_MAX_DSPACE_16_			(0x00000000)
#define DMAC_CFG_MAX_DSPACE_32_			(0x00000001)
#define DMAC_CFG_MAX_DSPACE_64_			BIT(1)
#define DMAC_CFG_MAX_DSPACE_128_		(0x00000003)

#define DMAC_COAL_CFG				(0xC04)
#define DMAC_COAL_CFG_TIMER_LIMIT_MASK_		(0xFFF00000)
#define DMAC_COAL_CFG_FLUSH_INTS_		BIT(18)
#define DMAC_COAL_CFG_INT_EXIT_COAL_		BIT(17)
#define DMAC_COAL_CFG_CSR_EXIT_COAL_		BIT(16)
#define DMAC_COAL_CFG_TX_THRES_MASK_		(0x0000FF00)
#define DMAC_COAL_CFG_RX_THRES_MASK_		(0x000000FF)

#define DMAC_OBFF_CFG				(0xC08)
#define DMAC_OBFF_TX_THRES_MASK_		(0x0000FF00)
#define DMAC_OBFF_RX_THRES_MASK_		(0x000000FF)

#define DMAC_CMD				(0xC0C)
#define DMAC_CMD_SWR_				BIT(31)
#define DMAC_CMD_COAL_EXIT_			BIT(28)
#define DMAC_CMD_TX_SWR_(channel)		BIT(24 + (channel))
#define DMAC_CMD_START_T_(channel)		BIT(20 + (channel))
#define DMAC_CMD_STOP_T_(channel)		BIT(16 + (channel))
#define DMAC_CMD_RX_SWR_(channel)		BIT(8 + (channel))
#define DMAC_CMD_START_R_(channel)		BIT(4 + (channel))
#define DMAC_CMD_STOP_R_(channel)		BIT(0 + (channel))

#define DMAC_INT_STS				(0xC10)
#define DMAC_INT_EN_SET				(0xC14)
#define DMAC_INT_EN_CLR				(0xC18)
#define DMAC_INT_BIT_RXPRI_(channel)		BIT(24 + (channel))
#define DMAC_INT_BIT_ERR_			BIT(21)
#define DMAC_INT_BIT_RXFRM_(channel)		BIT(16 + (channel))
#define DMAC_INT_BIT_RX_STOP_(channel)		BIT(12 + (channel))
#define DMAC_INT_BIT_TX_STOP_(channel)		BIT(8 + (channel))
#define DMAC_INT_BIT_TX_IOC_(channel)		BIT(0 + (channel))

#define DMAC_RX_ABS_TIMER_CFG			(0xC1C)
#define DMAC_RX_ABS_TIMER_CFG_SHARE_MASK_	(0x00F00000)
#define DMAC_RX_ABS_TIMER_CFG_SHARE_3_		BIT(23)
#define DMAC_RX_ABS_TIMER_CFG_SHARE_2_		BIT(22)
#define DMAC_RX_ABS_TIMER_CFG_SHARE_1_		BIT(21)
#define DMAC_RX_ABS_TIMER_CFG_SHARE_0_		BIT(20)
#define DMAC_RX_ABS_TIMER_CFG_WR_		BIT(19)
#define DMAC_RX_ABS_TIMER_CFG_SEL_MASK_		(0x00070000)
#define DMAC_RX_ABS_TIMER_CFG_CNT_MASK_		(0x0000FFFF)

#define DMAC_RX_TIMER_CFG				(0xC20)
#define DMAC_RX_TIMER_CFG_TMR_MODE_MASK_		(0x1F000000)
#define DMAC_RX_TIMER_CFG_TMR_SHARED_FRAME_MODE_	BIT(28)
#define DMAC_RX_TIMER_CFG_TMR_TIMER3_FRAME_MODE_	BIT(27)
#define DMAC_RX_TIMER_CFG_TMR_TIMER2_FRAME_MODE_	BIT(26)
#define DMAC_RX_TIMER_CFG_TMR_TIMER1_FRAME_MODE_	BIT(25)
#define DMAC_RX_TIMER_CFG_TMR_TIMER0_FRAME_MODE_	BIT(24)
#define DMAC_RX_TIMER_CFG_SHARE_MAP_MASK_		(0x00F00000)
#define DMAC_RX_TIMER_CFG_SHARE_MAP_TIMER3_		BIT(23)
#define DMAC_RX_TIMER_CFG_SHARE_MAP_TIMER2_		BIT(22)
#define DMAC_RX_TIMER_CFG_SHARE_MAP_TIMER1_		BIT(21)
#define DMAC_RX_TIMER_CFG_SHARE_MAP_TIMER0_		BIT(20)
#define DMAC_RX_TIMER_CFG_WR_				BIT(19)
#define DMAC_RX_TIMER_CFG_CH_SEL_MASK_			(0x00070000)
#define DMAC_RX_TIMER_CFG_CH_SEL_TIMER0_		(0x00000000)
#define DMAC_RX_TIMER_CFG_CH_SEL_TIMER1_		(0x00010000)
#define DMAC_RX_TIMER_CFG_CH_SEL_TIMER2_		(0x00020000)
#define DMAC_RX_TIMER_CFG_CH_SEL_TIMER3_		(0x00030000)
#define DMAC_RX_TIMER_CFG_CH_SEL_SHARED_		(0x00040000)
#define DMAC_RX_TIMER_CFG_CNT_MASK_			(0x0000FFFF)

#define DMAC_TXTMR_CFG				(0xC24)
#define DMAC_TXTMR_CFG_TX_DELAY_WR_		BIT(23)
#define DMAC_TXTMR_CFG_TX_DELAY_CNT_		(0x0000FFFF)

#define DMAC_TX_ABSTMR_CFG		(0xC28)
#define DMAC_TX_ABSTMR_WR_		BIT(23)
#define DMAC_TX_ABSTMR_CNT_		(0x0000FFFF)

#define RX_CFG_A(channel)			(0xC40 + ((channel) << 6))
#define RX_CFG_A_RX_WB_SWFLUSH_			BIT(31)
#define RX_CFG_A_RX_WB_ON_INT_TMR_		BIT(30)
#define RX_CFG_A_RX_WB_THRES_MASK_		(0x1F000000)
#define RX_CFG_A_RX_PF_THRES_MASK_		(0x001F0000)
#define RX_CFG_A_RX_PF_PRI_THRES_MASK_		(0x00001F00)
#define RX_CFG_A_RX_HP_WB_EN_			BIT(5)
#define RX_CFG_A_RX_HP_WB_THRES_MASK_		(0x0000000F)

#define RX_CFG_B(channel)			(0xC44 + ((channel) << 6))
#define RX_CFG_B_TS_ALL_RX_			BIT(29)
#define RX_CFG_B_TS_DECR_EN_			BIT(28)
#define RX_CFG_B_RX_PAD_MASK_			(0x03000000)
#define RX_CFG_B_RX_PAD_0_			(0x00000000)
#define RX_CFG_B_RX_PAD_2_			(0x02000000)
#define RX_CFG_B_RX_COAL_DIS_			BIT(23)
#define RX_CFG_B_RX_DESCR_RO_EN_		BIT(21)
#define RX_CFG_B_RX_DATA_RO_EN_			BIT(20)
#define RX_CFG_B_RDMABL_MASK_			(0x00070000)
#define RX_CFG_B_RDMABL_32_			(0x00000000)
#define RX_CFG_B_RDMABL_64_			(0x00010000)
#define RX_CFG_B_RDMABL_128_			(0x00020000)
#define RX_CFG_B_RDMABL_256_			(0x00030000)
#define RX_CFG_B_RDMABL_512_			(0x00040000)
#define RX_CFG_B_RDMABL_1024_			(0x00050000)
#define RX_CFG_B_RDMABL_2048_			(0x00060000)
#define RX_CFG_B_RDMABL_4096_			(0x00070000)
#define RX_CFG_B_RX_RING_LEN_MASK_		(0x0000FFFF)

#define RX_BASE_ADDRH(channel)			(0xC48 + ((channel) << 6))
#define RX_BASE_ADDRH_MASK_			(0xFFFFFFFF)

#define RX_BASE_ADDRL(channel)			(0xC4C + ((channel) << 6))
#define RX_BASE_ADDRL_MASK_			(0xFFFFFFFC)

#define RX_HEAD_WRITEBACK_ADDRH(channel)	(0xC50 + ((channel) << 6))

#define RX_HEAD_WRITEBACK_ADDRL(channel)	(0xC54 + ((channel) << 6))

#define RX_HEAD(channel)			(0xC58 + ((channel) << 6))
#define RX_HEAD_MASK_				(0x0000FFFF)

#define RX_TAIL(channel)			(0xC5C + ((channel) << 6))
#define RX_TAIL_MASK_				(0x0000FFFF)

#define DMAC_RX_ERR_STS(channel)		(0xC60 + ((channel) << 6))
#define DMAC_RX_ERR_STS_RESERVED_		(0xFFDFFF9F)
#define DMAC_RX_ERR_STS_RX_DESC_TAIL_ERR_EN_	BIT(21)
#define DMAC_RX_ERR_STS_RX_DESC_READ_ERR_	BIT(6)
#define DMAC_RX_ERR_STS_RX_DESC_TAIL_ERR_	BIT(5)

#define TX_CFG_A(channel)			(0xD40 + ((channel) << 6))
#define TX_CFG_A_TX_HP_WB_SWFLUSH_		BIT(31)
#define TX_CFG_A_TX_HP_WB_ON_INT_TMR_		BIT(30)
#define TX_CFG_A_TX_TMR_HPWB_SEL_MASK_		(0x30000000)
#define TX_CFG_A_TX_TMR_HPWB_SEL_DIS_		(0x00000000)
#define TX_CFG_A_TX_TMR_HPWB_SEL_IOC_		(0x10000000)
#define TX_CFG_A_TX_TMR_HPWB_SEL_LS_		(0x20000000)
#define TX_CFG_A_TX_TMR_HPWB_SEL_IOC_LS_	(0x30000000)
#define TX_CFG_A_TX_PF_THRES_MASK_		(0x001F0000)
#define TX_CFG_A_TX_PF_PRI_THRES_MASK_		(0x00001F00)
#define TX_CFG_A_TX_STOP_TXE_			BIT(7)
#define TX_CFG_A_TX_HP_WB_EN_			BIT(5)
#define TX_CFG_A_TX_HP_WB_ON_TXTMR_		BIT(4)
#define TX_CFG_A_TX_HP_WB_THRES_MASK_		(0x0000000F)

#define TX_CFG_B(channel)			(0xD44 + ((channel) << 6))
#define TX_CFG_B_TX_COAL_DIS_			BIT(23)
#define TX_CFG_B_TX_DESC_RO_EN_			BIT(22)
#define TX_CFG_B_TX_DATA_RO_EN_			BIT(21)
#define TX_CFG_B_TX_HEAD_RO_EN_			BIT(20)
#define TX_CFG_B_TDMABL_MASK_			(0x00070000)
#define TX_CFG_B_TDMABL_32_			(0x00000000)
#define TX_CFG_B_TDMABL_64_			(0x00010000)
#define TX_CFG_B_TDMABL_128_			(0x00020000)
#define TX_CFG_B_TDMABL_256_			(0x00030000)
#define TX_CFG_B_TDMABL_512_			(0x00040000)
#define TX_CFG_B_TX_RING_LEN_MASK_		(0x0000FFFF)

#define TX_BASE_ADDRH(channel)			(0xD48 + ((channel) << 6))
#define TX_BASE_ADDRH_MASK_			(0xFFFFFFFF)

#define TX_BASE_ADDRL(channel)			(0xD4C + ((channel) << 6))
#define TX_BASE_ADDRL_MASK_			(0xFFFFFFFC)

#define TX_HEAD_WRITEBACK_ADDRH(channel)	(0xD50 + ((channel) << 6))
#define TX_HEAD_WRITEBACK_ADDRH_MASK_		(0xFFFFFFFF)

#define TX_HEAD_WRITEBACK_ADDRL(channel)	(0xD54 + ((channel) << 6))
#define TX_HEAD_WRITEBACK_ADDRL_MASK_		(0xFFFFFFFC)

#define TX_HEAD(channel)			(0xD58 + ((channel) << 6))
#define TX_HEAD_MASK_				(0x0000FFFF)

#define TX_TAIL(channel)			(0xD5C + ((channel) << 6))
#define TX_TAIL_MASK_				(0x0000FFFF)

#define DMAC_TX_ERR_STS(channel)		(0xD60 + ((channel) << 6))
#define DMAC_TX_ERR_STS_RESERVED_		(0xFFDEFF00)
#define DMAC_TX_ERR_STS_TX_DESC_TAIL_ERR_EN_	BIT(21)
#define DMAC_TX_ERR_STS_TX_DESC_SEQ_ERR_EN_	BIT(16)
#define DMAC_TX_ERR_STS_TX_DATA_READ_ERR_	BIT(7)
#define DMAC_TX_ERR_STS_TX_DESC_READ_ERR_	BIT(6)
#define DMAC_TX_ERR_STS_TX_DESC_TAIL_ERR_	BIT(5)
#define DMAC_TX_ERR_STS_TX_FCT_TXE_		BIT(4)
#define DMAC_TX_ERR_STS_TX_DESC_DATATYPE_ERR_	BIT(3)
#define DMAC_TX_ERR_STS_TX_DESC_EXTNTYPE_ERR_	BIT(2)
#define DMAC_TX_ERR_STS_TX_DESC_EXTRAFS_ERR_	BIT(1)
#define DMAC_TX_ERR_STS_TX_DESC_NOFS_ERR_	BIT(0)

#define DMAC_DEBUG_0	(0xFF0)
#define DMAC_DEBUG_1	(0xFF4)
#define DMAC_DEBUG_2	(0xFF8)

/* MAC statistics registers */
#define STAT_RX_FCS_ERRORS			(0x1200)
#define STAT_RX_ALIGNMENT_ERRORS		(0x1204)
#define STAT_RX_FRAGMENT_ERRORS			(0x1208)
#define STAT_RX_JABBER_ERRORS			(0x120C)
#define STAT_RX_UNDERSIZE_FRAME_ERRORS		(0x1210)
#define STAT_RX_OVERSIZE_FRAME_ERRORS		(0x1214)
#define STAT_RX_DROPPED_FRAMES			(0x1218)
#define STAT_RX_UNICAST_BYTE_COUNT		(0x121C)
#define STAT_RX_BROADCAST_BYTE_COUNT		(0x1220)
#define STAT_RX_MULTICAST_BYTE_COUNT		(0x1224)
#define STAT_RX_UNICAST_FRAMES			(0x1228)
#define STAT_RX_BROADCAST_FRAMES		(0x122C)
#define STAT_RX_MULTICAST_FRAMES		(0x1230)
#define STAT_RX_PAUSE_FRAMES			(0x1234)
#define STAT_RX_64_BYTE_FRAMES			(0x1238)
#define STAT_RX_65_127_BYTE_FRAMES		(0x123C)
#define STAT_RX_128_255_BYTE_FRAMES		(0x1240)
#define STAT_RX_256_511_BYTES_FRAMES		(0x1244)
#define STAT_RX_512_1023_BYTE_FRAMES		(0x1248)
#define STAT_RX_1024_1518_BYTE_FRAMES		(0x124C)
#define STAT_RX_GREATER_1518_BYTE_FRAMES	(0x1250)
#define STAT_RX_TOTAL_FRAMES			(0x1254)
#define STAT_EEE_RX_LPI_TRANSITIONS		(0x1258)
#define STAT_EEE_RX_LPI_TIME			(0x125C)
#define STAT_RX_COUNTER_ROLLOVER_STATUS		(0x127C)

#define STAT_TX_FCS_ERRORS			(0x1280)
#define STAT_TX_EXCESS_DEFERRAL_ERRORS		(0x1284)
#define STAT_TX_CARRIER_ERRORS			(0x1288)
#define STAT_TX_BAD_BYTE_COUNT			(0x128C)
#define STAT_TX_SINGLE_COLLISIONS		(0x1290)
#define STAT_TX_MULTIPLE_COLLISIONS		(0x1294)
#define STAT_TX_EXCESSIVE_COLLISION		(0x1298)
#define STAT_TX_LATE_COLLISIONS			(0x129C)
#define STAT_TX_UNICAST_BYTE_COUNT		(0x12A0)
#define STAT_TX_BROADCAST_BYTE_COUNT		(0x12A4)
#define STAT_TX_MULTICAST_BYTE_COUNT		(0x12A8)
#define STAT_TX_UNICAST_FRAMES			(0x12AC)
#define STAT_TX_BROADCAST_FRAMES		(0x12B0)
#define STAT_TX_MULTICAST_FRAMES		(0x12B4)
#define STAT_TX_PAUSE_FRAMES			(0x12B8)
#define STAT_TX_64_BYTE_FRAMES			(0x12BC)
#define STAT_TX_65_127_BYTE_FRAMES		(0x12C0)
#define STAT_TX_128_255_BYTE_FRAMES		(0x12C4)
#define STAT_TX_256_511_BYTES_FRAMES		(0x12C8)
#define STAT_TX_512_1023_BYTE_FRAMES		(0x12CC)
#define STAT_TX_1024_1518_BYTE_FRAMES		(0x12D0)
#define STAT_TX_GREATER_1518_BYTE_FRAMES	(0x12D4)
#define STAT_TX_TOTAL_FRAMES			(0x12D8)
#define STAT_EEE_TX_LPI_TRANSITIONS		(0x12DC)
#define STAT_EEE_TX_LPI_TIME			(0x12E0)
#define STAT_TX_COUNTER_ROLLOVER_STATUS		(0x12FC)

/* End of Register definitions */

#define LAN743X_NUMBER_OF_TX_CHANNELS	(1)
#define LAN743X_NUMBER_OF_RX_CHANNELS	(4)
#define LAN743X_PHY_TRACE_ENABLE	(0)
struct lan743x_adapter;

#define NETIF_INFO(adapter, type, netdev, fmt, ...) \
	netif_info(adapter, type, netdev, "%s.INFO: " fmt "\n", \
	__func__, ##__VA_ARGS__)
#define NETIF_WARNING(adapter, type, netdev, fmt, ...) \
	netif_warn(adapter, type, netdev, "%s.WARNING: " fmt "\n", \
	__func__, ##__VA_ARGS__)
#define NETIF_ERROR(adapter, type, netdev, fmt, ...) \
	netif_err(adapter, type, netdev, "%s.ERROR: " fmt "\n", \
	__func__, ##__VA_ARGS__)
#define NETIF_ASSERT(adapter, type, netdev, condition) \
	do { if (!(condition)) netif_err(adapter, type, netdev, \
	"ASSERTION_FAILURE, File = %s, Line = %d\n", \
	__FILE__, __LINE__); } while (0)

/* PCI */
/* SMSC acquired EFAR late 1990's, MCHP acquired SMSC 2012 */
#define PCI_VENDOR_ID_SMSC		PCI_VENDOR_ID_EFAR
#define PCI_DEVICE_ID_SMSC_LAN7430	(0x7430)

#define PCI_CONFIG_LENGTH		(0x1000)

struct lan743x_pci {
	struct pci_dev *pdev;
	int init_flags;
	unsigned long bar_flags;
};

static int lan743x_pci_init(struct lan743x_adapter *adapter,
			    struct pci_dev *pdev);
static void lan743x_pci_cleanup(struct lan743x_adapter *adapter);
static u8 __iomem *lan743x_pci_get_bar_address(struct lan743x_adapter *adapter,
					       int bar);
static void lan743x_pci_release_bar_address(struct lan743x_adapter *adapter,
					    int bar, u8 __iomem *bar_address);
static unsigned int lan743x_pci_get_irq(struct lan743x_adapter *adapter);

/* CSR */
#define CSR_LENGTH					(0x2000)

struct lan743x_csr {
	u8 __iomem *csr_address;
	u32 id_rev;
	u32 fpga_rev;
};

static int lan743x_csr_init(struct lan743x_adapter *adapter);
static void lan743x_csr_cleanup(struct lan743x_adapter *adapter);
static int lan743x_csr_light_reset(struct lan743x_adapter *adapter);
static inline u32 lan743x_csr_read(struct lan743x_adapter *adapter, int offset);
static inline void lan743x_csr_write(struct lan743x_adapter *adapter,
				     int offset, u32 data);

/* INTERRUPTS */
typedef void(*lan743x_vector_handler)(void *context, u32 int_sts);

struct lan743x_vector {
	struct lan743x_adapter	*adapter;
	int			vector_index;
	int			irq;
	u32			int_mask;
	lan743x_vector_handler	handler;
	void			*context;
};

#define LAN743X_MAX_VECTOR_COUNT	(6)

struct lan743x_intr {
	int			flags;

	unsigned int		irq;

	struct msix_entry	msix_entries[LAN743X_MAX_VECTOR_COUNT];

	struct lan743x_vector	vector_list[LAN743X_MAX_VECTOR_COUNT];
	int			number_of_vectors;

	int			software_isr_flag;
};

static void lan743x_vector_init(struct lan743x_vector *vector,
				struct lan743x_adapter *adapter,
				int vector_index, int irq, u32 int_mask,
				lan743x_vector_handler handler, void *context);

static int lan743x_intr_init(struct lan743x_adapter *adapter);
static void lan743x_intr_cleanup(struct lan743x_adapter *adapter);
static int lan743x_intr_open(struct lan743x_adapter *adapter);
static void lan743x_intr_close(struct lan743x_adapter *adapter);

/* DP */
struct lan743x_dp {
	int		flags;

	/* lock, used to prevent concurrent access to data port */
	struct mutex	lock;
};

static int lan743x_dp_init(struct lan743x_adapter *adapter);
static void lan743x_dp_cleanup(struct lan743x_adapter *adapter);
static int lan743x_dp_open(struct lan743x_adapter *adapter);
static void lan743x_dp_close(struct lan743x_adapter *adapter);

static int lan743x_dp_write_hash_filter(
	struct lan743x_adapter *adapter,
	u32 *hash_data);

/* GPIO */
struct lan743x_gpio {
	/* gpio_lock: used to prevent concurrent access to gpio settings */
	spinlock_t gpio_lock;

	int used_bits;
	int output_bits;
	int ptp_bits;
	u32 gpio_cfg0;
	u32 gpio_cfg1;
	u32 gpio_cfg2;
	u32 gpio_cfg3;
};

static int lan743x_gpio_init(struct lan743x_adapter *adapter);
static void lan743x_gpio_cleanup(struct lan743x_adapter *adapter);
static int lan743x_gpio_open(struct lan743x_adapter *adapter);
static void lan743x_gpio_close(struct lan743x_adapter *adapter);

/* PTP */
#include "linux/ptp_clock_kernel.h"

#define LAN743X_PTP_NUMBER_OF_TX_TIMESTAMPS (4)

#define PTP_FLAG_PTP_CLOCK_REGISTERED	BIT(1)
#define PTP_FLAG_ISR_ENABLED			BIT(2)

struct lan743x_ptp {
	int flags;

	/* command_lock: used to prevent concurrent ptp commands */
	struct mutex	command_lock;

#ifdef CONFIG_PTP_1588_CLOCK
	struct ptp_clock *ptp_clock;
	struct ptp_clock_info ptp_clock_info;
	struct ptp_pin_desc pin_config[1];
#endif /* CONFIG_PTP_1588_CLOCK */

	struct tasklet_struct	ptp_isr_bottom_half;

#define LAN743X_PTP_NUMBER_OF_EVENT_CHANNELS (2)
	unsigned long used_event_ch;

	int pps_event_ch;
	int pps_gpio_bit;

	/* tx_ts_lock: used to prevent concurrent access to timestamp arrays */
	struct mutex	tx_ts_lock;
	int pending_tx_timestamps;
	struct sk_buff *tx_ts_skb_queue[
		LAN743X_PTP_NUMBER_OF_TX_TIMESTAMPS];
	int tx_ts_skb_queue_size;
	u32 tx_ts_seconds_queue[
		LAN743X_PTP_NUMBER_OF_TX_TIMESTAMPS];
	u32 tx_ts_nseconds_queue[
		LAN743X_PTP_NUMBER_OF_TX_TIMESTAMPS];
	int tx_ts_queue_size;
};

static void lan743x_ptp_isr(void *context);

static int lan743x_ptp_init(struct lan743x_adapter *adapter);
static void lan743x_ptp_cleanup(struct lan743x_adapter *adapter);
static int lan743x_ptp_open(struct lan743x_adapter *adapter);
static void lan743x_ptp_close(struct lan743x_adapter *adapter);

static bool lan743x_ptp_is_enabled(struct lan743x_adapter *adapter);
static void lan743x_ptp_enable(struct lan743x_adapter *adapter);
static void lan743x_ptp_disable(struct lan743x_adapter *adapter);
static void lan743x_ptp_reset(struct lan743x_adapter *adapter);

#ifdef CONFIG_PTP_1588_CLOCK
static void lan743x_ptp_clock_get(struct lan743x_adapter *adapter,
				  u32 *seconds, u32 *nano_seconds,
				  u32 *sub_nano_seconds);
#endif /* CONFIG_PTP_1588_CLOCK */

static void lan743x_ptp_clock_set(struct lan743x_adapter *adapter,
				  u32 seconds, u32 nano_seconds,
				  u32 sub_nano_seconds);

#ifdef CONFIG_PTP_1588_CLOCK
static void lan743x_ptp_clock_step(struct lan743x_adapter *adapter,
				   s64 time_step_ns);
#endif /* CONFIG_PTP_1588_CLOCK */

static bool lan743x_ptp_request_tx_timestamp(struct lan743x_adapter *adapter);
static void lan743x_ptp_tx_timestamp_skb(struct lan743x_adapter *adapter,
					 struct sk_buff *skb);

#ifdef CONFIG_PTP_1588_CLOCK
static int lan743x_ptp_get_clock_index(struct lan743x_adapter *adapter);
#endif

/* MAC */
struct lan743x_mac {
	int	flags;

	struct mii_bus	*mdiobus;
	/* mii_mutex: used to prevent concurrent access to mdiobus */
	struct mutex	mii_mutex;

	u8	mac_address[ETH_ALEN];

	/* tx_mutext: used to prevent concurrent access to tx_enable_bits */
	struct mutex tx_mutex;
	unsigned long tx_enable_bits;

	/* rx_mutex: used to prevent concurrent access to rx_enable_bits */
	struct mutex rx_mutex;
	unsigned long rx_enable_bits;

	struct net_device_stats statistics;
};

#define LAN743X_MAX_FRAME_SIZE			(9 * 1024)

static void lan743x_mac_isr(void *context);

static int lan743x_mac_init(struct lan743x_adapter *adapter);
static void lan743x_mac_cleanup(struct lan743x_adapter *adapter);
static int lan743x_mac_open(struct lan743x_adapter *adapter);
static void lan743x_mac_close(struct lan743x_adapter *adapter);

static void lan743x_mac_get_address(struct lan743x_adapter *adapter,
				    u8 *mac_addr);

static int lan743x_mac_mii_read(struct lan743x_adapter *adapter,
				int phy_id, int index);
static int lan743x_mac_mii_write(struct lan743x_adapter *adapter,
				 int phy_id, int index, u16 regval);

static void lan743x_mac_flow_ctrl_set_enables(struct lan743x_adapter *adapter,
					      bool tx_enable, bool rx_enable);

static int lan743x_mac_tx_enable(struct lan743x_adapter *adapter,
				 int tx_channel);
static int lan743x_mac_tx_disable(struct lan743x_adapter *adapter,
				  int tx_channel);
static int lan743x_mac_rx_enable(struct lan743x_adapter *adapter,
				 int rx_channel);
static int lan743x_mac_rx_disable(struct lan743x_adapter *adapter,
				  int rx_channel);
static int lan743x_mac_set_mtu(struct lan743x_adapter *adapter, int new_mtu);

static struct net_device_stats *mac_get_stats(struct lan743x_adapter *adapter);

/* PHY */
struct lan743x_phy {
	int	flags;

	bool	fc_autoneg;
	u8	fc_request_control;
};

static int lan743x_phy_init(struct lan743x_adapter *adapter);
static void lan743x_phy_cleanup(struct lan743x_adapter *adapter);
static int lan743x_phy_open(struct lan743x_adapter *adapter);
static void lan743x_phy_close(struct lan743x_adapter *adapter);

/* RFE */
struct lan743x_rfe {
	int	flags;
};

static int lan743x_rfe_init(struct lan743x_adapter *adapter);
static void lan743x_rfe_cleanup(struct lan743x_adapter *adapter);
static int lan743x_rfe_open(struct lan743x_adapter *adapter);
static void lan743x_rfe_close(struct lan743x_adapter *adapter);

static void lan743x_rfe_set_multicast(struct lan743x_adapter *adapter);

/* FCT */

static void lan743x_fct_isr(void *context);

static int lan743x_fct_init(struct lan743x_adapter *adapter);
static void lan743x_fct_cleanup(struct lan743x_adapter *adapter);
static int lan743x_fct_open(struct lan743x_adapter *adapter);
static void lan743x_fct_close(struct lan743x_adapter *adapter);

static int lan743x_fct_rx_reset(struct lan743x_adapter *adapter,
				int rx_channel);
static int lan743x_fct_rx_enable(struct lan743x_adapter *adapter,
				 int rx_channel);
static int lan743x_fct_rx_disable(struct lan743x_adapter *adapter,
				  int rx_channel);

static int lan743x_fct_tx_reset(struct lan743x_adapter *adapter,
				int tx_channel);
static int lan743x_fct_tx_enable(struct lan743x_adapter *adapter,
				 int tx_channel);
static int lan743x_fct_tx_disable(struct lan743x_adapter *adapter,
				  int tx_channel);

/* DMAC */
struct lan743x_dmac {
	int flags;

	int descriptor_spacing;
};

static void lan743x_dmac_isr(void *context);

static int lan743x_dmac_init(struct lan743x_adapter *adapter);
static void lan743x_dmac_cleanup(struct lan743x_adapter *adapter);
static int lan743x_dmac_open(struct lan743x_adapter *adapter);
static void lan743x_dmac_close(struct lan743x_adapter *adapter);

static int lan743x_dmac_get_descriptor_spacing(struct lan743x_adapter *adapter);

static int lan743x_dmac_reserve_tx_channel(struct lan743x_adapter *adapter,
					   int tx_channel);
static void lan743x_dmac_release_tx_channel(struct lan743x_adapter *adapter,
					    int tx_channel);
static int lan743x_dmac_reserve_rx_channel(struct lan743x_adapter *adapter,
					   int rx_channel);
static void lan743x_dmac_release_rx_channel(struct lan743x_adapter *adapter,
					    int rx_channel);
static int lan743x_dmac_tx_reset(struct lan743x_adapter *adapter,
				 int tx_channel);
static int lan743x_dmac_tx_start(struct lan743x_adapter *adapter,
				 int tx_channel);
static int lan743x_dmac_tx_stop(struct lan743x_adapter *adapter,
				int tx_channel);
static int lan743x_dmac_rx_reset(struct lan743x_adapter *adapter,
				 int rx_channel);
static int lan743x_dmac_rx_start(struct lan743x_adapter *adapter,
				 int rx_channel);
static int lan743x_dmac_rx_stop(struct lan743x_adapter *adapter,
				int rx_channel);

/* TX */
struct lan743x_tx_descriptor;
struct lan743x_tx_buffer_info;

#define TX_FLAG_MAC_ENABLED		BIT(1)
#define TX_FLAG_FIFO_ENABLED		BIT(2)
#define TX_FLAG_ISR_ENABLED		BIT(3)
#define TX_FLAG_DMAC_STARTED		BIT(4)
#define TX_FLAG_GPIO0_RESERVED		BIT(5)
#define TX_FLAG_GPIO1_RESERVED		BIT(6)
#define TX_FLAG_GPIO2_RESERVED		BIT(7)
#define TX_FLAG_GPIO3_RESERVED		BIT(8)
#define TX_FLAG_TIMESTAMPING_ENABLED	BIT(9)
#define TX_FLAG_RING_ALLOCATED		BIT(10)

#define GPIO_QUEUE_STARTED		(0)
#define GPIO_TX_FUNCTION		(1)
#define GPIO_TX_COMPLETION		(2)
#define GPIO_TX_FRAGMENT		(3)

#define TX_FRAME_FLAG_IN_PROGRESS	BIT(0)

struct lan743x_tx {
	struct lan743x_adapter *adapter;
	int	flags;
	int	channel_number;

	int	ring_size;
	size_t	ring_allocation_size;
	struct lan743x_tx_descriptor *ring_cpu_ptr;
	dma_addr_t ring_dma_ptr;
	/* ring_lock: used to prevent concurrent access to tx ring */
	spinlock_t ring_lock;
	u32		frame_flags;
	u32		frame_first;
	u32		frame_data0;
	u32		frame_tail;

	struct lan743x_tx_buffer_info *buffer_info;

	u32		*head_cpu_ptr;
	dma_addr_t	head_dma_ptr;
	int		last_head;
	int		last_tail;

	struct tasklet_struct	tx_isr_bottom_half;

	struct sk_buff *overflow_skb;
};

static void lan743x_tx_isr(void *context, u32 int_sts);

static int lan743x_tx_ring_init(struct lan743x_tx *tx);
static void lan743x_tx_ring_cleanup(struct lan743x_tx *tx);
static int lan743x_tx_init(struct lan743x_tx *tx,
			   struct lan743x_adapter *adapter,
			   int channel_number);
static void lan743x_tx_cleanup(struct lan743x_tx *tx);
static int lan743x_tx_open(struct lan743x_tx *tx);
static void lan743x_tx_close(struct lan743x_tx *tx);

static void lan743x_tx_set_timestamping_enable(struct lan743x_tx *tx,
					       bool enabled);

static netdev_tx_t lan743x_tx_xmit_frame(struct lan743x_tx *tx,
					 struct sk_buff *skb);

/* RX */
struct lan743x_rx_descriptor;
struct lan743x_rx_buffer_info;

#define RX_FLAG_NAPI_ADDED	BIT(0)
#define RX_FLAG_DMAC_STARTED	BIT(1)
#define RX_FLAG_ISR_ENABLED	BIT(2)
#define RX_FLAG_FIFO_ENABLED	BIT(3)
#define RX_FLAG_MAC_ENABLED	BIT(4)
#define RX_FLAG_RING_ALLOCATED	BIT(5)

struct lan743x_rx {
	struct lan743x_adapter *adapter;
	int	flags;
	int	channel_number;

	int	ring_size;
	size_t	ring_allocation_size;
	struct lan743x_rx_descriptor *ring_cpu_ptr;
	dma_addr_t ring_dma_ptr;

	struct lan743x_rx_buffer_info *buffer_info;

	u32		*head_cpu_ptr;
	dma_addr_t	head_dma_ptr;
	u32		last_head;

	struct napi_struct napi;
};

static void lan743x_rx_isr(void *context, u32 int_sts);

static int lan743x_rx_ring_init(struct lan743x_rx *rx);
static void lan743x_rx_ring_cleanup(struct lan743x_rx *rx);
static int lan743x_rx_init(struct lan743x_rx *rx,
			   struct lan743x_adapter *adapter, int channel_number);
static void lan743x_rx_cleanup(struct lan743x_rx *rx);
static int lan743x_rx_open(struct lan743x_rx *rx);
static void lan743x_rx_close(struct lan743x_rx *rx);

struct lan743x_adapter {
	struct net_device       *netdev;
	int                     init_flags;
	int                     open_flags;

	int                     msg_enable;

	struct lan743x_pci      pci;
	struct lan743x_csr      csr;
	struct lan743x_intr     intr;
	struct lan743x_dp       dp;
	struct lan743x_gpio     gpio;
	struct lan743x_ptp      ptp;
	struct lan743x_mac      mac;
	struct lan743x_phy      phy;
	struct lan743x_rfe      rfe;
	struct lan743x_dmac     dmac;
	struct lan743x_tx       tx[LAN743X_NUMBER_OF_TX_CHANNELS];
	struct lan743x_rx       rx[LAN743X_NUMBER_OF_RX_CHANNELS];
};

#endif /* _LAN743X_H */
