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

/* Receive Address
 * Number of high/low register pairs in the RAR. The RAR (Receive Address
 * Registers) holds the directed and multicast addresses that we monitor.
 * Technically, we have 16 spots.  However, we reserve one of these spots
 * (RAR[15]) for our directed address used by controllers with
 * manageability enabled, allowing us room for 15 multicast addresses.
 */
#define E1000_RAH_AV		0x80000000 /* Receive descriptor valid */
#define E1000_RAH_POOL_1	0x00040000

/* Error Codes */
#define E1000_SUCCESS				0
#define E1000_ERR_NVM				1
#define E1000_ERR_PHY				2
#define E1000_ERR_CONFIG			3
#define E1000_ERR_PARAM				4
#define E1000_ERR_MAC_INIT			5
#define E1000_ERR_RESET				9

/* PBA constants */
#define E1000_PBA_34K			0x0022

/* Device Status */
#define E1000_STATUS_FD		0x00000001      /* Full duplex.0=half,1=full */
#define E1000_STATUS_LU		0x00000002      /* Link up.0=no,1=link */
#define E1000_STATUS_FUNC_MASK	0x0000000C      /* PCI Function Mask */
#define E1000_STATUS_FUNC_SHIFT	2
#define E1000_STATUS_FUNC_1	0x00000004      /* Function 1 */
#define E1000_STATUS_TXOFF	0x00000010      /* transmission paused */
#define E1000_STATUS_SPEED_100	0x00000040      /* Speed 100Mb/s */
#define E1000_STATUS_SPEED_1000	0x00000080      /* Speed 1000Mb/s */

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

/* Management Control */
#define E1000_MANC_RCV_TCO_EN	0x00020000 /* Receive TCO Packets Enabled */

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

#endif /* _E1000_DEFINES_H_ */
