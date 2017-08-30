/**
 * netsec.h
 *
 *  Copyright (C) 2011 - 2014 Fujitsu Semiconductor Limited.
 *  Copyright (C) 2014 Linaro Ltd  Andy Green <andy.green@linaro.org>
 *  All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */
#ifndef NETSEC_INTERNAL_H
#define NETSEC_INTERNAL_H

#include <linux/netdevice.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/phy.h>
#include <linux/ethtool.h>
#include <linux/of_address.h>
#include <linux/of_mdio.h>
#include <linux/etherdevice.h>
#include <net/sock.h>

#define NETSEC_FLOW_CONTROL_START_THRESHOLD	36
#define NETSEC_FLOW_CONTROL_STOP_THRESHOLD	48

#define NETSEC_CLK_MHZ				1000000

#define NETSEC_RX_PKT_BUF_LEN			1522
#define NETSEC_RX_JUMBO_PKT_BUF_LEN		9022

#define NETSEC_NETDEV_TX_PKT_SCAT_NUM_MAX		19

#define DESC_NUM 128

#define NETSEC_TX_SHIFT_OWN_FIELD			31
#define NETSEC_TX_SHIFT_LD_FIELD			30
#define NETSEC_TX_SHIFT_DRID_FIELD		24
#define NETSEC_TX_SHIFT_PT_FIELD			21
#define NETSEC_TX_SHIFT_TDRID_FIELD		16
#define NETSEC_TX_SHIFT_CC_FIELD			15
#define NETSEC_TX_SHIFT_FS_FIELD			9
#define NETSEC_TX_LAST				8
#define NETSEC_TX_SHIFT_CO			7
#define NETSEC_TX_SHIFT_SO			6
#define NETSEC_TX_SHIFT_TRS_FIELD		4

#define NETSEC_RX_PKT_OWN_FIELD			31
#define NETSEC_RX_PKT_LD_FIELD			30
#define NETSEC_RX_PKT_SDRID_FIELD			24
#define NETSEC_RX_PKT_FR_FIELD			23
#define NETSEC_RX_PKT_ER_FIELD			21
#define NETSEC_RX_PKT_ERR_FIELD			16
#define NETSEC_RX_PKT_TDRID_FIELD			12
#define NETSEC_RX_PKT_FS_FIELD			9
#define NETSEC_RX_PKT_LS_FIELD			8
#define NETSEC_RX_PKT_CO_FIELD			6

#define NETSEC_RX_PKT_ERR_MASK			3

#define NETSEC_MAX_TX_PKT_LEN			1518
#define NETSEC_MAX_TX_JUMBO_PKT_LEN		9018

enum netsec_rings {
	NETSEC_RING_TX,
	NETSEC_RING_RX
};

#define NETSEC_RING_GMAC				15
#define NETSEC_RING_MAX				1

#define NETSEC_TCP_SEG_LEN_MAX			1460
#define NETSEC_TCP_JUMBO_SEG_LEN_MAX		8960

#define NETSEC_RX_CKSUM_NOTAVAIL			0
#define NETSEC_RX_CKSUM_OK			1
#define NETSEC_RX_CKSUM_NG			2

#define NETSEC_TOP_IRQ_REG_CODE_LOAD_END		BIT(20)
#define NETSEC_IRQ_TRANSITION_COMPLETE		BIT(4)
#define NETSEC_IRQ_RX				BIT(1)
#define NETSEC_IRQ_TX				BIT(0)

#define NETSEC_IRQ_EMPTY				BIT(17)
#define NETSEC_IRQ_ERR				BIT(16)
#define NETSEC_IRQ_PKT_CNT			BIT(15)
#define NETSEC_IRQ_TIMEUP				BIT(14)
#define NETSEC_IRQ_RCV			(NETSEC_IRQ_PKT_CNT | NETSEC_IRQ_TIMEUP)

#define NETSEC_IRQ_TX_DONE			BIT(15)
#define NETSEC_IRQ_SND			(NETSEC_IRQ_TX_DONE | NETSEC_IRQ_TIMEUP)

#define NETSEC_MODE_TRANS_COMP_IRQ_N2T		BIT(20)
#define NETSEC_MODE_TRANS_COMP_IRQ_T2N		BIT(19)

#define NETSEC_DESC_MIN				2
#define NETSEC_DESC_MAX				2047
#define NETSEC_INT_PKTCNT_MAX			2047

#define NETSEC_FLOW_START_TH_MAX			95
#define NETSEC_FLOW_STOP_TH_MAX			95
#define NETSEC_FLOW_PAUSE_TIME_MIN		5

#define NETSEC_CLK_EN_REG_DOM_ALL			0x3f

#define NETSEC_REG_TOP_STATUS			0x80
#define NETSEC_REG_TOP_INTEN			0x81
#define NETSEC_REG_INTEN_SET			0x8d
#define NETSEC_REG_INTEN_CLR			0x8e
#define NETSEC_REG_NRM_TX_STATUS			0x100
#define NETSEC_REG_NRM_TX_INTEN			0x101
#define NETSEC_REG_NRM_TX_INTEN_SET		0x10a
#define NETSEC_REG_NRM_TX_INTEN_CLR		0x10b
#define NETSEC_REG_NRM_RX_STATUS			0x110
#define NETSEC_REG_NRM_RX_INTEN			0x111
#define NETSEC_REG_NRM_RX_INTEN_SET		0x11a
#define NETSEC_REG_NRM_RX_INTEN_CLR		0x11b
#define NETSEC_REG_RESERVED_RX_DESC_START		0x122
#define NETSEC_REG_RESERVED_TX_DESC_START		0x132
#define NETSEC_REG_CLK_EN				0x40
#define NETSEC_REG_SOFT_RST			0x41
#define NETSEC_REG_PKT_CTRL			0x50
#define NETSEC_REG_COM_INIT			0x48
#define NETSEC_REG_DMA_TMR_CTRL			0x83
#define NETSEC_REG_F_TAIKI_MC_VER			0x8b
#define NETSEC_REG_F_TAIKI_VER			0x8c
#define NETSEC_REG_DMA_HM_CTRL			0x85
#define NETSEC_REG_DMA_MH_CTRL			0x88
#define NETSEC_REG_NRM_TX_PKTCNT			0x104
#define NETSEC_REG_NRM_TX_DONE_TXINT_PKTCNT	0x106
#define NETSEC_REG_NRM_RX_RXINT_PKTCNT		0x116
#define NETSEC_REG_NRM_TX_TXINT_TMR		0x108
#define NETSEC_REG_NRM_RX_RXINT_TMR		0x118
#define NETSEC_REG_NRM_TX_DONE_PKTCNT		0x105
#define NETSEC_REG_NRM_RX_PKTCNT			0x115
#define NETSEC_REG_NRM_TX_TMR			0x107
#define NETSEC_REG_NRM_RX_TMR			0x117
#define NETSEC_REG_NRM_TX_DESC_START_UP		0x10d
#define NETSEC_REG_NRM_TX_DESC_START_LW		0x102
#define NETSEC_REG_NRM_RX_DESC_START_UP		0x11d
#define NETSEC_REG_NRM_RX_DESC_START_LW		0x112
#define NETSEC_REG_NRM_TX_CONFIG			0x10c
#define NETSEC_REG_NRM_RX_CONFIG			0x11c
#define MAC_REG_DATA				0x470
#define MAC_REG_CMD				0x471
#define MAC_REG_FLOW_TH				0x473
#define MAC_REG_INTF_SEL			0x475
#define MAC_REG_DESC_INIT			0x47f
#define MAC_REG_DESC_SOFT_RST			0x481
#define NETSEC_REG_MODE_TRANS_COMP_STATUS		0x140
#define GMAC_REG_MCR				0x0000
#define GMAC_REG_MFFR				0x0004
#define GMAC_REG_GAR				0x0010
#define GMAC_REG_GDR				0x0014
#define GMAC_REG_FCR				0x0018
#define GMAC_REG_BMR				0x1000
#define GMAC_REG_RDLAR				0x100c
#define GMAC_REG_TDLAR				0x1010
#define GMAC_REG_OMR				0x1018

#define NETSEC_PKT_CTRL_REG_MODE_NRM		BIT(28)
#define NETSEC_PKT_CTRL_REG_EN_JUMBO		BIT(27)
#define NETSEC_PKT_CTRL_REG_LOG_CHKSUM_ER		BIT(3)
#define NETSEC_PKT_CTRL_REG_LOG_HD_INCOMPLETE	BIT(2)
#define NETSEC_PKT_CTRL_REG_LOG_HD_ER		BIT(1)
#define NETSEC_PKT_CTRL_REG_DRP_NO_MATCH		BIT(0)

#define NETSEC_CLK_EN_REG_DOM_G			BIT(5)
#define NETSEC_CLK_EN_REG_DOM_C			BIT(1)
#define NETSEC_CLK_EN_REG_DOM_D			BIT(0)

#define NETSEC_COM_INIT_REG_PKT			BIT(1)
#define NETSEC_COM_INIT_REG_CORE			BIT(0)

#define NETSEC_SOFT_RST_REG_RESET			0
#define NETSEC_SOFT_RST_REG_RUN			BIT(31)

#define NETSEC_DMA_CTRL_REG_STOP			1
#define MH_CTRL__MODE_TRANS			BIT(20)

#define NETSEC_GMAC_CMD_ST_READ			0
#define NETSEC_GMAC_CMD_ST_WRITE			BIT(28)
#define NETSEC_GMAC_CMD_ST_BUSY			BIT(31)

#define NETSEC_GMAC_BMR_REG_COMMON		0x00412080
#define NETSEC_GMAC_BMR_REG_RESET			0x00020181
#define NETSEC_GMAC_BMR_REG_SWR			0x00000001

#define NETSEC_GMAC_OMR_REG_ST			BIT(13)
#define NETSEC_GMAC_OMR_REG_SR			BIT(1)

#define NETSEC_GMAC_MCR_REG_IBN			BIT(30)
#define NETSEC_GMAC_MCR_REG_CST			BIT(25)
#define NETSEC_GMAC_MCR_REG_JE			BIT(20)
#define NETSEC_MCR_PS				BIT(15)
#define NETSEC_GMAC_MCR_REG_FES			BIT(14)
#define NETSEC_GMAC_MCR_REG_FULL_DUPLEX_COMMON	0x0000280c
#define NETSEC_GMAC_MCR_REG_HALF_DUPLEX_COMMON	0x0001a00c

#define NETSEC_FCR_RFE				BIT(2)
#define NETSEC_FCR_TFE				BIT(1)

#define NETSEC_GMAC_GAR_REG_GW			BIT(1)
#define NETSEC_GMAC_GAR_REG_GB			BIT(0)

#define NETSEC_GMAC_GAR_REG_SHIFT_PA		11
#define NETSEC_GMAC_GAR_REG_SHIFT_GR		6
#define GMAC_REG_SHIFT_CR_GAR			2

#define NETSEC_GMAC_GAR_REG_CR_25_35_MHZ		2
#define NETSEC_GMAC_GAR_REG_CR_35_60_MHZ		3
#define NETSEC_GMAC_GAR_REG_CR_60_100_MHZ		0
#define NETSEC_GMAC_GAR_REG_CR_100_150_MHZ	1
#define NETSEC_GMAC_GAR_REG_CR_150_250_MHZ	4
#define NETSEC_GMAC_GAR_REG_CR_250_300_MHZ	5

#define NETSEC_REG_NETSEC_VER_F_TAIKI		0x50000

#define NETSEC_REG_DESC_RING_CONFIG_CFG_UP	BIT(31)
#define NETSEC_REG_DESC_RING_CONFIG_CH_RST	BIT(30)
#define NETSEC_REG_DESC_TMR_MODE		4
#define NETSEC_REG_DESC_ENDIAN			0

#define NETSEC_MAC_DESC_SOFT_RST_SOFT_RST		1
#define NETSEC_MAC_DESC_INIT_REG_INIT		1

/* this is used to interpret a register layout */
struct netsec_pkt_ctrlaram {
	u8 log_chksum_er_flag:1;
	u8 log_hd_imcomplete_flag:1;
	u8 log_hd_er_flag:1;
};

struct netsec_param {
	struct netsec_pkt_ctrlaram pkt_ctrlaram;
	bool use_jumbo_pkt_flag;
};

struct netsec_mac_mode {
	u16 flow_start_th;
	u16 flow_stop_th;
	u16 pause_time;
	bool flow_ctrl_enable_flag;
};

struct netsec_desc_ring {
	spinlock_t spinlock_desc; /* protect descriptor access */
	phys_addr_t desc_phys;
	struct netsec_frag_info *frag;
	struct sk_buff **priv;
	void *ring_vaddr;
	enum netsec_rings id;
	int len;
	u16 tx_done_num;
	u16 rx_num;
	u16 head;
	u16 tail;
	bool running;
	bool full;
};

struct netsec_frag_info {
	dma_addr_t dma_addr;
	void *addr;
	u16 len;
};

struct netsec_priv {
	struct netsec_desc_ring desc_ring[NETSEC_RING_MAX + 1];
	struct ethtool_coalesce et_coalesce;
	struct netsec_mac_mode mac_mode;
	struct netsec_param param;
	struct napi_struct napi;
	phys_addr_t rdlar_pa, tdlar_pa;
	phy_interface_t phy_interface;
	spinlock_t tx_queue_lock; /* protect transmit queue */
	struct netsec_frag_info tx_info[MAX_SKB_FRAGS];
	struct net_device *ndev;
	struct device_node *phy_np;
	struct mii_bus *mii_bus;
	void __iomem *ioaddr;
	struct device *dev;
	struct clk *clk[3];
	phys_addr_t scb_set_normal_tx_paddr;
	u32 scb_pkt_ctrl_reg;
	u32 rx_pkt_buf_len;
	u32 msg_enable;
	u32 freq;
	int actual_link_speed;
	int clock_count;
	bool rx_cksum_offload_flag;
	bool actual_duplex;
	bool irq_registered;
};

struct netsec_tx_de {
	u32 attr;
	u32 data_buf_addr_up;
	u32 data_buf_addr_lw;
	u32 buf_len_info;
};

struct netsec_rx_de {
	u32 attr;
	u32 data_buf_addr_up;
	u32 data_buf_addr_lw;
	u32 buf_len_info;
};

struct netsec_tx_pkt_ctrl {
	u16 tcp_seg_len;
	bool tcp_seg_offload_flag;
	bool cksum_offload_flag;
};

struct netsec_rx_pkt_info {
	int rx_cksum_result;
	int err_code;
	bool is_fragmented;
	bool err_flag;
};

struct netsec_skb_cb {
	bool is_rx;
};

static inline void netsec_writel(struct netsec_priv *priv,
				 u32 reg_addr, u32 val)
{
	writel_relaxed(val, priv->ioaddr + (reg_addr << 2));
}

static inline u32 netsec_readl(struct netsec_priv *priv, u32 reg_addr)
{
	return readl_relaxed(priv->ioaddr + (reg_addr << 2));
}

static inline void netsec_mark_skb_type(struct sk_buff *skb, bool is_rx)
{
	struct netsec_skb_cb *cb = (struct netsec_skb_cb *)skb->cb;

	cb->is_rx = is_rx;
}

static inline bool skb_is_rx(struct sk_buff *skb)
{
	struct netsec_skb_cb *cb = (struct netsec_skb_cb *)skb->cb;

	return cb->is_rx;
}

extern const struct net_device_ops netsec_netdev_ops;
extern const struct ethtool_ops netsec_ethtool_ops;

int netsec_start_gmac(struct netsec_priv *priv);
int netsec_stop_gmac(struct netsec_priv *priv);
int netsec_mii_register(struct netsec_priv *priv);
void netsec_mii_unregister(struct netsec_priv *priv);
int netsec_start_desc_ring(struct netsec_priv *priv, enum netsec_rings id);
void netsec_stop_desc_ring(struct netsec_priv *priv, enum netsec_rings id);
u16 netsec_get_rx_num(struct netsec_priv *priv);
u16 netsec_get_tx_avail_num(struct netsec_priv *priv);
int netsec_clean_tx_desc_ring(struct netsec_priv *priv);
int netsec_clean_rx_desc_ring(struct netsec_priv *priv);
int netsec_set_tx_pkt_data(struct netsec_priv *priv,
			   const struct netsec_tx_pkt_ctrl *tx_ctrl,
			   u8 count_frags, const struct netsec_frag_info *info,
			   struct sk_buff *skb);
int netsec_get_rx_pkt_data(struct netsec_priv *priv,
			   struct netsec_rx_pkt_info *rxpi,
			   struct netsec_frag_info *frag, u16 *len,
			   struct sk_buff **skb);
void netsec_ring_irq_enable(struct netsec_priv *priv,
			    enum netsec_rings id, u32 i);
void netsec_ring_irq_disable(struct netsec_priv *priv,
			     enum netsec_rings id, u32 i);
int netsec_alloc_desc_ring(struct netsec_priv *priv, enum netsec_rings id);
void netsec_free_desc_ring(struct netsec_priv *priv,
			   struct netsec_desc_ring *desc);
int netsec_setup_rx_desc(struct netsec_priv *priv,
			 struct netsec_desc_ring *desc);
int netsec_netdev_napi_poll(struct napi_struct *napi_p, int budget);

#endif /* NETSEC_INTERNAL_H */
