/*
 * Copyright (c) 2016-2017, National Instruments Corp.
 *
 * Network Driver for Ettus Research XGE MAC
 *
 * This is largely based on the Xilinx AXI Ethernet Driver,
 * and uses the same DMA engine in the FPGA
 *
 * SPDX-License-Identifier: GPL-2.0
 *
 */

#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of_address.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/of_irq.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/phy.h>
#include <linux/mii.h>
#include <linux/nvmem-consumer.h>
#include <linux/ethtool.h>
#include <linux/if_vlan.h>
#include <linux/iopoll.h>

#define TX_BD_NUM		64
#define RX_BD_NUM		128

/* Axi DMA Register definitions */

#define XAXIDMA_TX_CR_OFFSET	0x00000000 /* Channel control */
#define XAXIDMA_TX_SR_OFFSET	0x00000004 /* Status */
#define XAXIDMA_TX_CDESC_OFFSET	0x00000008 /* Current descriptor pointer */
#define XAXIDMA_TX_TDESC_OFFSET	0x00000010 /* Tail descriptor pointer */

#define XAXIDMA_RX_CR_OFFSET	0x00000030 /* Channel control */
#define XAXIDMA_RX_SR_OFFSET	0x00000034 /* Status */
#define XAXIDMA_RX_CDESC_OFFSET	0x00000038 /* Current descriptor pointer */
#define XAXIDMA_RX_TDESC_OFFSET	0x00000040 /* Tail descriptor pointer */

#define XAXIDMA_CR_RUNSTOP_MASK	0x00000001 /* Start/stop DMA channel */
#define XAXIDMA_CR_RESET_MASK	0x00000004 /* Reset DMA engine */

#define XAXIDMA_BD_NDESC_OFFSET		0x00 /* Next descriptor pointer */
#define XAXIDMA_BD_BUFA_OFFSET		0x08 /* Buffer address */
#define XAXIDMA_BD_CTRL_LEN_OFFSET	0x18 /* Control/buffer length */
#define XAXIDMA_BD_STS_OFFSET		0x1C /* Status */
#define XAXIDMA_BD_USR0_OFFSET		0x20 /* User IP specific word0 */
#define XAXIDMA_BD_USR1_OFFSET		0x24 /* User IP specific word1 */
#define XAXIDMA_BD_USR2_OFFSET		0x28 /* User IP specific word2 */
#define XAXIDMA_BD_USR3_OFFSET		0x2C /* User IP specific word3 */
#define XAXIDMA_BD_USR4_OFFSET		0x30 /* User IP specific word4 */
#define XAXIDMA_BD_ID_OFFSET		0x34 /* Sw ID */
#define XAXIDMA_BD_HAS_STSCNTRL_OFFSET	0x38 /* Whether has stscntrl strm */
#define XAXIDMA_BD_HAS_DRE_OFFSET	0x3C /* Whether has DRE */

#define XAXIDMA_BD_HAS_DRE_SHIFT	8 /* Whether has DRE shift */
#define XAXIDMA_BD_HAS_DRE_MASK		0xF00 /* Whether has DRE mask */
#define XAXIDMA_BD_WORDLEN_MASK		0xFF /* Whether has DRE mask */

#define XAXIDMA_BD_CTRL_LENGTH_MASK	0x007FFFFF /* Requested len */
#define XAXIDMA_BD_CTRL_TXSOF_MASK	0x08000000 /* First tx packet */
#define XAXIDMA_BD_CTRL_TXEOF_MASK	0x04000000 /* Last tx packet */
#define XAXIDMA_BD_CTRL_ALL_MASK	0x0C000000 /* All control bits */

#define XAXIDMA_DELAY_MASK		0xFF000000 /* Delay timeout counter */
#define XAXIDMA_COALESCE_MASK		0x00FF0000 /* Coalesce counter */

#define XAXIDMA_DELAY_SHIFT		24
#define XAXIDMA_COALESCE_SHIFT		16

#define XAXIDMA_IRQ_IOC_MASK		0x00001000 /* Completion intr */
#define XAXIDMA_IRQ_DELAY_MASK		0x00002000 /* Delay interrupt */
#define XAXIDMA_IRQ_ERROR_MASK		0x00004000 /* Error interrupt */
#define XAXIDMA_IRQ_ALL_MASK		0x00007000 /* All interrupts */

/* Default TX/RX Threshold and waitbound values for SGDMA mode */
#define XAXIDMA_DFT_TX_THRESHOLD	24
#define XAXIDMA_DFT_TX_WAITBOUND	254
#define XAXIDMA_DFT_RX_THRESHOLD	24
#define XAXIDMA_DFT_RX_WAITBOUND	254

#define XAXIDMA_BD_CTRL_TXSOF_MASK	0x08000000 /* First tx packet */
#define XAXIDMA_BD_CTRL_TXEOF_MASK	0x04000000 /* Last tx packet */
#define XAXIDMA_BD_CTRL_ALL_MASK	0x0C000000 /* All control bits */

#define XAXIDMA_BD_STS_ACTUAL_LEN_MASK	0x007FFFFF /* Actual len */
#define XAXIDMA_BD_STS_COMPLETE_MASK	0x80000000 /* Completed */
#define XAXIDMA_BD_STS_DEC_ERR_MASK	0x40000000 /* Decode error */
#define XAXIDMA_BD_STS_SLV_ERR_MASK	0x20000000 /* Slave error */
#define XAXIDMA_BD_STS_INT_ERR_MASK	0x10000000 /* Internal err */
#define XAXIDMA_BD_STS_ALL_ERR_MASK	0x70000000 /* All errors */
#define XAXIDMA_BD_STS_RXSOF_MASK	0x08000000 /* First rx pkt */
#define XAXIDMA_BD_STS_RXEOF_MASK	0x04000000 /* Last rx pkt */
#define XAXIDMA_BD_STS_ALL_MASK		0xFC000000 /* All status bits */

#define XAXIDMA_BD_MINIMUM_ALIGNMENT	0x40

#define NIXGE_REG_CTRL_OFFSET	0x4000
#define NIXGE_REG_MDIO_DATA	0x10
#define NIXGE_REG_MDIO_ADDR	0x14
#define NIXGE_REG_MDIO_OP	0x18
#define NIXGE_REG_MDIO_CTRL	0x1c

#define NIXGE_MDIO_CLAUSE45	BIT(12)
#define NIXGE_MDIO_CLAUSE22	0
#define NIXGE_MDIO_OP(n)     (((n) & 0x3) << 10)
#define NIXGE_MDIO_OP_ADDRESS	0
#define NIXGE_MDIO_OP_WRITE	BIT(0)
#define NIXGE_MDIO_OP_READ	(BIT(1) | BIT(0))
#define MDIO_C22_WRITE		BIT(0)
#define MDIO_C22_READ		BIT(1)
#define MDIO_READ_POST		2
#define NIXGE_MDIO_ADDR(n)   (((n) & 0x1f) << 5)
#define NIXGE_MDIO_MMD(n)    (((n) & 0x1f) << 0)

#define NIXGE_MAX_PHY_ADDR	32

#define NIXGE_REG_MAC_LSB	0x1000
#define NIXGE_REG_MAC_MSB	0x1004

/* Packet size info */
#define NIXGE_HDR_SIZE			14 /* Size of Ethernet header */
#define NIXGE_TRL_SIZE			4 /* Size of Ethernet trailer (FCS) */
#define NIXGE_MTU			1500 /* Max MTU of an Ethernet frame */
#define NIXGE_JUMBO_MTU			9000 /* Max MTU of a jumbo Eth. frame */

#define NIXGE_MAX_FRAME_SIZE	 (NIXGE_MTU + NIXGE_HDR_SIZE + NIXGE_TRL_SIZE)
#define NIXGE_MAX_VLAN_FRAME_SIZE  (NIXGE_MTU + VLAN_ETH_HLEN + NIXGE_TRL_SIZE)
#define NIXGE_MAX_JUMBO_FRAME_SIZE \
	(NIXGE_JUMBO_MTU + NIXGE_HDR_SIZE + NIXGE_TRL_SIZE)

#define NIXGE_DEFAULT_RX_MEM	10000

struct nixge_dma_bd {
	u32 next;	/* Physical address of next buffer descriptor */
	u32 reserved1;
	u32 phys;
	u32 reserved2;
	u32 reserved3;
	u32 reserved4;
	u32 cntrl;
	u32 status;
	u32 app0;
	u32 app1;	/* TX start << 16 | insert */
	u32 app2;	/* TX csum seed */
	u32 app3;
	u32 app4;
	u32 sw_id_offset;
	u32 reserved5;
	u32 reserved6;
};

struct nixge_priv {
	struct net_device *ndev;
	struct device *dev;

	/* Connection to PHY device */
	struct device_node *phy_node;
	phy_interface_t		phy_mode;

	/* protecting link parameters */
	spinlock_t              lock;
	int link;
	unsigned int speed;
	unsigned int duplex;

	/* MDIO bus data */
	struct mii_bus *mii_bus;	/* MII bus reference */

	/* IO registers, dma functions and IRQs */
	void __iomem *ctrl_regs;
	void __iomem *dma_regs;

	struct tasklet_struct dma_err_tasklet;

	int tx_irq;
	int rx_irq;

	/* Buffer descriptors */
	struct nixge_dma_bd *tx_bd_v;
	dma_addr_t tx_bd_p;
	struct nixge_dma_bd *rx_bd_v;
	dma_addr_t rx_bd_p;
	u32 tx_bd_ci;
	u32 tx_bd_tail;
	u32 rx_bd_ci;

	u32 max_frm_size;
	u32 rxmem;

	u32 coalesce_count_rx;
	u32 coalesce_count_tx;
};

static void nixge_dma_write_reg(struct nixge_priv *priv, off_t offset, u32 val)
{
	writel(val, priv->dma_regs + offset);
}

static u32 nixge_dma_read_reg(const struct nixge_priv *priv, off_t offset)
{
	return readl(priv->dma_regs + offset);
}

static void nixge_ctrl_write_reg(struct nixge_priv *priv, off_t offset, u32 val)
{
	writel(val, priv->ctrl_regs + offset);
}

static u32 nixge_ctrl_read_reg(struct nixge_priv *priv, off_t offset)
{
	return readl(priv->ctrl_regs + offset);
}

#define nixge_ctrl_poll_timeout(priv, addr, val, cond, sleep_us, timeout_us) \
	readl_poll_timeout((priv)->ctrl_regs + (addr), (val), cond, \
			   (sleep_us), (timeout_us))

#define nixge_dma_poll_timeout(priv, addr, val, cond, sleep_us, timeout_us) \
	readl_poll_timeout((priv)->dma_regs + (addr), (val), cond, \
			   (sleep_us), (timeout_us))

static void nixge_dma_bd_release(struct net_device *ndev)
{
	int i;
	struct nixge_priv *priv = netdev_priv(ndev);

	for (i = 0; i < RX_BD_NUM; i++) {
		dma_unmap_single(ndev->dev.parent, priv->rx_bd_v[i].phys,
				 priv->max_frm_size, DMA_FROM_DEVICE);
		dev_kfree_skb((struct sk_buff *)
			      (priv->rx_bd_v[i].sw_id_offset));
	}

	if (priv->rx_bd_v) {
		dma_free_coherent(ndev->dev.parent,
				  sizeof(*priv->rx_bd_v) * RX_BD_NUM,
				  priv->rx_bd_v,
				  priv->rx_bd_p);
	}
	if (priv->tx_bd_v) {
		dma_free_coherent(ndev->dev.parent,
				  sizeof(*priv->tx_bd_v) * TX_BD_NUM,
				  priv->tx_bd_v,
				  priv->tx_bd_p);
	}
}

static int nixge_dma_bd_init(struct net_device *ndev)
{
	u32 cr;
	int i;
	struct sk_buff *skb;
	struct nixge_priv *priv = netdev_priv(ndev);

	/* Reset the indexes which are used for accessing the BDs */
	priv->tx_bd_ci = 0;
	priv->tx_bd_tail = 0;
	priv->rx_bd_ci = 0;

	/* Allocate the Tx and Rx buffer descriptors. */
	priv->tx_bd_v = dma_zalloc_coherent(ndev->dev.parent,
					  sizeof(*priv->tx_bd_v) * TX_BD_NUM,
					  &priv->tx_bd_p, GFP_KERNEL);
	if (!priv->tx_bd_v)
		goto out;

	priv->rx_bd_v = dma_zalloc_coherent(ndev->dev.parent,
					  sizeof(*priv->rx_bd_v) * RX_BD_NUM,
					  &priv->rx_bd_p, GFP_KERNEL);
	if (!priv->rx_bd_v)
		goto out;

	for (i = 0; i < TX_BD_NUM; i++) {
		priv->tx_bd_v[i].next = priv->tx_bd_p +
				      sizeof(*priv->tx_bd_v) *
				      ((i + 1) % TX_BD_NUM);
	}

	for (i = 0; i < RX_BD_NUM; i++) {
		priv->rx_bd_v[i].next = priv->rx_bd_p +
				      sizeof(*priv->rx_bd_v) *
				      ((i + 1) % RX_BD_NUM);

		skb = netdev_alloc_skb_ip_align(ndev, priv->max_frm_size);
		if (!skb)
			goto out;

		priv->rx_bd_v[i].sw_id_offset = (u32)skb;
		priv->rx_bd_v[i].phys = dma_map_single(ndev->dev.parent,
						     skb->data,
						     priv->max_frm_size,
						     DMA_FROM_DEVICE);
		priv->rx_bd_v[i].cntrl = priv->max_frm_size;
	}

	/* Start updating the Rx channel control register */
	cr = nixge_dma_read_reg(priv, XAXIDMA_RX_CR_OFFSET);
	/* Update the interrupt coalesce count */
	cr = ((cr & ~XAXIDMA_COALESCE_MASK) |
	      ((priv->coalesce_count_rx) << XAXIDMA_COALESCE_SHIFT));
	/* Update the delay timer count */
	cr = ((cr & ~XAXIDMA_DELAY_MASK) |
	      (XAXIDMA_DFT_RX_WAITBOUND << XAXIDMA_DELAY_SHIFT));
	/* Enable coalesce, delay timer and error interrupts */
	cr |= XAXIDMA_IRQ_ALL_MASK;
	/* Write to the Rx channel control register */
	nixge_dma_write_reg(priv, XAXIDMA_RX_CR_OFFSET, cr);

	/* Start updating the Tx channel control register */
	cr = nixge_dma_read_reg(priv, XAXIDMA_TX_CR_OFFSET);
	/* Update the interrupt coalesce count */
	cr = (((cr & ~XAXIDMA_COALESCE_MASK)) |
	      ((priv->coalesce_count_tx) << XAXIDMA_COALESCE_SHIFT));
	/* Update the delay timer count */
	cr = (((cr & ~XAXIDMA_DELAY_MASK)) |
	      (XAXIDMA_DFT_TX_WAITBOUND << XAXIDMA_DELAY_SHIFT));
	/* Enable coalesce, delay timer and error interrupts */
	cr |= XAXIDMA_IRQ_ALL_MASK;
	/* Write to the Tx channel control register */
	nixge_dma_write_reg(priv, XAXIDMA_TX_CR_OFFSET, cr);

	/* Populate the tail pointer and bring the Rx Axi DMA engine out of
	 * halted state. This will make the Rx side ready for reception.
	 */
	nixge_dma_write_reg(priv, XAXIDMA_RX_CDESC_OFFSET, priv->rx_bd_p);
	cr = nixge_dma_read_reg(priv, XAXIDMA_RX_CR_OFFSET);
	nixge_dma_write_reg(priv, XAXIDMA_RX_CR_OFFSET,
			    cr | XAXIDMA_CR_RUNSTOP_MASK);
	nixge_dma_write_reg(priv, XAXIDMA_RX_TDESC_OFFSET, priv->rx_bd_p +
			    (sizeof(*priv->rx_bd_v) * (RX_BD_NUM - 1)));

	/* Write to the RS (Run-stop) bit in the Tx channel control register.
	 * Tx channel is now ready to run. But only after we write to the
	 * tail pointer register that the Tx channel will start transmitting.
	 */
	nixge_dma_write_reg(priv, XAXIDMA_TX_CDESC_OFFSET, priv->tx_bd_p);
	cr = nixge_dma_read_reg(priv, XAXIDMA_TX_CR_OFFSET);
	nixge_dma_write_reg(priv, XAXIDMA_TX_CR_OFFSET,
			    cr | XAXIDMA_CR_RUNSTOP_MASK);

	return 0;
out:
	nixge_dma_bd_release(ndev);
	return -ENOMEM;
}

static void __nixge_device_reset(struct nixge_priv *priv, off_t offset)
{
	u32 status;
	int err;
	/* Reset Axi DMA. This would reset NIXGE Ethernet core as well.
	 * The reset process of Axi DMA takes a while to complete as all
	 * pending commands/transfers will be flushed or completed during
	 * this reset process.
	 */
	nixge_dma_write_reg(priv, offset, XAXIDMA_CR_RESET_MASK);
	err = nixge_dma_poll_timeout(priv, offset, status,
				     !(status & XAXIDMA_CR_RESET_MASK), 10,
				     1000);
	if (err)
		netdev_err(priv->ndev, "%s: DMA reset timeout!\n", __func__);
}

static void nixge_device_reset(struct net_device *ndev)
{
	struct nixge_priv *priv = netdev_priv(ndev);

	__nixge_device_reset(priv, XAXIDMA_TX_CR_OFFSET);
	__nixge_device_reset(priv, XAXIDMA_RX_CR_OFFSET);

	priv->max_frm_size = NIXGE_MAX_VLAN_FRAME_SIZE;

	if ((ndev->mtu > NIXGE_MTU) && (ndev->mtu <= NIXGE_JUMBO_MTU))
		priv->max_frm_size = ndev->mtu + VLAN_ETH_HLEN + NIXGE_TRL_SIZE;

	if (nixge_dma_bd_init(ndev))
		netdev_err(ndev, "%s: descriptor allocation failed\n",
			   __func__);

	netif_trans_update(ndev);
}

static void nixge_handle_link_change(struct net_device *ndev)
{
	struct nixge_priv *priv = netdev_priv(ndev);
	struct phy_device *phydev = ndev->phydev;
	unsigned long flags;
	int status_change = 0;

	spin_lock_irqsave(&priv->lock, flags);

	if (phydev->link != priv->link || phydev->speed != priv->speed ||
	    phydev->duplex != priv->duplex) {
		priv->link = phydev->link;
		priv->speed = phydev->speed;
		priv->duplex = phydev->duplex;
		status_change = 1;
	}

	spin_unlock_irqrestore(&priv->lock, flags);

	if (status_change)
		phy_print_status(phydev);
}

static void nixge_start_xmit_done(struct net_device *ndev)
{
	u32 size = 0;
	u32 packets = 0;
	struct nixge_priv *priv = netdev_priv(ndev);
	struct nixge_dma_bd *cur_p;
	unsigned int status = 0;

	cur_p = &priv->tx_bd_v[priv->tx_bd_ci];
	status = cur_p->status;

	while (status & XAXIDMA_BD_STS_COMPLETE_MASK) {
		dma_unmap_single(ndev->dev.parent, cur_p->phys,
				 (cur_p->cntrl & XAXIDMA_BD_CTRL_LENGTH_MASK),
				DMA_TO_DEVICE);
		if (cur_p->app4)
			dev_kfree_skb_irq((struct sk_buff *)cur_p->app4);
		/*cur_p->phys = 0;*/
		cur_p->app0 = 0;
		cur_p->app1 = 0;
		cur_p->app2 = 0;
		cur_p->app4 = 0;
		cur_p->status = 0;

		size += status & XAXIDMA_BD_STS_ACTUAL_LEN_MASK;
		packets++;

		++priv->tx_bd_ci;
		priv->tx_bd_ci %= TX_BD_NUM;
		cur_p = &priv->tx_bd_v[priv->tx_bd_ci];
		status = cur_p->status;
	}

	ndev->stats.tx_packets += packets;
	ndev->stats.tx_bytes += size;
	netif_wake_queue(ndev);
}

static inline int nixge_check_tx_bd_space(struct nixge_priv *priv,
					  int num_frag)
{
	struct nixge_dma_bd *cur_p;

	cur_p = &priv->tx_bd_v[(priv->tx_bd_tail + num_frag) % TX_BD_NUM];
	if (cur_p->status & XAXIDMA_BD_STS_ALL_MASK)
		return NETDEV_TX_BUSY;
	return 0;
}

static int nixge_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	u32 ii;
	u32 num_frag;
	skb_frag_t *frag;
	dma_addr_t tail_p;
	struct nixge_priv *priv = netdev_priv(ndev);
	struct nixge_dma_bd *cur_p;

	num_frag = skb_shinfo(skb)->nr_frags;
	cur_p = &priv->tx_bd_v[priv->tx_bd_tail];

	if (nixge_check_tx_bd_space(priv, num_frag)) {
		if (!netif_queue_stopped(ndev))
			netif_stop_queue(ndev);
		return NETDEV_TX_BUSY;
	}

	cur_p->cntrl = skb_headlen(skb) | XAXIDMA_BD_CTRL_TXSOF_MASK;
	cur_p->phys = dma_map_single(ndev->dev.parent, skb->data,
				     skb_headlen(skb), DMA_TO_DEVICE);

	for (ii = 0; ii < num_frag; ii++) {
		++priv->tx_bd_tail;
		priv->tx_bd_tail %= TX_BD_NUM;
		cur_p = &priv->tx_bd_v[priv->tx_bd_tail];
		frag = &skb_shinfo(skb)->frags[ii];
		cur_p->phys = dma_map_single(ndev->dev.parent,
					     skb_frag_address(frag),
					     skb_frag_size(frag),
					     DMA_TO_DEVICE);
		cur_p->cntrl = skb_frag_size(frag);
	}

	cur_p->cntrl |= XAXIDMA_BD_CTRL_TXEOF_MASK;
	cur_p->app4 = (unsigned long)skb;

	tail_p = priv->tx_bd_p + sizeof(*priv->tx_bd_v) * priv->tx_bd_tail;
	/* Start the transfer */
	nixge_dma_write_reg(priv, XAXIDMA_TX_TDESC_OFFSET, tail_p);
	++priv->tx_bd_tail;
	priv->tx_bd_tail %= TX_BD_NUM;

	return NETDEV_TX_OK;
}

static void nixge_recv(struct net_device *ndev)
{
	u32 length;
	u32 size = 0;
	u32 packets = 0;
	dma_addr_t tail_p = 0;
	struct nixge_priv *priv = netdev_priv(ndev);
	struct sk_buff *skb, *new_skb;
	struct nixge_dma_bd *cur_p;

	cur_p = &priv->rx_bd_v[priv->rx_bd_ci];

	while ((cur_p->status & XAXIDMA_BD_STS_COMPLETE_MASK)) {
		tail_p = priv->rx_bd_p
			+ sizeof(*priv->rx_bd_v) * priv->rx_bd_ci;
		skb = (struct sk_buff *)(cur_p->sw_id_offset);

		length = cur_p->status & 0x7fffff;
		dma_unmap_single(ndev->dev.parent, cur_p->phys,
				 priv->max_frm_size,
				 DMA_FROM_DEVICE);

		skb_put(skb, length);

		skb->protocol = eth_type_trans(skb, ndev);
		skb_checksum_none_assert(skb);

		/* For now mark them as CHECKSUM_NONE since
		 * we don't have offload capabilities
		 */
		skb->ip_summed = CHECKSUM_NONE;

		netif_rx(skb);

		size += length;
		packets++;

		new_skb = netdev_alloc_skb_ip_align(ndev, priv->max_frm_size);
		if (!new_skb)
			return;

		cur_p->phys = dma_map_single(ndev->dev.parent, new_skb->data,
					     priv->max_frm_size,
					     DMA_FROM_DEVICE);
		cur_p->cntrl = priv->max_frm_size;
		cur_p->status = 0;
		cur_p->sw_id_offset = (u32)new_skb;

		++priv->rx_bd_ci;
		priv->rx_bd_ci %= RX_BD_NUM;
		cur_p = &priv->rx_bd_v[priv->rx_bd_ci];
	}

	ndev->stats.rx_packets += packets;
	ndev->stats.rx_bytes += size;

	if (tail_p)
		nixge_dma_write_reg(priv, XAXIDMA_RX_TDESC_OFFSET, tail_p);
}

static irqreturn_t nixge_tx_irq(int irq, void *_ndev)
{
	u32 cr;
	unsigned int status;
	struct net_device *ndev = _ndev;
	struct nixge_priv *priv = netdev_priv(ndev);

	status = nixge_dma_read_reg(priv, XAXIDMA_TX_SR_OFFSET);
	if (status & (XAXIDMA_IRQ_IOC_MASK | XAXIDMA_IRQ_DELAY_MASK)) {
		nixge_dma_write_reg(priv, XAXIDMA_TX_SR_OFFSET, status);
		nixge_start_xmit_done(priv->ndev);
		goto out;
	}
	if (!(status & XAXIDMA_IRQ_ALL_MASK))
		dev_err(&ndev->dev, "No interrupts asserted in Tx path\n");
	if (status & XAXIDMA_IRQ_ERROR_MASK) {
		dev_err(&ndev->dev, "DMA Tx error 0x%x\n", status);
		dev_err(&ndev->dev, "Current BD is at: 0x%x\n",
			(priv->tx_bd_v[priv->tx_bd_ci]).phys);

		cr = nixge_dma_read_reg(priv, XAXIDMA_TX_CR_OFFSET);
		/* Disable coalesce, delay timer and error interrupts */
		cr &= (~XAXIDMA_IRQ_ALL_MASK);
		/* Write to the Tx channel control register */
		nixge_dma_write_reg(priv, XAXIDMA_TX_CR_OFFSET, cr);

		cr = nixge_dma_read_reg(priv, XAXIDMA_RX_CR_OFFSET);
		/* Disable coalesce, delay timer and error interrupts */
		cr &= (~XAXIDMA_IRQ_ALL_MASK);
		/* Write to the Rx channel control register */
		nixge_dma_write_reg(priv, XAXIDMA_RX_CR_OFFSET, cr);

		tasklet_schedule(&priv->dma_err_tasklet);
		nixge_dma_write_reg(priv, XAXIDMA_TX_SR_OFFSET, status);
	}
out:
	return IRQ_HANDLED;
}

static irqreturn_t nixge_rx_irq(int irq, void *_ndev)
{
	u32 cr;
	unsigned int status;
	struct net_device *ndev = _ndev;
	struct nixge_priv *priv = netdev_priv(ndev);

	status = nixge_dma_read_reg(priv, XAXIDMA_RX_SR_OFFSET);
	if (status & (XAXIDMA_IRQ_IOC_MASK | XAXIDMA_IRQ_DELAY_MASK)) {
		nixge_dma_write_reg(priv, XAXIDMA_RX_SR_OFFSET, status);
		nixge_recv(priv->ndev);
		goto out;
	}
	if (!(status & XAXIDMA_IRQ_ALL_MASK))
		dev_err(&ndev->dev, "No interrupts asserted in Rx path\n");
	if (status & XAXIDMA_IRQ_ERROR_MASK) {
		dev_err(&ndev->dev, "DMA Rx error 0x%x\n", status);
		dev_err(&ndev->dev, "Current BD is at: 0x%x\n",
			(priv->rx_bd_v[priv->rx_bd_ci]).phys);

		cr = nixge_dma_read_reg(priv, XAXIDMA_TX_CR_OFFSET);
		/* Disable coalesce, delay timer and error interrupts */
		cr &= (~XAXIDMA_IRQ_ALL_MASK);
		/* Finally write to the Tx channel control register */
		nixge_dma_write_reg(priv, XAXIDMA_TX_CR_OFFSET, cr);

		cr = nixge_dma_read_reg(priv, XAXIDMA_RX_CR_OFFSET);
		/* Disable coalesce, delay timer and error interrupts */
		cr &= (~XAXIDMA_IRQ_ALL_MASK);
		/* write to the Rx channel control register */
		nixge_dma_write_reg(priv, XAXIDMA_RX_CR_OFFSET, cr);

		tasklet_schedule(&priv->dma_err_tasklet);
		nixge_dma_write_reg(priv, XAXIDMA_RX_SR_OFFSET, status);
	}
out:
	return IRQ_HANDLED;
}

static void nixge_dma_err_handler(unsigned long data)
{
	u32 cr, i;
	struct nixge_priv *lp = (struct nixge_priv *)data;
	struct net_device *ndev = lp->ndev;
	struct nixge_dma_bd *cur_p;

	__nixge_device_reset(lp, XAXIDMA_TX_CR_OFFSET);
	__nixge_device_reset(lp, XAXIDMA_RX_CR_OFFSET);

	for (i = 0; i < TX_BD_NUM; i++) {
		cur_p = &lp->tx_bd_v[i];
		if (cur_p->phys)
			dma_unmap_single(ndev->dev.parent, cur_p->phys,
					 (cur_p->cntrl &
					  XAXIDMA_BD_CTRL_LENGTH_MASK),
					 DMA_TO_DEVICE);
		if (cur_p->app4)
			dev_kfree_skb_irq((struct sk_buff *)cur_p->app4);
		cur_p->phys = 0;
		cur_p->cntrl = 0;
		cur_p->status = 0;
		cur_p->app0 = 0;
		cur_p->app1 = 0;
		cur_p->app2 = 0;
		cur_p->app3 = 0;
		cur_p->app4 = 0;
		cur_p->sw_id_offset = 0;
	}

	for (i = 0; i < RX_BD_NUM; i++) {
		cur_p = &lp->rx_bd_v[i];
		cur_p->status = 0;
		cur_p->app0 = 0;
		cur_p->app1 = 0;
		cur_p->app2 = 0;
		cur_p->app3 = 0;
		cur_p->app4 = 0;
	}

	lp->tx_bd_ci = 0;
	lp->tx_bd_tail = 0;
	lp->rx_bd_ci = 0;

	/* Start updating the Rx channel control register */
	cr = nixge_dma_read_reg(lp, XAXIDMA_RX_CR_OFFSET);
	/* Update the interrupt coalesce count */
	cr = ((cr & ~XAXIDMA_COALESCE_MASK) |
	      (XAXIDMA_DFT_RX_THRESHOLD << XAXIDMA_COALESCE_SHIFT));
	/* Update the delay timer count */
	cr = ((cr & ~XAXIDMA_DELAY_MASK) |
	      (XAXIDMA_DFT_RX_WAITBOUND << XAXIDMA_DELAY_SHIFT));
	/* Enable coalesce, delay timer and error interrupts */
	cr |= XAXIDMA_IRQ_ALL_MASK;
	/* Finally write to the Rx channel control register */
	nixge_dma_write_reg(lp, XAXIDMA_RX_CR_OFFSET, cr);

	/* Start updating the Tx channel control register */
	cr = nixge_dma_read_reg(lp, XAXIDMA_TX_CR_OFFSET);
	/* Update the interrupt coalesce count */
	cr = (((cr & ~XAXIDMA_COALESCE_MASK)) |
	      (XAXIDMA_DFT_TX_THRESHOLD << XAXIDMA_COALESCE_SHIFT));
	/* Update the delay timer count */
	cr = (((cr & ~XAXIDMA_DELAY_MASK)) |
	      (XAXIDMA_DFT_TX_WAITBOUND << XAXIDMA_DELAY_SHIFT));
	/* Enable coalesce, delay timer and error interrupts */
	cr |= XAXIDMA_IRQ_ALL_MASK;
	/* Finally write to the Tx channel control register */
	nixge_dma_write_reg(lp, XAXIDMA_TX_CR_OFFSET, cr);

	/* Populate the tail pointer and bring the Rx Axi DMA engine out of
	 * halted state. This will make the Rx side ready for reception.
	 */
	nixge_dma_write_reg(lp, XAXIDMA_RX_CDESC_OFFSET, lp->rx_bd_p);
	cr = nixge_dma_read_reg(lp, XAXIDMA_RX_CR_OFFSET);
	nixge_dma_write_reg(lp, XAXIDMA_RX_CR_OFFSET,
			    cr | XAXIDMA_CR_RUNSTOP_MASK);
	nixge_dma_write_reg(lp, XAXIDMA_RX_TDESC_OFFSET, lp->rx_bd_p +
			    (sizeof(*lp->rx_bd_v) * (RX_BD_NUM - 1)));

	/* Write to the RS (Run-stop) bit in the Tx channel control register.
	 * Tx channel is now ready to run. But only after we write to the
	 * tail pointer register that the Tx channel will start transmitting
	 */
	nixge_dma_write_reg(lp, XAXIDMA_TX_CDESC_OFFSET, lp->tx_bd_p);
	cr = nixge_dma_read_reg(lp, XAXIDMA_TX_CR_OFFSET);
	nixge_dma_write_reg(lp, XAXIDMA_TX_CR_OFFSET,
			    cr | XAXIDMA_CR_RUNSTOP_MASK);
}

static int nixge_open(struct net_device *ndev)
{
	struct nixge_priv *priv = netdev_priv(ndev);
	struct phy_device *phy;
	int ret;

	nixge_device_reset(ndev);

	phy = of_phy_connect(ndev, priv->phy_node,
			     &nixge_handle_link_change, 0, priv->phy_mode);
	if (!phy)
		return -ENODEV;

	phy_start(phy);

	/* Enable tasklets for Axi DMA error handling */
	tasklet_init(&priv->dma_err_tasklet, nixge_dma_err_handler,
		     (unsigned long)priv);

	/* Enable interrupts for Axi DMA Tx */
	ret = request_irq(priv->tx_irq, nixge_tx_irq, 0, ndev->name, ndev);
	if (ret)
		goto err_tx_irq;
	/* Enable interrupts for Axi DMA Rx */
	ret = request_irq(priv->rx_irq, nixge_rx_irq, 0, ndev->name, ndev);
	if (ret)
		goto err_rx_irq;

	return 0;

err_rx_irq:
	free_irq(priv->tx_irq, ndev);
err_tx_irq:
	tasklet_kill(&priv->dma_err_tasklet);
	netdev_err(ndev, "request_irq() failed\n");
	return ret;
}

static int nixge_stop(struct net_device *ndev)
{
	u32 cr;
	struct nixge_priv *priv = netdev_priv(ndev);

	cr = nixge_dma_read_reg(priv, XAXIDMA_RX_CR_OFFSET);
	nixge_dma_write_reg(priv, XAXIDMA_RX_CR_OFFSET,
			    cr & (~XAXIDMA_CR_RUNSTOP_MASK));
	cr = nixge_dma_read_reg(priv, XAXIDMA_TX_CR_OFFSET);
	nixge_dma_write_reg(priv, XAXIDMA_TX_CR_OFFSET,
			    cr & (~XAXIDMA_CR_RUNSTOP_MASK));

	tasklet_kill(&priv->dma_err_tasklet);

	free_irq(priv->tx_irq, ndev);
	free_irq(priv->rx_irq, ndev);

	nixge_dma_bd_release(ndev);

	if (ndev->phydev) {
		phy_stop(ndev->phydev);
		phy_disconnect(ndev->phydev);
	}

	return 0;
}

static int nixge_change_mtu(struct net_device *ndev, int new_mtu)
{
	struct nixge_priv *priv = netdev_priv(ndev);

	if (netif_running(ndev))
		return -EBUSY;

	if ((new_mtu + VLAN_ETH_HLEN +
		NIXGE_TRL_SIZE) > priv->rxmem)
		return -EINVAL;

	ndev->mtu = new_mtu;

	return 0;
}

static s32 __nixge_hw_set_mac_address(struct net_device *ndev)
{
	struct nixge_priv *priv = netdev_priv(ndev);

	nixge_ctrl_write_reg(priv, NIXGE_REG_MAC_LSB,
			     (ndev->dev_addr[2]) << 24 |
			     (ndev->dev_addr[3] << 16) |
			     (ndev->dev_addr[4] << 8) |
			     (ndev->dev_addr[5] << 0));

	nixge_ctrl_write_reg(priv, NIXGE_REG_MAC_MSB,
			     (ndev->dev_addr[1] | (ndev->dev_addr[0] << 8)));

	return 0;
}

static int nixge_net_set_mac_address(struct net_device *ndev, void *p)
{
	int err;

	err = eth_mac_addr(ndev, p);
	if (!err)
		__nixge_hw_set_mac_address(ndev);

	return err;
}

static const struct net_device_ops nixge_netdev_ops = {
	.ndo_open = nixge_open,
	.ndo_stop = nixge_stop,
	.ndo_start_xmit = nixge_start_xmit,
	.ndo_change_mtu	= nixge_change_mtu,
	.ndo_set_mac_address = nixge_net_set_mac_address,
	.ndo_validate_addr = eth_validate_addr,
};

static void nixge_ethtools_get_drvinfo(struct net_device *ndev,
				       struct ethtool_drvinfo *ed)
{
	strlcpy(ed->driver, "nixge", sizeof(ed->driver));
}

static int nixge_ethtools_get_coalesce(struct net_device *ndev,
				       struct ethtool_coalesce *ecoalesce)
{
	u32 regval = 0;
	struct nixge_priv *priv = netdev_priv(ndev);

	regval = nixge_dma_read_reg(priv, XAXIDMA_RX_CR_OFFSET);
	ecoalesce->rx_max_coalesced_frames = (regval & XAXIDMA_COALESCE_MASK)
					     >> XAXIDMA_COALESCE_SHIFT;
	regval = nixge_dma_read_reg(priv, XAXIDMA_TX_CR_OFFSET);
	ecoalesce->tx_max_coalesced_frames = (regval & XAXIDMA_COALESCE_MASK)
					     >> XAXIDMA_COALESCE_SHIFT;
	return 0;
}

static int nixge_ethtools_set_coalesce(struct net_device *ndev,
				       struct ethtool_coalesce *ecoalesce)
{
	struct nixge_priv *priv = netdev_priv(ndev);

	if (netif_running(ndev)) {
		netdev_err(ndev,
			   "Please stop netif before applying configuration\n");
		return -EFAULT;
	}

	if ((ecoalesce->rx_coalesce_usecs) ||
	    (ecoalesce->rx_coalesce_usecs_irq) ||
	    (ecoalesce->rx_max_coalesced_frames_irq) ||
	    (ecoalesce->tx_coalesce_usecs) ||
	    (ecoalesce->tx_coalesce_usecs_irq) ||
	    (ecoalesce->tx_max_coalesced_frames_irq) ||
	    (ecoalesce->stats_block_coalesce_usecs) ||
	    (ecoalesce->use_adaptive_rx_coalesce) ||
	    (ecoalesce->use_adaptive_tx_coalesce) ||
	    (ecoalesce->pkt_rate_low) ||
	    (ecoalesce->rx_coalesce_usecs_low) ||
	    (ecoalesce->rx_max_coalesced_frames_low) ||
	    (ecoalesce->tx_coalesce_usecs_low) ||
	    (ecoalesce->tx_max_coalesced_frames_low) ||
	    (ecoalesce->pkt_rate_high) ||
	    (ecoalesce->rx_coalesce_usecs_high) ||
	    (ecoalesce->rx_max_coalesced_frames_high) ||
	    (ecoalesce->tx_coalesce_usecs_high) ||
	    (ecoalesce->tx_max_coalesced_frames_high) ||
	    (ecoalesce->rate_sample_interval))
		return -EOPNOTSUPP;
	if (ecoalesce->rx_max_coalesced_frames)
		priv->coalesce_count_rx = ecoalesce->rx_max_coalesced_frames;
	if (ecoalesce->tx_max_coalesced_frames)
		priv->coalesce_count_tx = ecoalesce->tx_max_coalesced_frames;

	return 0;
}

static const struct ethtool_ops nixge_ethtool_ops = {
	.get_drvinfo    = nixge_ethtools_get_drvinfo,
	.get_coalesce   = nixge_ethtools_get_coalesce,
	.set_coalesce   = nixge_ethtools_set_coalesce,
};

static int nixge_mdio_read(struct mii_bus *bus, int phy_id, int reg)
{
	struct nixge_priv *priv = bus->priv;
	u32 status, tmp;
	int err;
	u16 device;

	if (reg & MII_ADDR_C45) {
		device = (reg >> 16) & 0x1f;

		nixge_ctrl_write_reg(priv, NIXGE_REG_MDIO_ADDR, reg & 0xffff);

		tmp = NIXGE_MDIO_CLAUSE45 | NIXGE_MDIO_OP(NIXGE_MDIO_OP_ADDRESS)
			| NIXGE_MDIO_ADDR(phy_id) | NIXGE_MDIO_MMD(device);

		nixge_ctrl_write_reg(priv, NIXGE_REG_MDIO_OP, tmp);
		nixge_ctrl_write_reg(priv, NIXGE_REG_MDIO_CTRL, 1);

		err = nixge_ctrl_poll_timeout(priv, NIXGE_REG_MDIO_CTRL, status,
					      !status, 10, 1000);
		if (err) {
			dev_err(priv->dev, "timeout setting address");
			return err;
		}

		tmp = NIXGE_MDIO_CLAUSE45 | NIXGE_MDIO_OP(NIXGE_MDIO_OP_READ) |
			NIXGE_MDIO_ADDR(phy_id) | NIXGE_MDIO_MMD(device);
	} else {
		device = reg & 0x1f;

		tmp = NIXGE_MDIO_CLAUSE22 | NIXGE_MDIO_OP(MDIO_C22_READ) |
			NIXGE_MDIO_ADDR(phy_id) | NIXGE_MDIO_MMD(device);
	}

	nixge_ctrl_write_reg(priv, NIXGE_REG_MDIO_OP, tmp);
	nixge_ctrl_write_reg(priv, NIXGE_REG_MDIO_CTRL, 1);

	err = nixge_ctrl_poll_timeout(priv, NIXGE_REG_MDIO_CTRL, status,
				      !status, 10, 1000);
	if (err) {
		dev_err(priv->dev, "timeout setting read command");
		return err;
	}

	status = nixge_ctrl_read_reg(priv, NIXGE_REG_MDIO_DATA);

	dev_dbg(priv->dev, "%s: phy_id = %x reg = %x got %x\n", __func__,
		phy_id, reg & 0xffff, status);

	return status;
}

static int nixge_mdio_write(struct mii_bus *bus, int phy_id, int reg, u16 val)
{
	struct nixge_priv *priv = bus->priv;
	u32 status, tmp;
	int err;
	u16 device;

	/* FIXME: Currently don't do writes */
	if (reg & MII_ADDR_C45)
		return -EOPNOTSUPP;

	device = reg & 0x1f;

	tmp = NIXGE_MDIO_CLAUSE22 | NIXGE_MDIO_OP(MDIO_C22_WRITE) |
		NIXGE_MDIO_ADDR(phy_id) | NIXGE_MDIO_MMD(device);

	nixge_ctrl_write_reg(priv, NIXGE_REG_MDIO_DATA, val);
	nixge_ctrl_write_reg(priv, NIXGE_REG_MDIO_OP, tmp);
	nixge_ctrl_write_reg(priv, NIXGE_REG_MDIO_CTRL, 1);

	err = nixge_ctrl_poll_timeout(priv, NIXGE_REG_MDIO_CTRL, status,
				      !status, 10, 1000);
	if (err) {
		dev_err(priv->dev, "timeout setting write command");
		return -ETIMEDOUT;
	}

	dev_dbg(priv->dev, "%x %x <- %x\n", phy_id, reg, val);

	return 0;
}

static int nixge_mdio_setup(struct nixge_priv *priv, struct device_node *np)
{
	struct mii_bus *bus;
	struct resource res;
	int err;

	bus = mdiobus_alloc();
	if (!bus)
		return -ENOMEM;

	of_address_to_resource(np, 0, &res);
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s-mii", dev_name(priv->dev));
	bus->priv = priv;
	bus->name = "nixge_mii_bus";
	bus->read = nixge_mdio_read;
	bus->write = nixge_mdio_write;
	bus->parent = priv->dev;

	priv->mii_bus = bus;
	err = of_mdiobus_register(bus, np);
	if (err)
		goto err_register;

	dev_info(priv->dev, "MDIO bus registered\n");

	return 0;

err_register:
	mdiobus_free(bus);
	return err;
}

static void *nixge_get_nvmem_address(struct device *dev)
{
	struct nvmem_cell *cell;
	size_t cell_size;
	char *mac;

	cell = nvmem_cell_get(dev, "address");
	if (IS_ERR(cell))
		return cell;

	mac = nvmem_cell_read(cell, &cell_size);
	nvmem_cell_put(cell);

	return mac;
}

static int nixge_probe(struct platform_device *pdev)
{
	int err;
	struct nixge_priv *priv;
	struct net_device *ndev;
	struct resource *dmares;
	const char *mac_addr;

	ndev = alloc_etherdev(sizeof(*priv));
	if (!ndev)
		return -ENOMEM;

	platform_set_drvdata(pdev, ndev);
	SET_NETDEV_DEV(ndev, &pdev->dev);

	ndev->flags &= ~IFF_MULTICAST;  /* clear multicast */
	ndev->features = NETIF_F_SG;
	ndev->netdev_ops = &nixge_netdev_ops;
	ndev->ethtool_ops = &nixge_ethtool_ops;

	/* MTU range: 64 - 9000 */
	ndev->min_mtu = 64;
	ndev->max_mtu = NIXGE_JUMBO_MTU;

	mac_addr = nixge_get_nvmem_address(&pdev->dev);
	if (mac_addr && is_valid_ether_addr(mac_addr))
		ether_addr_copy(ndev->dev_addr, mac_addr);
	else
		eth_hw_addr_random(ndev);

	priv = netdev_priv(ndev);
	priv->ndev = ndev;
	priv->dev = &pdev->dev;
	priv->rxmem = NIXGE_DEFAULT_RX_MEM;

	dmares = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->dma_regs = devm_ioremap_resource(&pdev->dev, dmares);
	if (IS_ERR(priv->dma_regs)) {
		netdev_err(ndev, "failed to map dma regs\n");
		return PTR_ERR(priv->dma_regs);
	}
	priv->ctrl_regs = priv->dma_regs + NIXGE_REG_CTRL_OFFSET;
	__nixge_hw_set_mac_address(ndev);

	priv->tx_irq = platform_get_irq_byname(pdev, "tx-irq");
	if (priv->tx_irq < 0) {
		netdev_err(ndev, "no tx irq available");
		return priv->tx_irq;
	}

	priv->rx_irq = platform_get_irq_byname(pdev, "rx-irq");
	if (priv->rx_irq < 0) {
		netdev_err(ndev, "no rx irq available");
		return priv->rx_irq;
	}

	priv->coalesce_count_rx = XAXIDMA_DFT_RX_THRESHOLD;
	priv->coalesce_count_tx = XAXIDMA_DFT_TX_THRESHOLD;

	spin_lock_init(&priv->lock);

	err = nixge_mdio_setup(priv, pdev->dev.of_node);
	if (err) {
		netdev_err(ndev, "error registering mdio bus");
		goto free_netdev;
	}

	priv->phy_mode = of_get_phy_mode(pdev->dev.of_node);
	if (priv->phy_mode < 0) {
		netdev_err(ndev, "not find phy-mode\n");
		err = -EINVAL;
		goto unregister_mdio;
	}

	priv->phy_node = of_parse_phandle(pdev->dev.of_node, "phy-handle", 0);
	if (!priv->phy_node) {
		netdev_err(ndev, "not find phy-handle\n");
		err = -EINVAL;
		goto unregister_mdio;
	}

	err = register_netdev(priv->ndev);
	if (err) {
		netdev_err(ndev, "register_netdev() error (%i)\n", err);
		goto unregister_mdio;
	}

	return 0;

unregister_mdio:
	mdiobus_unregister(priv->mii_bus);
	mdiobus_free(priv->mii_bus);

free_netdev:
	free_netdev(ndev);

	return err;
}

static int nixge_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct nixge_priv *priv = netdev_priv(ndev);

	if (ndev->phydev)
		phy_disconnect(ndev->phydev);
	ndev->phydev = NULL;

	mdiobus_unregister(priv->mii_bus);
	mdiobus_free(priv->mii_bus);
	priv->mii_bus = NULL;

	unregister_netdev(ndev);

	free_netdev(ndev);

	return 0;
}

/* Match table for of_platform binding */
static const struct of_device_id nixge_dt_ids[] = {
	{ .compatible = "ni,xge-enet-2.00", },
	{},
};
MODULE_DEVICE_TABLE(of, nixge_dt_ids);

static struct platform_driver nixge_driver = {
	.probe		= nixge_probe,
	.remove		= nixge_remove,
	.driver		= {
		.name		= "nixge",
		.of_match_table	= of_match_ptr(nixge_dt_ids),
	},
};
module_platform_driver(nixge_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("National Instruments XGE Management MAC");
MODULE_AUTHOR("Moritz Fischer <mdf@kernel.org>");
