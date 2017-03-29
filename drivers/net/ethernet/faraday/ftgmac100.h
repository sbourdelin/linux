/*
 * Faraday FTGMAC100 Gigabit Ethernet
 *
 * (C) Copyright 2009-2011 Faraday Technology
 * Po-Yu Chuang <ratbert@faraday-tech.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef __FTGMAC100_H
#define __FTGMAC100_H

#define FTGMAC100_OFFSET_ISR		0x00
#define FTGMAC100_OFFSET_IER		0x04
#define FTGMAC100_OFFSET_MAC_MADR	0x08
#define FTGMAC100_OFFSET_MAC_LADR	0x0c
#define FTGMAC100_OFFSET_MAHT0		0x10
#define FTGMAC100_OFFSET_MAHT1		0x14
#define FTGMAC100_OFFSET_NPTXPD		0x18
#define FTGMAC100_OFFSET_RXPD		0x1c
#define FTGMAC100_OFFSET_NPTXR_BADR	0x20
#define FTGMAC100_OFFSET_RXR_BADR	0x24
#define FTGMAC100_OFFSET_HPTXPD		0x28
#define FTGMAC100_OFFSET_HPTXR_BADR	0x2c
#define FTGMAC100_OFFSET_ITC		0x30
#define FTGMAC100_OFFSET_APTC		0x34
#define FTGMAC100_OFFSET_DBLAC		0x38
#define FTGMAC100_OFFSET_DMAFIFOS	0x3c
#define FTGMAC100_OFFSET_REVR		0x40
#define FTGMAC100_OFFSET_FEAR		0x44
#define FTGMAC100_OFFSET_TPAFCR		0x48
#define FTGMAC100_OFFSET_RBSR		0x4c
#define FTGMAC100_OFFSET_MACCR		0x50
#define FTGMAC100_OFFSET_MACSR		0x54
#define FTGMAC100_OFFSET_TM		0x58
#define FTGMAC100_OFFSET_PHYCR		0x60
#define FTGMAC100_OFFSET_PHYDATA	0x64
#define FTGMAC100_OFFSET_FCR		0x68
#define FTGMAC100_OFFSET_BPR		0x6c
#define FTGMAC100_OFFSET_WOLCR		0x70
#define FTGMAC100_OFFSET_WOLSR		0x74
#define FTGMAC100_OFFSET_WFCRC		0x78
#define FTGMAC100_OFFSET_WFBM1		0x80
#define FTGMAC100_OFFSET_WFBM2		0x84
#define FTGMAC100_OFFSET_WFBM3		0x88
#define FTGMAC100_OFFSET_WFBM4		0x8c
#define FTGMAC100_OFFSET_NPTXR_PTR	0x90
#define FTGMAC100_OFFSET_HPTXR_PTR	0x94
#define FTGMAC100_OFFSET_RXR_PTR	0x98
#define FTGMAC100_OFFSET_TX		0xa0
#define FTGMAC100_OFFSET_TX_MCOL_SCOL	0xa4
#define FTGMAC100_OFFSET_TX_ECOL_FAIL	0xa8
#define FTGMAC100_OFFSET_TX_LCOL_UND	0xac
#define FTGMAC100_OFFSET_RX		0xb0
#define FTGMAC100_OFFSET_RX_BC		0xb4
#define FTGMAC100_OFFSET_RX_MC		0xb8
#define FTGMAC100_OFFSET_RX_PF_AEP	0xbc
#define FTGMAC100_OFFSET_RX_RUNT	0xc0
#define FTGMAC100_OFFSET_RX_CRCER_FTL	0xc4
#define FTGMAC100_OFFSET_RX_COL_LOST	0xc8

/*
 * Interrupt status register & interrupt enable register
 */
#define FTGMAC100_INT_RPKT_BUF		(1 << 0)
#define FTGMAC100_INT_RPKT_FIFO		(1 << 1)
#define FTGMAC100_INT_NO_RXBUF		(1 << 2)
#define FTGMAC100_INT_RPKT_LOST		(1 << 3)
#define FTGMAC100_INT_XPKT_ETH		(1 << 4)
#define FTGMAC100_INT_XPKT_FIFO		(1 << 5)
#define FTGMAC100_INT_NO_NPTXBUF	(1 << 6)
#define FTGMAC100_INT_XPKT_LOST		(1 << 7)
#define FTGMAC100_INT_AHB_ERR		(1 << 8)
#define FTGMAC100_INT_PHYSTS_CHG	(1 << 9)
#define FTGMAC100_INT_NO_HPTXBUF	(1 << 10)

/* These are all the interrupts we care about */
#define FTGMAC100_INT_ALL (FTGMAC100_INT_RPKT_LOST |	\
			   FTGMAC100_INT_XPKT_ETH |	\
			   FTGMAC100_INT_XPKT_LOST |	\
			   FTGMAC100_INT_AHB_ERR |	\
			   FTGMAC100_INT_RPKT_BUF |	\
			   FTGMAC100_INT_NO_RXBUF)

/* These are the interrupts we care about in NAPI mode */
#define FTGMAC100_INT_BAD (FTGMAC100_INT_RPKT_LOST |	\
			   FTGMAC100_INT_AHB_ERR |	\
			   FTGMAC100_INT_NO_RXBUF)

/*
 * Interrupt timer control register
 */
#define FTGMAC100_ITC_RXINT_CNT(x)	(((x) & 0xf) << 0)
#define FTGMAC100_ITC_RXINT_THR(x)	(((x) & 0x7) << 4)
#define FTGMAC100_ITC_RXINT_TIME_SEL	(1 << 7)
#define FTGMAC100_ITC_TXINT_CNT(x)	(((x) & 0xf) << 8)
#define FTGMAC100_ITC_TXINT_THR(x)	(((x) & 0x7) << 12)
#define FTGMAC100_ITC_TXINT_TIME_SEL	(1 << 15)

/*
 * Automatic polling timer control register
 */
#define FTGMAC100_APTC_RXPOLL_CNT(x)	(((x) & 0xf) << 0)
#define FTGMAC100_APTC_RXPOLL_TIME_SEL	(1 << 4)
#define FTGMAC100_APTC_TXPOLL_CNT(x)	(((x) & 0xf) << 8)
#define FTGMAC100_APTC_TXPOLL_TIME_SEL	(1 << 12)

/*
 * DMA burst length and arbitration control register
 */
#define FTGMAC100_DBLAC_RXFIFO_LTHR(x)	(((x) & 0x7) << 0)
#define FTGMAC100_DBLAC_RXFIFO_HTHR(x)	(((x) & 0x7) << 3)
#define FTGMAC100_DBLAC_RX_THR_EN	(1 << 6)
#define FTGMAC100_DBLAC_RXBURST_SIZE(x)	(((x) & 0x3) << 8)
#define FTGMAC100_DBLAC_TXBURST_SIZE(x)	(((x) & 0x3) << 10)
#define FTGMAC100_DBLAC_RXDES_SIZE(x)	(((x) & 0xf) << 12)
#define FTGMAC100_DBLAC_TXDES_SIZE(x)	(((x) & 0xf) << 16)
#define FTGMAC100_DBLAC_IFG_CNT(x)	(((x) & 0x7) << 20)
#define FTGMAC100_DBLAC_IFG_INC		(1 << 23)

/*
 * DMA FIFO status register
 */
#define FTGMAC100_DMAFIFOS_RXDMA1_SM(dmafifos)	((dmafifos) & 0xf)
#define FTGMAC100_DMAFIFOS_RXDMA2_SM(dmafifos)	(((dmafifos) >> 4) & 0xf)
#define FTGMAC100_DMAFIFOS_RXDMA3_SM(dmafifos)	(((dmafifos) >> 8) & 0x7)
#define FTGMAC100_DMAFIFOS_TXDMA1_SM(dmafifos)	(((dmafifos) >> 12) & 0xf)
#define FTGMAC100_DMAFIFOS_TXDMA2_SM(dmafifos)	(((dmafifos) >> 16) & 0x3)
#define FTGMAC100_DMAFIFOS_TXDMA3_SM(dmafifos)	(((dmafifos) >> 18) & 0xf)
#define FTGMAC100_DMAFIFOS_RXFIFO_EMPTY		(1 << 26)
#define FTGMAC100_DMAFIFOS_TXFIFO_EMPTY		(1 << 27)
#define FTGMAC100_DMAFIFOS_RXDMA_GRANT		(1 << 28)
#define FTGMAC100_DMAFIFOS_TXDMA_GRANT		(1 << 29)
#define FTGMAC100_DMAFIFOS_RXDMA_REQ		(1 << 30)
#define FTGMAC100_DMAFIFOS_TXDMA_REQ		(1 << 31)

/*
 * Feature Register
 */
#define FTGMAC100_REVR_NEW_MDIO_INTERFACE	BIT(31)

/*
 * Receive buffer size register
 */
#define FTGMAC100_RBSR_SIZE(x)		((x) & 0x3fff)

/*
 * MAC control register
 */
#define FTGMAC100_MACCR_TXDMA_EN	(1 << 0)
#define FTGMAC100_MACCR_RXDMA_EN	(1 << 1)
#define FTGMAC100_MACCR_TXMAC_EN	(1 << 2)
#define FTGMAC100_MACCR_RXMAC_EN	(1 << 3)
#define FTGMAC100_MACCR_RM_VLAN		(1 << 4)
#define FTGMAC100_MACCR_HPTXR_EN	(1 << 5)
#define FTGMAC100_MACCR_LOOP_EN		(1 << 6)
#define FTGMAC100_MACCR_ENRX_IN_HALFTX	(1 << 7)
#define FTGMAC100_MACCR_FULLDUP		(1 << 8)
#define FTGMAC100_MACCR_GIGA_MODE	(1 << 9)
#define FTGMAC100_MACCR_CRC_APD		(1 << 10)
#define FTGMAC100_MACCR_PHY_LINK_LEVEL	(1 << 11)
#define FTGMAC100_MACCR_RX_RUNT		(1 << 12)
#define FTGMAC100_MACCR_JUMBO_LF	(1 << 13)
#define FTGMAC100_MACCR_RX_ALL		(1 << 14)
#define FTGMAC100_MACCR_HT_MULTI_EN	(1 << 15)
#define FTGMAC100_MACCR_RX_MULTIPKT	(1 << 16)
#define FTGMAC100_MACCR_RX_BROADPKT	(1 << 17)
#define FTGMAC100_MACCR_DISCARD_CRCERR	(1 << 18)
#define FTGMAC100_MACCR_FAST_MODE	(1 << 19)
#define FTGMAC100_MACCR_SW_RST		(1 << 31)

/*
 * PHY control register
 */
#define FTGMAC100_PHYCR_MDC_CYCTHR_MASK	0x3f
#define FTGMAC100_PHYCR_MDC_CYCTHR(x)	((x) & 0x3f)
#define FTGMAC100_PHYCR_PHYAD(x)	(((x) & 0x1f) << 16)
#define FTGMAC100_PHYCR_REGAD(x)	(((x) & 0x1f) << 21)
#define FTGMAC100_PHYCR_MIIRD		(1 << 26)
#define FTGMAC100_PHYCR_MIIWR		(1 << 27)

/*
 * PHY data register
 */
#define FTGMAC100_PHYDATA_MIIWDATA(x)		((x) & 0xffff)
#define FTGMAC100_PHYDATA_MIIRDATA(phydata)	(((phydata) >> 16) & 0xffff)

/*
 * Flow control register
 */
#define FTGMAC100_FCR_FC_EN		(1 << 0)
#define FTGMAC100_FCR_FCTHR_EN		(1 << 2)
#define FTGMAC100_FCR_PAUSE_TIME(x)	(((x) & 0xffff) << 16)

/*
 * Transmit descriptor, aligned to 16 bytes
 */
struct ftgmac100_txdes {
	unsigned int	txdes0;
	unsigned int	txdes1;
	unsigned int	txdes2;	/* not used by HW */
	unsigned int	txdes3;	/* TXBUF_BADR */
} __attribute__ ((aligned(16)));

#define FTGMAC100_TXDES0_TXBUF_SIZE(x)	((x) & 0x3fff)
#define FTGMAC100_TXDES0_CRC_ERR	(1 << 19)
#define FTGMAC100_TXDES0_LTS		(1 << 28)
#define FTGMAC100_TXDES0_FTS		(1 << 29)
#define FTGMAC100_TXDES0_TXDMA_OWN	(1 << 31)

#define FTGMAC100_TXDES1_VLANTAG_CI(x)	((x) & 0xffff)
#define FTGMAC100_TXDES1_INS_VLANTAG	(1 << 16)
#define FTGMAC100_TXDES1_TCP_CHKSUM	(1 << 17)
#define FTGMAC100_TXDES1_UDP_CHKSUM	(1 << 18)
#define FTGMAC100_TXDES1_IP_CHKSUM	(1 << 19)
#define FTGMAC100_TXDES1_LLC		(1 << 22)
#define FTGMAC100_TXDES1_TX2FIC		(1 << 30)
#define FTGMAC100_TXDES1_TXIC		(1 << 31)

static inline bool ftgmac100_txdes_owned_by_dma(struct ftgmac100_txdes *txdes)
{
	return txdes->txdes0 & cpu_to_le32(FTGMAC100_TXDES0_TXDMA_OWN);
}

static inline void ftgmac100_txdes_set_dma_own(struct ftgmac100_txdes *txdes)
{
	txdes->txdes0 |= cpu_to_le32(FTGMAC100_TXDES0_TXDMA_OWN);
}

static inline void ftgmac100_txdes_set_first_segment(struct ftgmac100_txdes *txdes)
{
	txdes->txdes0 |= cpu_to_le32(FTGMAC100_TXDES0_FTS);
}

static inline bool ftgmac100_txdes_get_first_segment(struct ftgmac100_txdes *txdes)
{
	return (txdes->txdes0 & cpu_to_le32(FTGMAC100_TXDES0_FTS)) != 0;
}

static inline void ftgmac100_txdes_set_last_segment(struct ftgmac100_txdes *txdes)
{
	txdes->txdes0 |= cpu_to_le32(FTGMAC100_TXDES0_LTS);
}

static inline bool ftgmac100_txdes_get_last_segment(struct ftgmac100_txdes *txdes)
{
	return (txdes->txdes0 & cpu_to_le32(FTGMAC100_TXDES0_LTS)) != 0;
}

static inline void ftgmac100_txdes_set_buffer_size(struct ftgmac100_txdes *txdes,
						   unsigned int len)
{
	txdes->txdes0 |= cpu_to_le32(FTGMAC100_TXDES0_TXBUF_SIZE(len));
}

static inline unsigned int ftgmac100_txdes_get_buffer_size(struct ftgmac100_txdes *txdes)
{
	return FTGMAC100_TXDES0_TXBUF_SIZE(cpu_to_le32(txdes->txdes0));
}

static inline void ftgmac100_txdes_set_txint(struct ftgmac100_txdes *txdes)
{
	txdes->txdes1 |= cpu_to_le32(FTGMAC100_TXDES1_TXIC);
}

static inline void ftgmac100_txdes_set_tcpcs(struct ftgmac100_txdes *txdes)
{
	txdes->txdes1 |= cpu_to_le32(FTGMAC100_TXDES1_TCP_CHKSUM);
}

static inline void ftgmac100_txdes_set_udpcs(struct ftgmac100_txdes *txdes)
{
	txdes->txdes1 |= cpu_to_le32(FTGMAC100_TXDES1_UDP_CHKSUM);
}

static inline void ftgmac100_txdes_set_ipcs(struct ftgmac100_txdes *txdes)
{
	txdes->txdes1 |= cpu_to_le32(FTGMAC100_TXDES1_IP_CHKSUM);
}

static inline void ftgmac100_txdes_set_dma_addr(struct ftgmac100_txdes *txdes,
						dma_addr_t addr)
{
	txdes->txdes3 = cpu_to_le32(addr);
}

static inline dma_addr_t ftgmac100_txdes_get_dma_addr(struct ftgmac100_txdes *txdes)
{
	return le32_to_cpu(txdes->txdes3);
}

/*
 * Receive descriptor, aligned to 16 bytes
 */
struct ftgmac100_rxdes {
	unsigned int	rxdes0;
	unsigned int	rxdes1;
	unsigned int	rxdes2;	/* not used by HW */
	unsigned int	rxdes3;	/* RXBUF_BADR */
} __attribute__ ((aligned(16)));

#define FTGMAC100_RXDES0_VDBC		0x3fff
#define FTGMAC100_RXDES0_MULTICAST	(1 << 16)
#define FTGMAC100_RXDES0_BROADCAST	(1 << 17)
#define FTGMAC100_RXDES0_RX_ERR		(1 << 18)
#define FTGMAC100_RXDES0_CRC_ERR	(1 << 19)
#define FTGMAC100_RXDES0_FTL		(1 << 20)
#define FTGMAC100_RXDES0_RUNT		(1 << 21)
#define FTGMAC100_RXDES0_RX_ODD_NB	(1 << 22)
#define FTGMAC100_RXDES0_FIFO_FULL	(1 << 23)
#define FTGMAC100_RXDES0_PAUSE_OPCODE	(1 << 24)
#define FTGMAC100_RXDES0_PAUSE_FRAME	(1 << 25)
#define FTGMAC100_RXDES0_LRS		(1 << 28)
#define FTGMAC100_RXDES0_FRS		(1 << 29)
#define FTGMAC100_RXDES0_RXPKT_RDY	(1 << 31)

#define FTGMAC100_RXDES1_VLANTAG_CI	0xffff
#define FTGMAC100_RXDES1_PROT(x)	(((x) >> 20) & 3)
#define   FTGMAC100_PROT_NONIP	0
#define   FTGMAC100_PROT_IP	1
#define   FTGMAC100_PROT_TCPIP	2
#define   FTGMAC100_PROT_UDPIP	3
#define FTGMAC100_RXDES1_LLC		(1 << 22)
#define FTGMAC100_RXDES1_DF		(1 << 23)
#define FTGMAC100_RXDES1_VLANTAG_AVAIL	(1 << 24)
#define FTGMAC100_RXDES1_TCP_CHKSUM_ERR	(1 << 25)
#define FTGMAC100_RXDES1_UDP_CHKSUM_ERR	(1 << 26)
#define FTGMAC100_RXDES1_IP_CHKSUM_ERR	(1 << 27)

static bool ftgmac100_rxdes_first_segment(struct ftgmac100_rxdes *rxdes)
{
	return rxdes->rxdes0 & cpu_to_le32(FTGMAC100_RXDES0_FRS);
}

static bool ftgmac100_rxdes_last_segment(struct ftgmac100_rxdes *rxdes)
{
	return rxdes->rxdes0 & cpu_to_le32(FTGMAC100_RXDES0_LRS);
}

static bool ftgmac100_rxdes_packet_ready(struct ftgmac100_rxdes *rxdes)
{
	return rxdes->rxdes0 & cpu_to_le32(FTGMAC100_RXDES0_RXPKT_RDY);
}

#define RXDES0_ANY_ERROR		( \
	FTGMAC100_RXDES0_RX_ERR		| \
	FTGMAC100_RXDES0_CRC_ERR	| \
	FTGMAC100_RXDES0_FTL		| \
	FTGMAC100_RXDES0_RUNT		| \
	FTGMAC100_RXDES0_RX_ODD_NB)

static inline bool ftgmac100_rxdes_any_error(struct ftgmac100_rxdes *rxdes)
{
	return rxdes->rxdes0 & cpu_to_le32(RXDES0_ANY_ERROR);
}

static inline bool ftgmac100_rxdes_rx_error(struct ftgmac100_rxdes *rxdes)
{
	return rxdes->rxdes0 & cpu_to_le32(FTGMAC100_RXDES0_RX_ERR);
}

static inline bool ftgmac100_rxdes_crc_error(struct ftgmac100_rxdes *rxdes)
{
	return rxdes->rxdes0 & cpu_to_le32(FTGMAC100_RXDES0_CRC_ERR);
}

static inline bool ftgmac100_rxdes_frame_too_long(struct ftgmac100_rxdes *rxdes)
{
	return rxdes->rxdes0 & cpu_to_le32(FTGMAC100_RXDES0_FTL);
}

static inline bool ftgmac100_rxdes_runt(struct ftgmac100_rxdes *rxdes)
{
	return rxdes->rxdes0 & cpu_to_le32(FTGMAC100_RXDES0_RUNT);
}

static inline bool ftgmac100_rxdes_odd_nibble(struct ftgmac100_rxdes *rxdes)
{
	return rxdes->rxdes0 & cpu_to_le32(FTGMAC100_RXDES0_RX_ODD_NB);
}

static inline unsigned int ftgmac100_rxdes_data_length(struct ftgmac100_rxdes *rxdes)
{
	return le32_to_cpu(rxdes->rxdes0) & FTGMAC100_RXDES0_VDBC;
}

static inline bool ftgmac100_rxdes_multicast(struct ftgmac100_rxdes *rxdes)
{
	return rxdes->rxdes0 & cpu_to_le32(FTGMAC100_RXDES0_MULTICAST);
}

static inline void ftgmac100_rxdes_set_dma_addr(struct ftgmac100_rxdes *rxdes,
						dma_addr_t addr)
{
	rxdes->rxdes3 = cpu_to_le32(addr);
}

static inline dma_addr_t ftgmac100_rxdes_get_dma_addr(struct ftgmac100_rxdes *rxdes)
{
	return le32_to_cpu(rxdes->rxdes3);
}

static inline bool ftgmac100_rxdes_csum_err(struct ftgmac100_rxdes *rxdes)
{
	return !!(rxdes->rxdes1 & cpu_to_le32(FTGMAC100_RXDES1_TCP_CHKSUM_ERR |
					      FTGMAC100_RXDES1_UDP_CHKSUM_ERR |
					      FTGMAC100_RXDES1_IP_CHKSUM_ERR));
}

#endif /* __FTGMAC100_H */
