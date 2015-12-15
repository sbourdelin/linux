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

/* Qualcomm Technologies, Inc. EMAC Gigabit Ethernet Driver
 * The EMAC driver supports following features:
 * 1) Receive Side Scaling (RSS).
 * 2) Checksum offload.
 * 3) Multiple PHY support on MDIO bus.
 * 4) Runtime power management support.
 * 5) Interrupt coalescing support.
 * 6) SGMII phy.
 * 7) SGMII direct connection (without external phy).
 */

#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/of_gpio.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include "emac.h"
#include "emac-mac.h"
#include "emac-phy.h"

#define DRV_VERSION "1.1.0.0"

static int debug = -1;
module_param(debug, int, S_IRUGO | S_IWUSR | S_IWGRP);

static int emac_irq_use_extended;
module_param(emac_irq_use_extended, int, S_IRUGO | S_IWUSR | S_IWGRP);

const char emac_drv_name[] = "qcom-emac";
const char emac_drv_description[] =
			"Qualcomm Technologies, Inc. EMAC Ethernet Driver";
const char emac_drv_version[] = DRV_VERSION;

#define EMAC_MSG_DEFAULT (NETIF_MSG_DRV | NETIF_MSG_PROBE | NETIF_MSG_LINK |  \
		NETIF_MSG_TIMER | NETIF_MSG_IFDOWN | NETIF_MSG_IFUP |         \
		NETIF_MSG_RX_ERR | NETIF_MSG_TX_ERR | NETIF_MSG_TX_QUEUED |   \
		NETIF_MSG_INTR | NETIF_MSG_TX_DONE | NETIF_MSG_RX_STATUS |    \
		NETIF_MSG_PKTDATA | NETIF_MSG_HW | NETIF_MSG_WOL)

#define EMAC_RRD_SIZE					     4
#define EMAC_TS_RRD_SIZE				     6
#define EMAC_TPD_SIZE					     4
#define EMAC_RFD_SIZE					     2

#define REG_MAC_RX_STATUS_BIN		 EMAC_RXMAC_STATC_REG0
#define REG_MAC_RX_STATUS_END		EMAC_RXMAC_STATC_REG22
#define REG_MAC_TX_STATUS_BIN		 EMAC_TXMAC_STATC_REG0
#define REG_MAC_TX_STATUS_END		EMAC_TXMAC_STATC_REG24

#define RXQ0_NUM_RFD_PREF_DEF				     8
#define TXQ0_NUM_TPD_PREF_DEF				     5

#define EMAC_PREAMBLE_DEF				     7

#define DMAR_DLY_CNT_DEF				    15
#define DMAW_DLY_CNT_DEF				     4

#define IMR_NORMAL_MASK         (\
		ISR_ERROR       |\
		ISR_GPHY_LINK   |\
		ISR_TX_PKT      |\
		GPHY_WAKEUP_INT)

#define IMR_EXTENDED_MASK       (\
		SW_MAN_INT      |\
		ISR_OVER        |\
		ISR_ERROR       |\
		ISR_GPHY_LINK   |\
		ISR_TX_PKT      |\
		GPHY_WAKEUP_INT)

#define ISR_TX_PKT      (\
	TX_PKT_INT      |\
	TX_PKT_INT1     |\
	TX_PKT_INT2     |\
	TX_PKT_INT3)

#define ISR_GPHY_LINK        (\
	GPHY_LINK_UP_INT     |\
	GPHY_LINK_DOWN_INT)

#define ISR_OVER        (\
	RFD0_UR_INT     |\
	RFD1_UR_INT     |\
	RFD2_UR_INT     |\
	RFD3_UR_INT     |\
	RFD4_UR_INT     |\
	RXF_OF_INT      |\
	TXF_UR_INT)

#define ISR_ERROR       (\
	DMAR_TO_INT     |\
	DMAW_TO_INT     |\
	TXQ_TO_INT)

static irqreturn_t emac_isr(int irq, void *data);
static irqreturn_t emac_wol_isr(int irq, void *data);

/* RSS SW woraround:
 * EMAC HW has an issue with interrupt assignment because of which receive queue
 * 1 is disabled and following receive rss queue to interrupt mapping is used:
 * rss-queue   intr
 *    0        core0
 *    1        core3 (disabled)
 *    2        core1
 *    3        core2
 */
const struct emac_irq_config emac_irq_cfg_tbl[EMAC_IRQ_CNT] = {
{ "core0_irq", emac_isr, EMAC_INT_STATUS,  EMAC_INT_MASK,  RX_PKT_INT0, 0},
{ "core3_irq", emac_isr, EMAC_INT3_STATUS, EMAC_INT3_MASK, 0,           0},
{ "core1_irq", emac_isr, EMAC_INT1_STATUS, EMAC_INT1_MASK, RX_PKT_INT2, 0},
{ "core2_irq", emac_isr, EMAC_INT2_STATUS, EMAC_INT2_MASK, RX_PKT_INT3, 0},
{ "wol_irq",   emac_wol_isr,            0,              0, 0,           0},
};

const char * const emac_gpio_name[] = {
	"qcom,emac-gpio-mdc", "qcom,emac-gpio-mdio"
};

/* in sync with enum emac_clk_id */
static const char * const emac_clk_name[] = {
	"axi_clk", "cfg_ahb_clk", "high_speed_clk", "mdio_clk", "tx_clk",
	"rx_clk", "sys_clk"
};

void emac_reg_update32(void __iomem *addr, u32 mask, u32 val)
{
	u32 data = readl_relaxed(addr);

	writel_relaxed(((data & ~mask) | val), addr);
}

/* reinitialize */
void emac_reinit_locked(struct emac_adapter *adpt)
{
	WARN_ON(in_interrupt());

	while (test_and_set_bit(EMAC_STATUS_RESETTING, &adpt->status))
		msleep(20); /* Reset might take few 10s of ms */

	if (test_bit(EMAC_STATUS_DOWN, &adpt->status)) {
		clear_bit(EMAC_STATUS_RESETTING, &adpt->status);
		return;
	}

	emac_mac_down(adpt, true);

	emac_phy_reset(adpt);
	emac_mac_up(adpt);

	clear_bit(EMAC_STATUS_RESETTING, &adpt->status);
}

void emac_work_thread_reschedule(struct emac_adapter *adpt)
{
	if (!test_bit(EMAC_STATUS_DOWN, &adpt->status) &&
	    !test_bit(EMAC_STATUS_WATCH_DOG, &adpt->status)) {
		set_bit(EMAC_STATUS_WATCH_DOG, &adpt->status);
		schedule_work(&adpt->work_thread);
	}
}

void emac_lsc_schedule_check(struct emac_adapter *adpt)
{
	set_bit(EMAC_STATUS_TASK_LSC_REQ, &adpt->status);
	adpt->link_chk_timeout = jiffies + EMAC_TRY_LINK_TIMEOUT;

	if (!test_bit(EMAC_STATUS_DOWN, &adpt->status))
		emac_work_thread_reschedule(adpt);
}

/* Change MAC address */
static int emac_set_mac_address(struct net_device *netdev, void *p)
{
	struct emac_adapter *adpt = netdev_priv(netdev);

	struct sockaddr *addr = p;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	if (netif_running(netdev))
		return -EBUSY;

	memcpy(netdev->dev_addr, addr->sa_data, netdev->addr_len);
	memcpy(adpt->mac_addr, addr->sa_data, netdev->addr_len);

	emac_mac_addr_clear(adpt, adpt->mac_addr);
	return 0;
}

/* NAPI */
static int emac_napi_rtx(struct napi_struct *napi, int budget)
{
	struct emac_rx_queue *rx_q = container_of(napi, struct emac_rx_queue,
						   napi);
	struct emac_adapter *adpt = netdev_priv(rx_q->netdev);
	struct emac_irq *irq = rx_q->irq;

	int work_done = 0;

	/* Keep link state information with original netdev */
	if (!netif_carrier_ok(adpt->netdev))
		goto quit_polling;

	emac_mac_rx_process(adpt, rx_q, &work_done, budget);

	if (work_done < budget) {
quit_polling:
		napi_complete(napi);

		irq->mask |= rx_q->intr;
		writel_relaxed(irq->mask, adpt->base +
			       emac_irq_cfg_tbl[irq->idx].mask_reg);
		wmb(); /* ensure that interrupt enable is flushed to HW */
	}

	return work_done;
}

/* Transmit the packet */
static int emac_start_xmit(struct sk_buff *skb,
			   struct net_device *netdev)
{
	struct emac_adapter *adpt = netdev_priv(netdev);
	struct emac_tx_queue *tx_q = &adpt->tx_q[EMAC_ACTIVE_TXQ];

	return emac_mac_tx_buf_send(adpt, tx_q, skb);
}

/* ISR */
static irqreturn_t emac_wol_isr(int irq, void *data)
{
	netif_dbg(emac_irq_get_adpt(data), wol, emac_irq_get_adpt(data)->netdev,
		  "EMAC wol interrupt received\n");
	return IRQ_HANDLED;
}

static irqreturn_t emac_isr(int _irq, void *data)
{
	struct emac_irq *irq = data;
	const struct emac_irq_config *irq_cfg = &emac_irq_cfg_tbl[irq->idx];
	struct emac_adapter *adpt = emac_irq_get_adpt(data);
	struct emac_rx_queue *rx_q = &adpt->rx_q[irq->idx];

	int max_ints = 1;
	u32 isr, status;

	/* disable the interrupt */
	writel_relaxed(0, adpt->base + irq_cfg->mask_reg);
	wmb(); /* ensure that interrupt disable is flushed to HW */

	do {
		isr = readl_relaxed(adpt->base + irq_cfg->status_reg);
		status = isr & irq->mask;

		if (status == 0)
			break;

		if (status & ISR_ERROR) {
			netif_warn(adpt,  intr, adpt->netdev,
				   "warning: error irq status 0x%x\n",
				   status & ISR_ERROR);
			/* reset MAC */
			set_bit(EMAC_STATUS_TASK_REINIT_REQ, &adpt->status);
			emac_work_thread_reschedule(adpt);
		}

		/* Schedule the napi for receive queue with interrupt
		 * status bit set
		 */
		if ((status & rx_q->intr)) {
			if (napi_schedule_prep(&rx_q->napi)) {
				irq->mask &= ~rx_q->intr;
				__napi_schedule(&rx_q->napi);
			}
		}

		if (status & ISR_TX_PKT) {
			if (status & TX_PKT_INT)
				emac_mac_tx_process(adpt, &adpt->tx_q[0]);
			if (status & TX_PKT_INT1)
				emac_mac_tx_process(adpt, &adpt->tx_q[1]);
			if (status & TX_PKT_INT2)
				emac_mac_tx_process(adpt, &adpt->tx_q[2]);
			if (status & TX_PKT_INT3)
				emac_mac_tx_process(adpt, &adpt->tx_q[3]);
		}

		if (status & ISR_OVER)
			netif_warn(adpt, intr, adpt->netdev,
				   "warning: TX/RX overflow status 0x%x\n",
				   status & ISR_OVER);

		/* link event */
		if (status & (ISR_GPHY_LINK | SW_MAN_INT)) {
			emac_lsc_schedule_check(adpt);
			break;
		}
	} while (--max_ints > 0);

	/* enable the interrupt */
	writel_relaxed(irq->mask, adpt->base + irq_cfg->mask_reg);
	wmb(); /* ensure that interrupt enable is flushed to HW */
	return IRQ_HANDLED;
}

/* Configure VLAN tag strip/insert feature */
static int emac_set_features(struct net_device *netdev,
			     netdev_features_t features)
{
	struct emac_adapter *adpt = netdev_priv(netdev);

	netdev_features_t changed = features ^ netdev->features;

	if (!(changed & (NETIF_F_HW_VLAN_CTAG_TX | NETIF_F_HW_VLAN_CTAG_RX)))
		return 0;

	netdev->features = features;
	if (netdev->features & NETIF_F_HW_VLAN_CTAG_RX)
		set_bit(EMAC_STATUS_VLANSTRIP_EN, &adpt->status);
	else
		clear_bit(EMAC_STATUS_VLANSTRIP_EN, &adpt->status);

	if (netif_running(netdev))
		emac_reinit_locked(adpt);

	return 0;
}

/* Configure Multicast and Promiscuous modes */
void emac_rx_mode_set(struct net_device *netdev)
{
	struct emac_adapter *adpt = netdev_priv(netdev);

	struct netdev_hw_addr *ha;

	/* Check for Promiscuous and All Multicast modes */
	if (netdev->flags & IFF_PROMISC) {
		set_bit(EMAC_STATUS_PROMISC_EN, &adpt->status);
	} else if (netdev->flags & IFF_ALLMULTI) {
		set_bit(EMAC_STATUS_MULTIALL_EN, &adpt->status);
		clear_bit(EMAC_STATUS_PROMISC_EN, &adpt->status);
	} else {
		clear_bit(EMAC_STATUS_MULTIALL_EN, &adpt->status);
		clear_bit(EMAC_STATUS_PROMISC_EN, &adpt->status);
	}
	emac_mac_mode_config(adpt);

	/* update multicast address filtering */
	emac_mac_multicast_addr_clear(adpt);
	netdev_for_each_mc_addr(ha, netdev)
		emac_mac_multicast_addr_set(adpt, ha->addr);
}

/* Change the Maximum Transfer Unit (MTU) */
static int emac_change_mtu(struct net_device *netdev, int new_mtu)
{
	struct emac_adapter *adpt = netdev_priv(netdev);
	int old_mtu   = netdev->mtu;
	int max_frame = new_mtu + ETH_HLEN + ETH_FCS_LEN + VLAN_HLEN;

	if ((max_frame < EMAC_MIN_ETH_FRAME_SIZE) ||
	    (max_frame > EMAC_MAX_ETH_FRAME_SIZE)) {
		netdev_err(adpt->netdev, "error: invalid MTU setting\n");
		return -EINVAL;
	}

	if ((old_mtu != new_mtu) && netif_running(netdev)) {
		netif_info(adpt, hw, adpt->netdev,
			   "changing MTU from %d to %d\n", netdev->mtu,
			   new_mtu);
		netdev->mtu = new_mtu;
		adpt->mtu = new_mtu;
		adpt->rxbuf_size = new_mtu > EMAC_DEF_RX_BUF_SIZE ?
			ALIGN(max_frame, 8) : EMAC_DEF_RX_BUF_SIZE;
		emac_reinit_locked(adpt);
	}

	return 0;
}

/* Called when the network interface is made active */
static int emac_open(struct net_device *netdev)
{
	struct emac_adapter *adpt = netdev_priv(netdev);
	int retval;

	netif_carrier_off(netdev);

	/* allocate rx/tx dma buffer & descriptors */
	retval = emac_mac_rx_tx_rings_alloc_all(adpt);
	if (retval) {
		netdev_err(adpt->netdev, "error allocating rx/tx rings\n");
		goto err_alloc_rtx;
	}

	pm_runtime_set_active(netdev->dev.parent);
	pm_runtime_enable(netdev->dev.parent);

	retval = emac_mac_up(adpt);
	if (retval)
		goto err_up;

	return retval;

err_up:
	emac_mac_rx_tx_rings_free_all(adpt);
err_alloc_rtx:
	return retval;
}

/* Called when the network interface is disabled */
static int emac_close(struct net_device *netdev)
{
	struct emac_adapter *adpt = netdev_priv(netdev);

	/* ensure no task is running and no reset is in progress */
	while (test_and_set_bit(EMAC_STATUS_RESETTING, &adpt->status))
		msleep(20); /* Reset might take few 10s of ms */

	pm_runtime_disable(netdev->dev.parent);
	if (!test_bit(EMAC_STATUS_DOWN, &adpt->status))
		emac_mac_down(adpt, true);
	else
		emac_mac_reset(adpt);

	emac_mac_rx_tx_rings_free_all(adpt);

	clear_bit(EMAC_STATUS_RESETTING, &adpt->status);
	return 0;
}

/* PHY related IOCTLs */
static int emac_mii_ioctl(struct net_device *netdev,
			  struct ifreq *ifr, int cmd)
{
	struct emac_adapter *adpt = netdev_priv(netdev);
	struct emac_phy *phy = &adpt->phy;
	struct mii_ioctl_data *data = if_mii(ifr);
	int retval = 0;

	switch (cmd) {
	case SIOCGMIIPHY:
		data->phy_id = phy->addr;
		break;

	case SIOCGMIIREG:
		if (!capable(CAP_NET_ADMIN)) {
			retval = -EPERM;
			break;
		}

		if (data->reg_num & ~(0x1F)) {
			retval = -EFAULT;
			break;
		}

		if (data->phy_id >= PHY_MAX_ADDR) {
			retval = -EFAULT;
			break;
		}

		if (phy->external && data->phy_id != phy->addr) {
			retval = -EFAULT;
			break;
		}

		retval = emac_phy_read(adpt, data->phy_id, data->reg_num,
				       &data->val_out);
		break;

	case SIOCSMIIREG:
		if (!capable(CAP_NET_ADMIN)) {
			retval = -EPERM;
			break;
		}

		if (data->reg_num & ~(0x1F)) {
			retval = -EFAULT;
			break;
		}

		if (data->phy_id >= PHY_MAX_ADDR) {
			retval = -EFAULT;
			break;
		}

		if (phy->external && data->phy_id != phy->addr) {
			retval = -EFAULT;
			break;
		}

		retval = emac_phy_write(adpt, data->phy_id, data->reg_num,
					data->val_in);

		break;
	}

	return retval;
}

/* Respond to a TX hang */
static void emac_tx_timeout(struct net_device *netdev)
{
	struct emac_adapter *adpt = netdev_priv(netdev);

	if (!test_bit(EMAC_STATUS_DOWN, &adpt->status)) {
		set_bit(EMAC_STATUS_TASK_REINIT_REQ, &adpt->status);
		emac_work_thread_reschedule(adpt);
	}
}

/* IOCTL support for the interface */
static int emac_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd)
{
	switch (cmd) {
	case SIOCGMIIPHY:
	case SIOCGMIIREG:
	case SIOCSMIIREG:
		return emac_mii_ioctl(netdev, ifr, cmd);
	case SIOCSHWTSTAMP:
	default:
		return -EOPNOTSUPP;
	}
}

/* Provide network statistics info for the interface */
struct rtnl_link_stats64 *emac_get_stats64(struct net_device *netdev,
					   struct rtnl_link_stats64 *net_stats)
{
	struct emac_adapter *adpt = netdev_priv(netdev);
	struct emac_stats *stats = &adpt->stats;
	u16 addr = REG_MAC_RX_STATUS_BIN;
	u64 *stats_itr = &adpt->stats.rx_ok;
	u32 val;

	while (addr <= REG_MAC_RX_STATUS_END) {
		val = readl_relaxed(adpt->base + addr);
		*stats_itr += val;
		++stats_itr;
		addr += sizeof(u32);
	}

	/* additional rx status */
	val = readl_relaxed(adpt->base + EMAC_RXMAC_STATC_REG23);
	adpt->stats.rx_crc_align += val;
	val = readl_relaxed(adpt->base + EMAC_RXMAC_STATC_REG24);
	adpt->stats.rx_jubbers += val;

	/* update tx status */
	addr = REG_MAC_TX_STATUS_BIN;
	stats_itr = &adpt->stats.tx_ok;

	while (addr <= REG_MAC_TX_STATUS_END) {
		val = readl_relaxed(adpt->base + addr);
		*stats_itr += val;
		++stats_itr;
		addr += sizeof(u32);
	}

	/* additional tx status */
	val = readl_relaxed(adpt->base + EMAC_TXMAC_STATC_REG25);
	adpt->stats.tx_col += val;

	/* return parsed statistics */
	net_stats->rx_packets = stats->rx_ok;
	net_stats->tx_packets = stats->tx_ok;
	net_stats->rx_bytes = stats->rx_byte_cnt;
	net_stats->tx_bytes = stats->tx_byte_cnt;
	net_stats->multicast = stats->rx_mcast;
	net_stats->collisions = stats->tx_1_col + stats->tx_2_col * 2 +
				stats->tx_late_col + stats->tx_abort_col;

	net_stats->rx_errors = stats->rx_frag + stats->rx_fcs_err +
			       stats->rx_len_err + stats->rx_sz_ov +
			       stats->rx_align_err;
	net_stats->rx_fifo_errors = stats->rx_rxf_ov;
	net_stats->rx_length_errors = stats->rx_len_err;
	net_stats->rx_crc_errors = stats->rx_fcs_err;
	net_stats->rx_frame_errors = stats->rx_align_err;
	net_stats->rx_over_errors = stats->rx_rxf_ov;
	net_stats->rx_missed_errors = stats->rx_rxf_ov;

	net_stats->tx_errors = stats->tx_late_col + stats->tx_abort_col +
			       stats->tx_underrun + stats->tx_trunc;
	net_stats->tx_fifo_errors = stats->tx_underrun;
	net_stats->tx_aborted_errors = stats->tx_abort_col;
	net_stats->tx_window_errors = stats->tx_late_col;

	return net_stats;
}

static const struct net_device_ops emac_netdev_ops = {
	.ndo_open		= &emac_open,
	.ndo_stop		= &emac_close,
	.ndo_validate_addr	= &eth_validate_addr,
	.ndo_start_xmit		= &emac_start_xmit,
	.ndo_set_mac_address	= &emac_set_mac_address,
	.ndo_change_mtu		= &emac_change_mtu,
	.ndo_do_ioctl		= &emac_ioctl,
	.ndo_tx_timeout		= &emac_tx_timeout,
	.ndo_get_stats64	= &emac_get_stats64,
	.ndo_set_features       = emac_set_features,
	.ndo_set_rx_mode        = emac_rx_mode_set,
};

static inline char *emac_link_speed_to_str(u32 speed)
{
	switch (speed) {
	case EMAC_LINK_SPEED_1GB_FULL:
		return  "1 Gbps Duplex Full";
	case EMAC_LINK_SPEED_100_FULL:
		return "100 Mbps Duplex Full";
	case EMAC_LINK_SPEED_100_HALF:
		return "100 Mbps Duplex Half";
	case EMAC_LINK_SPEED_10_FULL:
		return "10 Mbps Duplex Full";
	case EMAC_LINK_SPEED_10_HALF:
		return "10 Mbps Duplex HALF";
	default:
		return "unknown speed";
	}
}

/* Check link status and handle link state changes */
static void emac_work_thread_link_check(struct emac_adapter *adpt)
{
	struct net_device *netdev = adpt->netdev;
	struct emac_phy *phy = &adpt->phy;

	char *speed;

	if (!test_bit(EMAC_STATUS_TASK_LSC_REQ, &adpt->status))
		return;
	clear_bit(EMAC_STATUS_TASK_LSC_REQ, &adpt->status);

	/* ensure that no reset is in progress while link task is running */
	while (test_and_set_bit(EMAC_STATUS_RESETTING, &adpt->status))
		msleep(20); /* Reset might take few 10s of ms */

	if (test_bit(EMAC_STATUS_DOWN, &adpt->status))
		goto link_task_done;

	emac_phy_link_check(adpt, &phy->link_speed, &phy->link_up);
	speed = emac_link_speed_to_str(phy->link_speed);

	if (phy->link_up) {
		if (netif_carrier_ok(netdev))
			goto link_task_done;

		pm_runtime_get_sync(netdev->dev.parent);
		netif_info(adpt, timer, adpt->netdev, "NIC Link is Up %s\n",
			   speed);

		emac_mac_start(adpt);
		netif_carrier_on(netdev);
		netif_wake_queue(netdev);
	} else {
		if (time_after(adpt->link_chk_timeout, jiffies))
			set_bit(EMAC_STATUS_TASK_LSC_REQ, &adpt->status);

		/* only continue if link was up previously */
		if (!netif_carrier_ok(netdev))
			goto link_task_done;

		phy->link_speed = 0;
		netif_info(adpt,  timer, adpt->netdev, "NIC Link is Down\n");
		netif_stop_queue(netdev);
		netif_carrier_off(netdev);

		emac_mac_stop(adpt);
		pm_runtime_put_sync(netdev->dev.parent);
	}

	/* link state transition, kick timer */
	mod_timer(&adpt->timers, jiffies);

link_task_done:
	clear_bit(EMAC_STATUS_RESETTING, &adpt->status);
}

/* Watchdog task routine */
static void emac_work_thread(struct work_struct *work)
{
	struct emac_adapter *adpt = container_of(work, struct emac_adapter,
						 work_thread);

	if (!test_bit(EMAC_STATUS_WATCH_DOG, &adpt->status))
		netif_warn(adpt,  timer, adpt->netdev,
			   "warning: WATCH_DOG flag isn't set\n");

	if (test_bit(EMAC_STATUS_TASK_REINIT_REQ, &adpt->status)) {
		clear_bit(EMAC_STATUS_TASK_REINIT_REQ, &adpt->status);

		if ((!test_bit(EMAC_STATUS_DOWN, &adpt->status)) &&
		    (!test_bit(EMAC_STATUS_RESETTING, &adpt->status)))
			emac_reinit_locked(adpt);
	}

	emac_work_thread_link_check(adpt);
	emac_phy_periodic_check(adpt);
	clear_bit(EMAC_STATUS_WATCH_DOG, &adpt->status);
}

/* Timer routine */
static void emac_timer_thread(unsigned long data)
{
	struct emac_adapter *adpt = (struct emac_adapter *)data;
	unsigned long delay;

	if (pm_runtime_status_suspended(adpt->netdev->dev.parent))
		return;

	/* poll faster when waiting for link */
	if (test_bit(EMAC_STATUS_TASK_LSC_REQ, &adpt->status))
		delay = HZ / 10;
	else
		delay = 2 * HZ;

	/* Reset the timer */
	mod_timer(&adpt->timers, delay + jiffies);

	emac_work_thread_reschedule(adpt);
}

/* Initialize various data structures  */
static void emac_init_adapter(struct emac_adapter *adpt)
{
	struct emac_phy *phy = &adpt->phy;
	int max_frame;
	u32 reg;

	/* ids */
	reg =  readl_relaxed(adpt->base + EMAC_DMA_MAS_CTRL);
	adpt->devid = (reg & DEV_ID_NUM_BMSK)  >> DEV_ID_NUM_SHFT;
	adpt->revid = (reg & DEV_REV_NUM_BMSK) >> DEV_REV_NUM_SHFT;

	/* descriptors */
	adpt->tx_desc_cnt = EMAC_DEF_TX_DESCS;
	adpt->rx_desc_cnt = EMAC_DEF_RX_DESCS;

	/* mtu */
	adpt->netdev->mtu = ETH_DATA_LEN;
	adpt->mtu = adpt->netdev->mtu;
	max_frame = adpt->netdev->mtu + ETH_HLEN + ETH_FCS_LEN + VLAN_HLEN;
	adpt->rxbuf_size = adpt->netdev->mtu > EMAC_DEF_RX_BUF_SIZE ?
			   ALIGN(max_frame, 8) : EMAC_DEF_RX_BUF_SIZE;

	/* dma */
	adpt->dma_order = emac_dma_ord_out;
	adpt->dmar_block = emac_dma_req_4096;
	adpt->dmaw_block = emac_dma_req_128;
	adpt->dmar_dly_cnt = DMAR_DLY_CNT_DEF;
	adpt->dmaw_dly_cnt = DMAW_DLY_CNT_DEF;
	adpt->tpd_burst = TXQ0_NUM_TPD_PREF_DEF;
	adpt->rfd_burst = RXQ0_NUM_RFD_PREF_DEF;

	/* link */
	phy->link_up = false;
	phy->link_speed = EMAC_LINK_SPEED_UNKNOWN;

	/* flow control */
	phy->req_fc_mode = EMAC_FC_FULL;
	phy->cur_fc_mode = EMAC_FC_FULL;
	phy->disable_fc_autoneg = false;

	/* rss */
	adpt->rss_initialized = false;
	adpt->rss_hstype = 0;
	adpt->rss_idt_size = 0;
	adpt->rss_base_cpu = 0;
	memset(adpt->rss_idt, 0x0, sizeof(adpt->rss_idt));
	memset(adpt->rss_key, 0x0, sizeof(adpt->rss_key));

	/* irq moderator */
	reg = ((EMAC_DEF_RX_IRQ_MOD >> 1) << IRQ_MODERATOR2_INIT_SHFT) |
	      ((EMAC_DEF_TX_IRQ_MOD >> 1) << IRQ_MODERATOR_INIT_SHFT);
	adpt->irq_mod = reg;

	/* others */
	adpt->preamble = EMAC_PREAMBLE_DEF;
	adpt->wol = EMAC_WOL_MAGIC | EMAC_WOL_PHY;
}

#ifdef CONFIG_PM
static int emac_runtime_suspend(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);
	struct net_device *netdev = dev_get_drvdata(&pdev->dev);
	struct emac_adapter *adpt = netdev_priv(netdev);

	emac_mac_pm(adpt, adpt->phy.link_speed, !!adpt->wol,
		    !!(adpt->wol & EMAC_WOL_MAGIC));
	return 0;
}

static int emac_runtime_idle(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);
	struct net_device *netdev = dev_get_drvdata(&pdev->dev);

	/* schedule to enter runtime suspend state if the link does
	 * not come back up within the specified time
	 */
	pm_schedule_suspend(netdev->dev.parent,
			    jiffies_to_msecs(EMAC_TRY_LINK_TIMEOUT));
	return -EBUSY;
}
#endif /* CONFIG_PM */

#ifdef CONFIG_PM_SLEEP
static int emac_suspend(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);
	struct net_device *netdev = dev_get_drvdata(&pdev->dev);
	struct emac_adapter *adpt = netdev_priv(netdev);
	struct emac_phy *phy = &adpt->phy;
	int i;
	u32 speed, adv_speed;
	bool link_up = false;
	int retval = 0;

	/* cannot suspend if WOL is disabled */
	if (!adpt->irq[EMAC_WOL_IRQ].irq)
		return -EPERM;

	netif_device_detach(netdev);
	if (netif_running(netdev)) {
		/* ensure no task is running and no reset is in progress */
		while (test_and_set_bit(EMAC_STATUS_RESETTING, &adpt->status))
			msleep(20); /* Reset might take few 10s of ms */

		emac_mac_down(adpt, false);

		clear_bit(EMAC_STATUS_RESETTING, &adpt->status);
	}

	emac_phy_link_check(adpt, &speed, &link_up);

	if (link_up) {
		adv_speed = EMAC_LINK_SPEED_10_HALF;
		emac_phy_link_speed_get(adpt, &adv_speed);

		retval = emac_phy_link_setup(adpt, adv_speed, true,
					     !adpt->phy.disable_fc_autoneg);
		if (retval)
			return retval;

		link_up = false;
		for (i = 0; i < EMAC_MAX_SETUP_LNK_CYCLE; i++) {
			retval = emac_phy_link_check(adpt, &speed, &link_up);
			if ((!retval) && link_up)
				break;

			/* link can take upto few seconds to come up */
			msleep(100);
		}
	}

	if (!link_up)
		speed = EMAC_LINK_SPEED_10_HALF;

	phy->link_speed = speed;
	phy->link_up = link_up;

	emac_mac_wol_config(adpt, adpt->wol);
	emac_mac_pm(adpt, phy->link_speed, !!adpt->wol,
		    !!(adpt->wol & EMAC_WOL_MAGIC));
	return 0;
}

static int emac_resume(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);
	struct net_device *netdev = dev_get_drvdata(&pdev->dev);
	struct emac_adapter *adpt = netdev_priv(netdev);
	struct emac_phy *phy = &adpt->phy;
	u32 retval;

	emac_mac_reset(adpt);
	retval = emac_phy_link_setup(adpt, phy->autoneg_advertised, true,
				     !phy->disable_fc_autoneg);
	if (retval)
		return retval;

	emac_mac_wol_config(adpt, 0);
	if (netif_running(netdev)) {
		retval = emac_mac_up(adpt);
		if (retval)
			return retval;
	}

	netif_device_attach(netdev);
	return 0;
}
#endif /* CONFIG_PM_SLEEP */

/* Get the clock */
static int emac_clks_get(struct platform_device *pdev,
			 struct emac_adapter *adpt)
{
	struct clk *clk;
	int i;

	for (i = 0; i < EMAC_CLK_CNT; i++) {
		clk = clk_get(&pdev->dev, emac_clk_name[i]);

		if (IS_ERR(clk)) {
			netdev_err(adpt->netdev, "error:%ld on clk_get(%s)\n",
				   PTR_ERR(clk), emac_clk_name[i]);

			while (--i >= 0)
				if (adpt->clk[i])
					clk_put(adpt->clk[i]);
			return PTR_ERR(clk);
		}

		adpt->clk[i] = clk;
	}

	return 0;
}

/* Initialize clocks */
static int emac_clks_phase1_init(struct emac_adapter *adpt)
{
	int retval;

	retval = clk_prepare_enable(adpt->clk[EMAC_CLK_AXI]);
	if (retval)
		return retval;

	retval = clk_prepare_enable(adpt->clk[EMAC_CLK_CFG_AHB]);
	if (retval)
		return retval;

	retval = clk_set_rate(adpt->clk[EMAC_CLK_HIGH_SPEED],
			      EMC_CLK_RATE_19_2MHZ);
	if (retval)
		return retval;

	retval = clk_prepare_enable(adpt->clk[EMAC_CLK_HIGH_SPEED]);

	return retval;
}

/* Enable clocks; needs emac_clks_phase1_init to be called before */
static int emac_clks_phase2_init(struct emac_adapter *adpt)
{
	int retval;

	retval = clk_set_rate(adpt->clk[EMAC_CLK_TX], EMC_CLK_RATE_125MHZ);
	if (retval)
		return retval;

	retval = clk_prepare_enable(adpt->clk[EMAC_CLK_TX]);
	if (retval)
		return retval;

	retval = clk_set_rate(adpt->clk[EMAC_CLK_HIGH_SPEED],
			      EMC_CLK_RATE_125MHZ);
	if (retval)
		return retval;

	retval = clk_set_rate(adpt->clk[EMAC_CLK_MDIO],
			      EMC_CLK_RATE_25MHZ);
	if (retval)
		return retval;

	retval = clk_prepare_enable(adpt->clk[EMAC_CLK_MDIO]);
	if (retval)
		return retval;

	retval = clk_prepare_enable(adpt->clk[EMAC_CLK_RX]);
	if (retval)
		return retval;

	retval = clk_prepare_enable(adpt->clk[EMAC_CLK_SYS]);

	return retval;
}

static void emac_clks_phase1_teardown(struct emac_adapter *adpt)
{
	clk_disable_unprepare(adpt->clk[EMAC_CLK_AXI]);
	clk_disable_unprepare(adpt->clk[EMAC_CLK_CFG_AHB]);
	clk_disable_unprepare(adpt->clk[EMAC_CLK_HIGH_SPEED]);
}

static void emac_clks_phase2_teardown(struct emac_adapter *adpt)
{
	clk_disable_unprepare(adpt->clk[EMAC_CLK_TX]);
	clk_disable_unprepare(adpt->clk[EMAC_CLK_MDIO]);
	clk_disable_unprepare(adpt->clk[EMAC_CLK_RX]);
	clk_disable_unprepare(adpt->clk[EMAC_CLK_SYS]);
}

/* Get the resources */
static int emac_probe_resources(struct platform_device *pdev,
				struct emac_adapter *adpt)
{
	struct net_device *netdev = adpt->netdev;
	struct device_node *node = pdev->dev.of_node;
	struct resource *res;
	const void *maddr;
	int retval = 0;
	int i;

	if (!node)
		return -ENODEV;

	/* get id */
	retval = of_property_read_u32(node, "cell-index", &pdev->id);
	if (retval)
		return retval;

	/* get time stamp enable flag */
	adpt->timestamp_en = of_property_read_bool(node, "qcom,emac-tstamp-en");

	/* get gpios */
	for (i = 0; adpt->phy.uses_gpios && i < EMAC_GPIO_CNT; i++) {
		retval = of_get_named_gpio(node, emac_gpio_name[i], 0);
		if (retval < 0)
			return retval;

		adpt->gpio[i] = retval;
	}

	/* get mac address */
	maddr = of_get_mac_address(node);
	if (!maddr)
		return -ENODEV;

	memcpy(adpt->mac_perm_addr, maddr, netdev->addr_len);

	/* get irqs */
	for (i = 0; i < EMAC_IRQ_CNT; i++) {
		retval = platform_get_irq_byname(pdev,
						 emac_irq_cfg_tbl[i].name);
		adpt->irq[i].irq = (retval > 0) ? retval : 0;
	}

	retval = emac_clks_get(pdev, adpt);
	if (retval)
		return retval;

	/* get register addresses */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "base");
	if (!res) {
		netdev_err(adpt->netdev, "error: missing 'base' resource\n");
		retval = -ENXIO;
		goto err_reg_res;
	}

	adpt->base = devm_ioremap_resource(&pdev->dev, res);
	if (!adpt->base) {
		retval = -ENOMEM;
		goto err_reg_res;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "csr");
	if (!res) {
		netdev_err(adpt->netdev, "error: missing 'csr' resource\n");
		retval = -ENXIO;
		goto err_reg_res;
	}

	adpt->csr = devm_ioremap_resource(&pdev->dev, res);
	if (!adpt->csr) {
		retval = -ENOMEM;
		goto err_reg_res;
	}

	netdev->base_addr = (unsigned long)adpt->base;
	return 0;

err_reg_res:
	for (i = 0; i < EMAC_CLK_CNT; i++) {
		if (adpt->clk[i])
			clk_put(adpt->clk[i]);
	}

	return retval;
}

/* Release resources */
static void emac_release_resources(struct emac_adapter *adpt)
{
	int i;

	for (i = 0; i < EMAC_CLK_CNT; i++) {
		if (adpt->clk[i])
			clk_put(adpt->clk[i]);
	}
}

/* Probe function */
static int emac_probe(struct platform_device *pdev)
{
	struct net_device *netdev;
	struct emac_adapter *adpt;
	struct emac_phy *phy;
	int i, retval = 0;
	u32 hw_ver;

	netdev = alloc_etherdev(sizeof(struct emac_adapter));
	if (!netdev)
		return -ENOMEM;

	dev_set_drvdata(&pdev->dev, netdev);
	SET_NETDEV_DEV(netdev, &pdev->dev);

	adpt = netdev_priv(netdev);
	adpt->netdev = netdev;
	phy = &adpt->phy;
	adpt->msg_enable = netif_msg_init(debug, EMAC_MSG_DEFAULT);

	adpt->dma_mask = DMA_BIT_MASK(32);
	pdev->dev.dma_mask = &adpt->dma_mask;
	pdev->dev.dma_parms = &adpt->dma_parms;
	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);

	dma_set_max_seg_size(&pdev->dev, 65536);
	dma_set_seg_boundary(&pdev->dev, 0xffffffff);

	for (i = 0; i < EMAC_IRQ_CNT; i++) {
		adpt->irq[i].idx  = i;
		adpt->irq[i].mask = emac_irq_cfg_tbl[i].init_mask;
	}
	adpt->irq[0].mask |= (emac_irq_use_extended ? IMR_EXTENDED_MASK :
			      IMR_NORMAL_MASK);

	retval = emac_probe_resources(pdev, adpt);
	if (retval)
		goto err_undo_netdev;

	/* initialize clocks */
	retval = emac_clks_phase1_init(adpt);
	if (retval)
		goto err_undo_resources;

	hw_ver = readl_relaxed(adpt->base + EMAC_CORE_HW_VERSION);

	netdev->watchdog_timeo = EMAC_WATCHDOG_TIME;
	netdev->irq = adpt->irq[0].irq;

	if (adpt->timestamp_en)
		adpt->rrd_size = EMAC_TS_RRD_SIZE;
	else
		adpt->rrd_size = EMAC_RRD_SIZE;

	adpt->tpd_size = EMAC_TPD_SIZE;
	adpt->rfd_size = EMAC_RFD_SIZE;

	/* init netdev */
	netdev->netdev_ops = &emac_netdev_ops;

	/* init adapter */
	emac_init_adapter(adpt);

	/* init phy */
	retval = emac_phy_config(pdev, adpt);
	if (retval)
		goto err_undo_clk_phase1;

	/* enable clocks */
	retval = emac_clks_phase2_init(adpt);
	if (retval)
		goto err_undo_clk_phase1;

	/* init external phy */
	retval = emac_phy_external_init(adpt);
	if (retval)
		goto err_undo_clk_phase2;

	/* reset mac */
	emac_mac_reset(adpt);

	/* setup link to put it in a known good starting state */
	retval = emac_phy_link_setup(adpt, phy->autoneg_advertised, true,
				     !phy->disable_fc_autoneg);
	if (retval)
		goto err_undo_clk_phase2;

	/* set mac address */
	memcpy(adpt->mac_addr, adpt->mac_perm_addr, netdev->addr_len);
	memcpy(netdev->dev_addr, adpt->mac_addr, netdev->addr_len);
	emac_mac_addr_clear(adpt, adpt->mac_addr);

	/* set hw features */
	netdev->features = NETIF_F_SG | NETIF_F_HW_CSUM | NETIF_F_RXCSUM |
			NETIF_F_TSO | NETIF_F_TSO6 | NETIF_F_HW_VLAN_CTAG_RX |
			NETIF_F_HW_VLAN_CTAG_TX;
	netdev->hw_features = netdev->features;

	netdev->vlan_features |= NETIF_F_SG | NETIF_F_HW_CSUM |
				 NETIF_F_TSO | NETIF_F_TSO6;

	setup_timer(&adpt->timers, &emac_timer_thread,
		    (unsigned long)adpt);
	INIT_WORK(&adpt->work_thread, emac_work_thread);

	/* Initialize queues */
	emac_mac_rx_tx_ring_init_all(pdev, adpt);

	for (i = 0; i < adpt->rx_q_cnt; i++)
		netif_napi_add(netdev, &adpt->rx_q[i].napi,
			       emac_napi_rtx, 64);

	spin_lock_init(&adpt->tx_ts_lock);
	skb_queue_head_init(&adpt->tx_ts_pending_queue);
	skb_queue_head_init(&adpt->tx_ts_ready_queue);
	INIT_WORK(&adpt->tx_ts_task, emac_mac_tx_ts_periodic_routine);

	set_bit(EMAC_STATUS_VLANSTRIP_EN, &adpt->status);
	set_bit(EMAC_STATUS_DOWN, &adpt->status);
	strlcpy(netdev->name, "eth%d", sizeof(netdev->name));

	retval = register_netdev(netdev);
	if (retval)
		goto err_undo_clk_phase2;

	pr_info("%s - version %s\n", emac_drv_description, emac_drv_version);
	netif_dbg(adpt, probe, adpt->netdev, "EMAC HW ID %d.%d\n", adpt->devid,
		  adpt->revid);
	netif_dbg(adpt, probe, adpt->netdev, "EMAC HW version %d.%d.%d\n",
		  (hw_ver & MAJOR_BMSK) >> MAJOR_SHFT,
		  (hw_ver & MINOR_BMSK) >> MINOR_SHFT,
		  (hw_ver & STEP_BMSK)  >> STEP_SHFT);
	return 0;

err_undo_clk_phase2:
	emac_clks_phase2_teardown(adpt);
err_undo_clk_phase1:
	emac_clks_phase1_teardown(adpt);
err_undo_resources:
	emac_release_resources(adpt);
err_undo_netdev:
	free_netdev(netdev);
	return retval;
}

static int emac_remove(struct platform_device *pdev)
{
	struct net_device *netdev = dev_get_drvdata(&pdev->dev);
	struct emac_adapter *adpt = netdev_priv(netdev);

	pr_info("removing %s\n", emac_drv_name);

	unregister_netdev(netdev);
	emac_clks_phase2_teardown(adpt);
	emac_clks_phase1_teardown(adpt);
	emac_release_resources(adpt);
	free_netdev(netdev);
	dev_set_drvdata(&pdev->dev, NULL);

	return 0;
}

static const struct dev_pm_ops emac_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(
		emac_suspend,
		emac_resume
	)
	SET_RUNTIME_PM_OPS(
		emac_runtime_suspend,
		NULL,
		emac_runtime_idle
	)
};

static const struct of_device_id emac_dt_match[] = {
	{
		.compatible = "qcom,emac",
	},
	{}
};

static struct platform_driver emac_platform_driver = {
	.probe	= emac_probe,
	.remove	= emac_remove,
	.driver = {
		.owner		= THIS_MODULE,
		.name		= emac_drv_name,
		.pm		= &emac_pm_ops,
		.of_match_table = emac_dt_match,
	},
};

static int __init emac_module_init(void)
{
	return platform_driver_register(&emac_platform_driver);
}

static void __exit emac_module_exit(void)
{
	platform_driver_unregister(&emac_platform_driver);
}

module_init(emac_module_init);
module_exit(emac_module_exit);

MODULE_LICENSE("GPL");
