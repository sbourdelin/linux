/**********************************************************************
 * Author: Cavium, Inc.
 *
 * Contact: support@cavium.com
 *          Please include "LiquidIO" in the subject.
 *
 * Copyright (c) 2003-2017 Cavium, Inc.
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, Version 2, as
 * published by the Free Software Foundation.
 *
 * This file is distributed in the hope that it will be useful, but
 * AS-IS and WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE, TITLE, or
 * NONINFRINGEMENT.  See the GNU General Public License for more details.
 ***********************************************************************/
#include <linux/version.h>
#include <linux/module.h>
#include <linux/crc32.h>
#include <linux/dma-mapping.h>
#include <linux/pci.h>
#include <linux/pci_ids.h>
#include <linux/ip.h>
#include <net/ip.h>
#include <linux/ipv6.h>
#include <linux/net_tstamp.h>
#include <linux/if_vlan.h>
#include <linux/firmware.h>
#include <linux/ethtool.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>
#include "octeon_config.h"
#include "liquidio_common.h"
#include "octeon_droq.h"
#include "octeon_iq.h"
#include "response_manager.h"
#include "octeon_device.h"
#include "octeon_nic.h"
#include "octeon_main.h"
#include "octeon_network.h"

#define OPCODE_MGMT_PKT_DATA   0x10

struct lio_mgmt {
	atomic_t ifstate;
	struct net_device *parent_netdev;
	struct octeon_device *oct_dev;
	struct net_device *netdev;
	u64 dev_capability;
	struct oct_link_info linfo;
	u32 intf_open;
};

struct lio_mgmt_rx_pkt {
	struct list_head list;
	struct sk_buff *skb;
};

#define LIO_MGMT_SIZE (sizeof(struct lio_mgmt))
#define GET_LIO_MGMT(netdev)  ((struct lio_mgmt *)netdev_priv(netdev))

static int lio_mgmt_open(struct net_device *netdev)
{
	struct lio_mgmt *lio_mgmt = GET_LIO_MGMT(netdev);

	ifstate_set((struct lio *)lio_mgmt, LIO_IFSTATE_RUNNING);
	netif_carrier_on(netdev);

	netif_start_queue(netdev);

	/* Ready for link status updates */
	lio_mgmt->intf_open = 1;

	return 0;
}

/**
 * \brief Net device stop for LiquidIO
 * @param netdev network device
 */
static int lio_mgmt_stop(struct net_device *netdev)
{
	struct lio_mgmt *lio_mgmt = GET_LIO_MGMT(netdev);

	ifstate_reset((struct lio *)lio_mgmt, LIO_IFSTATE_RUNNING);

	netif_tx_disable(netdev);

	/* Inform that netif carrier is down */
	netif_carrier_off(netdev);
	lio_mgmt->intf_open = 0;

	return 0;
}

/**
 * \brief callback for octeon soft command completions
 * @oct     octeon device on which the command was executed
 * @status  command completion status
 * @buf     soft command buffer
 */
static void packet_sent_callback(struct octeon_device *oct,
				 u32 status __attribute__((unused)), void *buf)
{
	struct octeon_soft_command *sc = (struct octeon_soft_command *)buf;
	struct sk_buff *skb = sc->ctxptr;

	dma_unmap_single(&oct->pci_dev->dev, sc->dmadptr,
			 sc->datasize, DMA_TO_DEVICE);
	dev_kfree_skb_any(skb);
	kfree(sc);
}

/** \brief Transmit networks packets to the Octeon interface
 * @param skbuff   skbuff struct to be passed to network layer.
 * @param netdev    pointer to network device
 * @returns whether the packet was transmitted to the device okay or not
 *             (NETDEV_TX_OK or NETDEV_TX_BUSY)
 */
static int lio_mgmt_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct octeon_soft_command *sc = NULL;
	struct octeon_instr_pki_ih3 *pki_ih3;
	struct lio_mgmt *lio_mgmt;
	struct lio *parent_lio;
	int status;

	lio_mgmt = GET_LIO_MGMT(netdev);
	parent_lio = GET_LIO(lio_mgmt->parent_netdev);

	if (eth_skb_pad(skb))
		return NETDEV_TX_OK;

	/* Check for all conditions in which the current packet cannot be
	 * transmitted.
	 */
	if (!(atomic_read(&lio_mgmt->ifstate) & LIO_IFSTATE_RUNNING) ||
	    (skb->len > OCTNET_DEFAULT_FRM_SIZE)) {
		goto lio_xmit_failed;
	}

	if (octnet_iq_is_full(lio_mgmt->oct_dev, parent_lio->txq)) {
		/* defer sending if queue is full */
		return NETDEV_TX_BUSY;
	}

	if (!skb_shinfo(skb)->nr_frags) {
		sc = kzalloc(sizeof(*sc), GFP_ATOMIC);
		if (!sc)
			goto lio_xmit_failed;

		sc->dmadptr = dma_map_single(&lio_mgmt->oct_dev->pci_dev->dev,
					     skb->data,
					     skb->len, DMA_TO_DEVICE);
		if (dma_mapping_error
		    (&lio_mgmt->oct_dev->pci_dev->dev, sc->dmadptr)) {
			kfree(sc);
			return NETDEV_TX_BUSY;
		}
		sc->virtdptr = skb->data;
		sc->datasize = skb->len;
		sc->ctxptr = skb;	/* to be freed in sent callback */
		sc->dmarptr = 0;
		sc->rdatasize = 0;
		sc->iq_no = parent_lio->txq;	/* default input queue */
		octeon_prepare_soft_command(lio_mgmt->oct_dev, sc, OPCODE_MGMT,
					    OPCODE_MGMT_PKT_DATA, 0, 0,
					    0);

		/*prepare softcommand uses ATOMIC TAG, change it to ORDERED */
		pki_ih3 = (struct octeon_instr_pki_ih3 *)&sc->cmd.cmd3.pki_ih3;
		pki_ih3->tag = LIO_DATA((lio_mgmt->oct_dev->instr_queue
					[sc->iq_no]->txpciq.s.port));
		pki_ih3->tagtype = ORDERED_TAG;

		sc->callback = packet_sent_callback;
		sc->callback_arg = sc;
		status = octeon_send_soft_command(lio_mgmt->oct_dev, sc);
		if (status == IQ_SEND_FAILED) {
			dma_unmap_single(&lio_mgmt->oct_dev->pci_dev->dev,
					 sc->dmadptr, sc->datasize,
					 DMA_TO_DEVICE);
			kfree(sc);
			goto lio_xmit_failed;
		}

		if (status == IQ_SEND_STOP)
			netif_stop_queue(netdev);
	} else {
		goto lio_xmit_failed;
	}

	netdev->stats.tx_packets++;
	netdev->stats.tx_bytes += skb->len;

	return NETDEV_TX_OK;

lio_xmit_failed:
	netdev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static int lio_mgmt_rx(struct octeon_recv_info *recv_info, void *arg)
{
	struct octeon_device *octdev = (struct octeon_device *)arg;
	struct octeon_recv_pkt *recv_pkt = recv_info->recv_pkt;
	struct lio_mgmt *lio_mgmt;
	struct net_device *netdev;
	unsigned int pkt_size = 0;
	unsigned char *pkt_ptr;
	struct sk_buff *skb;
	int i;

	netdev = (struct net_device *)octdev->mgmt_ctx;
	lio_mgmt = GET_LIO_MGMT(netdev);
	/* Do not proceed if the interface is not in RUNNING state. */
	if (!ifstate_check((struct lio *)lio_mgmt, LIO_IFSTATE_RUNNING))
		goto fail;

	/* Not handling more than one buffer */
	if (recv_pkt->buffer_count > 1)
		goto fail;

	pkt_size = recv_pkt->buffer_size[0] - OCT_DROQ_INFO_SIZE;
	pkt_ptr = get_rbd(recv_pkt->buffer_ptr[0]) + OCT_DROQ_INFO_SIZE;

	skb = netdev_alloc_skb_ip_align(netdev, pkt_size);
	if (!skb)
		goto fail;

	skb_copy_to_linear_data(skb, pkt_ptr, pkt_size);

	skb_put(skb, pkt_size);
	netdev->stats.rx_packets++;
	netdev->stats.rx_bytes += skb->len;

	skb->dev = netdev;
	skb->protocol = eth_type_trans(skb, skb->dev);
	netif_receive_skb(skb);

fail:
	for (i = 0; i < recv_pkt->buffer_count; i++)
		recv_buffer_free(recv_pkt->buffer_ptr[i]);

	octeon_free_recv_info(recv_info);

	return 0;
}

const struct net_device_ops liocomdevops = {
	.ndo_open = lio_mgmt_open,
	.ndo_stop = lio_mgmt_stop,
	.ndo_start_xmit = lio_mgmt_xmit,
};

/* Initializes the LiquidIO management interface module
 * @param octdev - octeon device pointer
 * @returns 0 if init is success, -1 otherwise
 */
int lio_mgmt_init(struct octeon_device *octdev)
{
	struct lio_mgmt *lio_mgmt = NULL;
	struct net_device *netdev;
	struct lio *parent_lio;

	/* Register netdev only for pf 0 */
	if (octdev->pf_num == 0) {
		netdev = alloc_etherdev(LIO_MGMT_SIZE);
		if (!netdev) {
			dev_err(&octdev->pci_dev->dev, "Mgmt: Device allocation failed\n");
			goto nic_dev_fail;
		}

		/* SET_NETDEV_DEV(netdev, &octdev->pci_dev->dev); */
		netdev->netdev_ops = &liocomdevops;

		lio_mgmt = GET_LIO_MGMT(netdev);
		memset(lio_mgmt, 0, LIO_MGMT_SIZE);
		lio_mgmt->oct_dev = octdev;

		/*use ifidx zero of pf */
		lio_mgmt->parent_netdev = octdev->props[0].netdev;
		parent_lio = GET_LIO(lio_mgmt->parent_netdev);

		lio_mgmt->dev_capability = NETIF_F_HIGHDMA;

		netdev->vlan_features = lio_mgmt->dev_capability;
		netdev->features = lio_mgmt->dev_capability;
		netdev->hw_features = lio_mgmt->dev_capability;

		lio_mgmt->linfo = parent_lio->linfo;
		eth_hw_addr_random(netdev);

		/* Register the network device with the OS */
		if (register_netdev(netdev)) {
			dev_err(&octdev->pci_dev->dev, "Mgmt: Device registration failed\n");
			goto nic_dev_fail;
		}

		netif_carrier_on(netdev);
		ifstate_set((struct lio *)lio_mgmt, LIO_IFSTATE_REGISTERED);
		/*  Register RX dispatch function */
		if (octeon_register_dispatch_fn(octdev, OPCODE_MGMT,
						OPCODE_MGMT_PKT_DATA,
						lio_mgmt_rx, octdev)) {
			goto nic_dev_fail;
		}
		octdev->mgmt_ctx = (void *)netdev;
	}

	return 0;

nic_dev_fail:
	if (netdev) {
		struct lio_mgmt *lio_mgmt = GET_LIO_MGMT(netdev);

		if (atomic_read(&lio_mgmt->ifstate) &
		    LIO_IFSTATE_REGISTERED)
			unregister_netdev(netdev);

		free_netdev(netdev);
	}

	return -ENOMEM;
}

/* De-initializes the LiquidIO management interface module */
void lio_mgmt_exit(struct octeon_device *octdev)
{
	struct net_device *netdev = (struct net_device *)octdev->mgmt_ctx;
	struct lio_mgmt *lio_mgmt;

	if (netdev) {
		lio_mgmt = GET_LIO_MGMT(netdev);

		if (atomic_read(&lio_mgmt->ifstate) & LIO_IFSTATE_RUNNING)
			netif_stop_queue(netdev);

		if (atomic_read(&lio_mgmt->ifstate) &
		    LIO_IFSTATE_REGISTERED)
			unregister_netdev(netdev);

		free_netdev(netdev);
		octdev->mgmt_ctx = NULL;
	}
	pr_info("LiquidIO management module is now unloaded\n");
}
