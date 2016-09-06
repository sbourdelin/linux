 /***************************************************************************
 *
 * Copyright (C) 2007-2008 SMSC
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 *****************************************************************************/

#ifndef _SMSC95XX_H
#define _SMSC95XX_H

/* Tx command words */
#define TX_CMD_A_DATA_OFFSET		(0x001F0000)
#define TX_CMD_A_FIRST_SEG		(0x00002000)
#define TX_CMD_A_LAST_SEG		(0x00001000)
#define TX_CMD_A_BUF_SIZE		(0x000007FF)

#define TX_CMD_B_CSUM_ENABLE		(0x00004000)
#define TX_CMD_B_ADD_CRC_DISABLE	(0x00002000)
#define TX_CMD_B_DISABLE_PADDING	(0x00001000)
#define TX_CMD_B_PKT_BYTE_LENGTH	(0x000007FF)

/* Rx status word */
#define RX_STS_FF			(0x40000000)	/* Filter Fail */
#define RX_STS_FL			(0x3FFF0000)	/* Frame Length */
#define RX_STS_ES			(0x00008000)	/* Error Summary */
#define RX_STS_BF			(0x00002000)	/* Broadcast Frame */
#define RX_STS_LE			(0x00001000)	/* Length Error */
#define RX_STS_RF			(0x00000800)	/* Runt Frame */
#define RX_STS_MF			(0x00000400)	/* Multicast Frame */
#define RX_STS_TL			(0x00000080)	/* Frame too long */
#define RX_STS_CS			(0x00000040)	/* Collision Seen */
#define RX_STS_FT			(0x00000020)	/* Frame Type */
#define RX_STS_RW			(0x00000010)	/* Receive Watchdog */
#define RX_STS_ME			(0x00000008)	/* Mii Error */
#define RX_STS_DB			(0x00000004)	/* Dribbling */
#define RX_STS_CRC			(0x00000002)	/* CRC Error */

/* SCSRs */
#define ID_REV				(0x00)
#define ID_REV_CHIP_ID_MASK		(0xFFFF0000)
#define ID_REV_CHIP_REV_MASK		(0x0000FFFF)
#define ID_REV_CHIP_ID_9500		(0x9500)
#define ID_REV_CHIP_ID_9500A		(0x9E00)
#define ID_REV_CHIP_ID_9512		(0xEC00)
#define ID_REV_CHIP_ID_9530		(0x9530)
#define ID_REV_CHIP_ID_89530		(0x9E08)
#define ID_REV_CHIP_ID_9730		(0x9730)

#define INT_STS				(0x08)
#define INT_STS_TX_STOP			(0x00020000)
#define INT_STS_RX_STOP			(0x00010000)
#define INT_STS_PHY_INT			(0x00008000)
#define INT_STS_TXE			(0x00004000)
#define INT_STS_TDFU			(0x00002000)
#define INT_STS_TDFO			(0x00001000)
#define INT_STS_RXDF			(0x00000800)
#define INT_STS_GPIOS			(0x000007FF)
#define INT_STS_CLEAR_ALL		(0xFFFFFFFF)

#define RX_CFG				(0x0C)
#define RX_FIFO_FLUSH			(0x00000001)

#define TX_CFG				(0x10)
#define TX_CFG_ON			(0x00000004)
#define TX_CFG_STOP			(0x00000002)
#define TX_CFG_FIFO_FLUSH		(0x00000001)

#define HW_CFG				(0x14)
#define HW_CFG_BIR			(0x00001000)
#define HW_CFG_LEDB			(0x00000800)
#define HW_CFG_RXDOFF			(0x00000600)
#define HW_CFG_DRP			(0x00000040)
#define HW_CFG_MEF			(0x00000020)
#define HW_CFG_LRST			(0x00000008)
#define HW_CFG_PSEL			(0x00000004)
#define HW_CFG_BCE			(0x00000002)
#define HW_CFG_SRST			(0x00000001)

#define RX_FIFO_INF			(0x18)

#define PM_CTRL				(0x20)
#define PM_CTL_RES_CLR_WKP_STS		(0x00000200)
#define PM_CTL_DEV_RDY			(0x00000080)
#define PM_CTL_SUS_MODE			(0x00000060)
#define PM_CTL_SUS_MODE_0		(0x00000000)
#define PM_CTL_SUS_MODE_1		(0x00000020)
#define PM_CTL_SUS_MODE_2		(0x00000040)
#define PM_CTL_SUS_MODE_3		(0x00000060)
#define PM_CTL_PHY_RST			(0x00000010)
#define PM_CTL_WOL_EN			(0x00000008)
#define PM_CTL_ED_EN			(0x00000004)
#define PM_CTL_WUPS			(0x00000003)
#define PM_CTL_WUPS_NO			(0x00000000)
#define PM_CTL_WUPS_ED			(0x00000001)
#define PM_CTL_WUPS_WOL			(0x00000002)
#define PM_CTL_WUPS_MULTI		(0x00000003)

#define LED_GPIO_CFG			(0x24)
#define LED_GPIO_CFG_SPD_LED		(0x01000000)
#define LED_GPIO_CFG_LNK_LED		(0x00100000)
#define LED_GPIO_CFG_FDX_LED		(0x00010000)

#define GPIO_CFG			(0x28)

#define AFC_CFG				(0x2C)

/* Hi watermark = 15.5Kb (~10 mtu pkts) */
/* low watermark = 3k (~2 mtu pkts) */
/* backpressure duration = ~ 350us */
/* Apply FC on any frame. */
#define AFC_CFG_DEFAULT			(0x00F830A1)

#define E2P_CMD				(0x30)
#define E2P_CMD_BUSY			(0x80000000)
#define E2P_CMD_MASK			(0x70000000)
#define E2P_CMD_READ			(0x00000000)
#define E2P_CMD_EWDS			(0x10000000)
#define E2P_CMD_EWEN			(0x20000000)
#define E2P_CMD_WRITE			(0x30000000)
#define E2P_CMD_WRAL			(0x40000000)
#define E2P_CMD_ERASE			(0x50000000)
#define E2P_CMD_ERAL			(0x60000000)
#define E2P_CMD_RELOAD			(0x70000000)
#define E2P_CMD_TIMEOUT			(0x00000400)
#define E2P_CMD_LOADED			(0x00000200)
#define E2P_CMD_ADDR			(0x000001FF)

#define MAX_EEPROM_SIZE			(512)

#define E2P_DATA			(0x34)
#define E2P_DATA_MASK			(0x000000FF)

#define BURST_CAP			(0x38)

#define	STRAP_STATUS			(0x3C)
#define	STRAP_STATUS_PWR_SEL		(0x00000020)
#define	STRAP_STATUS_AMDIX_EN		(0x00000010)
#define	STRAP_STATUS_PORT_SWAP		(0x00000008)
#define	STRAP_STATUS_EEP_SIZE		(0x00000004)
#define	STRAP_STATUS_RMT_WKP		(0x00000002)
#define	STRAP_STATUS_EEP_DISABLE	(0x00000001)

#define GPIO_WAKE			(0x64)

#define INT_EP_CTL			(0x68)
#define INT_EP_CTL_INTEP		(0x80000000)
#define INT_EP_CTL_MACRTO		(0x00080000)
#define INT_EP_CTL_TX_STOP		(0x00020000)
#define INT_EP_CTL_RX_STOP		(0x00010000)
#define INT_EP_CTL_PHY_INT		(0x00008000)
#define INT_EP_CTL_TXE			(0x00004000)
#define INT_EP_CTL_TDFU			(0x00002000)
#define INT_EP_CTL_TDFO			(0x00001000)
#define INT_EP_CTL_RXDF			(0x00000800)
#define INT_EP_CTL_GPIOS		(0x000007FF)

#define BULK_IN_DLY			(0x6C)

/* MAC CSRs */
#define MAC_CR				(0x100)
#define MAC_CR_RXALL			(0x80000000)
#define MAC_CR_RCVOWN			(0x00800000)
#define MAC_CR_LOOPBK			(0x00200000)
#define MAC_CR_FDPX			(0x00100000)
#define MAC_CR_MCPAS			(0x00080000)
#define MAC_CR_PRMS			(0x00040000)
#define MAC_CR_INVFILT			(0x00020000)
#define MAC_CR_PASSBAD			(0x00010000)
#define MAC_CR_HFILT			(0x00008000)
#define MAC_CR_HPFILT			(0x00002000)
#define MAC_CR_LCOLL			(0x00001000)
#define MAC_CR_BCAST			(0x00000800)
#define MAC_CR_DISRTY			(0x00000400)
#define MAC_CR_PADSTR			(0x00000100)
#define MAC_CR_BOLMT_MASK		(0x000000C0)
#define MAC_CR_DFCHK			(0x00000020)
#define MAC_CR_TXEN			(0x00000008)
#define MAC_CR_RXEN			(0x00000004)

#define ADDRH				(0x104)

#define ADDRL				(0x108)

#define HASHH				(0x10C)

#define HASHL				(0x110)

#define MII_ADDR			(0x114)
#define MII_WRITE			(0x02)
#define MII_BUSY			(0x01)
#define MII_READ			(0x00) /* ~of MII Write bit */

#define MII_DATA			(0x118)

#define FLOW				(0x11C)
#define FLOW_FCPT			(0xFFFF0000)
#define FLOW_FCPASS			(0x00000004)
#define FLOW_FCEN			(0x00000002)
#define FLOW_FCBSY			(0x00000001)

#define VLAN1				(0x120)

#define VLAN2				(0x124)

#define WUFF				(0x128)
#define LAN9500_WUFF_NUM		(4)
#define LAN9500A_WUFF_NUM		(8)

#define WUCSR				(0x12C)
#define WUCSR_WFF_PTR_RST		(0x80000000)
#define WUCSR_GUE			(0x00000200)
#define WUCSR_WUFR			(0x00000040)
#define WUCSR_MPR			(0x00000020)
#define WUCSR_WAKE_EN			(0x00000004)
#define WUCSR_MPEN			(0x00000002)

#define COE_CR				(0x130)
#define TX_COE_EN			(0x00010000)
#define RX_COE_MODE			(0x00000002)
#define RX_COE_EN			(0x00000001)

/* Vendor-specific PHY Definitions */

/* EDPD NLP / crossover time configuration (LAN9500A only) */
#define PHY_EDPD_CONFIG			(16)
#define PHY_EDPD_CONFIG_TX_NLP_EN	((u16)0x8000)
#define PHY_EDPD_CONFIG_TX_NLP_1000	((u16)0x0000)
#define PHY_EDPD_CONFIG_TX_NLP_768	((u16)0x2000)
#define PHY_EDPD_CONFIG_TX_NLP_512	((u16)0x4000)
#define PHY_EDPD_CONFIG_TX_NLP_256	((u16)0x6000)
#define PHY_EDPD_CONFIG_RX_1_NLP	((u16)0x1000)
#define PHY_EDPD_CONFIG_RX_NLP_64	((u16)0x0000)
#define PHY_EDPD_CONFIG_RX_NLP_256	((u16)0x0400)
#define PHY_EDPD_CONFIG_RX_NLP_512	((u16)0x0800)
#define PHY_EDPD_CONFIG_RX_NLP_1000	((u16)0x0C00)
#define PHY_EDPD_CONFIG_EXT_CROSSOVER	((u16)0x0001)
#define PHY_EDPD_CONFIG_DEFAULT		(PHY_EDPD_CONFIG_TX_NLP_EN | \
					 PHY_EDPD_CONFIG_TX_NLP_768 | \
					 PHY_EDPD_CONFIG_RX_1_NLP)

/* Mode Control/Status Register */
#define PHY_MODE_CTRL_STS		(17)
#define MODE_CTRL_STS_EDPWRDOWN		((u16)0x2000)
#define MODE_CTRL_STS_ENERGYON		((u16)0x0002)

#define SPECIAL_CTRL_STS		(27)
#define SPECIAL_CTRL_STS_OVRRD_AMDIX	((u16)0x8000)
#define SPECIAL_CTRL_STS_AMDIX_ENABLE	((u16)0x4000)
#define SPECIAL_CTRL_STS_AMDIX_STATE	((u16)0x2000)

#define PHY_INT_SRC			(29)
#define PHY_INT_SRC_ENERGY_ON		((u16)0x0080)
#define PHY_INT_SRC_ANEG_COMP		((u16)0x0040)
#define PHY_INT_SRC_REMOTE_FAULT	((u16)0x0020)
#define PHY_INT_SRC_LINK_DOWN		((u16)0x0010)

#define PHY_INT_MASK			(30)
#define PHY_INT_MASK_ENERGY_ON		((u16)0x0080)
#define PHY_INT_MASK_ANEG_COMP		((u16)0x0040)
#define PHY_INT_MASK_REMOTE_FAULT	((u16)0x0020)
#define PHY_INT_MASK_LINK_DOWN		((u16)0x0010)
#define PHY_INT_MASK_DEFAULT		(PHY_INT_MASK_ANEG_COMP | \
					 PHY_INT_MASK_LINK_DOWN)

#define PHY_SPECIAL			(31)
#define PHY_SPECIAL_SPD			((u16)0x001C)
#define PHY_SPECIAL_SPD_10HALF		((u16)0x0004)
#define PHY_SPECIAL_SPD_10FULL		((u16)0x0014)
#define PHY_SPECIAL_SPD_100HALF		((u16)0x0008)
#define PHY_SPECIAL_SPD_100FULL		((u16)0x0018)

/* USB Vendor Requests */
#define USB_VENDOR_REQUEST_WRITE_REGISTER	0xA0
#define USB_VENDOR_REQUEST_READ_REGISTER	0xA1
#define USB_VENDOR_REQUEST_GET_STATS		0xA2

/* Interrupt Endpoint status word bitfields */
#define INT_ENP_TX_STOP			((u32)BIT(17))
#define INT_ENP_RX_STOP			((u32)BIT(16))
#define INT_ENP_PHY_INT			((u32)BIT(15))
#define INT_ENP_TXE			((u32)BIT(14))
#define INT_ENP_TDFU			((u32)BIT(13))
#define INT_ENP_TDFO			((u32)BIT(12))
#define INT_ENP_RXDF			((u32)BIT(11))

#endif /* _SMSC95XX_H */
