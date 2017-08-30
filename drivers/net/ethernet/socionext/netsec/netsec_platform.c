/**
 * drivers/net/ethernet/socionext/netsec/netsec_platform.c
 *
 *  Copyright (C) 2013-2014 Fujitsu Semiconductor Limited.
 *  Copyright (C) 2014 Linaro Ltd  Andy Green <andy.green@linaro.org>
 *  All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */

#include <linux/device.h>
#include <linux/ctype.h>
#include <linux/netdevice.h>
#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_net.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>

#include "netsec.h"

#define NETSEC_F_NETSEC_VER_MAJOR_NUM(x) (x & 0xffff0000)

static int napi_weight = 64;
unsigned short pause_time = 256;

static int netsec_probe(struct platform_device *pdev)
{
	struct net_device *ndev;
	struct netsec_priv *priv;
	struct resource *res;
	const void *mac;
	bool use_jumbo;
	u32 hw_ver;
	int err;
	int ret;

	ndev = alloc_etherdev(sizeof(*priv));
	if (!ndev)
		return -ENOMEM;

	priv = netdev_priv(ndev);
	priv->ndev = ndev;
	SET_NETDEV_DEV(ndev, &pdev->dev);
	platform_set_drvdata(pdev, priv);
	priv->dev = &pdev->dev;

	priv->msg_enable = NETIF_MSG_TX_ERR | NETIF_MSG_HW | NETIF_MSG_DRV |
			   NETIF_MSG_LINK | NETIF_MSG_PROBE;

	mac = of_get_mac_address(pdev->dev.of_node);
	if (mac)
		ether_addr_copy(ndev->dev_addr, mac);

	if (!is_valid_ether_addr(ndev->dev_addr)) {
		eth_hw_addr_random(ndev);
		dev_warn(&pdev->dev, "No MAC address found, using random\n");
	}

	priv->phy_np = of_parse_phandle(pdev->dev.of_node, "phy-handle", 0);
	if (!priv->phy_np) {
		netif_err(priv, probe, ndev, "missing phy in DT\n");
		goto err1;
	}

	priv->phy_interface = of_get_phy_mode(pdev->dev.of_node);
	if (priv->phy_interface < 0) {
		netif_err(priv, probe, ndev,
			  "%s: bad phy-if\n", __func__);
		goto err1;
	}

	priv->ioaddr = of_iomap(priv->dev->of_node, 0);
	if (!priv->ioaddr) {
		netif_err(priv, probe, ndev, "of_iomap() failed\n");
		err = -EINVAL;
		goto err1;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res) {
		netif_err(priv, probe, ndev,
			  "Missing rdlar resource\n");
		goto err1;
	}
	priv->rdlar_pa = res->start;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	if (!res) {
		netif_err(priv, probe, ndev,
			  "Missing tdlar resource\n");
		goto err1;
	}
	priv->tdlar_pa = res->start;

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!res) {
		netif_err(priv, probe, ndev,
			  "Missing IRQ resource\n");
		goto err2;
	}
	ndev->irq = res->start;

	while (priv->clock_count < ARRAY_SIZE(priv->clk)) {
		priv->clk[priv->clock_count] =
			of_clk_get(pdev->dev.of_node, priv->clock_count);
		if (IS_ERR(priv->clk[priv->clock_count])) {
			if (!priv->clock_count) {
				netif_err(priv, probe, ndev,
					  "Failed to get clock\n");
				goto err3;
			}
			break;
		}
		priv->clock_count++;
	}

	/* disable by default */
	priv->et_coalesce.rx_coalesce_usecs = 0;
	priv->et_coalesce.rx_max_coalesced_frames = 1;
	priv->et_coalesce.tx_coalesce_usecs = 0;
	priv->et_coalesce.tx_max_coalesced_frames = 1;

	use_jumbo = of_property_read_bool(pdev->dev.of_node, "use-jumbo");
	priv->param.use_jumbo_pkt_flag = use_jumbo;

	if (priv->param.use_jumbo_pkt_flag)
		priv->rx_pkt_buf_len = NETSEC_RX_JUMBO_PKT_BUF_LEN;
	else
		priv->rx_pkt_buf_len = NETSEC_RX_PKT_BUF_LEN;

	pm_runtime_enable(&pdev->dev);
	/* runtime_pm coverage just for probe, open/close also cover it */
	pm_runtime_get_sync(&pdev->dev);

	hw_ver = netsec_readl(priv, NETSEC_REG_F_TAIKI_VER);
	/* this driver only supports F_TAIKI style NETSEC */
	if (NETSEC_F_NETSEC_VER_MAJOR_NUM(hw_ver) !=
	    NETSEC_F_NETSEC_VER_MAJOR_NUM(NETSEC_REG_NETSEC_VER_F_TAIKI)) {
		ret = -ENODEV;
		goto err3;
	}

	dev_info(&pdev->dev, "IP rev %d.%d\n", hw_ver >> 16, hw_ver & 0xffff);

	priv->mac_mode.flow_start_th = NETSEC_FLOW_CONTROL_START_THRESHOLD;
	priv->mac_mode.flow_stop_th = NETSEC_FLOW_CONTROL_STOP_THRESHOLD;
	priv->mac_mode.pause_time = pause_time;
	priv->mac_mode.flow_ctrl_enable_flag = false;
	priv->freq = clk_get_rate(priv->clk[0]);

	netif_napi_add(ndev, &priv->napi, netsec_netdev_napi_poll,
		       napi_weight);

	/* MTU range */
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = NETSEC_RX_JUMBO_PKT_BUF_LEN;

	ndev->netdev_ops = &netsec_netdev_ops;
	ndev->ethtool_ops = &netsec_ethtool_ops;
	ndev->features = NETIF_F_SG | NETIF_F_IP_CSUM |
			       NETIF_F_IPV6_CSUM | NETIF_F_TSO |
			       NETIF_F_TSO6 | NETIF_F_GSO |
			       NETIF_F_HIGHDMA | NETIF_F_RXCSUM;
	ndev->hw_features = ndev->features;

	priv->rx_cksum_offload_flag = true;
	spin_lock_init(&priv->tx_queue_lock);

	err = netsec_mii_register(priv);
	if (err) {
		netif_err(priv, probe, ndev,
			  "mii bus registration failed %d\n", err);
		goto err3;
	}

	/* disable all other interrupt sources */
	netsec_writel(priv, NETSEC_REG_INTEN_CLR, ~0);
	netsec_writel(priv, NETSEC_REG_INTEN_SET,
		      NETSEC_IRQ_TX | NETSEC_IRQ_RX);

	err = register_netdev(ndev);
	if (err) {
		netif_err(priv, probe, ndev, "register_netdev() failed\n");
		goto err4;
	}

	pm_runtime_put_sync_suspend(&pdev->dev);

	netif_info(priv, probe, ndev, "initialized\n");

	return 0;

err4:
	netsec_mii_unregister(priv);

err3:
	pm_runtime_put_sync_suspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	while (priv->clock_count > 0) {
		priv->clock_count--;
		clk_put(priv->clk[priv->clock_count]);
	}
err2:
	iounmap(priv->ioaddr);
err1:
	free_netdev(ndev);

	dev_err(&pdev->dev, "init failed\n");

	return ret;
}

static int netsec_remove(struct platform_device *pdev)
{
	struct netsec_priv *priv = platform_get_drvdata(pdev);

	unregister_netdev(priv->ndev);
	netsec_mii_unregister(priv);
	pm_runtime_disable(&pdev->dev);
	iounmap(priv->ioaddr);
	free_netdev(priv->ndev);

	return 0;
}

#ifdef CONFIG_PM
static int netsec_runtime_suspend(struct device *dev)
{
	struct netsec_priv *priv = dev_get_drvdata(dev);
	int n;

	netif_dbg(priv, drv, priv->ndev, "%s\n", __func__);

	if (priv->irq_registered)
		disable_irq(priv->ndev->irq);

	netsec_writel(priv, NETSEC_REG_CLK_EN, 0);

	for (n = priv->clock_count - 1; n >= 0; n--)
		clk_disable_unprepare(priv->clk[n]);

	return 0;
}

static int netsec_runtime_resume(struct device *dev)
{
	struct netsec_priv *priv = dev_get_drvdata(dev);
	int n;

	netif_dbg(priv, drv, priv->ndev, "%s\n", __func__);

	/* first let the clocks back on */

	for (n = 0; n < priv->clock_count; n++)
		clk_prepare_enable(priv->clk[n]);

	netsec_writel(priv, NETSEC_REG_CLK_EN, NETSEC_CLK_EN_REG_DOM_D |
			NETSEC_CLK_EN_REG_DOM_C | NETSEC_CLK_EN_REG_DOM_G);

	if (priv->irq_registered)
		enable_irq(priv->ndev->irq);

	return 0;
}

static int netsec_pm_suspend(struct device *dev)
{
	struct netsec_priv *priv = dev_get_drvdata(dev);

	netif_dbg(priv, drv, priv->ndev, "%s\n", __func__);

	if (pm_runtime_status_suspended(dev))
		return 0;

	return netsec_runtime_suspend(dev);
}

static int netsec_pm_resume(struct device *dev)
{
	struct netsec_priv *priv = dev_get_drvdata(dev);

	netif_dbg(priv, drv, priv->ndev, "%s\n", __func__);

	if (pm_runtime_status_suspended(dev))
		return 0;

	return netsec_runtime_resume(dev);
}
#endif

static const struct dev_pm_ops netsec_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(netsec_pm_suspend, netsec_pm_resume)
	SET_RUNTIME_PM_OPS(netsec_runtime_suspend, netsec_runtime_resume, NULL)
};

static const struct of_device_id netsec_dt_ids[] = {
	{.compatible = "socionext,netsecv5"},
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, netsec_dt_ids);

static struct platform_driver netsec_driver = {
	.probe = netsec_probe,
	.remove = netsec_remove,
	.driver = {
		.name = "netsec",
		.of_match_table = netsec_dt_ids,
		.pm = &netsec_pm_ops,
	},
};

module_platform_driver(netsec_driver);

MODULE_AUTHOR("Socionext Inc");
MODULE_DESCRIPTION("NETSEC Ethernet driver");
MODULE_LICENSE("GPL");

MODULE_ALIAS("platform:netsec");
