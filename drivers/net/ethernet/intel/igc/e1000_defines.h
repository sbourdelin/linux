/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c)  2018 Intel Corporation */

#ifndef _E1000_DEFINES_H_
#define _E1000_DEFINES_H_

#define E1000_CTRL_EXT_DRV_LOAD         0x10000000 /* Drv loaded bit for FW */

#define E1000_CTRL_EXT_LINK_MODE_PCIE_SERDES	0x00C00000
/* Priority on PCI. 0=rx,1=fair */
#define E1000_CTRL_PRIOR			0x00000004
/* PCI Function Mask */
#define E1000_STATUS_FUNC_MASK			0x0000000C
#define E1000_EXTCNF_SIZE_EXT_PCIE_LENGTH_MASK	0x00FF0000
#define E1000_EXTCNF_SIZE_EXT_PCIE_LENGTH_SHIFT	16

/* PCI Bus Info */
#define PCI_HEADER_TYPE_REGISTER	0x0E
#define PCIE_LINK_STATUS		0x12
#define PCIE_DEVICE_CONTROL2		0x28
#define PCIE_LINK_WIDTH_MASK		0x3F0
#define PCIE_LINK_WIDTH_SHIFT		4
#define PCIE_LINK_SPEED_MASK		0x0F
#define PCIE_LINK_SPEED_2500		0x01
#define PCIE_LINK_SPEED_5000		0x02
#define PCIE_DEVICE_CONTROL2_16ms	0x0005

/* Lx when no PCIe transactions */
#define E1000_PCIEMISC_LX_DECISION	0x00000080
#define E1000_PCI_PMCSR			0x44
#define E1000_PCI_PMCSR_D3		0x03

/* Physical Func Reset Done Indication */
#define E1000_CTRL_EXT_LINK_MODE_MASK	0x00C00000

/* Loop limit on how long we wait for auto-negotiation to complete */
#define COPPER_LINK_UP_LIMIT		10
#define PHY_AUTO_NEG_LIMIT		45
#define PHY_FORCE_LIMIT			20

/* Number of 100 microseconds we wait for PCI Express master disable */
#define MASTER_DISABLE_TIMEOUT		800
/*Blocks new Master requests */
#define E1000_CTRL_GIO_MASTER_DISABLE	0x00000004
/* Status of Master requests. */
#define E1000_STATUS_GIO_MASTER_ENABLE	0x00080000

/* PCI Express Control */
#define E1000_GCR_CMPL_TMOUT_MASK	0x0000F000
#define E1000_GCR_CMPL_TMOUT_10ms	0x00001000
#define E1000_GCR_CMPL_TMOUT_RESEND	0x00010000
#define E1000_GCR_CAP_VER2		0x00040000

/* Receive Address
 * Number of high/low register pairs in the RAR. The RAR (Receive Address
 * Registers) holds the directed and multicast addresses that we monitor.
 * Technically, we have 16 spots.  However, we reserve one of these spots
 * (RAR[15]) for our directed address used by controllers with
 * manageability enabled, allowing us room for 15 multicast addresses.
 */
#define E1000_RAH_AV		0x80000000 /* Receive descriptor valid */
#define E1000_RAH_POOL_1	0x00040000
#define E1000_RAL_MAC_ADDR_LEN	4
#define E1000_RAH_MAC_ADDR_LEN	2

/* Error Codes */
#define E1000_SUCCESS				0
#define E1000_ERR_NVM				1
#define E1000_ERR_PHY				2
#define E1000_ERR_CONFIG			3
#define E1000_ERR_PARAM				4
#define E1000_ERR_MAC_INIT			5
#define E1000_ERR_RESET				9
#define E1000_ERR_MASTER_REQUESTS_PENDING	10
#define E1000_BLK_PHY_RESET			12
#define E1000_ERR_SWFW_SYNC			13

/* Device Control */
#define E1000_CTRL_RST		0x04000000  /* Global reset */

#define E1000_CTRL_PHY_RST	0x80000000  /* PHY Reset */
#define E1000_CTRL_SLU		0x00000040  /* Set link up (Force Link) */
#define E1000_CTRL_FRCSPD	0x00000800  /* Force Speed */
#define E1000_CTRL_FRCDPX	0x00001000  /* Force Duplex */

#define E1000_CTRL_RFCE		0x08000000  /* Receive Flow Control enable */
#define E1000_CTRL_TFCE		0x10000000  /* Transmit flow control enable */

#define E1000_CONNSW_AUTOSENSE_CONF	0x2
#define E1000_CONNSW_AUTOSENSE_EN	0x1

/* PBA constants */
#define E1000_PBA_34K			0x0022

/* SW Semaphore Register */
#define E1000_SWSM_SMBI		0x00000001 /* Driver Semaphore bit */
#define E1000_SWSM_SWESMBI	0x00000002 /* FW Semaphore bit */

/* SWFW_SYNC Definitions */
#define E1000_SWFW_EEP_SM	0x1
#define E1000_SWFW_PHY0_SM	0x2
#define E1000_SWFW_PHY1_SM	0x4
#define E1000_SWFW_PHY2_SM	0x20
#define E1000_SWFW_PHY3_SM	0x40

/* PHY 1000 MII Register/Bit Definitions */
/* PHY Registers defined by IEEE */
#define PHY_CONTROL		0x00 /* Control Register */
#define PHY_STATUS		0x01 /* Status Register */
#define PHY_ID1			0x02 /* Phy Id Reg (word 1) */
#define PHY_ID2			0x03 /* Phy Id Reg (word 2) */
#define PHY_AUTONEG_ADV		0x04 /* Autoneg Advertisement */
#define PHY_LP_ABILITY		0x05 /* Link Partner Ability (Base Page) */
#define PHY_1000T_CTRL		0x09 /* 1000Base-T Control Reg */
#define PHY_1000T_STATUS	0x0A /* 1000Base-T Status Reg */

/* Autoneg Advertisement Register */
#define NWAY_AR_10T_HD_CAPS	0x0020   /* 10T   Half Duplex Capable */
#define NWAY_AR_10T_FD_CAPS	0x0040   /* 10T   Full Duplex Capable */
#define NWAY_AR_100TX_HD_CAPS	0x0080   /* 100TX Half Duplex Capable */
#define NWAY_AR_100TX_FD_CAPS	0x0100   /* 100TX Full Duplex Capable */
#define NWAY_AR_PAUSE		0x0400   /* Pause operation desired */
#define NWAY_AR_ASM_DIR		0x0800   /* Asymmetric Pause Direction bit */

/* Link Partner Ability Register (Base Page) */
#define NWAY_LPAR_PAUSE		0x0400 /* LP Pause operation desired */
#define NWAY_LPAR_ASM_DIR	0x0800 /* LP Asymmetric Pause Direction bit */

/* 1000BASE-T Control Register */
#define CR_1000T_ASYM_PAUSE	0x0080 /* Advertise asymmetric pause bit */
#define CR_1000T_HD_CAPS	0x0100 /* Advertise 1000T HD capability */
#define CR_1000T_FD_CAPS	0x0200 /* Advertise 1000T FD capability  */

/* 1000BASE-T Status Register */
#define SR_1000T_REMOTE_RX_STATUS	0x1000 /* Remote receiver OK */
#define SR_1000T_LOCAL_RX_STATUS	0x2000 /* Local receiver OK */

/* PHY GPY 211 registers */
#define STANDARD_AN_REG_MASK	0x0007 /* MMD */
#define ANEG_MULTIGBT_AN_CTRL	0x0020 /* MULTI GBT AN Control Register */
#define MMD_DEVADDR_SHIFT	16     /* Shift MMD to higher bits */
#define CR_2500T_FD_CAPS	0x0080 /* Advertise 2500T FD capability */

/* NVM Control */
#define E1000_EECD_SK		0x00000001 /* NVM Clock */
#define E1000_EECD_CS		0x00000002 /* NVM Chip Select */
#define E1000_EECD_DI		0x00000004 /* NVM Data In */
#define E1000_EECD_DO		0x00000008 /* NVM Data Out */
#define E1000_EECD_REQ		0x00000040 /* NVM Access Request */
#define E1000_EECD_GNT		0x00000080 /* NVM Access Grant */
#define E1000_EECD_PRES		0x00000100 /* NVM Present */
/* NVM Addressing bits based on type 0=small, 1=large */
#define E1000_EECD_ADDR_BITS		0x00000400
#define E1000_NVM_GRANT_ATTEMPTS	1000 /* NVM # attempts to gain grant */
#define E1000_EECD_AUTO_RD		0x00000200  /* NVM Auto Read done */
#define E1000_EECD_SIZE_EX_MASK		0x00007800  /* NVM Size */
#define E1000_EECD_SIZE_EX_SHIFT	11
#define E1000_EECD_FLUPD_I225		0x00800000 /* Update FLASH */
#define E1000_EECD_FLUDONE_I225		0x04000000 /* Update FLASH done*/
#define E1000_EECD_FLASH_DETECTED_I225	0x00080000 /* FLASH detected */
#define E1000_FLUDONE_ATTEMPTS		20000
#define E1000_EERD_EEWR_MAX_COUNT	512 /* buffered EEPROM words rw */

/* Number of milliseconds for NVM auto read done after MAC reset. */
#define AUTO_READ_DONE_TIMEOUT		10
#define E1000_EECD_AUTO_RD	0x00000200  /* NVM Auto Read done */

/* Offset to data in NVM read/write registers */
#define E1000_NVM_RW_REG_DATA	16
#define E1000_NVM_RW_REG_DONE	2    /* Offset to READ/WRITE done bit */
#define E1000_NVM_RW_REG_START	1    /* Start operation */
#define E1000_NVM_RW_ADDR_SHIFT	2    /* Shift to the address bits */
#define E1000_NVM_POLL_READ	0    /* Flag for polling for read complete */

/* NVM Word Offsets */
#define NVM_COMPAT			0x0003
#define NVM_ID_LED_SETTINGS		0x0004 /* SERDES output amplitude */
#define NVM_VERSION			0x0005
#define NVM_INIT_CONTROL2_REG		0x000F
#define NVM_INIT_CONTROL3_PORT_B	0x0014
#define NVM_INIT_CONTROL3_PORT_A	0x0024
#define NVM_ALT_MAC_ADDR_PTR		0x0037
#define NVM_CHECKSUM_REG		0x003F
#define NVM_COMPATIBILITY_REG_3		0x0003
#define NVM_COMPATIBILITY_BIT_MASK	0x8000
#define NVM_MAC_ADDR			0x0000
#define NVM_SUB_DEV_ID			0x000B
#define NVM_SUB_VEN_ID			0x000C
#define NVM_DEV_ID			0x000D
#define NVM_VEN_ID			0x000E
#define NVM_INIT_CTRL_2			0x000F
#define NVM_INIT_CTRL_4			0x0013
#define NVM_LED_1_CFG			0x001C
#define NVM_LED_0_2_CFG			0x001F
#define NVM_ETRACK_WORD			0x0042
#define NVM_ETRACK_HIWORD		0x0043
#define NVM_COMB_VER_OFF		0x0083
#define NVM_COMB_VER_PTR		0x003d

/* For checksumming, the sum of all words in the NVM should equal 0xBABA. */
#define NVM_SUM				0xBABA

#define NVM_PBA_OFFSET_0		8
#define NVM_PBA_OFFSET_1		9
#define NVM_RESERVED_WORD		0xFFFF
#define NVM_PBA_PTR_GUARD		0xFAFA
#define NVM_WORD_SIZE_BASE_SHIFT	6

/* Collision related configuration parameters */
#define E1000_COLLISION_THRESHOLD	15
#define E1000_CT_SHIFT			4
#define E1000_COLLISION_DISTANCE	63
#define E1000_COLD_SHIFT		12

/* Device Status */
#define E1000_STATUS_FD		0x00000001      /* Full duplex.0=half,1=full */
#define E1000_STATUS_LU		0x00000002      /* Link up.0=no,1=link */
#define E1000_STATUS_FUNC_MASK	0x0000000C      /* PCI Function Mask */
#define E1000_STATUS_FUNC_SHIFT	2
#define E1000_STATUS_FUNC_1	0x00000004      /* Function 1 */
#define E1000_STATUS_TXOFF	0x00000010      /* transmission paused */
#define E1000_STATUS_SPEED_100	0x00000040      /* Speed 100Mb/s */
#define E1000_STATUS_SPEED_1000	0x00000080      /* Speed 1000Mb/s */
#define E1000_STATUS_SPEED_2500	0x00400000	/* Speed 2.5Gb/s */

#define SPEED_10		10
#define SPEED_100		100
#define SPEED_1000		1000
#define SPEED_2500		2500
#define HALF_DUPLEX		1
#define FULL_DUPLEX		2

/* 1Gbps and 2.5Gbps half duplex is not supported, nor spec-compliant. */
#define ADVERTISE_10_HALF		0x0001
#define ADVERTISE_10_FULL		0x0002
#define ADVERTISE_100_HALF		0x0004
#define ADVERTISE_100_FULL		0x0008
#define ADVERTISE_1000_HALF		0x0010 /* Not used, just FYI */
#define ADVERTISE_1000_FULL		0x0020
#define ADVERTISE_2500_HALF		0x0040 /* NOT used, just FYI */
#define ADVERTISE_2500_FULL		0x0080

#define E1000_ALL_SPEED_DUPLEX_2500 ( \
	ADVERTISE_10_HALF | ADVERTISE_10_FULL | ADVERTISE_100_HALF | \
	ADVERTISE_100_FULL | ADVERTISE_1000_FULL | ADVERTISE_2500_FULL)

#define AUTONEG_ADVERTISE_SPEED_DEFAULT_2500	E1000_ALL_SPEED_DUPLEX_2500

/* Interrupt Cause Read */
#define E1000_ICR_TXDW		0x00000001 /* Transmit desc written back */
#define E1000_ICR_TXQE		0x00000002 /* Transmit Queue empty */
#define E1000_ICR_LSC		0x00000004 /* Link Status Change */
#define E1000_ICR_RXSEQ		0x00000008 /* Rx sequence error */
#define E1000_ICR_RXDMT0	0x00000010 /* Rx desc min. threshold (0) */
#define E1000_ICR_RXO		0x00000040 /* Rx overrun */
#define E1000_ICR_RXT0		0x00000080 /* Rx timer intr (ring 0) */
#define E1000_ICR_DRSTA		0x40000000 /* Device Reset Asserted */

/* If this bit asserted, the driver should claim the interrupt */
#define E1000_ICR_INT_ASSERTED	0x80000000

#define E1000_ICS_RXT0		E1000_ICR_RXT0      /* Rx timer intr */

#define IMS_ENABLE_MASK ( \
	E1000_IMS_RXT0   |    \
	E1000_IMS_TXDW   |    \
	E1000_IMS_RXDMT0 |    \
	E1000_IMS_RXSEQ  |    \
	E1000_IMS_LSC)

/* Interrupt Mask Set */
#define E1000_IMS_TXDW		E1000_ICR_TXDW    /* Tx desc written back */
#define E1000_IMS_RXSEQ		E1000_ICR_RXSEQ   /* Rx sequence error */
#define E1000_IMS_LSC		E1000_ICR_LSC	/* Link Status Change */
#define E1000_IMS_DOUTSYNC	E1000_ICR_DOUTSYNC /* NIC DMA out of sync */
#define E1000_IMS_DRSTA		E1000_ICR_DRSTA   /* Device Reset Asserted */
#define E1000_IMS_RXT0		E1000_ICR_RXT0    /* Rx timer intr */
#define E1000_IMS_RXDMT0	E1000_ICR_RXDMT0  /* Rx desc min. threshold */

/* Interrupt Cause Set */
#define E1000_ICS_LSC		E1000_ICR_LSC       /* Link Status Change */
#define E1000_ICS_RXDMT0	E1000_ICR_RXDMT0    /* rx desc min. threshold */
#define E1000_ICS_DRSTA		E1000_ICR_DRSTA     /* Device Reset Aserted */

#define E1000_ICR_DOUTSYNC	0x10000000 /* NIC DMA out of sync */
#define E1000_EITR_CNT_IGNR	0x80000000 /* Don't reset counters on write */
#define E1000_IVAR_VALID	0x80
#define E1000_GPIE_NSICR	0x00000001
#define E1000_GPIE_MSIX_MODE	0x00000010
#define E1000_GPIE_EIAME	0x40000000
#define E1000_GPIE_PBA		0x80000000

/* Transmit Descriptor bit definitions */
#define E1000_TXD_DTYP_D	0x00100000 /* Data Descriptor */
#define E1000_TXD_DTYP_C	0x00000000 /* Context Descriptor */
#define E1000_TXD_POPTS_IXSM	0x01       /* Insert IP checksum */
#define E1000_TXD_POPTS_TXSM	0x02       /* Insert TCP/UDP checksum */
#define E1000_TXD_CMD_EOP	0x01000000 /* End of Packet */
#define E1000_TXD_CMD_IFCS	0x02000000 /* Insert FCS (Ethernet CRC) */
#define E1000_TXD_CMD_IC	0x04000000 /* Insert Checksum */
#define E1000_TXD_CMD_RS	0x08000000 /* Report Status */
#define E1000_TXD_CMD_RPS	0x10000000 /* Report Packet Sent */
#define E1000_TXD_CMD_DEXT	0x20000000 /* Desc extension (0 = legacy) */
#define E1000_TXD_CMD_VLE	0x40000000 /* Add VLAN tag */
#define E1000_TXD_CMD_IDE	0x80000000 /* Enable Tidv register */
#define E1000_TXD_STAT_DD	0x00000001 /* Descriptor Done */
#define E1000_TXD_STAT_EC	0x00000002 /* Excess Collisions */
#define E1000_TXD_STAT_LC	0x00000004 /* Late Collisions */
#define E1000_TXD_STAT_TU	0x00000008 /* Transmit underrun */
#define E1000_TXD_CMD_TCP	0x01000000 /* TCP packet */
#define E1000_TXD_CMD_IP	0x02000000 /* IP packet */
#define E1000_TXD_CMD_TSE	0x04000000 /* TCP Seg enable */
#define E1000_TXD_STAT_TC	0x00000004 /* Tx Underrun */
#define E1000_TXD_EXTCMD_TSTAMP	0x00000010 /* IEEE1588 Timestamp packet */

/* Transmit Control */
#define E1000_TCTL_EN		0x00000002 /* enable Tx */
#define E1000_TCTL_PSP		0x00000008 /* pad short packets */
#define E1000_TCTL_CT		0x00000ff0 /* collision threshold */
#define E1000_TCTL_COLD		0x003ff000 /* collision distance */
#define E1000_TCTL_RTLC		0x01000000 /* Re-transmit on late collision */
#define E1000_TCTL_MULR		0x10000000 /* Multiple request support */

#define E1000_CT_SHIFT			4
#define E1000_COLLISION_THRESHOLD	15

/* Flow Control Constants */
#define FLOW_CONTROL_ADDRESS_LOW	0x00C28001
#define FLOW_CONTROL_ADDRESS_HIGH	0x00000100
#define FLOW_CONTROL_TYPE		0x8808
/* Enable XON frame transmission */
#define E1000_FCRTL_XONE		0x80000000

/* Management Control */
#define E1000_MANC_RCV_TCO_EN		0x00020000 /* Receive TCO Enabled */
#define E1000_MANC_BLK_PHY_RST_ON_IDE	0x00040000 /* Block phy resets */

/* Receive Control */
#define E1000_RCTL_RST		0x00000001 /* Software reset */
#define E1000_RCTL_EN		0x00000002 /* enable */
#define E1000_RCTL_SBP		0x00000004 /* store bad packet */
#define E1000_RCTL_UPE		0x00000008 /* unicast promisc enable */
#define E1000_RCTL_MPE		0x00000010 /* multicast promisc enable */
#define E1000_RCTL_LPE		0x00000020 /* long packet enable */
#define E1000_RCTL_LBM_NO	0x00000000 /* no loopback mode */
#define E1000_RCTL_LBM_MAC	0x00000040 /* MAC loopback mode */
#define E1000_RCTL_LBM_TCVR	0x000000C0 /* tcvr loopback mode */
#define E1000_RCTL_DTYP_PS	0x00000400 /* Packet Split descriptor */

#define E1000_RCTL_RDMTS_HALF	0x00000000 /* Rx desc min thresh size */
#define E1000_RCTL_BAM		0x00008000 /* broadcast enable */

/* Receive Descriptor bit definitions */
#define E1000_RXD_STAT_DD	0x01    /* Descriptor Done */
#define E1000_RXD_STAT_EOP	0x02    /* End of Packet */
#define E1000_RXD_STAT_IXSM	0x04    /* Ignore checksum */
#define E1000_RXD_STAT_VP	0x08    /* IEEE VLAN Packet */
#define E1000_RXD_STAT_UDPCS	0x10    /* UDP xsum calculated */
#define E1000_RXD_STAT_TCPCS	0x20    /* TCP xsum calculated */
#define E1000_RXD_STAT_TS	0x10000 /* Pkt was time stamped */

#define E1000_RXDEXT_STATERR_LB		0x00040000
#define E1000_RXDEXT_STATERR_CE		0x01000000
#define E1000_RXDEXT_STATERR_SE		0x02000000
#define E1000_RXDEXT_STATERR_SEQ	0x04000000
#define E1000_RXDEXT_STATERR_CXE	0x10000000
#define E1000_RXDEXT_STATERR_TCPE	0x20000000
#define E1000_RXDEXT_STATERR_IPE	0x40000000
#define E1000_RXDEXT_STATERR_RXE	0x80000000

/* Same mask, but for extended and packet split descriptors */
#define E1000_RXDEXT_ERR_FRAME_ERR_MASK ( \
	E1000_RXDEXT_STATERR_CE  |            \
	E1000_RXDEXT_STATERR_SE  |            \
	E1000_RXDEXT_STATERR_SEQ |            \
	E1000_RXDEXT_STATERR_CXE |            \
	E1000_RXDEXT_STATERR_RXE)

/* Header split receive */
#define E1000_RFCTL_IPV6_EX_DIS	0x00010000
#define E1000_RFCTL_LEF		0x00040000

#define I225_RXPBSIZE_DEFAULT	0x000000A2 /* RXPBSIZE default */
#define I225_TXPBSIZE_DEFAULT	0x04000014 /* TXPBSIZE default */

/* these buffer sizes are valid if E1000_RCTL_BSEX is 0 */
#define E1000_RCTL_SZ_2048	0x00000000 /* Rx buffer size 2048 */
#define E1000_RCTL_SZ_1024	0x00010000 /* Rx buffer size 1024 */
#define E1000_RCTL_SZ_512	0x00020000 /* Rx buffer size 512 */
#define E1000_RCTL_SZ_256	0x00030000 /* Rx buffer size 256 */
/* these buffer sizes are valid if E1000_RCTL_BSEX is 1 */
#define E1000_RCTL_SZ_16384	0x00010000 /* Rx buffer size 16384 */
#define E1000_RCTL_SZ_8192	0x00020000 /* Rx buffer size 8192 */
#define E1000_RCTL_SZ_4096	0x00030000 /* Rx buffer size 4096 */

#define E1000_RCTL_MO_SHIFT	12 /* multicast offset shift */
#define E1000_RCTL_CFIEN	0x00080000 /* canonical form enable */
#define E1000_RCTL_DPF		0x00400000 /* discard pause frames */
#define E1000_RCTL_PMCF		0x00800000 /* pass MAC control frames */
#define E1000_RCTL_SECRC	0x04000000 /* Strip Ethernet CRC */

/* GPY211 - I225 defines */
#define GPY_MMD_MASK			0xFFFF0000
#define GPY_MMD_SHIFT			16
#define GPY_REG_MASK			0x0000FFFF

#define E1000_MMDAC_FUNC_DATA	0x4000 /* Data, no post increment */

/* MAC definitions */
#define E1000_FACTPS_MNGCG		0x20000000
#define E1000_FWSM_MODE_MASK		0xE
#define E1000_FWSM_MODE_SHIFT		1

/* Management Control */
#define E1000_MANC_SMBUS_EN	0x00000001 /* SMBus Enabled - RO */
#define E1000_MANC_ASF_EN	0x00000002 /* ASF Enabled - RO */

/* PHY */
#define PHY_REVISION_MASK	0xFFFFFFF0
#define MAX_PHY_REG_ADDRESS	0x1F  /* 5 bit address bus (0-0x1F) */
#define E1000_GEN_POLL_TIMEOUT	640

/* PHY Control Register */
#define MII_CR_FULL_DUPLEX	0x0100  /* FDX =1, half duplex =0 */
#define MII_CR_RESTART_AUTO_NEG	0x0200  /* Restart auto negotiation */
#define MII_CR_POWER_DOWN	0x0800  /* Power down */
#define MII_CR_AUTO_NEG_EN	0x1000  /* Auto Neg Enable */
#define MII_CR_LOOPBACK		0x4000  /* 0 = normal, 1 = loopback */
#define MII_CR_RESET		0x8000  /* 0 = normal, 1 = PHY reset */
#define MII_CR_SPEED_1000	0x0040
#define MII_CR_SPEED_100	0x2000
#define MII_CR_SPEED_10		0x0000

/* PHY Status Register */
#define MII_SR_LINK_STATUS	0x0004 /* Link Status 1 = link */
#define MII_SR_AUTONEG_COMPLETE	0x0020 /* Auto Neg Complete */

/* PHY 1000 MII Register/Bit Definitions */
/* PHY Registers defined by IEEE */
#define PHY_CONTROL	0x00 /* Control Register */
#define PHY_STATUS	0x01 /* Status Register */
#define PHY_ID1		0x02 /* Phy Id Reg (word 1) */
#define PHY_ID2		0x03 /* Phy Id Reg (word 2) */

/* Bit definitions for valid PHY IDs. I = Integrated E = External */
#define I225_I_PHY_ID		0x67C9DC00

/* MDI Control */
#define E1000_MDIC_DATA_MASK	0x0000FFFF
#define E1000_MDIC_REG_MASK	0x001F0000
#define E1000_MDIC_REG_SHIFT	16
#define E1000_MDIC_PHY_MASK	0x03E00000
#define E1000_MDIC_PHY_SHIFT	21
#define E1000_MDIC_OP_WRITE	0x04000000
#define E1000_MDIC_OP_READ	0x08000000
#define E1000_MDIC_READY	0x10000000
#define E1000_MDIC_INT_EN	0x20000000
#define E1000_MDIC_ERROR	0x40000000
#define E1000_MDIC_DEST		0x80000000

#endif /* _E1000_DEFINES_H_ */
