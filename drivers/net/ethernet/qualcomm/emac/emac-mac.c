/* Copyright (c) 2013-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/* Qualcomm Technologies, Inc. EMAC Ethernet Controller MAC layer support
 */

#include <linux/tcp.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/crc32.h>
#include <linux/if_vlan.h>
#include <linux/jiffies.h>
#include <linux/phy.h>
#include <linux/of.h>
#include <linux/gpio.h>
#include <linux/pm_runtime.h>
#include "emac.h"
#include "emac-mac.h"

/* EMAC base register offsets */
#define EMAC_MAC_CTRL                                         0x001480
#define EMAC_WOL_CTRL0                                        0x0014a0
#define EMAC_RSS_KEY0                                         0x0014b0
#define EMAC_H1TPD_BASE_ADDR_LO                               0x0014e0
#define EMAC_H2TPD_BASE_ADDR_LO                               0x0014e4
#define EMAC_H3TPD_BASE_ADDR_LO                               0x0014e8
#define EMAC_INTER_SRAM_PART9                                 0x001534
#define EMAC_DESC_CTRL_0                                      0x001540
#define EMAC_DESC_CTRL_1                                      0x001544
#define EMAC_DESC_CTRL_2                                      0x001550
#define EMAC_DESC_CTRL_10                                     0x001554
#define EMAC_DESC_CTRL_12                                     0x001558
#define EMAC_DESC_CTRL_13                                     0x00155c
#define EMAC_DESC_CTRL_3                                      0x001560
#define EMAC_DESC_CTRL_4                                      0x001564
#define EMAC_DESC_CTRL_5                                      0x001568
#define EMAC_DESC_CTRL_14                                     0x00156c
#define EMAC_DESC_CTRL_15                                     0x001570
#define EMAC_DESC_CTRL_16                                     0x001574
#define EMAC_DESC_CTRL_6                                      0x001578
#define EMAC_DESC_CTRL_8                                      0x001580
#define EMAC_DESC_CTRL_9                                      0x001584
#define EMAC_DESC_CTRL_11                                     0x001588
#define EMAC_TXQ_CTRL_0                                       0x001590
#define EMAC_TXQ_CTRL_1                                       0x001594
#define EMAC_TXQ_CTRL_2                                       0x001598
#define EMAC_RXQ_CTRL_0                                       0x0015a0
#define EMAC_RXQ_CTRL_1                                       0x0015a4
#define EMAC_RXQ_CTRL_2                                       0x0015a8
#define EMAC_RXQ_CTRL_3                                       0x0015ac
#define EMAC_BASE_CPU_NUMBER                                  0x0015b8
#define EMAC_DMA_CTRL                                         0x0015c0
#define EMAC_MAILBOX_0                                        0x0015e0
#define EMAC_MAILBOX_5                                        0x0015e4
#define EMAC_MAILBOX_6                                        0x0015e8
#define EMAC_MAILBOX_13                                       0x0015ec
#define EMAC_MAILBOX_2                                        0x0015f4
#define EMAC_MAILBOX_3                                        0x0015f8
#define EMAC_MAILBOX_11                                       0x00160c
#define EMAC_AXI_MAST_CTRL                                    0x001610
#define EMAC_MAILBOX_12                                       0x001614
#define EMAC_MAILBOX_9                                        0x001618
#define EMAC_MAILBOX_10                                       0x00161c
#define EMAC_ATHR_HEADER_CTRL                                 0x001620
#define EMAC_CLK_GATE_CTRL                                    0x001814
#define EMAC_MISC_CTRL                                        0x001990
#define EMAC_MAILBOX_7                                        0x0019e0
#define EMAC_MAILBOX_8                                        0x0019e4
#define EMAC_MAILBOX_15                                       0x001bd4
#define EMAC_MAILBOX_16                                       0x001bd8

/* EMAC_MAC_CTRL */
#define SINGLE_PAUSE_MODE                                   0x10000000
#define DEBUG_MODE                                           0x8000000
#define BROAD_EN                                             0x4000000
#define MULTI_ALL                                            0x2000000
#define RX_CHKSUM_EN                                         0x1000000
#define HUGE                                                  0x800000
#define SPEED_BMSK                                            0x300000
#define SPEED_SHFT                                                  20
#define SIMR                                                   0x80000
#define TPAUSE                                                 0x10000
#define PROM_MODE                                               0x8000
#define VLAN_STRIP                                              0x4000
#define PRLEN_BMSK                                              0x3c00
#define PRLEN_SHFT                                                  10
#define HUGEN                                                    0x200
#define FLCHK                                                    0x100
#define PCRCE                                                     0x80
#define CRCE                                                      0x40
#define FULLD                                                     0x20
#define MAC_LP_EN                                                 0x10
#define RXFC                                                       0x8
#define TXFC                                                       0x4
#define RXEN                                                       0x2
#define TXEN                                                       0x1

/* EMAC_WOL_CTRL0 */
#define LK_CHG_PME                                                0x20
#define LK_CHG_EN                                                 0x10
#define MG_FRAME_PME                                               0x8
#define MG_FRAME_EN                                                0x4
#define WK_FRAME_EN                                                0x1

/* EMAC_DESC_CTRL_3 */
#define RFD_RING_SIZE_BMSK                                       0xfff

/* EMAC_DESC_CTRL_4 */
#define RX_BUFFER_SIZE_BMSK                                     0xffff

/* EMAC_DESC_CTRL_6 */
#define RRD_RING_SIZE_BMSK                                       0xfff

/* EMAC_DESC_CTRL_9 */
#define TPD_RING_SIZE_BMSK                                      0xffff

/* EMAC_TXQ_CTRL_0 */
#define NUM_TXF_BURST_PREF_BMSK                             0xffff0000
#define NUM_TXF_BURST_PREF_SHFT                                     16
#define LS_8023_SP                                                0x80
#define TXQ_MODE                                                  0x40
#define TXQ_EN                                                    0x20
#define IP_OP_SP                                                  0x10
#define NUM_TPD_BURST_PREF_BMSK                                    0xf
#define NUM_TPD_BURST_PREF_SHFT                                      0

/* EMAC_TXQ_CTRL_1 */
#define JUMBO_TASK_OFFLOAD_THRESHOLD_BMSK                        0x7ff

/* EMAC_TXQ_CTRL_2 */
#define TXF_HWM_BMSK                                         0xfff0000
#define TXF_LWM_BMSK                                             0xfff

/* EMAC_RXQ_CTRL_0 */
#define RXQ_EN                                              0x80000000
#define CUT_THRU_EN                                         0x40000000
#define RSS_HASH_EN                                         0x20000000
#define NUM_RFD_BURST_PREF_BMSK                              0x3f00000
#define NUM_RFD_BURST_PREF_SHFT                                     20
#define IDT_TABLE_SIZE_BMSK                                    0x1ff00
#define IDT_TABLE_SIZE_SHFT                                          8
#define SP_IPV6                                                   0x80

/* EMAC_RXQ_CTRL_1 */
#define JUMBO_1KAH_BMSK                                         0xf000
#define JUMBO_1KAH_SHFT                                             12
#define RFD_PREF_LOW_TH                                           0x10
#define RFD_PREF_LOW_THRESHOLD_BMSK                              0xfc0
#define RFD_PREF_LOW_THRESHOLD_SHFT                                  6
#define RFD_PREF_UP_TH                                            0x10
#define RFD_PREF_UP_THRESHOLD_BMSK                                0x3f
#define RFD_PREF_UP_THRESHOLD_SHFT                                   0

/* EMAC_RXQ_CTRL_2 */
#define RXF_DOF_THRESFHOLD                                       0x1a0
#define RXF_DOF_THRESHOLD_BMSK                               0xfff0000
#define RXF_DOF_THRESHOLD_SHFT                                      16
#define RXF_UOF_THRESFHOLD                                        0xbe
#define RXF_UOF_THRESHOLD_BMSK                                   0xfff
#define RXF_UOF_THRESHOLD_SHFT                                       0

/* EMAC_RXQ_CTRL_3 */
#define RXD_TIMER_BMSK                                      0xffff0000
#define RXD_THRESHOLD_BMSK                                       0xfff
#define RXD_THRESHOLD_SHFT                                           0

/* EMAC_DMA_CTRL */
#define DMAW_DLY_CNT_BMSK                                      0xf0000
#define DMAW_DLY_CNT_SHFT                                           16
#define DMAR_DLY_CNT_BMSK                                       0xf800
#define DMAR_DLY_CNT_SHFT                                           11
#define DMAR_REQ_PRI                                             0x400
#define REGWRBLEN_BMSK                                           0x380
#define REGWRBLEN_SHFT                                               7
#define REGRDBLEN_BMSK                                            0x70
#define REGRDBLEN_SHFT                                               4
#define OUT_ORDER_MODE                                             0x4
#define ENH_ORDER_MODE                                             0x2
#define IN_ORDER_MODE                                              0x1

/* EMAC_MAILBOX_13 */
#define RFD3_PROC_IDX_BMSK                                   0xfff0000
#define RFD3_PROC_IDX_SHFT                                          16
#define RFD3_PROD_IDX_BMSK                                       0xfff
#define RFD3_PROD_IDX_SHFT                                           0

/* EMAC_MAILBOX_2 */
#define NTPD_CONS_IDX_BMSK                                  0xffff0000
#define NTPD_CONS_IDX_SHFT                                          16

/* EMAC_MAILBOX_3 */
#define RFD0_CONS_IDX_BMSK                                       0xfff
#define RFD0_CONS_IDX_SHFT                                           0

/* EMAC_MAILBOX_11 */
#define H3TPD_PROD_IDX_BMSK                                 0xffff0000
#define H3TPD_PROD_IDX_SHFT                                         16

/* EMAC_AXI_MAST_CTRL */
#define DATA_BYTE_SWAP                                             0x8
#define MAX_BOUND                                                  0x2
#define MAX_BTYPE                                                  0x1

/* EMAC_MAILBOX_12 */
#define H3TPD_CONS_IDX_BMSK                                 0xffff0000
#define H3TPD_CONS_IDX_SHFT                                         16

/* EMAC_MAILBOX_9 */
#define H2TPD_PROD_IDX_BMSK                                     0xffff
#define H2TPD_PROD_IDX_SHFT                                          0

/* EMAC_MAILBOX_10 */
#define H1TPD_CONS_IDX_BMSK                                 0xffff0000
#define H1TPD_CONS_IDX_SHFT                                         16
#define H2TPD_CONS_IDX_BMSK                                     0xffff
#define H2TPD_CONS_IDX_SHFT                                          0

/* EMAC_ATHR_HEADER_CTRL */
#define HEADER_CNT_EN                                              0x2
#define HEADER_ENABLE                                              0x1

/* EMAC_MAILBOX_0 */
#define RFD0_PROC_IDX_BMSK                                   0xfff0000
#define RFD0_PROC_IDX_SHFT                                          16
#define RFD0_PROD_IDX_BMSK                                       0xfff
#define RFD0_PROD_IDX_SHFT                                           0

/* EMAC_MAILBOX_5 */
#define RFD1_PROC_IDX_BMSK                                   0xfff0000
#define RFD1_PROC_IDX_SHFT                                          16
#define RFD1_PROD_IDX_BMSK                                       0xfff
#define RFD1_PROD_IDX_SHFT                                           0

/* EMAC_MISC_CTRL */
#define RX_UNCPL_INT_EN                                            0x1

/* EMAC_MAILBOX_7 */
#define RFD2_CONS_IDX_BMSK                                   0xfff0000
#define RFD2_CONS_IDX_SHFT                                          16
#define RFD1_CONS_IDX_BMSK                                       0xfff
#define RFD1_CONS_IDX_SHFT                                           0

/* EMAC_MAILBOX_8 */
#define RFD3_CONS_IDX_BMSK                                       0xfff
#define RFD3_CONS_IDX_SHFT                                           0

/* EMAC_MAILBOX_15 */
#define NTPD_PROD_IDX_BMSK                                      0xffff
#define NTPD_PROD_IDX_SHFT                                           0

/* EMAC_MAILBOX_16 */
#define H1TPD_PROD_IDX_BMSK                                     0xffff
#define H1TPD_PROD_IDX_SHFT                                          0

#define RXQ0_RSS_HSTYP_IPV6_TCP_EN                                0x20
#define RXQ0_RSS_HSTYP_IPV6_EN                                    0x10
#define RXQ0_RSS_HSTYP_IPV4_TCP_EN                                 0x8
#define RXQ0_RSS_HSTYP_IPV4_EN                                     0x4

/* DMA address */
#define DMA_ADDR_HI_MASK                         0xffffffff00000000ULL
#define DMA_ADDR_LO_MASK                         0x00000000ffffffffULL

#define EMAC_DMA_ADDR_HI(_addr)                                      \
		((u32)(((u64)(_addr) & DMA_ADDR_HI_MASK) >> 32))
#define EMAC_DMA_ADDR_LO(_addr)                                      \
		((u32)((u64)(_addr) & DMA_ADDR_LO_MASK))

/* EMAC_EMAC_WRAPPER_TX_TS_INX */
#define EMAC_WRAPPER_TX_TS_EMPTY                            0x80000000
#define EMAC_WRAPPER_TX_TS_INX_BMSK                             0xffff

struct emac_skb_cb {
	u32           tpd_idx;
	unsigned long jiffies;
};

struct emac_tx_ts_cb {
	u32 sec;
	u32 ns;
};

#define EMAC_SKB_CB(skb)	((struct emac_skb_cb *)(skb)->cb)
#define EMAC_TX_TS_CB(skb)	((struct emac_tx_ts_cb *)(skb)->cb)
#define EMAC_RSS_IDT_SIZE	256
#define JUMBO_1KAH		0x4
#define RXD_TH			0x100
#define EMAC_TPD_LAST_FRAGMENT	0x80000000
#define EMAC_TPD_TSTAMP_SAVE	0x80000000

/* EMAC Errors in emac_rrd.word[3] */
#define EMAC_RRD_L4F		BIT(14)
#define EMAC_RRD_IPF		BIT(15)
#define EMAC_RRD_CRC		BIT(21)
#define EMAC_RRD_FAE		BIT(22)
#define EMAC_RRD_TRN		BIT(23)
#define EMAC_RRD_RNT		BIT(24)
#define EMAC_RRD_INC		BIT(25)
#define EMAC_RRD_FOV		BIT(29)
#define EMAC_RRD_LEN		BIT(30)

/* Error bits that will result in a received frame being discarded */
#define EMAC_RRD_ERROR (EMAC_RRD_IPF | EMAC_RRD_CRC | EMAC_RRD_FAE | \
			EMAC_RRD_TRN | EMAC_RRD_RNT | EMAC_RRD_INC | \
			EMAC_RRD_FOV | EMAC_RRD_LEN)
#define EMAC_RRD_STATS_DW_IDX 3

#define EMAC_RRD(RXQ, SIZE, IDX)	((RXQ)->rrd.v_addr + (SIZE * (IDX)))
#define EMAC_RFD(RXQ, SIZE, IDX)	((RXQ)->rfd.v_addr + (SIZE * (IDX)))
#define EMAC_TPD(TXQ, SIZE, IDX)	((TXQ)->tpd.v_addr + (SIZE * (IDX)))

#define GET_RFD_BUFFER(RXQ, IDX)	(&((RXQ)->rfd.rfbuff[(IDX)]))
#define GET_TPD_BUFFER(RTQ, IDX)	(&((RTQ)->tpd.tpbuff[(IDX)]))

#define EMAC_TX_POLL_HWTXTSTAMP_THRESHOLD	8

#define ISR_RX_PKT      (\
	RX_PKT_INT0     |\
	RX_PKT_INT1     |\
	RX_PKT_INT2     |\
	RX_PKT_INT3)

static void emac_mac_irq_enable(struct emac_adapter *adpt)
{
	int i;

	for (i = 0; i < EMAC_NUM_CORE_IRQ; i++) {
		struct emac_irq			*irq = &adpt->irq[i];
		const struct emac_irq_config	*irq_cfg = &emac_irq_cfg_tbl[i];

		writel_relaxed(~DIS_INT, adpt->base + irq_cfg->status_reg);
		writel_relaxed(irq->mask, adpt->base + irq_cfg->mask_reg);
	}

	wmb(); /* ensure that irq and ptp setting are flushed to HW */
}

static void emac_mac_irq_disable(struct emac_adapter *adpt)
{
	int i;

	for (i = 0; i < EMAC_NUM_CORE_IRQ; i++) {
		const struct emac_irq_config *irq_cfg = &emac_irq_cfg_tbl[i];

		writel_relaxed(DIS_INT, adpt->base + irq_cfg->status_reg);
		writel_relaxed(0, adpt->base + irq_cfg->mask_reg);
	}
	wmb(); /* ensure that irq clearings are flushed to HW */

	for (i = 0; i < EMAC_NUM_CORE_IRQ; i++)
		if (adpt->irq[i].irq)
			synchronize_irq(adpt->irq[i].irq);
}

void emac_mac_multicast_addr_set(struct emac_adapter *adpt, u8 *addr)
{
	u32 crc32, bit, reg, mta;

	/* Calculate the CRC of the MAC address */
	crc32 = ether_crc(ETH_ALEN, addr);

	/* The HASH Table is an array of 2 32-bit registers. It is
	 * treated like an array of 64 bits (BitArray[hash_value]).
	 * Use the upper 6 bits of the above CRC as the hash value.
	 */
	reg = (crc32 >> 31) & 0x1;
	bit = (crc32 >> 26) & 0x1F;

	mta = readl_relaxed(adpt->base + EMAC_HASH_TAB_REG0 + (reg << 2));
	mta |= (0x1 << bit);
	writel_relaxed(mta, adpt->base + EMAC_HASH_TAB_REG0 + (reg << 2));
	wmb(); /* ensure that the mac address is flushed to HW */
}

void emac_mac_multicast_addr_clear(struct emac_adapter *adpt)
{
	writel_relaxed(0, adpt->base + EMAC_HASH_TAB_REG0);
	writel_relaxed(0, adpt->base + EMAC_HASH_TAB_REG1);
	wmb(); /* ensure that clearing the mac address is flushed to HW */
}

/* definitions for RSS */
#define EMAC_RSS_KEY(_i, _type) \
		(EMAC_RSS_KEY0 + ((_i) * sizeof(_type)))
#define EMAC_RSS_TBL(_i, _type) \
		(EMAC_IDT_TABLE0 + ((_i) * sizeof(_type)))

/* RSS */
static void emac_mac_rss_config(struct emac_adapter *adpt)
{
	int key_len_by_u32 = ARRAY_SIZE(adpt->rss_key);
	int idt_len_by_u32 = ARRAY_SIZE(adpt->rss_idt);
	u32 rxq0;
	int i;

	/* Fill out hash function keys */
	for (i = 0; i < key_len_by_u32; i++) {
		u32 key, idx_base;

		idx_base = (key_len_by_u32 - i) * 4;
		key = ((adpt->rss_key[idx_base - 1])       |
		       (adpt->rss_key[idx_base - 2] << 8)  |
		       (adpt->rss_key[idx_base - 3] << 16) |
		       (adpt->rss_key[idx_base - 4] << 24));
		writel_relaxed(key, adpt->base + EMAC_RSS_KEY(i, u32));
	}

	/* Fill out redirection table */
	for (i = 0; i < idt_len_by_u32; i++)
		writel_relaxed(adpt->rss_idt[i],
			       adpt->base + EMAC_RSS_TBL(i, u32));

	writel_relaxed(adpt->rss_base_cpu, adpt->base + EMAC_BASE_CPU_NUMBER);

	rxq0 = readl_relaxed(adpt->base + EMAC_RXQ_CTRL_0);
	if (adpt->rss_hstype & EMAC_RSS_HSTYP_IPV4_EN)
		rxq0 |= RXQ0_RSS_HSTYP_IPV4_EN;
	else
		rxq0 &= ~RXQ0_RSS_HSTYP_IPV4_EN;

	if (adpt->rss_hstype & EMAC_RSS_HSTYP_TCP4_EN)
		rxq0 |= RXQ0_RSS_HSTYP_IPV4_TCP_EN;
	else
		rxq0 &= ~RXQ0_RSS_HSTYP_IPV4_TCP_EN;

	if (adpt->rss_hstype & EMAC_RSS_HSTYP_IPV6_EN)
		rxq0 |= RXQ0_RSS_HSTYP_IPV6_EN;
	else
		rxq0 &= ~RXQ0_RSS_HSTYP_IPV6_EN;

	if (adpt->rss_hstype & EMAC_RSS_HSTYP_TCP6_EN)
		rxq0 |= RXQ0_RSS_HSTYP_IPV6_TCP_EN;
	else
		rxq0 &= ~RXQ0_RSS_HSTYP_IPV6_TCP_EN;

	rxq0 |= ((adpt->rss_idt_size << IDT_TABLE_SIZE_SHFT) &
		IDT_TABLE_SIZE_BMSK);
	rxq0 |= RSS_HASH_EN;

	wmb(); /* ensure all parameters are written before enabling RSS */

	writel(rxq0, adpt->base + EMAC_RXQ_CTRL_0);
}

/* Config MAC modes */
void emac_mac_mode_config(struct emac_adapter *adpt)
{
	u32 mac;

	mac = readl_relaxed(adpt->base + EMAC_MAC_CTRL);

	if (test_bit(EMAC_STATUS_VLANSTRIP_EN, &adpt->status))
		mac |= VLAN_STRIP;
	else
		mac &= ~VLAN_STRIP;

	if (test_bit(EMAC_STATUS_PROMISC_EN, &adpt->status))
		mac |= PROM_MODE;
	else
		mac &= ~PROM_MODE;

	if (test_bit(EMAC_STATUS_MULTIALL_EN, &adpt->status))
		mac |= MULTI_ALL;
	else
		mac &= ~MULTI_ALL;

	if (test_bit(EMAC_STATUS_LOOPBACK_EN, &adpt->status))
		mac |= MAC_LP_EN;
	else
		mac &= ~MAC_LP_EN;

	writel_relaxed(mac, adpt->base + EMAC_MAC_CTRL);
	wmb(); /* ensure MAC setting is flushed to HW */
}

/* Wake On LAN (WOL) */
void emac_mac_wol_config(struct emac_adapter *adpt, u32 wufc)
{
	u32 wol = 0;

	/* turn on magic packet event */
	if (wufc & EMAC_WOL_MAGIC)
		wol |= MG_FRAME_EN | MG_FRAME_PME | WK_FRAME_EN;

	/* turn on link up event */
	if (wufc & EMAC_WOL_PHY)
		wol |=  LK_CHG_EN | LK_CHG_PME;

	writel_relaxed(wol, adpt->base + EMAC_WOL_CTRL0);
	wmb(); /* ensure that WOL setting is flushed to HW */
}

/* Power Management */
void emac_mac_pm(struct emac_adapter *adpt, u32 speed, bool wol_en, bool rx_en)
{
	u32 dma_mas, mac;

	dma_mas = readl_relaxed(adpt->base + EMAC_DMA_MAS_CTRL);
	dma_mas &= ~LPW_CLK_SEL;
	dma_mas |= LPW_STATE;

	mac = readl_relaxed(adpt->base + EMAC_MAC_CTRL);
	mac &= ~(FULLD | RXEN | TXEN);
	mac = (mac & ~SPEED_BMSK) |
	  (((u32)emac_mac_speed_10_100 << SPEED_SHFT) & SPEED_BMSK);

	if (wol_en) {
		if (rx_en)
			mac |= RXEN | BROAD_EN;

		/* If WOL is enabled, set link speed/duplex for mac */
		if (speed == EMAC_LINK_SPEED_1GB_FULL)
			mac = (mac & ~SPEED_BMSK) |
			  (((u32)emac_mac_speed_1000 << SPEED_SHFT) &
			   SPEED_BMSK);

		if (speed == EMAC_LINK_SPEED_10_FULL  ||
		    speed == EMAC_LINK_SPEED_100_FULL ||
		    speed == EMAC_LINK_SPEED_1GB_FULL)
			mac |= FULLD;
	} else {
		/* select lower clock speed if WOL is disabled */
		dma_mas |= LPW_CLK_SEL;
	}

	writel_relaxed(dma_mas, adpt->base + EMAC_DMA_MAS_CTRL);
	writel_relaxed(mac, adpt->base + EMAC_MAC_CTRL);
	wmb(); /* ensure that power setting is flushed to HW */
}

/* Config descriptor rings */
static void emac_mac_dma_rings_config(struct emac_adapter *adpt)
{
	static const unsigned int tpd_q_offset[] = {
		EMAC_DESC_CTRL_8,        EMAC_H1TPD_BASE_ADDR_LO,
		EMAC_H2TPD_BASE_ADDR_LO, EMAC_H3TPD_BASE_ADDR_LO};
	static const unsigned int rfd_q_offset[] = {
		EMAC_DESC_CTRL_2,        EMAC_DESC_CTRL_10,
		EMAC_DESC_CTRL_12,       EMAC_DESC_CTRL_13};
	static const unsigned int rrd_q_offset[] = {
		EMAC_DESC_CTRL_5,        EMAC_DESC_CTRL_14,
		EMAC_DESC_CTRL_15,       EMAC_DESC_CTRL_16};
	int i;

	if (adpt->timestamp_en)
		emac_reg_update32(adpt->csr + EMAC_EMAC_WRAPPER_CSR1,
				  0, ENABLE_RRD_TIMESTAMP);

	/* TPD (Transmit Packet Descriptor) */
	writel_relaxed(EMAC_DMA_ADDR_HI(adpt->tx_q[0].tpd.p_addr),
		       adpt->base + EMAC_DESC_CTRL_1);

	for (i = 0; i < adpt->tx_q_cnt; ++i)
		writel_relaxed(EMAC_DMA_ADDR_LO(adpt->tx_q[i].tpd.p_addr),
			       adpt->base + tpd_q_offset[i]);

	writel_relaxed(adpt->tx_q[0].tpd.count & TPD_RING_SIZE_BMSK,
		       adpt->base + EMAC_DESC_CTRL_9);

	/* RFD (Receive Free Descriptor) & RRD (Receive Return Descriptor) */
	writel_relaxed(EMAC_DMA_ADDR_HI(adpt->rx_q[0].rfd.p_addr),
		       adpt->base + EMAC_DESC_CTRL_0);

	for (i = 0; i < adpt->rx_q_cnt; ++i) {
		writel_relaxed(EMAC_DMA_ADDR_LO(adpt->rx_q[i].rfd.p_addr),
			       adpt->base + rfd_q_offset[i]);
		writel_relaxed(EMAC_DMA_ADDR_LO(adpt->rx_q[i].rrd.p_addr),
			       adpt->base + rrd_q_offset[i]);
	}

	writel_relaxed(adpt->rx_q[0].rfd.count & RFD_RING_SIZE_BMSK,
		       adpt->base + EMAC_DESC_CTRL_3);
	writel_relaxed(adpt->rx_q[0].rrd.count & RRD_RING_SIZE_BMSK,
		       adpt->base + EMAC_DESC_CTRL_6);

	writel_relaxed(adpt->rxbuf_size & RX_BUFFER_SIZE_BMSK,
		       adpt->base + EMAC_DESC_CTRL_4);

	writel_relaxed(0, adpt->base + EMAC_DESC_CTRL_11);

	wmb(); /* ensure all parameters are written before we enable them */

	/* Load all of the base addresses above and ensure that triggering HW to
	 * read ring pointers is flushed
	 */
	writel(1, adpt->base + EMAC_INTER_SRAM_PART9);
}

/* Config transmit parameters */
static void emac_mac_tx_config(struct emac_adapter *adpt)
{
	u32 val;

	writel_relaxed((EMAC_MAX_TX_OFFLOAD_THRESH >> 3) &
		       JUMBO_TASK_OFFLOAD_THRESHOLD_BMSK,
		       adpt->base + EMAC_TXQ_CTRL_1);

	val = (adpt->tpd_burst << NUM_TPD_BURST_PREF_SHFT) &
		NUM_TPD_BURST_PREF_BMSK;

	val |= (TXQ_MODE | LS_8023_SP);
	val |= (0x0100 << NUM_TXF_BURST_PREF_SHFT) &
		NUM_TXF_BURST_PREF_BMSK;

	writel_relaxed(val, adpt->base + EMAC_TXQ_CTRL_0);
	emac_reg_update32(adpt->base + EMAC_TXQ_CTRL_2,
			  (TXF_HWM_BMSK | TXF_LWM_BMSK), 0);
	wmb(); /* ensure that Tx control settings are flushed to HW */
}

/* Config receive parameters */
static void emac_mac_rx_config(struct emac_adapter *adpt)
{
	u32 val;

	val = ((adpt->rfd_burst << NUM_RFD_BURST_PREF_SHFT) &
	       NUM_RFD_BURST_PREF_BMSK);
	val |= (SP_IPV6 | CUT_THRU_EN);

	writel_relaxed(val, adpt->base + EMAC_RXQ_CTRL_0);

	val = readl_relaxed(adpt->base + EMAC_RXQ_CTRL_1);
	val &= ~(JUMBO_1KAH_BMSK | RFD_PREF_LOW_THRESHOLD_BMSK |
		 RFD_PREF_UP_THRESHOLD_BMSK);
	val |= (JUMBO_1KAH << JUMBO_1KAH_SHFT) |
		(RFD_PREF_LOW_TH << RFD_PREF_LOW_THRESHOLD_SHFT) |
		(RFD_PREF_UP_TH << RFD_PREF_UP_THRESHOLD_SHFT);
	writel_relaxed(val, adpt->base + EMAC_RXQ_CTRL_1);

	val = readl_relaxed(adpt->base + EMAC_RXQ_CTRL_2);
	val &= ~(RXF_DOF_THRESHOLD_BMSK | RXF_UOF_THRESHOLD_BMSK);
	val |= (RXF_DOF_THRESFHOLD << RXF_DOF_THRESHOLD_SHFT) |
		(RXF_UOF_THRESFHOLD << RXF_UOF_THRESHOLD_SHFT);
	writel_relaxed(val, adpt->base + EMAC_RXQ_CTRL_2);

	val = readl_relaxed(adpt->base + EMAC_RXQ_CTRL_3);
	val &= ~(RXD_TIMER_BMSK | RXD_THRESHOLD_BMSK);
	val |= RXD_TH << RXD_THRESHOLD_SHFT;
	writel_relaxed(val, adpt->base + EMAC_RXQ_CTRL_3);
	wmb(); /* ensure that Rx control settings are flushed to HW */
}

/* Config dma */
static void emac_mac_dma_config(struct emac_adapter *adpt)
{
	u32 dma_ctrl;

	dma_ctrl = DMAR_REQ_PRI;

	switch (adpt->dma_order) {
	case emac_dma_ord_in:
		dma_ctrl |= IN_ORDER_MODE;
		break;
	case emac_dma_ord_enh:
		dma_ctrl |= ENH_ORDER_MODE;
		break;
	case emac_dma_ord_out:
		dma_ctrl |= OUT_ORDER_MODE;
		break;
	default:
		break;
	}

	dma_ctrl |= (((u32)adpt->dmar_block) << REGRDBLEN_SHFT) &
						REGRDBLEN_BMSK;
	dma_ctrl |= (((u32)adpt->dmaw_block) << REGWRBLEN_SHFT) &
						REGWRBLEN_BMSK;
	dma_ctrl |= (((u32)adpt->dmar_dly_cnt) << DMAR_DLY_CNT_SHFT) &
						DMAR_DLY_CNT_BMSK;
	dma_ctrl |= (((u32)adpt->dmaw_dly_cnt) << DMAW_DLY_CNT_SHFT) &
						DMAW_DLY_CNT_BMSK;

	/* config DMA and ensure that configuration is flushed to HW */
	writel(dma_ctrl, adpt->base + EMAC_DMA_CTRL);
}

void emac_mac_config(struct emac_adapter *adpt)
{
	u32 val;

	emac_mac_addr_clear(adpt, adpt->mac_addr);

	emac_mac_dma_rings_config(adpt);

	writel_relaxed(adpt->mtu + ETH_HLEN + VLAN_HLEN + ETH_FCS_LEN,
		       adpt->base + EMAC_MAX_FRAM_LEN_CTRL);

	emac_mac_tx_config(adpt);
	emac_mac_rx_config(adpt);
	emac_mac_dma_config(adpt);

	val = readl_relaxed(adpt->base + EMAC_AXI_MAST_CTRL);
	val &= ~(DATA_BYTE_SWAP | MAX_BOUND);
	val |= MAX_BTYPE;
	writel_relaxed(val, adpt->base + EMAC_AXI_MAST_CTRL);
	writel_relaxed(0, adpt->base + EMAC_CLK_GATE_CTRL);
	writel_relaxed(RX_UNCPL_INT_EN, adpt->base + EMAC_MISC_CTRL);
	wmb(); /* ensure that the MAC configuration is flushed to HW */
}

void emac_mac_reset(struct emac_adapter *adpt)
{
	writel_relaxed(0, adpt->base + EMAC_INT_MASK);
	writel_relaxed(DIS_INT, adpt->base + EMAC_INT_STATUS);

	emac_mac_stop(adpt);

	emac_reg_update32(adpt->base + EMAC_DMA_MAS_CTRL, 0, SOFT_RST);
	wmb(); /* ensure mac is fully reset */
	usleep_range(100, 150); /* reset may take upto 100usec */

	emac_reg_update32(adpt->base + EMAC_DMA_MAS_CTRL, 0, INT_RD_CLR_EN);
	wmb(); /* ensure the interrupt clear-on-read setting is flushed to HW */
}

void emac_mac_start(struct emac_adapter *adpt)
{
	struct emac_phy *phy = &adpt->phy;
	u32 mac, csr1;

	/* enable tx queue */
	if (adpt->tx_q_cnt && (adpt->tx_q_cnt <= EMAC_MAX_TX_QUEUES))
		emac_reg_update32(adpt->base + EMAC_TXQ_CTRL_0, 0, TXQ_EN);

	/* enable rx queue */
	if (adpt->rx_q_cnt && (adpt->rx_q_cnt <= EMAC_MAX_RX_QUEUES))
		emac_reg_update32(adpt->base + EMAC_RXQ_CTRL_0, 0, RXQ_EN);

	/* enable mac control */
	mac = readl_relaxed(adpt->base + EMAC_MAC_CTRL);
	csr1 = readl_relaxed(adpt->csr + EMAC_EMAC_WRAPPER_CSR1);

	mac |= TXEN | RXEN;     /* enable RX/TX */

	/* enable RX/TX Flow Control */
	switch (phy->cur_fc_mode) {
	case EMAC_FC_FULL:
		mac |= (TXFC | RXFC);
		break;
	case EMAC_FC_RX_PAUSE:
		mac |= RXFC;
		break;
	case EMAC_FC_TX_PAUSE:
		mac |= TXFC;
		break;
	default:
		break;
	}

	/* setup link speed */
	mac &= ~SPEED_BMSK;
	switch (phy->link_speed) {
	case EMAC_LINK_SPEED_1GB_FULL:
		mac |= ((emac_mac_speed_1000 << SPEED_SHFT) & SPEED_BMSK);
		csr1 |= FREQ_MODE;
		break;
	default:
		mac |= ((emac_mac_speed_10_100 << SPEED_SHFT) & SPEED_BMSK);
		csr1 &= ~FREQ_MODE;
		break;
	}

	switch (phy->link_speed) {
	case EMAC_LINK_SPEED_1GB_FULL:
	case EMAC_LINK_SPEED_100_FULL:
	case EMAC_LINK_SPEED_10_FULL:
		mac |= FULLD;
		break;
	default:
		mac &= ~FULLD;
	}

	/* other parameters */
	mac |= (CRCE | PCRCE);
	mac |= ((adpt->preamble << PRLEN_SHFT) & PRLEN_BMSK);
	mac |= BROAD_EN;
	mac |= FLCHK;
	mac &= ~RX_CHKSUM_EN;
	mac &= ~(HUGEN | VLAN_STRIP | TPAUSE | SIMR | HUGE | MULTI_ALL |
		 DEBUG_MODE | SINGLE_PAUSE_MODE);

	writel_relaxed(csr1, adpt->csr + EMAC_EMAC_WRAPPER_CSR1);

	writel_relaxed(mac, adpt->base + EMAC_MAC_CTRL);

	/* enable interrupt read clear, low power sleep mode and
	 * the irq moderators
	 */

	writel_relaxed(adpt->irq_mod, adpt->base + EMAC_IRQ_MOD_TIM_INIT);
	writel_relaxed(INT_RD_CLR_EN | LPW_MODE | IRQ_MODERATOR_EN |
			IRQ_MODERATOR2_EN, adpt->base + EMAC_DMA_MAS_CTRL);

	emac_mac_mode_config(adpt);

	emac_reg_update32(adpt->base + EMAC_ATHR_HEADER_CTRL,
			  (HEADER_ENABLE | HEADER_CNT_EN), 0);

	emac_reg_update32(adpt->csr + EMAC_EMAC_WRAPPER_CSR2, 0, WOL_EN);
	wmb(); /* ensure that MAC setting are flushed to HW */
}

void emac_mac_stop(struct emac_adapter *adpt)
{
	emac_reg_update32(adpt->base + EMAC_RXQ_CTRL_0, RXQ_EN, 0);
	emac_reg_update32(adpt->base + EMAC_TXQ_CTRL_0, TXQ_EN, 0);
	emac_reg_update32(adpt->base + EMAC_MAC_CTRL, (TXEN | RXEN), 0);
	wmb(); /* ensure mac is stopped before we proceed */
	usleep_range(1000, 1050); /* stopping may take upto 1msec */
}

/* set MAC address */
void emac_mac_addr_clear(struct emac_adapter *adpt, u8 *addr)
{
	u32 sta;

	/* for example: 00-A0-C6-11-22-33
	 * 0<-->C6112233, 1<-->00A0.
	 */

	/* low 32bit word */
	sta = (((u32)addr[2]) << 24) | (((u32)addr[3]) << 16) |
	      (((u32)addr[4]) << 8)  | (((u32)addr[5]));
	writel_relaxed(sta, adpt->base + EMAC_MAC_STA_ADDR0);

	/* hight 32bit word */
	sta = (((u32)addr[0]) << 8) | (((u32)addr[1]));
	writel_relaxed(sta, adpt->base + EMAC_MAC_STA_ADDR1);
	wmb(); /* ensure that the MAC address is flushed to HW */
}

/* Read one entry from the HW tx timestamp FIFO */
static bool emac_mac_tx_ts_read(struct emac_adapter *adpt,
				struct emac_tx_ts *ts)
{
	u32 ts_idx;

	ts_idx = readl_relaxed(adpt->csr + EMAC_EMAC_WRAPPER_TX_TS_INX);

	if (ts_idx & EMAC_WRAPPER_TX_TS_EMPTY)
		return false;

	ts->ns = readl_relaxed(adpt->csr + EMAC_EMAC_WRAPPER_TX_TS_LO);
	ts->sec = readl_relaxed(adpt->csr + EMAC_EMAC_WRAPPER_TX_TS_HI);
	ts->ts_idx = ts_idx & EMAC_WRAPPER_TX_TS_INX_BMSK;

	return true;
}

/* Free all descriptors of given transmit queue */
static void emac_tx_q_descs_free(struct emac_adapter *adpt,
				 struct emac_tx_queue *tx_q)
{
	size_t size;
	int i;

	/* ring already cleared, nothing to do */
	if (!tx_q->tpd.tpbuff)
		return;

	for (i = 0; i < tx_q->tpd.count; i++) {
		struct emac_buffer *tpbuf = GET_TPD_BUFFER(tx_q, i);

		if (tpbuf->dma) {
			dma_unmap_single(adpt->netdev->dev.parent, tpbuf->dma,
					 tpbuf->length, DMA_TO_DEVICE);
			tpbuf->dma = 0;
		}
		if (tpbuf->skb) {
			dev_kfree_skb_any(tpbuf->skb);
			tpbuf->skb = NULL;
		}
	}

	size = sizeof(struct emac_buffer) * tx_q->tpd.count;
	memset(tx_q->tpd.tpbuff, 0, size);

	/* clear the descriptor ring */
	memset(tx_q->tpd.v_addr, 0, tx_q->tpd.size);

	tx_q->tpd.consume_idx = 0;
	tx_q->tpd.produce_idx = 0;
}

static void emac_tx_q_descs_free_all(struct emac_adapter *adpt)
{
	int i;

	for (i = 0; i < adpt->tx_q_cnt; i++)
		emac_tx_q_descs_free(adpt, &adpt->tx_q[i]);
	netdev_reset_queue(adpt->netdev);
}

/* Free all descriptors of given receive queue */
static void emac_rx_q_free_descs(struct emac_adapter *adpt,
				 struct emac_rx_queue *rx_q)
{
	struct device *dev = adpt->netdev->dev.parent;
	size_t size;
	int i;

	/* ring already cleared, nothing to do */
	if (!rx_q->rfd.rfbuff)
		return;

	for (i = 0; i < rx_q->rfd.count; i++) {
		struct emac_buffer *rfbuf = GET_RFD_BUFFER(rx_q, i);

		if (rfbuf->dma) {
			dma_unmap_single(dev, rfbuf->dma, rfbuf->length,
					 DMA_FROM_DEVICE);
			rfbuf->dma = 0;
		}
		if (rfbuf->skb) {
			dev_kfree_skb(rfbuf->skb);
			rfbuf->skb = NULL;
		}
	}

	size =  sizeof(struct emac_buffer) * rx_q->rfd.count;
	memset(rx_q->rfd.rfbuff, 0, size);

	/* clear the descriptor rings */
	memset(rx_q->rrd.v_addr, 0, rx_q->rrd.size);
	rx_q->rrd.produce_idx = 0;
	rx_q->rrd.consume_idx = 0;

	memset(rx_q->rfd.v_addr, 0, rx_q->rfd.size);
	rx_q->rfd.produce_idx = 0;
	rx_q->rfd.consume_idx = 0;
}

static void emac_rx_q_free_descs_all(struct emac_adapter *adpt)
{
	int i;

	for (i = 0; i < adpt->rx_q_cnt; i++)
		emac_rx_q_free_descs(adpt, &adpt->rx_q[i]);
}

/* Free all buffers associated with given transmit queue */
static void emac_tx_q_bufs_free(struct emac_adapter *adpt, int que_idx)
{
	struct emac_tx_queue *tx_q = &adpt->tx_q[que_idx];

	emac_tx_q_descs_free(adpt, tx_q);

	kfree(tx_q->tpd.tpbuff);
	tx_q->tpd.tpbuff = NULL;
	tx_q->tpd.v_addr = NULL;
	tx_q->tpd.p_addr = 0;
	tx_q->tpd.size = 0;
}

static void emac_tx_q_bufs_free_all(struct emac_adapter *adpt)
{
	int i;

	for (i = 0; i < adpt->tx_q_cnt; i++)
		emac_tx_q_bufs_free(adpt, i);
}

/* Allocate TX descriptor ring for the given transmit queue */
static int emac_tx_q_desc_alloc(struct emac_adapter *adpt,
				struct emac_tx_queue *tx_q)
{
	struct emac_ring_header *ring_header = &adpt->ring_header;
	size_t size;

	size = sizeof(struct emac_buffer) * tx_q->tpd.count;
	tx_q->tpd.tpbuff = kzalloc(size, GFP_KERNEL);
	if (!tx_q->tpd.tpbuff)
		return -ENOMEM;

	tx_q->tpd.size = tx_q->tpd.count * (adpt->tpd_size * 4);
	tx_q->tpd.p_addr = ring_header->p_addr + ring_header->used;
	tx_q->tpd.v_addr = ring_header->v_addr + ring_header->used;
	ring_header->used += ALIGN(tx_q->tpd.size, 8);
	tx_q->tpd.produce_idx = 0;
	tx_q->tpd.consume_idx = 0;

	return 0;
}

static int emac_tx_q_desc_alloc_all(struct emac_adapter *adpt)
{
	int retval = 0;
	int i;

	for (i = 0; i < adpt->tx_q_cnt; i++) {
		retval = emac_tx_q_desc_alloc(adpt, &adpt->tx_q[i]);
		if (retval)
			break;
	}

	if (retval) {
		netdev_err(adpt->netdev, "error: Tx Queue %u alloc failed\n",
			   i);
		for (i--; i > 0; i--)
			emac_tx_q_bufs_free(adpt, i);
	}

	return retval;
}

/* Free all buffers associated with given transmit queue */
static void emac_rx_q_free_bufs(struct emac_adapter *adpt,
				struct emac_rx_queue *rx_q)
{
	emac_rx_q_free_descs(adpt, rx_q);

	kfree(rx_q->rfd.rfbuff);
	rx_q->rfd.rfbuff = NULL;

	rx_q->rfd.v_addr = NULL;
	rx_q->rfd.p_addr  = 0;
	rx_q->rfd.size   = 0;

	rx_q->rrd.v_addr = NULL;
	rx_q->rrd.p_addr  = 0;
	rx_q->rrd.size   = 0;
}

static void emac_rx_q_free_bufs_all(struct emac_adapter *adpt)
{
	int i;

	for (i = 0; i < adpt->rx_q_cnt; i++)
		emac_rx_q_free_bufs(adpt, &adpt->rx_q[i]);
}

/* Allocate RX descriptor rings for the given receive queue */
static int emac_rx_descs_alloc(struct emac_adapter *adpt,
			       struct emac_rx_queue *rx_q)
{
	struct emac_ring_header *ring_header = &adpt->ring_header;
	unsigned long size;

	size = sizeof(struct emac_buffer) * rx_q->rfd.count;
	rx_q->rfd.rfbuff = kzalloc(size, GFP_KERNEL);
	if (!rx_q->rfd.rfbuff)
		return -ENOMEM;

	rx_q->rrd.size = rx_q->rrd.count * (adpt->rrd_size * 4);
	rx_q->rfd.size = rx_q->rfd.count * (adpt->rfd_size * 4);

	rx_q->rrd.p_addr = ring_header->p_addr + ring_header->used;
	rx_q->rrd.v_addr = ring_header->v_addr + ring_header->used;
	ring_header->used += ALIGN(rx_q->rrd.size, 8);

	rx_q->rfd.p_addr = ring_header->p_addr + ring_header->used;
	rx_q->rfd.v_addr = ring_header->v_addr + ring_header->used;
	ring_header->used += ALIGN(rx_q->rfd.size, 8);

	rx_q->rrd.produce_idx = 0;
	rx_q->rrd.consume_idx = 0;

	rx_q->rfd.produce_idx = 0;
	rx_q->rfd.consume_idx = 0;

	return 0;
}

static int emac_rx_descs_allocs_all(struct emac_adapter *adpt)
{
	int retval = 0;
	int i;

	for (i = 0; i < adpt->rx_q_cnt; i++) {
		retval = emac_rx_descs_alloc(adpt, &adpt->rx_q[i]);
		if (retval)
			break;
	}

	if (retval) {
		netdev_err(adpt->netdev, "error: Rx Queue %d alloc failed\n",
			   i);
		for (i--; i > 0; i--)
			emac_rx_q_free_bufs(adpt, &adpt->rx_q[i]);
	}

	return retval;
}

/* Allocate all TX and RX descriptor rings */
int emac_mac_rx_tx_rings_alloc_all(struct emac_adapter *adpt)
{
	struct emac_ring_header *ring_header = &adpt->ring_header;
	int num_tques = adpt->tx_q_cnt;
	int num_rques = adpt->rx_q_cnt;
	unsigned int num_tx_descs = adpt->tx_desc_cnt;
	unsigned int num_rx_descs = adpt->rx_desc_cnt;
	struct device *dev = adpt->netdev->dev.parent;
	int retval, que_idx;

	for (que_idx = 0; que_idx < adpt->tx_q_cnt; que_idx++)
		adpt->tx_q[que_idx].tpd.count = adpt->tx_desc_cnt;

	for (que_idx = 0; que_idx < adpt->rx_q_cnt; que_idx++) {
		adpt->rx_q[que_idx].rrd.count = adpt->rx_desc_cnt;
		adpt->rx_q[que_idx].rfd.count = adpt->rx_desc_cnt;
	}

	/* Ring DMA buffer. Each ring may need up to 8 bytes for alignment,
	 * hence the additional padding bytes are allocated.
	 */
	ring_header->size =
		num_tques * num_tx_descs * (adpt->tpd_size * 4) +
		num_rques * num_rx_descs * (adpt->rfd_size * 4) +
		num_rques * num_rx_descs * (adpt->rrd_size * 4) +
		num_tques * 8 + num_rques * 2 * 8;

	netif_info(adpt, ifup, adpt->netdev,
		   "TX queues %d, TX descriptors %d\n", num_tques,
		   num_tx_descs);
	netif_info(adpt, ifup, adpt->netdev,
		   "RX queues %d, Rx descriptors %d\n", num_rques,
		   num_rx_descs);

	ring_header->used = 0;
	ring_header->v_addr = dma_alloc_coherent(dev, ring_header->size,
						 &ring_header->p_addr,
						 GFP_KERNEL);
	if (!ring_header->v_addr)
		return -ENOMEM;

	memset(ring_header->v_addr, 0, ring_header->size);
	ring_header->used = ALIGN(ring_header->p_addr, 8) - ring_header->p_addr;

	retval = emac_tx_q_desc_alloc_all(adpt);
	if (retval)
		goto err_alloc_tx;

	retval = emac_rx_descs_allocs_all(adpt);
	if (retval)
		goto err_alloc_rx;

	return 0;

err_alloc_rx:
	emac_tx_q_bufs_free_all(adpt);
err_alloc_tx:
	dma_free_coherent(dev, ring_header->size,
			  ring_header->v_addr, ring_header->p_addr);

	ring_header->v_addr = NULL;
	ring_header->p_addr = 0;
	ring_header->size   = 0;
	ring_header->used   = 0;

	return retval;
}

/* Free all TX and RX descriptor rings */
void emac_mac_rx_tx_rings_free_all(struct emac_adapter *adpt)
{
	struct emac_ring_header *ring_header = &adpt->ring_header;
	struct device *dev = adpt->netdev->dev.parent;

	emac_tx_q_bufs_free_all(adpt);
	emac_rx_q_free_bufs_all(adpt);

	dma_free_coherent(dev, ring_header->size,
			  ring_header->v_addr, ring_header->p_addr);

	ring_header->v_addr = NULL;
	ring_header->p_addr = 0;
	ring_header->size   = 0;
	ring_header->used   = 0;
}

/* Initialize descriptor rings */
static void emac_mac_rx_tx_ring_reset_all(struct emac_adapter *adpt)
{
	int i, j;

	for (i = 0; i < adpt->tx_q_cnt; i++) {
		struct emac_tx_queue *tx_q = &adpt->tx_q[i];
		struct emac_buffer *tpbuf = tx_q->tpd.tpbuff;

		tx_q->tpd.produce_idx = 0;
		tx_q->tpd.consume_idx = 0;
		for (j = 0; j < tx_q->tpd.count; j++)
			tpbuf[j].dma = 0;
	}

	for (i = 0; i < adpt->rx_q_cnt; i++) {
		struct emac_rx_queue *rx_q = &adpt->rx_q[i];
		struct emac_buffer *rfbuf = rx_q->rfd.rfbuff;

		rx_q->rrd.produce_idx = 0;
		rx_q->rrd.consume_idx = 0;
		rx_q->rfd.produce_idx = 0;
		rx_q->rfd.consume_idx = 0;
		for (j = 0; j < rx_q->rfd.count; j++)
			rfbuf[j].dma = 0;
	}
}

/* Configure Receive Side Scaling (RSS) */
static void emac_rss_config(struct emac_adapter *adpt)
{
	static const u8 key[40] = {
		0x6D, 0x5A, 0x56, 0xDA, 0x25, 0x5B, 0x0E, 0xC2,
		0x41, 0x67, 0x25, 0x3D, 0x43, 0xA3, 0x8F, 0xB0,
		0xD0, 0xCA, 0x2B, 0xCB, 0xAE, 0x7B, 0x30, 0xB4,
		0x77, 0xCB, 0x2D, 0xA3, 0x80, 0x30, 0xF2, 0x0C,
		0x6A, 0x42, 0xB7, 0x3B, 0xBE, 0xAC, 0x01, 0xFA
	};
	u32 reta = 0;
	int i, j;

	if (adpt->rx_q_cnt == 1)
		return;

	if (!adpt->rss_initialized) {
		adpt->rss_initialized = true;
		/* initialize rss hash type and idt table size */
		adpt->rss_hstype      = EMAC_RSS_HSTYP_ALL_EN;
		adpt->rss_idt_size    = EMAC_RSS_IDT_SIZE;

		/* Fill out RSS key */
		memcpy(adpt->rss_key, key, sizeof(adpt->rss_key));

		/* Fill out redirection table */
		memset(adpt->rss_idt, 0x0, sizeof(adpt->rss_idt));
		for (i = 0, j = 0; i < EMAC_RSS_IDT_SIZE; i++, j++) {
			if (j == adpt->rx_q_cnt)
				j = 0;
			if (j > 1)
				reta |= (j << ((i & 7) * 4));
			if ((i & 7) == 7) {
				adpt->rss_idt[(i >> 3)] = reta;
				reta = 0;
			}
		}
	}

	emac_mac_rss_config(adpt);
}

/* Produce new receive free descriptor */
static void emac_mac_rx_rfd_create(struct emac_adapter *adpt,
				   struct emac_rx_queue *rx_q,
				   union emac_rfd *rfd)
{
	u32 *hw_rfd = EMAC_RFD(rx_q, adpt->rfd_size,
			       rx_q->rfd.produce_idx);

	*(hw_rfd++) = rfd->word[0];
	*hw_rfd = rfd->word[1];

	if (++rx_q->rfd.produce_idx == rx_q->rfd.count)
		rx_q->rfd.produce_idx = 0;
}

/* Fill up receive queue's RFD with preallocated receive buffers */
static int emac_mac_rx_descs_refill(struct emac_adapter *adpt,
				    struct emac_rx_queue *rx_q)
{
	struct emac_buffer *curr_rxbuf;
	struct emac_buffer *next_rxbuf;
	union emac_rfd rfd;
	struct sk_buff *skb;
	void *skb_data = NULL;
	int count = 0;
	u32 next_produce_idx;

	next_produce_idx = rx_q->rfd.produce_idx;
	if (++next_produce_idx == rx_q->rfd.count)
		next_produce_idx = 0;
	curr_rxbuf = GET_RFD_BUFFER(rx_q, rx_q->rfd.produce_idx);
	next_rxbuf = GET_RFD_BUFFER(rx_q, next_produce_idx);

	/* this always has a blank rx_buffer*/
	while (!next_rxbuf->dma) {
		skb = dev_alloc_skb(adpt->rxbuf_size + NET_IP_ALIGN);
		if (!skb)
			break;

		/* Make buffer alignment 2 beyond a 16 byte boundary
		 * this will result in a 16 byte aligned IP header after
		 * the 14 byte MAC header is removed
		 */
		skb_reserve(skb, NET_IP_ALIGN);
		skb_data = skb->data;
		curr_rxbuf->skb = skb;
		curr_rxbuf->length = adpt->rxbuf_size;
		curr_rxbuf->dma = dma_map_single(adpt->netdev->dev.parent,
						 skb_data, curr_rxbuf->length,
						 DMA_FROM_DEVICE);
		rfd.addr = curr_rxbuf->dma;
		emac_mac_rx_rfd_create(adpt, rx_q, &rfd);
		next_produce_idx = rx_q->rfd.produce_idx;
		if (++next_produce_idx == rx_q->rfd.count)
			next_produce_idx = 0;

		curr_rxbuf = GET_RFD_BUFFER(rx_q, rx_q->rfd.produce_idx);
		next_rxbuf = GET_RFD_BUFFER(rx_q, next_produce_idx);
		count++;
	}

	if (count) {
		u32 prod_idx = (rx_q->rfd.produce_idx << rx_q->produce_shft) &
				rx_q->produce_mask;
		wmb(); /* ensure that the descriptors are properly set */
		emac_reg_update32(adpt->base + rx_q->produce_reg,
				  rx_q->produce_mask, prod_idx);
		wmb(); /* ensure that the producer's index is flushed to HW */
		netif_dbg(adpt, rx_status, adpt->netdev,
			  "RX[%d]: prod idx 0x%x\n", rx_q->que_idx,
			  rx_q->rfd.produce_idx);
	}

	return count;
}

/* Bringup the interface/HW */
int emac_mac_up(struct emac_adapter *adpt)
{
	struct emac_phy *phy = &adpt->phy;

	struct net_device *netdev = adpt->netdev;
	int retval = 0;
	int i;

	emac_mac_rx_tx_ring_reset_all(adpt);
	emac_rx_mode_set(netdev);

	emac_mac_config(adpt);
	emac_rss_config(adpt);

	retval = emac_phy_up(adpt);
	if (retval)
		return retval;

	for (i = 0; phy->uses_gpios && i < EMAC_GPIO_CNT; i++) {
		retval = gpio_request(adpt->gpio[i], emac_gpio_name[i]);
		if (retval) {
			netdev_err(adpt->netdev,
				   "error:%d on gpio_request(%d:%s)\n",
				   retval, adpt->gpio[i], emac_gpio_name[i]);
			while (--i >= 0)
				gpio_free(adpt->gpio[i]);
			goto err_request_gpio;
		}
	}

	for (i = 0; i < EMAC_IRQ_CNT; i++) {
		struct emac_irq			*irq = &adpt->irq[i];
		const struct emac_irq_config	*irq_cfg = &emac_irq_cfg_tbl[i];

		if (!irq->irq)
			continue;

		retval = request_irq(irq->irq, irq_cfg->handler,
				     irq_cfg->irqflags, irq_cfg->name, irq);
		if (retval) {
			netdev_err(adpt->netdev,
				   "error:%d on request_irq(%d:%s flags:0x%lx)\n",
				   retval, irq->irq, irq_cfg->name,
				   irq_cfg->irqflags);
			while (--i >= 0)
				if (adpt->irq[i].irq)
					free_irq(adpt->irq[i].irq,
						 &adpt->irq[i]);
			goto err_request_irq;
		}
	}

	for (i = 0; i < adpt->rx_q_cnt; i++)
		emac_mac_rx_descs_refill(adpt, &adpt->rx_q[i]);

	for (i = 0; i < adpt->rx_q_cnt; i++)
		napi_enable(&adpt->rx_q[i].napi);

	emac_mac_irq_enable(adpt);

	netif_start_queue(netdev);
	clear_bit(EMAC_STATUS_DOWN, &adpt->status);

	/* check link status */
	set_bit(EMAC_STATUS_TASK_LSC_REQ, &adpt->status);
	adpt->link_chk_timeout = jiffies + EMAC_TRY_LINK_TIMEOUT;
	mod_timer(&adpt->timers, jiffies);

	return retval;

err_request_irq:
	for (i = 0; adpt->phy.uses_gpios && i < EMAC_GPIO_CNT; i++)
		gpio_free(adpt->gpio[i]);
err_request_gpio:
	emac_phy_down(adpt);
	return retval;
}

/* Bring down the interface/HW */
void emac_mac_down(struct emac_adapter *adpt, bool reset)
{
	struct net_device *netdev = adpt->netdev;
	struct emac_phy *phy = &adpt->phy;

	unsigned long flags;
	int i;

	set_bit(EMAC_STATUS_DOWN, &adpt->status);

	netif_stop_queue(netdev);
	netif_carrier_off(netdev);
	emac_mac_irq_disable(adpt);

	for (i = 0; i < adpt->rx_q_cnt; i++)
		napi_disable(&adpt->rx_q[i].napi);

	emac_phy_down(adpt);

	for (i = 0; i < EMAC_IRQ_CNT; i++)
		if (adpt->irq[i].irq)
			free_irq(adpt->irq[i].irq, &adpt->irq[i]);

	for (i = 0; phy->uses_gpios && i < EMAC_GPIO_CNT; i++)
		gpio_free(adpt->gpio[i]);

	clear_bit(EMAC_STATUS_TASK_LSC_REQ, &adpt->status);
	clear_bit(EMAC_STATUS_TASK_REINIT_REQ, &adpt->status);
	clear_bit(EMAC_STATUS_TASK_CHK_SGMII_REQ, &adpt->status);
	del_timer_sync(&adpt->timers);

	cancel_work_sync(&adpt->tx_ts_task);
	spin_lock_irqsave(&adpt->tx_ts_lock, flags);
	__skb_queue_purge(&adpt->tx_ts_pending_queue);
	__skb_queue_purge(&adpt->tx_ts_ready_queue);
	spin_unlock_irqrestore(&adpt->tx_ts_lock, flags);

	if (reset)
		emac_mac_reset(adpt);

	pm_runtime_put_noidle(netdev->dev.parent);
	phy->link_speed = EMAC_LINK_SPEED_UNKNOWN;
	emac_tx_q_descs_free_all(adpt);
	emac_rx_q_free_descs_all(adpt);
}

/* Consume next received packet descriptor */
static bool emac_rx_process_rrd(struct emac_adapter *adpt,
				struct emac_rx_queue *rx_q,
				struct emac_rrd *rrd)
{
	u32 *hw_rrd = EMAC_RRD(rx_q, adpt->rrd_size,
			       rx_q->rrd.consume_idx);

	/* If time stamping is enabled, it will be added in the beginning of
	 * the hw rrd (hw_rrd). In sw rrd (rrd), 32bit words 4 & 5 are reserved
	 * for the time stamp; hence the conversion.
	 * Also, read the rrd word with update flag first; read rest of rrd
	 * only if update flag is set.
	 */
	if (adpt->timestamp_en)
		rrd->word[3] = *(hw_rrd + 5);
	else
		rrd->word[3] = *(hw_rrd + 3);
	rmb(); /* ensure hw receive returned descriptor timestamp is read */

	if (!RRD_UPDT(rrd))
		return false;

	if (adpt->timestamp_en) {
		rrd->word[4] = *(hw_rrd++);
		rrd->word[5] = *(hw_rrd++);
	} else {
		rrd->word[4] = 0;
		rrd->word[5] = 0;
	}

	rrd->word[0] = *(hw_rrd++);
	rrd->word[1] = *(hw_rrd++);
	rrd->word[2] = *(hw_rrd++);
	rmb(); /* ensure descriptor is read */

	netif_dbg(adpt, rx_status, adpt->netdev,
		  "RX[%d]:SRRD[%x]: %x:%x:%x:%x:%x:%x\n",
		  rx_q->que_idx, rx_q->rrd.consume_idx, rrd->word[0],
		  rrd->word[1], rrd->word[2], rrd->word[3],
		  rrd->word[4], rrd->word[5]);

	if (unlikely(RRD_NOR(rrd) != 1)) {
		netdev_err(adpt->netdev,
			   "error: multi-RFD not support yet! nor:%lu\n",
			   RRD_NOR(rrd));
	}

	/* mark rrd as processed */
	RRD_UPDT_SET(rrd, 0);
	*hw_rrd = rrd->word[3];

	if (++rx_q->rrd.consume_idx == rx_q->rrd.count)
		rx_q->rrd.consume_idx = 0;

	return true;
}

/* Produce new transmit descriptor */
static bool emac_tx_tpd_create(struct emac_adapter *adpt,
			       struct emac_tx_queue *tx_q, struct emac_tpd *tpd)
{
	u32 *hw_tpd;

	tx_q->tpd.last_produce_idx = tx_q->tpd.produce_idx;
	hw_tpd = EMAC_TPD(tx_q, adpt->tpd_size, tx_q->tpd.produce_idx);

	if (++tx_q->tpd.produce_idx == tx_q->tpd.count)
		tx_q->tpd.produce_idx = 0;

	*(hw_tpd++) = tpd->word[0];
	*(hw_tpd++) = tpd->word[1];
	*(hw_tpd++) = tpd->word[2];
	*hw_tpd = tpd->word[3];

	netif_dbg(adpt, tx_done, adpt->netdev, "TX[%d]:STPD[%x]: %x:%x:%x:%x\n",
		  tx_q->que_idx, tx_q->tpd.last_produce_idx, tpd->word[0],
		  tpd->word[1], tpd->word[2], tpd->word[3]);

	return true;
}

/* Mark the last transmit descriptor as such (for the transmit packet) */
static void emac_tx_tpd_mark_last(struct emac_adapter *adpt,
				  struct emac_tx_queue *tx_q)
{
	u32 tmp_tpd;
	u32 *hw_tpd = EMAC_TPD(tx_q, adpt->tpd_size,
			     tx_q->tpd.last_produce_idx);

	tmp_tpd = *(hw_tpd + 1);
	tmp_tpd |= EMAC_TPD_LAST_FRAGMENT;
	*(hw_tpd + 1) = tmp_tpd;
}

void emac_tx_tpd_ts_save(struct emac_adapter *adpt, struct emac_tx_queue *tx_q)
{
	u32 tmp_tpd;
	u32 *hw_tpd = EMAC_TPD(tx_q, adpt->tpd_size,
			       tx_q->tpd.last_produce_idx);

	tmp_tpd = *(hw_tpd + 3);
	tmp_tpd |= EMAC_TPD_TSTAMP_SAVE;
	*(hw_tpd + 3) = tmp_tpd;
}

static void emac_rx_rfd_clean(struct emac_rx_queue *rx_q,
			      struct emac_rrd *rrd)
{
	struct emac_buffer *rfbuf = rx_q->rfd.rfbuff;
	u32 consume_idx = RRD_SI(rrd);
	int i;

	for (i = 0; i < RRD_NOR(rrd); i++) {
		rfbuf[consume_idx].skb = NULL;
		if (++consume_idx == rx_q->rfd.count)
			consume_idx = 0;
	}

	rx_q->rfd.consume_idx = consume_idx;
	rx_q->rfd.process_idx = consume_idx;
}

/* proper lock must be acquired before polling */
static void emac_tx_ts_poll(struct emac_adapter *adpt)
{
	struct sk_buff_head *pending_q = &adpt->tx_ts_pending_queue;
	struct sk_buff_head *q = &adpt->tx_ts_ready_queue;
	struct sk_buff *skb, *skb_tmp;
	struct emac_tx_ts tx_ts;

	while (emac_mac_tx_ts_read(adpt, &tx_ts)) {
		bool found = false;

		adpt->tx_ts_stats.rx++;

		skb_queue_walk_safe(pending_q, skb, skb_tmp) {
			if (EMAC_SKB_CB(skb)->tpd_idx == tx_ts.ts_idx) {
				struct sk_buff *pskb;

				EMAC_TX_TS_CB(skb)->sec = tx_ts.sec;
				EMAC_TX_TS_CB(skb)->ns = tx_ts.ns;
				/* the tx timestamps for all the pending
				 * packets before this one are lost
				 */
				while ((pskb = __skb_dequeue(pending_q))
				       != skb) {
					EMAC_TX_TS_CB(pskb)->sec = 0;
					EMAC_TX_TS_CB(pskb)->ns = 0;
					__skb_queue_tail(q, pskb);
					adpt->tx_ts_stats.lost++;
				}
				__skb_queue_tail(q, skb);
				found = true;
				break;
			}
		}

		if (!found) {
			netif_dbg(adpt, tx_done, adpt->netdev,
				  "no entry(tpd=%d) found, drop tx timestamp\n",
				  tx_ts.ts_idx);
			adpt->tx_ts_stats.drop++;
		}
	}

	skb_queue_walk_safe(pending_q, skb, skb_tmp) {
		/* No packet after this one expires */
		if (time_is_after_jiffies(EMAC_SKB_CB(skb)->jiffies +
					  msecs_to_jiffies(100)))
			break;
		adpt->tx_ts_stats.timeout++;
		netif_dbg(adpt, tx_done, adpt->netdev,
			  "tx timestamp timeout: tpd_idx=%d\n",
			  EMAC_SKB_CB(skb)->tpd_idx);

		__skb_unlink(skb, pending_q);
		EMAC_TX_TS_CB(skb)->sec = 0;
		EMAC_TX_TS_CB(skb)->ns = 0;
		__skb_queue_tail(q, skb);
	}
}

static void emac_schedule_tx_ts_task(struct emac_adapter *adpt)
{
	if (test_bit(EMAC_STATUS_DOWN, &adpt->status))
		return;

	if (schedule_work(&adpt->tx_ts_task))
		adpt->tx_ts_stats.sched++;
}

void emac_mac_tx_ts_periodic_routine(struct work_struct *work)
{
	struct emac_adapter *adpt = container_of(work, struct emac_adapter,
						 tx_ts_task);
	struct sk_buff *skb;
	struct sk_buff_head q;
	unsigned long flags;

	adpt->tx_ts_stats.poll++;

	__skb_queue_head_init(&q);

	while (1) {
		spin_lock_irqsave(&adpt->tx_ts_lock, flags);
		if (adpt->tx_ts_pending_queue.qlen)
			emac_tx_ts_poll(adpt);
		skb_queue_splice_tail_init(&adpt->tx_ts_ready_queue, &q);
		spin_unlock_irqrestore(&adpt->tx_ts_lock, flags);

		if (!q.qlen)
			break;

		while ((skb = __skb_dequeue(&q))) {
			struct emac_tx_ts_cb *cb = EMAC_TX_TS_CB(skb);

			if (cb->sec || cb->ns) {
				struct skb_shared_hwtstamps ts;

				ts.hwtstamp = ktime_set(cb->sec, cb->ns);
				skb_tstamp_tx(skb, &ts);
				adpt->tx_ts_stats.deliver++;
			}
			dev_kfree_skb_any(skb);
		}
	}

	if (adpt->tx_ts_pending_queue.qlen)
		emac_schedule_tx_ts_task(adpt);
}

/* Push the received skb to upper layers */
static void emac_receive_skb(struct emac_rx_queue *rx_q,
			     struct sk_buff *skb,
			     u16 vlan_tag, bool vlan_flag)
{
	if (vlan_flag) {
		u16 vlan;

		EMAC_TAG_TO_VLAN(vlan_tag, vlan);
		__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), vlan);
	}

	napi_gro_receive(&rx_q->napi, skb);
}

/* Process receive event */
void emac_mac_rx_process(struct emac_adapter *adpt, struct emac_rx_queue *rx_q,
			 int *num_pkts, int max_pkts)
{
	struct net_device *netdev  = adpt->netdev;

	struct emac_rrd rrd;
	struct emac_buffer *rfbuf;
	struct sk_buff *skb;

	u32 hw_consume_idx, num_consume_pkts;
	unsigned int count = 0;
	u32 proc_idx;
	u32 reg = readl_relaxed(adpt->base + rx_q->consume_reg);

	hw_consume_idx = (reg & rx_q->consume_mask) >> rx_q->consume_shft;
	num_consume_pkts = (hw_consume_idx >= rx_q->rrd.consume_idx) ?
		(hw_consume_idx -  rx_q->rrd.consume_idx) :
		(hw_consume_idx + rx_q->rrd.count - rx_q->rrd.consume_idx);

	do {
		if (!num_consume_pkts)
			break;

		if (!emac_rx_process_rrd(adpt, rx_q, &rrd))
			break;

		if (likely(RRD_NOR(&rrd) == 1)) {
			/* good receive */
			rfbuf = GET_RFD_BUFFER(rx_q, RRD_SI(&rrd));
			dma_unmap_single(adpt->netdev->dev.parent, rfbuf->dma,
					 rfbuf->length, DMA_FROM_DEVICE);
			rfbuf->dma = 0;
			skb = rfbuf->skb;
		} else {
			netdev_err(adpt->netdev,
				   "error: multi-RFD not support yet!\n");
			break;
		}
		emac_rx_rfd_clean(rx_q, &rrd);
		num_consume_pkts--;
		count++;

		/* Due to a HW issue in L4 check sum detection (UDP/TCP frags
		 * with DF set are marked as error), drop packets based on the
		 * error mask rather than the summary bit (ignoring L4F errors)
		 */
		if (rrd.word[EMAC_RRD_STATS_DW_IDX] & EMAC_RRD_ERROR) {
			netif_dbg(adpt, rx_status, adpt->netdev,
				  "Drop error packet[RRD: 0x%x:0x%x:0x%x:0x%x]\n",
				  rrd.word[0], rrd.word[1],
				  rrd.word[2], rrd.word[3]);

			dev_kfree_skb(skb);
			continue;
		}

		skb_put(skb, RRD_PKT_SIZE(&rrd) - ETH_FCS_LEN);
		skb->dev = netdev;
		skb->protocol = eth_type_trans(skb, skb->dev);
		if (netdev->features & NETIF_F_RXCSUM)
			skb->ip_summed = (RRD_L4F(&rrd) ?
					  CHECKSUM_NONE : CHECKSUM_UNNECESSARY);
		else
			skb_checksum_none_assert(skb);

		if (test_bit(EMAC_STATUS_TS_RX_EN, &adpt->status)) {
			struct skb_shared_hwtstamps *hwts = skb_hwtstamps(skb);

			hwts->hwtstamp = ktime_set(RRD_TS_HI(&rrd),
						   RRD_TS_LOW(&rrd));
		}

		emac_receive_skb(rx_q, skb, (u16)RRD_CVALN_TAG(&rrd),
				 (bool)RRD_CVTAG(&rrd));

		netdev->last_rx = jiffies;
		(*num_pkts)++;
	} while (*num_pkts < max_pkts);

	if (count) {
		proc_idx = (rx_q->rfd.process_idx << rx_q->process_shft) &
				rx_q->process_mask;
		wmb(); /* ensure that the descriptors are properly cleared */
		emac_reg_update32(adpt->base + rx_q->process_reg,
				  rx_q->process_mask, proc_idx);
		wmb(); /* ensure that RFD producer index is flushed to HW */
		netif_dbg(adpt, rx_status, adpt->netdev,
			  "RX[%d]: proc idx 0x%x\n", rx_q->que_idx,
			  rx_q->rfd.process_idx);

		emac_mac_rx_descs_refill(adpt, rx_q);
	}
}

/* Process transmit event */
void emac_mac_tx_process(struct emac_adapter *adpt, struct emac_tx_queue *tx_q)
{
	struct emac_buffer *tpbuf;
	u32 hw_consume_idx;
	u32 pkts_compl = 0, bytes_compl = 0;
	u32 reg = readl_relaxed(adpt->base + tx_q->consume_reg);

	hw_consume_idx = (reg & tx_q->consume_mask) >> tx_q->consume_shft;

	netif_dbg(adpt, tx_done, adpt->netdev, "TX[%d]: cons idx 0x%x\n",
		  tx_q->que_idx, hw_consume_idx);

	while (tx_q->tpd.consume_idx != hw_consume_idx) {
		tpbuf = GET_TPD_BUFFER(tx_q, tx_q->tpd.consume_idx);
		if (tpbuf->dma) {
			dma_unmap_single(adpt->netdev->dev.parent, tpbuf->dma,
					 tpbuf->length, DMA_TO_DEVICE);
			tpbuf->dma = 0;
		}

		if (tpbuf->skb) {
			pkts_compl++;
			bytes_compl += tpbuf->skb->len;
			dev_kfree_skb_irq(tpbuf->skb);
			tpbuf->skb = NULL;
		}

		if (++tx_q->tpd.consume_idx == tx_q->tpd.count)
			tx_q->tpd.consume_idx = 0;
	}

	if (pkts_compl || bytes_compl)
		netdev_completed_queue(adpt->netdev, pkts_compl, bytes_compl);
}

/* Initialize all queue data structures */
void emac_mac_rx_tx_ring_init_all(struct platform_device *pdev,
				  struct emac_adapter *adpt)
{
	int que_idx;

	adpt->tx_q_cnt = EMAC_DEF_TX_QUEUES;
	adpt->rx_q_cnt = EMAC_DEF_RX_QUEUES;

	for (que_idx = 0; que_idx < adpt->tx_q_cnt; que_idx++)
		adpt->tx_q[que_idx].que_idx = que_idx;

	for (que_idx = 0; que_idx < adpt->rx_q_cnt; que_idx++) {
		struct emac_rx_queue *rx_q = &adpt->rx_q[que_idx];

		rx_q->que_idx = que_idx;
		rx_q->netdev  = adpt->netdev;
	}

	switch (adpt->rx_q_cnt) {
	case 4:
		adpt->rx_q[3].produce_reg = EMAC_MAILBOX_13;
		adpt->rx_q[3].produce_mask = RFD3_PROD_IDX_BMSK;
		adpt->rx_q[3].produce_shft = RFD3_PROD_IDX_SHFT;

		adpt->rx_q[3].process_reg = EMAC_MAILBOX_13;
		adpt->rx_q[3].process_mask = RFD3_PROC_IDX_BMSK;
		adpt->rx_q[3].process_shft = RFD3_PROC_IDX_SHFT;

		adpt->rx_q[3].consume_reg = EMAC_MAILBOX_8;
		adpt->rx_q[3].consume_mask = RFD3_CONS_IDX_BMSK;
		adpt->rx_q[3].consume_shft = RFD3_CONS_IDX_SHFT;

		adpt->rx_q[3].irq = &adpt->irq[3];
		adpt->rx_q[3].intr = adpt->irq[3].mask & ISR_RX_PKT;

		/* fall through */
	case 3:
		adpt->rx_q[2].produce_reg = EMAC_MAILBOX_6;
		adpt->rx_q[2].produce_mask = RFD2_PROD_IDX_BMSK;
		adpt->rx_q[2].produce_shft = RFD2_PROD_IDX_SHFT;

		adpt->rx_q[2].process_reg = EMAC_MAILBOX_6;
		adpt->rx_q[2].process_mask = RFD2_PROC_IDX_BMSK;
		adpt->rx_q[2].process_shft = RFD2_PROC_IDX_SHFT;

		adpt->rx_q[2].consume_reg = EMAC_MAILBOX_7;
		adpt->rx_q[2].consume_mask = RFD2_CONS_IDX_BMSK;
		adpt->rx_q[2].consume_shft = RFD2_CONS_IDX_SHFT;

		adpt->rx_q[2].irq = &adpt->irq[2];
		adpt->rx_q[2].intr = adpt->irq[2].mask & ISR_RX_PKT;

		/* fall through */
	case 2:
		adpt->rx_q[1].produce_reg = EMAC_MAILBOX_5;
		adpt->rx_q[1].produce_mask = RFD1_PROD_IDX_BMSK;
		adpt->rx_q[1].produce_shft = RFD1_PROD_IDX_SHFT;

		adpt->rx_q[1].process_reg = EMAC_MAILBOX_5;
		adpt->rx_q[1].process_mask = RFD1_PROC_IDX_BMSK;
		adpt->rx_q[1].process_shft = RFD1_PROC_IDX_SHFT;

		adpt->rx_q[1].consume_reg = EMAC_MAILBOX_7;
		adpt->rx_q[1].consume_mask = RFD1_CONS_IDX_BMSK;
		adpt->rx_q[1].consume_shft = RFD1_CONS_IDX_SHFT;

		adpt->rx_q[1].irq = &adpt->irq[1];
		adpt->rx_q[1].intr = adpt->irq[1].mask & ISR_RX_PKT;

		/* fall through */
	case 1:
		adpt->rx_q[0].produce_reg = EMAC_MAILBOX_0;
		adpt->rx_q[0].produce_mask = RFD0_PROD_IDX_BMSK;
		adpt->rx_q[0].produce_shft = RFD0_PROD_IDX_SHFT;

		adpt->rx_q[0].process_reg = EMAC_MAILBOX_0;
		adpt->rx_q[0].process_mask = RFD0_PROC_IDX_BMSK;
		adpt->rx_q[0].process_shft = RFD0_PROC_IDX_SHFT;

		adpt->rx_q[0].consume_reg = EMAC_MAILBOX_3;
		adpt->rx_q[0].consume_mask = RFD0_CONS_IDX_BMSK;
		adpt->rx_q[0].consume_shft = RFD0_CONS_IDX_SHFT;

		adpt->rx_q[0].irq = &adpt->irq[0];
		adpt->rx_q[0].intr = adpt->irq[0].mask & ISR_RX_PKT;
		break;
	}

	switch (adpt->tx_q_cnt) {
	case 4:
		adpt->tx_q[3].produce_reg = EMAC_MAILBOX_11;
		adpt->tx_q[3].produce_mask = H3TPD_PROD_IDX_BMSK;
		adpt->tx_q[3].produce_shft = H3TPD_PROD_IDX_SHFT;

		adpt->tx_q[3].consume_reg = EMAC_MAILBOX_12;
		adpt->tx_q[3].consume_mask = H3TPD_CONS_IDX_BMSK;
		adpt->tx_q[3].consume_shft = H3TPD_CONS_IDX_SHFT;

		/* fall through */
	case 3:
		adpt->tx_q[2].produce_reg = EMAC_MAILBOX_9;
		adpt->tx_q[2].produce_mask = H2TPD_PROD_IDX_BMSK;
		adpt->tx_q[2].produce_shft = H2TPD_PROD_IDX_SHFT;

		adpt->tx_q[2].consume_reg = EMAC_MAILBOX_10;
		adpt->tx_q[2].consume_mask = H2TPD_CONS_IDX_BMSK;
		adpt->tx_q[2].consume_shft = H2TPD_CONS_IDX_SHFT;

		/* fall through */
	case 2:
		adpt->tx_q[1].produce_reg = EMAC_MAILBOX_16;
		adpt->tx_q[1].produce_mask = H1TPD_PROD_IDX_BMSK;
		adpt->tx_q[1].produce_shft = H1TPD_PROD_IDX_SHFT;

		adpt->tx_q[1].consume_reg = EMAC_MAILBOX_10;
		adpt->tx_q[1].consume_mask = H1TPD_CONS_IDX_BMSK;
		adpt->tx_q[1].consume_shft = H1TPD_CONS_IDX_SHFT;

		/* fall through */
	case 1:
		adpt->tx_q[0].produce_reg = EMAC_MAILBOX_15;
		adpt->tx_q[0].produce_mask = NTPD_PROD_IDX_BMSK;
		adpt->tx_q[0].produce_shft = NTPD_PROD_IDX_SHFT;

		adpt->tx_q[0].consume_reg = EMAC_MAILBOX_2;
		adpt->tx_q[0].consume_mask = NTPD_CONS_IDX_BMSK;
		adpt->tx_q[0].consume_shft = NTPD_CONS_IDX_SHFT;
		break;
	}
}

/* get the number of free transmit descriptors */
static u32 emac_tpd_num_free_descs(struct emac_tx_queue *tx_q)
{
	u32 produce_idx = tx_q->tpd.produce_idx;
	u32 consume_idx = tx_q->tpd.consume_idx;

	return (consume_idx > produce_idx) ?
		(consume_idx - produce_idx - 1) :
		(tx_q->tpd.count + consume_idx - produce_idx - 1);
}

/* Check if enough transmit descriptors are available */
static bool emac_tx_has_enough_descs(struct emac_tx_queue *tx_q,
				     const struct sk_buff *skb)
{
	u32 num_required = 1;
	int i;
	u16 proto_hdr_len = 0;

	if (skb_is_gso(skb)) {
		proto_hdr_len = skb_transport_offset(skb) + tcp_hdrlen(skb);
		if (proto_hdr_len < skb_headlen(skb))
			num_required++;
		if (skb_shinfo(skb)->gso_type & SKB_GSO_TCPV6)
			num_required++;
	}

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++)
		num_required++;

	return num_required < emac_tpd_num_free_descs(tx_q);
}

/* Fill up transmit descriptors with TSO and Checksum offload information */
static int emac_tso_csum(struct emac_adapter *adpt,
			 struct emac_tx_queue *tx_q,
			 struct sk_buff *skb,
			 struct emac_tpd *tpd)
{
	u8  hdr_len;
	int retval;

	if (skb_is_gso(skb)) {
		if (skb_header_cloned(skb)) {
			retval = pskb_expand_head(skb, 0, 0, GFP_ATOMIC);
			if (unlikely(retval))
				return retval;
		}

		if (skb->protocol == htons(ETH_P_IP)) {
			u32 pkt_len = ((unsigned char *)ip_hdr(skb) - skb->data)
				       + ntohs(ip_hdr(skb)->tot_len);
			if (skb->len > pkt_len)
				pskb_trim(skb, pkt_len);
		}

		hdr_len = skb_transport_offset(skb) + tcp_hdrlen(skb);
		if (unlikely(skb->len == hdr_len)) {
			/* we only need to do csum */
			netif_warn(adpt, tx_err, adpt->netdev,
				   "tso not needed for packet with 0 data\n");
			goto do_csum;
		}

		if (skb_shinfo(skb)->gso_type & SKB_GSO_TCPV4) {
			ip_hdr(skb)->check = 0;
			tcp_hdr(skb)->check = ~csum_tcpudp_magic(
						ip_hdr(skb)->saddr,
						ip_hdr(skb)->daddr,
						0, IPPROTO_TCP, 0);
			TPD_IPV4_SET(tpd, 1);
		}

		if (skb_shinfo(skb)->gso_type & SKB_GSO_TCPV6) {
			/* ipv6 tso need an extra tpd */
			struct emac_tpd extra_tpd;

			memset(tpd, 0, sizeof(*tpd));
			memset(&extra_tpd, 0, sizeof(extra_tpd));

			ipv6_hdr(skb)->payload_len = 0;
			tcp_hdr(skb)->check = ~csum_ipv6_magic(
						&ipv6_hdr(skb)->saddr,
						&ipv6_hdr(skb)->daddr,
						0, IPPROTO_TCP, 0);
			TPD_PKT_LEN_SET(&extra_tpd, skb->len);
			TPD_LSO_SET(&extra_tpd, 1);
			TPD_LSOV_SET(&extra_tpd, 1);
			emac_tx_tpd_create(adpt, tx_q, &extra_tpd);
			TPD_LSOV_SET(tpd, 1);
		}

		TPD_LSO_SET(tpd, 1);
		TPD_TCPHDR_OFFSET_SET(tpd, skb_transport_offset(skb));
		TPD_MSS_SET(tpd, skb_shinfo(skb)->gso_size);
		return 0;
	}

do_csum:
	if (likely(skb->ip_summed == CHECKSUM_PARTIAL)) {
		u8 css, cso;

		cso = skb_transport_offset(skb);
		if (unlikely(cso & 0x1)) {
			netdev_err(adpt->netdev,
				   "error: payload offset should be even\n");
			return -EINVAL;
		}
		css = cso + skb->csum_offset;

		TPD_PAYLOAD_OFFSET_SET(tpd, cso >> 1);
		TPD_CXSUM_OFFSET_SET(tpd, css >> 1);
		TPD_CSX_SET(tpd, 1);
	}

	return 0;
}

/* Fill up transmit descriptors */
static void emac_tx_fill_tpd(struct emac_adapter *adpt,
			     struct emac_tx_queue *tx_q, struct sk_buff *skb,
			     struct emac_tpd *tpd)
{
	struct emac_buffer *tpbuf = NULL;
	u16 nr_frags = skb_shinfo(skb)->nr_frags;
	u32 len = skb_headlen(skb);
	u16 map_len = 0;
	u16 mapped_len = 0;
	u16 hdr_len = 0;
	int i;

	/* if Large Segment Offload is (in TCP Segmentation Offload struct) */
	if (TPD_LSO(tpd)) {
		hdr_len = skb_transport_offset(skb) + tcp_hdrlen(skb);
		map_len = hdr_len;

		tpbuf = GET_TPD_BUFFER(tx_q, tx_q->tpd.produce_idx);
		tpbuf->length = map_len;
		tpbuf->dma = dma_map_single(adpt->netdev->dev.parent, skb->data,
					    hdr_len, DMA_TO_DEVICE);
		mapped_len += map_len;
		TPD_BUFFER_ADDR_L_SET(tpd, EMAC_DMA_ADDR_LO(tpbuf->dma));
		TPD_BUFFER_ADDR_H_SET(tpd, EMAC_DMA_ADDR_HI(tpbuf->dma));
		TPD_BUF_LEN_SET(tpd, tpbuf->length);
		emac_tx_tpd_create(adpt, tx_q, tpd);
	}

	if (mapped_len < len) {
		tpbuf = GET_TPD_BUFFER(tx_q, tx_q->tpd.produce_idx);
		tpbuf->length = len - mapped_len;
		tpbuf->dma = dma_map_single(adpt->netdev->dev.parent,
					    skb->data + mapped_len,
					    tpbuf->length, DMA_TO_DEVICE);
		TPD_BUFFER_ADDR_L_SET(tpd, EMAC_DMA_ADDR_LO(tpbuf->dma));
		TPD_BUFFER_ADDR_H_SET(tpd, EMAC_DMA_ADDR_HI(tpbuf->dma));
		TPD_BUF_LEN_SET(tpd, tpbuf->length);
		emac_tx_tpd_create(adpt, tx_q, tpd);
	}

	for (i = 0; i < nr_frags; i++) {
		struct skb_frag_struct *frag;

		frag = &skb_shinfo(skb)->frags[i];

		tpbuf = GET_TPD_BUFFER(tx_q, tx_q->tpd.produce_idx);
		tpbuf->length = frag->size;
		tpbuf->dma = dma_map_page(adpt->netdev->dev.parent,
					  frag->page.p, frag->page_offset,
					  tpbuf->length, DMA_TO_DEVICE);
		TPD_BUFFER_ADDR_L_SET(tpd, EMAC_DMA_ADDR_LO(tpbuf->dma));
		TPD_BUFFER_ADDR_H_SET(tpd, EMAC_DMA_ADDR_HI(tpbuf->dma));
		TPD_BUF_LEN_SET(tpd, tpbuf->length);
		emac_tx_tpd_create(adpt, tx_q, tpd);
	}

	/* The last tpd */
	emac_tx_tpd_mark_last(adpt, tx_q);

	if (test_bit(EMAC_STATUS_TS_TX_EN, &adpt->status) &&
	    (skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP)) {
		struct sk_buff *skb_ts = skb_clone(skb, GFP_ATOMIC);

		if (likely(skb_ts)) {
			unsigned long flags;

			emac_tx_tpd_ts_save(adpt, tx_q);
			skb_ts->sk = skb->sk;
			EMAC_SKB_CB(skb_ts)->tpd_idx =
				tx_q->tpd.last_produce_idx;
			EMAC_SKB_CB(skb_ts)->jiffies = get_jiffies_64();
			skb_shinfo(skb_ts)->tx_flags |= SKBTX_IN_PROGRESS;
			spin_lock_irqsave(&adpt->tx_ts_lock, flags);
			if (adpt->tx_ts_pending_queue.qlen >=
			    EMAC_TX_POLL_HWTXTSTAMP_THRESHOLD) {
				emac_tx_ts_poll(adpt);
				adpt->tx_ts_stats.tx_poll++;
			}
			__skb_queue_tail(&adpt->tx_ts_pending_queue,
					 skb_ts);
			spin_unlock_irqrestore(&adpt->tx_ts_lock, flags);
			adpt->tx_ts_stats.tx++;
			emac_schedule_tx_ts_task(adpt);
		}
	}

	/* The last buffer info contain the skb address,
	 * so it will be freed after unmap
	 */
	tpbuf->skb = skb;
}

/* Transmit the packet using specified transmit queue */
int emac_mac_tx_buf_send(struct emac_adapter *adpt, struct emac_tx_queue *tx_q,
			 struct sk_buff *skb)
{
	struct emac_tpd tpd;
	u32 prod_idx;

	if (test_bit(EMAC_STATUS_DOWN, &adpt->status)) {
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	if (!emac_tx_has_enough_descs(tx_q, skb)) {
		/* not enough descriptors, just stop queue */
		netif_stop_queue(adpt->netdev);
		return NETDEV_TX_BUSY;
	}

	memset(&tpd, 0, sizeof(tpd));

	if (emac_tso_csum(adpt, tx_q, skb, &tpd) != 0) {
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	if (skb_vlan_tag_present(skb)) {
		u16 tag;

		EMAC_VLAN_TO_TAG(skb_vlan_tag_get(skb), tag);
		TPD_CVLAN_TAG_SET(&tpd, tag);
		TPD_INSTC_SET(&tpd, 1);
	}

	if (skb_network_offset(skb) != ETH_HLEN)
		TPD_TYP_SET(&tpd, 1);

	emac_tx_fill_tpd(adpt, tx_q, skb, &tpd);

	netdev_sent_queue(adpt->netdev, skb->len);

	/* update produce idx */
	prod_idx = (tx_q->tpd.produce_idx << tx_q->produce_shft) &
		    tx_q->produce_mask;
	emac_reg_update32(adpt->base + tx_q->produce_reg,
			  tx_q->produce_mask, prod_idx);
	wmb(); /* ensure that RFD producer index is flushed to HW */
	netif_dbg(adpt, tx_queued, adpt->netdev, "TX[%d]: prod idx 0x%x\n",
		  tx_q->que_idx, tx_q->tpd.produce_idx);

	return NETDEV_TX_OK;
}
