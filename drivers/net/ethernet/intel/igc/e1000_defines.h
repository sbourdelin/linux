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

#endif /* _E1000_DEFINES_H_ */
