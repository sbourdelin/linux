/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c)  2018 Intel Corporation */

#ifndef _E1000_REGS_H_
#define _E1000_REGS_H_

/* General Register Descriptions */
#define E1000_CTRL		0x00000  /* Device Control - RW */
#define E1000_STATUS		0x00008  /* Device Status - RO */
#define E1000_CTRL_EXT		0x00018  /* Extended Device Control - RW */
#define E1000_MDIC		0x00020  /* MDI Control - RW */
#define E1000_MDICNFG		0x00E04  /* MDC/MDIO Configuration - RW */
#define E1000_PHYSCTRL		0x00E08  /* PHY SERDES control - RW */
#define E1000_CONNSW		0x00034  /* Copper/Fiber switch control - RW */
#define E1000_VET		0x00038  /* VLAN Ether Type - RW */
#define E1000_UFUSE		0x05B78  /* FUSE register - RO */
#define E1000_LEDCTL		0x00E00  /* LED Control - RW */

/* Internal Packet Buffer Size Registers */
#define E1000_RXPBS		0x02404  /* Rx Packet Buffer Size - RW */
#define E1000_TXPBS		0x03404  /* Tx Packet Buffer Size - RW */

/* NVM  Register Descriptions */
#define E1000_EEC		0x12010  /* EEprom Mode control - RW */
#define E1000_EELOADCTL		0x12020  /* EEprom Mode load control - RO */
#define E1000_EERD		0x12014  /* EEprom mode read - RW */
#define E1000_EELOADCRC		0x00001  /* EEprom load CRC error - RO */
#define E1000_EEWR		0x12018  /* EEprom mode write - RW */
#define E1000_FLA		0x1201C  /* Flash access - RW */
#define E1000_FL_SECU		0x12114  /* Flash security - RO to host */

/* Flow Control Register Descriptions */
#define E1000_FCAL		0x00028  /* FC Address Low - RW */
#define E1000_FCAH		0x0002C  /* FC Address High - RW */
#define E1000_FCT		0x00030  /* FC Type - RW */
#define E1000_FCTTV		0x00170  /* FC Transmit Timer - RW */
#define E1000_FCRTL		0x02160  /* FC Receive Threshold Low - RW */
#define E1000_FCRTH		0x02168  /* FC Receive Threshold High - RW */
#define E1000_FCRTV		0x02460  /* FC Refresh Timer Value - RW */
#define E1000_FCSTS		0x02464  /* FC Status - RO */

/* PCIe Register Description */
#define E1000_GCR		0x05B00  /* PCIe control- RW */
#define E1000_GSCL_1		0x05B10  /* PCIe statistics control 1 - RW */
#define E1000_GSCL_2		0x05B14  /* PCIe statistics control 2 - RW */
#define E1000_BARCTRL		0x05BFC  /* PCIe BAR ctrl reg */
#define E1000_PCIE_ANA_AD	0x05BF8  /* PCIe PHY analog address data - RW */
#define E1000_PCIEPHYADR	0x05B40  /* PCIE PHY address - RW */
#define E1000_PCIEPHYDAT	0x05B44  /* PCIE PHY data - RW */
#define E1000_PPHY_CTL		0x05B48  /* PCIE PHY control - RW */

/* Semaphore registers */
#define E1000_SW_FW_SYNC	0x05B5C  /* SW-FW Synchronization - RW */
#define E1000_SWSM		0x05B50  /* SW Semaphore */
#define E1000_FWSM		0x05B54  /* FW Semaphore */

/* Interrupt Register Description */
#define E1000_PICAUSE		0x05B88  /* PCIe Interrupt Cause - RW1/C */
#define E1000_PIENA		0x05B8C  /* PCIe Interrupt enable - RW */
#define E1000_EICR		0x01580  /* PCIe Ex Interrupt Cause - RC/W1C */
#define E1000_EICS		0x01520  /* Ext. Interrupt Cause Set - W0 */
#define E1000_EIMS		0x01524  /* Ext. Interrupt Mask Set/Read - RW */
#define E1000_EIMC		0x01528  /* Ext. Interrupt Mask Clear - WO */
#define E1000_EIAC		0x0152C  /* Ext. Interrupt Auto Clear - RW */
#define E1000_EIAM		0x01530  /* Ext. Interrupt Auto Mask - RW */
#define E1000_ICR		0x01500  /* Intr Cause Read - RC/W1C */
#define E1000_ICS		0x01504  /* Intr Cause Set - WO */
#define E1000_IMS		0x01508  /* Intr Mask Set/Read - RW */
#define E1000_IMC		0x0150C  /* Intr Mask Clear - WO */
#define E1000_IAM		0x01510  /* Intr Ack Auto Mask- RW */
/* Intr Throttle - RW */
#define E1000_EITR(_n)		(0x01680 + (0x4 * (_n)))
/* Interrupt Vector Allocation (array) - RW */
#define E1000_IVAR(_n)		(0x01700 + (0x4 * (_n)))
#define E1000_IVAR_MISC		0x01740   /* IVAR for "other" causes - RW */
#define E1000_GPIE		0x01514  /* General Purpose Intr Enable - RW */

/* MSI-X Table Register Descriptions */
#define E1000_PBACL		0x05B68  /* MSIx PBA Clear - R/W 1 to clear */

/* Receive Register Descriptions */
#define E1000_RCTL		0x00100  /* Rx Control - RW */
#define E1000_SRRCTL(_n)	(0x0C00C + ((_n) * 0x40))
#define E1000_PSRTYPE(_i)	(0x05480 + ((_i) * 4))
#define E1000_RDBAL(_n)		(0x0C000 + ((_n) * 0x40))
#define E1000_RDBAH(_n)		(0x0C004 + ((_n) * 0x40))
#define E1000_RDLEN(_n)		(0x0C008 + ((_n) * 0x40))
#define E1000_RDH(_n)		(0x0C010 + ((_n) * 0x40))
#define E1000_RDT(_n)		(0x0C018 + ((_n) * 0x40))
#define E1000_RXDCTL(_n)	(0x0C028 + ((_n) * 0x40))
#define E1000_RQDPC(_n)		(0x0C030 + ((_n) * 0x40))
#define E1000_RXCSUM		0x05000  /* Rx Checksum Control - RW */
#define E1000_RLPML		0x05004  /* Rx Long Packet Max Length */
#define E1000_RFCTL		0x05008  /* Receive Filter Control*/
#define E1000_RAL(_n)		(0x05400 + ((_n) * 0x08))
#define E1000_RAH(_n)		(0x05404 + ((_n) * 0x08))
#define E1000_VLAPQF		0x055B0  /* VLAN Priority Queuue - RW */
#define E1000_VFTA		0x05600  /* VLAN Filter Table Array - RW */
#define E1000_MRQC		0x05818  /* Multiple Receive Control - RW */
#define E1000_RSSRK(_i)		(0x05C80 + ((_i) * 4)) /* RSS Rand Key - RW */
#define E1000_RETA(_i)		(0x05C00 + ((_i) * 4)) /* Redirection - RW */
#define E1000_DVMOLR(_n)	(0x0C038 + (0x40 * (_n))) /* DMA VM offload */
#define E1000_DRXMXOD		0x02540  /* DMA Rx max outstanding data - RW */
#define E1000_IMIR(_i)		(0x05A80 + ((_i) * 4))  /* Immediate Intr */
#define E1000_IMIREXT(_i)	(0x05AA0 + ((_i) * 4)) /* Immediate INTR Ext*/
#define E1000_TTQF(_n)		(0x059E0 + (4 * (_n))) /* 2-tuple Queue Fltr */
#define E1000_IMIRVP		0x05AC0 /* Immediate INT Rx VLAN Priority-RW */
#define E1000_SYNQF(_n)		(0x055FC + (4 * (_n))) /* SYN Pack Queue Ftr */
#define E1000_ETQF(_n)		(0x05CB0 + (4 * (_n))) /* EType Queue Fltr */

/* Transmit Register Descriptions */
#define E1000_TCTL		0x00400  /* Tx Control - RW */
#define E1000_TCTL_EXT		0x00404  /* Extended Tx Control - RW */
#define E1000_TIPG		0x00410  /* Tx Inter-packet gap - RW */
#define E1000_REXT_CTL		0x0041C  /* Retry buffer control -  RW */
#define E1000_DTXCTL		0x03590  /* DMA Tx Control - RW */
#define E1000_DTXBCTL		0x035A4  /* DMA Tx behaviour control - RW */
#define E1000_DTXPARSE		0x0350C  /* DMA parsing control - RW */
#define E1000_DTXTCPFLGL	0x0359C  /* DMA Tx Control flag low - RW */
#define E1000_DTXTCPFLGH	0x035A0  /* DMA Tx Control flag high - RW */
#define E1000_DTXMXSZRQ		0x03540  /* DMA Tx max total allow size req */
#define E1000_DTXMXPKTSZ	0x0355C  /* DMA Tx max packet size i- RW */
#define E1000_TQDPC		0x0E030  /* Tx queue drop packet count - RW */
#define E1000_TDBAL(_n)		(0x0E000 + ((_n) * 0x40))
#define E1000_TDBAH(_n)		(0x0E004 + ((_n) * 0x40))
#define E1000_TDLEN(_n)		(0x0E008 + ((_n) * 0x40))
#define E1000_TDH(_n)		(0x0E010 + ((_n) * 0x40))
#define E1000_TDT(_n)		(0x0E018 + ((_n) * 0x40))
#define E1000_TXDCTL(_n)	(0x0E028 + ((_n) * 0x40))
#define E1000_TDWBAL(_n)	(0x0E038 + ((_n) * 0x40))
#define E1000_TDWBAH(_n)	(0x0E03C + ((_n) * 0x40))

/* MMD Registers Descriptions */
#define E1000_MMDAC			13 /* MMD Access Control */
#define E1000_MMDAAD			14 /* MMD Access Address/Data */

/* Good transmitted packets counter registers */
#define E1000_PQGPTC(_n)		(0x010014 + (0x100 * (_n)))

/* Statistics Register Descriptions */
#define E1000_CRCERRS	0x04000  /* CRC Error Count - R/clr */
#define E1000_ALGNERRC	0x04004  /* Alignment Error Count - R/clr */
#define E1000_SYMERRS	0x04008  /* Symbol Error Count - R/clr */
#define E1000_RXERRC	0x0400C  /* Receive Error Count - R/clr */
#define E1000_MPC	0x04010  /* Missed Packet Count - R/clr */
#define E1000_SCC	0x04014  /* Single Collision Count - R/clr */
#define E1000_ECOL	0x04018  /* Excessive Collision Count - R/clr */
#define E1000_MCC	0x0401C  /* Multiple Collision Count - R/clr */
#define E1000_LATECOL	0x04020  /* Late Collision Count - R/clr */
#define E1000_COLC	0x04028  /* Collision Count - R/clr */
#define E1000_DC	0x04030  /* Defer Count - R/clr */
#define E1000_TNCRS	0x04034  /* Tx-No CRS - R/clr */
#define E1000_SEC	0x04038  /* Sequence Error Count - R/clr */
#define E1000_CEXTERR	0x0403C  /* Carrier Extension Error Count - R/clr */
#define E1000_RLEC	0x04040  /* Receive Length Error Count - R/clr */
#define E1000_XONRXC	0x04048  /* XON Rx Count - R/clr */
#define E1000_XONTXC	0x0404C  /* XON Tx Count - R/clr */
#define E1000_XOFFRXC	0x04050  /* XOFF Rx Count - R/clr */
#define E1000_XOFFTXC	0x04054  /* XOFF Tx Count - R/clr */
#define E1000_FCRUC	0x04058  /* Flow Control Rx Unsupported Count- R/clr */
#define E1000_PRC64	0x0405C  /* Packets Rx (64 bytes) - R/clr */
#define E1000_PRC127	0x04060  /* Packets Rx (65-127 bytes) - R/clr */
#define E1000_PRC255	0x04064  /* Packets Rx (128-255 bytes) - R/clr */
#define E1000_PRC511	0x04068  /* Packets Rx (255-511 bytes) - R/clr */
#define E1000_PRC1023	0x0406C  /* Packets Rx (512-1023 bytes) - R/clr */
#define E1000_PRC1522	0x04070  /* Packets Rx (1024-1522 bytes) - R/clr */
#define E1000_GPRC	0x04074  /* Good Packets Rx Count - R/clr */
#define E1000_BPRC	0x04078  /* Broadcast Packets Rx Count - R/clr */
#define E1000_MPRC	0x0407C  /* Multicast Packets Rx Count - R/clr */
#define E1000_GPTC	0x04080  /* Good Packets Tx Count - R/clr */
#define E1000_GORCL	0x04088  /* Good Octets Rx Count Low - R/clr */
#define E1000_GORCH	0x0408C  /* Good Octets Rx Count High - R/clr */
#define E1000_GOTCL	0x04090  /* Good Octets Tx Count Low - R/clr */
#define E1000_GOTCH	0x04094  /* Good Octets Tx Count High - R/clr */
#define E1000_RNBC	0x040A0  /* Rx No Buffers Count - R/clr */
#define E1000_RUC	0x040A4  /* Rx Undersize Count - R/clr */
#define E1000_RFC	0x040A8  /* Rx Fragment Count - R/clr */
#define E1000_ROC	0x040AC  /* Rx Oversize Count - R/clr */
#define E1000_RJC	0x040B0  /* Rx Jabber Count - R/clr */
#define E1000_MGTPRC	0x040B4  /* Management Packets Rx Count - R/clr */
#define E1000_MGTPDC	0x040B8  /* Management Packets Dropped Count - R/clr */
#define E1000_MGTPTC	0x040BC  /* Management Packets Tx Count - R/clr */
#define E1000_TORL	0x040C0  /* Total Octets Rx Low - R/clr */
#define E1000_TORH	0x040C4  /* Total Octets Rx High - R/clr */
#define E1000_TOTL	0x040C8  /* Total Octets Tx Low - R/clr */
#define E1000_TOTH	0x040CC  /* Total Octets Tx High - R/clr */
#define E1000_TPR	0x040D0  /* Total Packets Rx - R/clr */
#define E1000_TPT	0x040D4  /* Total Packets Tx - R/clr */
#define E1000_PTC64	0x040D8  /* Packets Tx (64 bytes) - R/clr */
#define E1000_PTC127	0x040DC  /* Packets Tx (65-127 bytes) - R/clr */
#define E1000_PTC255	0x040E0  /* Packets Tx (128-255 bytes) - R/clr */
#define E1000_PTC511	0x040E4  /* Packets Tx (256-511 bytes) - R/clr */
#define E1000_PTC1023	0x040E8  /* Packets Tx (512-1023 bytes) - R/clr */
#define E1000_PTC1522	0x040EC  /* Packets Tx (1024-1522 Bytes) - R/clr */
#define E1000_MPTC	0x040F0  /* Multicast Packets Tx Count - R/clr */
#define E1000_BPTC	0x040F4  /* Broadcast Packets Tx Count - R/clr */
#define E1000_TSCTC	0x040F8  /* TCP Segmentation Context Tx - R/clr */
#define E1000_TSCTFC	0x040FC  /* TCP Segmentation Context Tx Fail - R/clr */
#define E1000_IAC	0x04100  /* Interrupt Assertion Count */
#define E1000_ICTXPTC	0x0410C  /* Interrupt Cause Tx Pkt Timer Expire Count */
#define E1000_ICTXATC	0x04110  /* Interrupt Cause Tx Abs Timer Expire Count */
#define E1000_ICTXQEC	0x04118  /* Interrupt Cause Tx Queue Empty Count */
#define E1000_ICTXQMTC	0x0411C  /* Interrupt Cause Tx Queue Min Thresh Count */
#define E1000_RPTHC	0x04104  /* Rx Packets To Host */
#define E1000_HGPTC	0x04118  /* Host Good Packets Tx Count */
#define E1000_RXDMTC	0x04120  /* Rx Descriptor Minimum Threshold Count */
#define E1000_HGORCL	0x04128  /* Host Good Octets Received Count Low */
#define E1000_HGORCH	0x0412C  /* Host Good Octets Received Count High */
#define E1000_HGOTCL	0x04130  /* Host Good Octets Transmit Count Low */
#define E1000_HGOTCH	0x04134  /* Host Good Octets Transmit Count High */
#define E1000_LENERRS	0x04138  /* Length Errors Count */
#define E1000_SCVPC	0x04228  /* SerDes/SGMII Code Violation Pkt Count */
#define E1000_HRMPC	0x0A018  /* Header Redirection Missed Packet Count */

/* DMA Coalescing registers */
#define E1000_DMACR	0x02508 /* Control Register */
#define E1000_DMCTXTH	0x03550 /* Transmit Threshold */
#define E1000_DMCTLX	0x02514 /* Time to Lx Request */
#define E1000_DMCRTRH	0x05DD0 /* Receive Packet Rate Threshold */
#define E1000_DMCCNT	0x05DD4 /* Current Rx Count */
#define E1000_FCRTC	0x02170 /* Flow Control Rx high watermark */
#define E1000_PCIEMISC	0x05BB8 /* PCIE misc config register */

/* Energy Efficient Ethernet "EEE" registers */
#define E1000_IPCNFG	0x0E38 /* Internal PHY Configuration */
#define E1000_LTRC	0x01A0 /* Latency Tolerance Reporting Control */
#define E1000_EEER	0x0E30 /* Energy Efficient Ethernet "EEE"*/
#define E1000_EEE_SU	0x0E34 /* EEE Setup */
#define E1000_TLPIC	0x4148 /* EEE Tx LPI Count - TLPIC */
#define E1000_RLPIC	0x414C /* EEE Rx LPI Count - RLPIC */

/* forward declaration */
struct e1000_hw;
u32 igc_rd32(struct e1000_hw *hw, u32 reg);

/* write operations, indexed using DWORDS */
#define wr32(reg, val) \
do { \
	u8 __iomem *hw_addr = READ_ONCE((hw)->hw_addr); \
	if (!E1000_REMOVED(hw_addr)) \
		writel((val), &hw_addr[(reg)]); \
} while (0)

#define rd32(reg) (igc_rd32(hw, reg))

#define wrfl() ((void)rd32(E1000_STATUS))

#define array_wr32(reg, offset, value) \
	wr32((reg) + ((offset) << 2), (value))

#define array_rd32(reg, offset) (igc_rd32(hw, (reg) + ((offset) << 2)))

#endif
