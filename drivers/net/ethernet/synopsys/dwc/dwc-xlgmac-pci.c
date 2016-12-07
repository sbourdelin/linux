/*
 * Synopsys DesignWare Core Enterprise Ethernet (XLGMAC) Driver
 *
 * Copyright (C) 2015-2016 Synopsys, Inc. (www.synopsys.com)
 *
 * Author: Jie Deng <jiedeng@synopsys.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "dwc-eth.h"
#include "dwc-eth-regacc.h"
#include "dwc-xlgmac.h"

static int debug = -1;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "DWC ethernet debug level (0=none,...,16=all)");
static const u32 default_msg_level = (NETIF_MSG_LINK | NETIF_MSG_IFDOWN |
				      NETIF_MSG_IFUP);

/* Currently it does not support MDIO */
static int mdio_en;
module_param(mdio_en, int, 0644);
MODULE_PARM_DESC(mdio_en, "Enable MDIO. Disable it when using FPGA for test");

static unsigned char dev_addr[6] = {0, 0x55, 0x7b, 0xb5, 0x7d, 0xf7};
static unsigned long dwc_xlgmac_pci_base_addr;

static void xlgmac_read_mac_addr(struct dwc_eth_pdata *pdata)
{
	struct net_device *netdev = pdata->netdev;

	/* Currently it uses a static mac address for test */
	memcpy(pdata->mac_addr, dev_addr, netdev->addr_len);
}

static void xlgmac_init_tx_coalesce(struct dwc_eth_pdata *pdata)
{
	TRACE("-->");

	pdata->tx_usecs = XLGMAC_INIT_DMA_TX_USECS;
	pdata->tx_frames = XLGMAC_INIT_DMA_TX_FRAMES;

	TRACE("<--");
}

static void xlgmac_init_rx_coalesce(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;

	TRACE("-->");

	pdata->rx_riwt = hw_ops->usec_to_riwt(pdata, XLGMAC_INIT_DMA_RX_USECS);
	pdata->rx_usecs = XLGMAC_INIT_DMA_RX_USECS;
	pdata->rx_frames = XLGMAC_INIT_DMA_RX_FRAMES;

	TRACE("<--");
}

static void xlgmac_init_coalesce(struct dwc_eth_pdata *pdata)
{
	xlgmac_init_tx_coalesce(pdata);
	xlgmac_init_rx_coalesce(pdata);
}

static void xlgmac_default_config(struct dwc_eth_pdata *pdata)
{
	TRACE("-->");

	pdata->pblx8 = DMA_PBL_X8_ENABLE;
	pdata->tx_sf_mode = MTL_TSF_ENABLE;
	pdata->tx_threshold = MTL_TX_THRESHOLD_64;
	pdata->tx_pbl = DMA_PBL_16;
	pdata->tx_osp_mode = DMA_OSP_ENABLE;
	pdata->rx_sf_mode = MTL_RSF_DISABLE;
	pdata->rx_threshold = MTL_RX_THRESHOLD_64;
	pdata->rx_pbl = DMA_PBL_16;
	pdata->pause_autoneg = 1;
	pdata->tx_pause = 1;
	pdata->rx_pause = 1;
	pdata->phy_speed = SPEED_UNKNOWN;
	pdata->power_down = 0;
	pdata->default_autoneg = AUTONEG_ENABLE;
	pdata->default_speed = SPEED_100000;
	pdata->coherent = 1;
	pdata->mdio_en = mdio_en;

	pdata->sysclk_rate		= XLGMAC_SYSCLOCK;
	pdata->ptpclk_rate		= XLGMAC_SYSCLOCK;
	pdata->tx_max_buf_size		= XLGMAC_TX_MAX_BUF_SIZE;
	pdata->rx_min_buf_size		= XLGMAC_RX_MIN_BUF_SIZE;
	pdata->rx_buf_align		= XLGMAC_RX_BUF_ALIGN;
	pdata->tx_max_desc_nr		= XLGMAC_TX_MAX_DESC_NR;
	pdata->skb_alloc_size		= XLGMAC_SKB_ALLOC_SIZE;
	pdata->tx_desc_max_proc		= XLGMAC_TX_DESC_MAX_PROC;
	pdata->tx_desc_min_free		= XLGMAC_TX_DESC_MIN_FREE;
	pdata->rx_desc_max_dirty	= XLGMAC_RX_DESC_MAX_DIRTY;
	pdata->dma_stop_timeout		= XLGMAC_DMA_STOP_TIMEOUT;
	pdata->max_flow_control_queues	= XLGMAC_MAX_FLOW_CONTROL_QUEUES;
	pdata->max_dma_riwt		= XLGMAC_MAX_DMA_RIWT;
	pdata->tstamp_ssinc		= XLGMAC_TSTAMP_SSINC;
	pdata->tstamp_snsinc		= XLGMAC_TSTAMP_SNSINC;
	pdata->sph_hdsms_size		= XLGMAC_SPH_HDSMS_SIZE;

	strlcpy(pdata->drv_name, XLGMAC_DRV_NAME, sizeof(pdata->drv_name));
	strlcpy(pdata->drv_ver, XLGMAC_DRV_VERSION, sizeof(pdata->drv_ver));

	TRACE("<--");
}

static void xlgmac_init_all_ops(struct dwc_eth_pdata *pdata)
{
	dwc_eth_init_desc_ops(&pdata->desc_ops);
	dwc_eth_init_hw_ops(&pdata->hw_ops);
	xlgmac_init_hw_ops(pdata->hw2_ops);
}

static int xlgmac_get_resources(struct dwc_eth_pdata *pdata)
{
	struct pci_dev *pcidev = pdata->pcidev;
	resource_size_t bar_length;
	unsigned int i;
	int ret = 0;

	pdata->dev_irq = pcidev->irq;

	for (i = 0; i < 6; i++) {
		bar_length = pci_resource_len(pcidev, i);
		if (bar_length == 0)
			continue;

		pdata->mac_regs = pci_iomap(pcidev, i, bar_length);
		dwc_xlgmac_pci_base_addr = (unsigned long)(pdata->mac_regs);
		if (!pdata->mac_regs) {
			dev_err(pdata->dev, "cannot map register memory\n");
			ret = -EIO;
		}
		break;
	}

	return ret;
}

static int xlgmac_init(struct dwc_eth_pdata *pdata)
{
	struct net_device *netdev = pdata->netdev;
	unsigned int i;
	int ret;

	/* Set default configuration data */
	xlgmac_default_config(pdata);

	/* Set irq, base_addr, MAC address, */
	netdev->irq = pdata->dev_irq;
	netdev->base_addr = (unsigned long)pdata->mac_regs;
	xlgmac_read_mac_addr(pdata);
	memcpy(netdev->dev_addr, pdata->mac_addr, netdev->addr_len);

	/* Set the DMA coherency values */
	if (pdata->coherent) {
		pdata->axdomain = XLGMAC_DMA_OS_AXDOMAIN;
		pdata->arcache = XLGMAC_DMA_OS_ARCACHE;
		pdata->awcache = XLGMAC_DMA_OS_AWCACHE;
	} else {
		pdata->axdomain = XLGMAC_DMA_SYS_AXDOMAIN;
		pdata->arcache = XLGMAC_DMA_SYS_ARCACHE;
		pdata->awcache = XLGMAC_DMA_SYS_AWCACHE;
	}

	/* Set all the function pointers */
	xlgmac_init_all_ops(pdata);

	/* Issue software reset to device */
	pdata->hw_ops.exit(pdata);

	/* Populate the hardware features */
	dwc_eth_get_all_hw_features(pdata);
	dwc_eth_print_all_hw_features(pdata);

	/* Get the PHY mode */
	pdata->phy_mode = PHY_INTERFACE_MODE_XLGMII;

	/* Set the DMA mask */
	ret = dma_set_mask_and_coherent(pdata->dev,
					DMA_BIT_MASK(pdata->hw_feat.dma_width));
	if (ret) {
		dev_err(pdata->dev, "dma_set_mask_and_coherent failed\n");
		goto err_out;
	}

	/* Channel and ring params initializtion
	 *  pdata->channel_count;
	 *  pdata->tx_ring_count;
	 *  pdata->rx_ring_count;
	 *  pdata->tx_desc_count;
	 *  pdata->rx_desc_count;
	 */
	BUILD_BUG_ON_NOT_POWER_OF_2(XLGMAC_TX_DESC_CNT);
	pdata->tx_desc_count = XLGMAC_TX_DESC_CNT;
	if (pdata->tx_desc_count & (pdata->tx_desc_count - 1)) {
		dev_err(pdata->dev, "tx descriptor count (%d) is not valid\n",
			pdata->tx_desc_count);
		ret = -EINVAL;
		goto err_out;
	}
	BUILD_BUG_ON_NOT_POWER_OF_2(XLGMAC_RX_DESC_CNT);
	pdata->rx_desc_count = XLGMAC_RX_DESC_CNT;
	if (pdata->rx_desc_count & (pdata->rx_desc_count - 1)) {
		dev_err(pdata->dev, "rx descriptor count (%d) is not valid\n",
			pdata->rx_desc_count);
		ret = -EINVAL;
		goto err_out;
	}

	/* Calculate the number of Tx and Rx rings to be created
	 *  -Tx (DMA) Channels map 1-to-1 to Tx Queues so set
	 *   the number of Tx queues to the number of Tx channels
	 *   enabled
	 *  -Rx (DMA) Channels do not map 1-to-1 so use the actual
	 *   number of Rx queues
	 */
	pdata->tx_ring_count = min_t(unsigned int, num_online_cpus(),
				     pdata->hw_feat.tx_ch_cnt);
	pdata->tx_q_count = pdata->tx_ring_count;
	ret = netif_set_real_num_tx_queues(netdev, pdata->tx_ring_count);
	if (ret) {
		dev_err(pdata->dev, "error setting real tx queue count\n");
		goto err_out;
	}

	pdata->rx_ring_count = min_t(unsigned int,
				     netif_get_num_default_rss_queues(),
				     pdata->hw_feat.rx_ch_cnt);
	pdata->rx_q_count = pdata->hw_feat.rx_q_cnt;
	ret = netif_set_real_num_rx_queues(netdev, pdata->rx_ring_count);
	if (ret) {
		dev_err(pdata->dev, "error setting real rx queue count\n");
		goto err_out;
	}

	pdata->channel_count =
		max_t(unsigned int, pdata->tx_ring_count, pdata->rx_ring_count);

	DBGPR("  channel_count=%u\n", pdata->channel_count);
	DBGPR("  tx_ring_count=%u, tx_q_count=%u\n",
	      pdata->tx_ring_count, pdata->tx_q_count);
	DBGPR("  rx_ring_count=%u, rx_q_count=%u\n",
	      pdata->rx_ring_count, pdata->rx_q_count);

	/* Initialize RSS hash key and lookup table */
	netdev_rss_key_fill(pdata->rss_key, sizeof(pdata->rss_key));

	for (i = 0; i < DWC_ETH_RSS_MAX_TABLE_SIZE; i++)
		DWC_ETH_SET_BITS(pdata->rss_table[i], MAC_RSSDR, DMCH,
				 i % pdata->rx_ring_count);

	DWC_ETH_SET_BITS(pdata->rss_options, MAC_RSSCR, IP2TE, 1);
	DWC_ETH_SET_BITS(pdata->rss_options, MAC_RSSCR, TCP4TE, 1);
	DWC_ETH_SET_BITS(pdata->rss_options, MAC_RSSCR, UDP4TE, 1);

	/* Set device operations */
	netdev->netdev_ops = dwc_eth_get_netdev_ops();
	netdev->ethtool_ops = dwc_eth_get_ethtool_ops();
#ifdef CONFIG_DWC_ETH_DCB
	netdev->dcbnl_ops = dwc_eth_get_dcbnl_ops();
#endif

	/* Set device features */
	if (pdata->hw_feat.tso) {
		netdev->hw_features = NETIF_F_TSO;
		netdev->hw_features |= NETIF_F_TSO6;
		netdev->hw_features |= NETIF_F_SG;
		netdev->hw_features |= NETIF_F_IP_CSUM;
		netdev->hw_features |= NETIF_F_IPV6_CSUM;
	} else if (pdata->hw_feat.tx_coe) {
		netdev->hw_features = NETIF_F_IP_CSUM;
		netdev->hw_features |= NETIF_F_IPV6_CSUM;
	}

	if (pdata->hw_feat.rx_coe) {
		netdev->hw_features |= NETIF_F_RXCSUM;
		netdev->hw_features |= NETIF_F_GRO;
	}

	if (pdata->hw_feat.rss)
		netdev->hw_features |= NETIF_F_RXHASH;

	netdev->vlan_features |= netdev->hw_features;

	netdev->hw_features |= NETIF_F_HW_VLAN_CTAG_RX;
	if (pdata->hw_feat.sa_vlan_ins)
		netdev->hw_features |= NETIF_F_HW_VLAN_CTAG_TX;
	if (pdata->hw_feat.vlhash)
		netdev->hw_features |= NETIF_F_HW_VLAN_CTAG_FILTER;

	netdev->features |= netdev->hw_features;
	pdata->netdev_features = netdev->features;

	netdev->priv_flags |= IFF_UNICAST_FLT;

	/* Use default watchdog timeout */
	netdev->watchdog_timeo = 0;

	xlgmac_init_coalesce(pdata);

	return 0;

err_out:

	return ret;
}

#ifdef CONFIG_PM

static int xlgmac_suspend(struct device *dev)
{
	struct net_device *netdev = dev_get_drvdata(dev);
	int ret = 0;

	TRACE("-->");

	if (netif_running(netdev))
		ret = dwc_eth_powerdown(netdev, DWC_ETH_DRIVER_CONTEXT);

	TRACE("<--");

	return ret;
}

static int xlgmac_resume(struct device *dev)
{
	struct net_device *netdev = dev_get_drvdata(dev);
	int ret = 0;

	TRACE("-->");

	if (netif_running(netdev))
		ret = dwc_eth_powerup(netdev, DWC_ETH_DRIVER_CONTEXT);

	TRACE("<--");

	return ret;
}

static const struct dev_pm_ops xlgmac_pm_ops = {
	.suspend		= xlgmac_suspend,
	.resume			= xlgmac_resume,
};

#define XLGMAC_PM_OPS	(&xlgmac_pm_ops)

#else /* CONFIG_PM */

#define XLGMAC_PM_OPS	NULL

#endif /* CONFIG_PM */

static int xlgmac_probe(struct pci_dev *pcidev, const struct pci_device_id *id)
{
	struct net_device *netdev;
	struct dwc_eth_pdata *pdata;
	struct device *dev = &pcidev->dev;
	int ret;

	TRACE("-->");

	ret = pci_enable_device(pcidev);
	if (ret) {
		dev_err(dev, "fail to enable device\n");
		goto err_enable_fail;
	}

	ret = pci_request_regions(pcidev, XLGMAC_DRV_NAME);
	if (ret) {
		dev_err(dev, "fail to get pci regions\n");
		goto err_request_regions_fail;
	}
	pci_set_master(pcidev);

	netdev = alloc_etherdev_mq(sizeof(struct dwc_eth_pdata),
				   DWC_ETH_MAX_DMA_CHANNELS);

	if (!netdev) {
		dev_err(dev, "alloc_etherdev failed\n");
		ret = -ENOMEM;
		goto err_alloc_eth;
	}
	SET_NETDEV_DEV(netdev, dev);
	pdata = netdev_priv(netdev);
	pdata->netdev = netdev;
	pdata->pcidev = pcidev;
	pdata->dev = dev;
	pci_set_drvdata(pcidev, netdev);

	spin_lock_init(&pdata->lock);
	mutex_init(&pdata->pcs_mutex);
	mutex_init(&pdata->rss_mutex);
	spin_lock_init(&pdata->tstamp_lock);

	pdata->msg_enable = netif_msg_init(debug, default_msg_level);

	/* Get the reg base and irq */
	ret = xlgmac_get_resources(pdata);
	if (ret) {
		dev_err(dev, "xlgmac can not get resources\n");
		goto err_resources;
	}

	ret = xlgmac_init(pdata);
	if (ret) {
		dev_err(dev, "xlgmac init failed\n");
		goto err_init;
	}

	if (pdata->mdio_en) {
		/* Prepare to regsiter with MDIO */
		pdata->mii_bus_id = kasprintf(GFP_KERNEL, "%s",
					      pci_name(pcidev));
		if (!pdata->mii_bus_id) {
			dev_err(dev, "failed to allocate mii bus id\n");
			ret = -ENOMEM;
			goto err_mii_bus_id;
		}
		ret = dwc_eth_mdio_register(pdata);
		if (ret)
			goto err_mdio_register;

		netif_carrier_off(netdev);
	}

	ret = register_netdev(netdev);
	if (ret) {
		dev_err(dev, "net device registration failed\n");
		goto err_netdev;
	}

	/* Create workqueues */
	pdata->dev_workqueue =
		create_singlethread_workqueue(netdev_name(netdev));
	if (!pdata->dev_workqueue) {
		dev_err(dev, "device workqueue creation failed\n");
		ret = -ENOMEM;
		goto err_wq;
	}

	dwc_eth_ptp_register(pdata);

	xlgmac_debugfs_init(pdata);

	netdev_notice(netdev, "net device enabled\n");

	TRACE("<--");

	return 0;

err_wq:
	unregister_netdev(netdev);

err_netdev:
	if (pdata->mdio_en)
		dwc_eth_mdio_unregister(pdata);

err_mdio_register:
	if (pdata->mdio_en)
		kfree(pdata->mii_bus_id);

err_mii_bus_id:
err_resources:
err_init:
	free_netdev(netdev);

err_alloc_eth:
	pci_release_regions(pcidev);

err_request_regions_fail:
	pci_disable_device(pcidev);

err_enable_fail:
	return ret;
}

static void xlgmac_remove(struct pci_dev *pcidev)
{
	struct net_device *netdev = pci_get_drvdata(pcidev);
	struct dwc_eth_pdata *pdata = netdev_priv(netdev);

	TRACE("-->");

	xlgmac_debugfs_exit(pdata);

	dwc_eth_ptp_unregister(pdata);

	flush_workqueue(pdata->dev_workqueue);
	destroy_workqueue(pdata->dev_workqueue);

	unregister_netdev(netdev);

	if (pdata->mdio_en) {
		dwc_eth_mdio_unregister(pdata);
		kfree(pdata->mii_bus_id);
	}

	free_netdev(netdev);

	pci_set_drvdata(pcidev, NULL);
	pci_iounmap(pcidev, (void __iomem *)dwc_xlgmac_pci_base_addr);
	pci_release_regions(pcidev);
	pci_disable_device(pcidev);

	TRACE("<--");
}

static const struct pci_device_id xlgmac_pci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_SYNOPSYS, 0x1018) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, xlgmac_pci_tbl);

static struct pci_driver xlgmac_pci_driver = {
	.name		= XLGMAC_DRV_NAME,
	.id_table	= xlgmac_pci_tbl,
	.probe		= xlgmac_probe,
	.remove		= xlgmac_remove,
	.driver.pm	= XLGMAC_PM_OPS,
};

module_pci_driver(xlgmac_pci_driver);

MODULE_DESCRIPTION("PCI driver for Synopsys XLGMAC");
MODULE_VERSION(XLGMAC_DRV_VERSION);
MODULE_AUTHOR("Jie Deng <jiedeng@synopsys.com>");
MODULE_LICENSE("GPL v2");
