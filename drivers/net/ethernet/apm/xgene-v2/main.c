/*
 * Applied Micro X-Gene SoC Ethernet v2 Driver
 *
 * Copyright (c) 2017, Applied Micro Circuits Corporation
 * Author(s): Iyappan Subramanian <isubramanian@apm.com>
 *	      Keyur Chudgar <kchudgar@apm.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "main.h"

static const struct acpi_device_id xge_acpi_match[];

static int xge_get_resources(struct xge_pdata *pdata)
{
	struct platform_device *pdev;
	struct net_device *ndev;
	struct device *dev;
	struct resource *res;
	int phy_mode, ret = 0;

	pdev = pdata->pdev;
	dev = &pdev->dev;
	ndev = pdata->ndev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "Resource enet_csr not defined\n");
		return -ENODEV;
	}

	pdata->resources.base_addr = devm_ioremap(dev, res->start,
						  resource_size(res));
	if (!pdata->resources.base_addr) {
		dev_err(dev, "Unable to retrieve ENET Port CSR region\n");
		return -ENOMEM;
	}

	if (!device_get_mac_address(dev, ndev->dev_addr, ETH_ALEN))
		eth_hw_addr_random(ndev);

	memcpy(ndev->perm_addr, ndev->dev_addr, ndev->addr_len);

	phy_mode = device_get_phy_mode(dev);
	if (phy_mode < 0) {
		dev_err(dev, "Unable to get phy-connection-type\n");
		return phy_mode;
	}
	pdata->resources.phy_mode = phy_mode;

	if (pdata->resources.phy_mode != PHY_INTERFACE_MODE_RGMII) {
		dev_err(dev, "Incorrect phy-connection-type specified\n");
		return -ENODEV;
	}

	ret = platform_get_irq(pdev, 0);
	if (ret <= 0) {
		dev_err(dev, "Unable to get ENET IRQ\n");
		ret = ret ? : -ENXIO;
		return ret;
	}
	pdata->resources.irq = ret;

	return 0;
}

static void xge_delete_desc_rings(struct net_device *ndev)
{
	struct xge_pdata *pdata = netdev_priv(ndev);
	struct device *dev = &pdata->pdev->dev;
	struct xge_desc_ring *ring;

	ring = pdata->tx_ring;
	if (ring) {
		if (ring->skbs)
			devm_kfree(dev, ring->skbs);
		if (ring->pkt_bufs)
			devm_kfree(dev, ring->pkt_bufs);
		devm_kfree(dev, ring);
	}

	ring = pdata->rx_ring;
	if (ring) {
		if (ring->skbs)
			devm_kfree(dev, ring->skbs);
		devm_kfree(dev, ring);
	}
}

static struct xge_desc_ring *xge_create_desc_ring(struct net_device *ndev)
{
	struct xge_pdata *pdata = netdev_priv(ndev);
	struct device *dev = &pdata->pdev->dev;
	struct xge_desc_ring *ring;
	u16 size;

	ring = devm_kzalloc(dev, sizeof(struct xge_desc_ring), GFP_KERNEL);
	if (!ring)
		return NULL;

	ring->ndev = ndev;

	size = XGENE_ENET_DESC_SIZE * XGENE_ENET_NUM_DESC;
	ring->desc_addr = dmam_alloc_coherent(dev, size, &ring->dma_addr,
					      GFP_KERNEL | __GFP_ZERO);
	if (!ring->desc_addr) {
		devm_kfree(dev, ring);
		return NULL;
	}

	xge_setup_desc(ring);

	return ring;
}

static int xge_refill_buffers(struct net_device *ndev, u32 nbuf)
{
	struct xge_pdata *pdata = netdev_priv(ndev);
	struct xge_desc_ring *ring = pdata->rx_ring;
	const u8 slots = XGENE_ENET_NUM_DESC - 1;
	struct device *dev = &pdata->pdev->dev;
	struct xge_raw_desc *raw_desc;
	u64 addr_lo, addr_hi;
	u8 tail = ring->tail;
	struct sk_buff *skb;
	dma_addr_t dma_addr;
	u16 len;
	int i;

	for (i = 0; i < nbuf; i++) {
		raw_desc = &ring->raw_desc[tail];

		len = XGENE_ENET_STD_MTU;
		skb = netdev_alloc_skb(ndev, len);
		if (unlikely(!skb))
			return -ENOMEM;

		dma_addr = dma_map_single(dev, skb->data, len, DMA_FROM_DEVICE);
		if (dma_mapping_error(dev, dma_addr)) {
			netdev_err(ndev, "DMA mapping error\n");
			dev_kfree_skb_any(skb);
			return -EINVAL;
		}

		addr_hi = GET_BITS(NEXT_DESC_ADDRH, le64_to_cpu(raw_desc->m1));
		addr_lo = GET_BITS(NEXT_DESC_ADDRL, le64_to_cpu(raw_desc->m1));
		raw_desc->m1 = cpu_to_le64(SET_BITS(NEXT_DESC_ADDRL, addr_lo) |
					   SET_BITS(NEXT_DESC_ADDRH, addr_hi) |
					   SET_BITS(PKT_ADDRH,
						    dma_addr >> PKT_ADDRL_LEN));

		dma_wmb();
		raw_desc->m0 = cpu_to_le64(SET_BITS(PKT_ADDRL, dma_addr) |
					   SET_BITS(E, 1));

		ring->skbs[tail] = skb;
		tail = (tail + 1) & slots;
	}
	xge_wr_csr(pdata, DMARXCTRL, 1);

	ring->tail = tail;

	return 0;
}

static int xge_create_desc_rings(struct net_device *ndev)
{
	struct xge_pdata *pdata = netdev_priv(ndev);
	struct device *dev = &pdata->pdev->dev;
	struct xge_desc_ring *ring;
	int ret;

	/* create tx ring */
	ring = xge_create_desc_ring(ndev);
	if (!ring)
		return -ENOMEM;

	ring->skbs = devm_kcalloc(dev, XGENE_ENET_NUM_DESC,
				  sizeof(struct sk_buff *), GFP_KERNEL);
	if (!ring->skbs)
		goto err;

	ring->pkt_bufs = devm_kcalloc(dev, XGENE_ENET_NUM_DESC,
				  sizeof(void *), GFP_KERNEL);
	if (!ring->pkt_bufs)
		goto err;

	pdata->tx_ring = ring;
	xge_update_tx_desc_addr(pdata);

	/* create rx ring */
	ring = xge_create_desc_ring(ndev);
	if (!ring)
		goto err;

	ring->skbs = devm_kcalloc(dev, XGENE_ENET_NUM_DESC,
				  sizeof(struct sk_buff *), GFP_KERNEL);
	if (!ring->skbs)
		goto err;

	pdata->rx_ring = ring;
	xge_update_rx_desc_addr(pdata);

	ret = xge_refill_buffers(ndev, XGENE_ENET_NUM_DESC);
	if (!ret)
		return 0;

err:
	xge_delete_desc_rings(ndev);

	return -ENOMEM;
}

static int xge_init_hw(struct net_device *ndev)
{
	struct xge_pdata *pdata = netdev_priv(ndev);
	int ret;

	ret = xge_port_reset(ndev);
	if (ret)
		return ret;

	xge_create_desc_rings(ndev);
	xge_port_init(ndev);
	pdata->nbufs = NUM_BUFS;

	return 0;
}

static irqreturn_t xge_irq(const int irq, void *data)
{
	struct xge_pdata *pdata = data;

	if (napi_schedule_prep(&pdata->napi)) {
		xge_intr_disable(pdata);
		__napi_schedule(&pdata->napi);
	}

	return IRQ_HANDLED;
}

static int xge_request_irq(struct net_device *ndev)
{
	struct xge_pdata *pdata = netdev_priv(ndev);
	struct device *dev = &pdata->pdev->dev;
	int ret;

	snprintf(pdata->irq_name, IRQ_ID_SIZE, "%s", ndev->name);

	ret = devm_request_irq(dev, pdata->resources.irq, xge_irq,
			       0, pdata->irq_name, pdata);
	if (ret)
		netdev_err(ndev, "Failed to request irq %s\n", pdata->irq_name);

	return ret;
}

static void xge_free_irq(struct net_device *ndev)
{
	struct xge_pdata *pdata = netdev_priv(ndev);
	struct device *dev = &pdata->pdev->dev;

	devm_free_irq(dev, pdata->resources.irq, pdata);
}

static int xge_open(struct net_device *ndev)
{
	struct xge_pdata *pdata = netdev_priv(ndev);
	int ret;

	napi_enable(&pdata->napi);

	ret = xge_request_irq(ndev);
	if (ret)
		return ret;

	xge_intr_enable(pdata);

	xge_mac_enable(pdata);
	netif_start_queue(ndev);

	return 0;
}

static int xge_close(struct net_device *ndev)
{
	struct xge_pdata *pdata = netdev_priv(ndev);

	netif_stop_queue(ndev);
	xge_mac_disable(pdata);

	xge_intr_disable(pdata);
	xge_free_irq(ndev);
	napi_disable(&pdata->napi);

	return 0;
}

static netdev_tx_t xge_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct xge_pdata *pdata = netdev_priv(ndev);
	struct device *dev = &pdata->pdev->dev;
	static dma_addr_t dma_addr;
	struct xge_desc_ring *tx_ring;
	struct xge_raw_desc *raw_desc;
	u64 addr_lo, addr_hi;
	void *pkt_buf;
	u8 tail;
	u16 len;

	tx_ring = pdata->tx_ring;
	tail = tx_ring->tail;
	len = skb_headlen(skb);
	raw_desc = &tx_ring->raw_desc[tail];

	/* Tx descriptor not available */
	if (!GET_BITS(E, le64_to_cpu(raw_desc->m0)) ||
	    GET_BITS(PKT_SIZE, le64_to_cpu(raw_desc->m0)))
		return NETDEV_TX_BUSY;

	/* Packet buffers should be 64B aligned */
	pkt_buf = dma_alloc_coherent(dev, XGENE_ENET_STD_MTU, &dma_addr,
				     GFP_ATOMIC);
	if (unlikely(!pkt_buf))
		goto out;

	memcpy(pkt_buf, skb->data, len);

	addr_hi = GET_BITS(NEXT_DESC_ADDRH, le64_to_cpu(raw_desc->m1));
	addr_lo = GET_BITS(NEXT_DESC_ADDRL, le64_to_cpu(raw_desc->m1));
	raw_desc->m1 = cpu_to_le64(SET_BITS(NEXT_DESC_ADDRL, addr_lo) |
				   SET_BITS(NEXT_DESC_ADDRH, addr_hi) |
				   SET_BITS(PKT_ADDRH,
					    dma_addr >> PKT_ADDRL_LEN));

	dma_wmb();

	raw_desc->m0 = cpu_to_le64(SET_BITS(PKT_ADDRL, dma_addr) |
				   SET_BITS(PKT_SIZE, len) |
				   SET_BITS(E, 0));

	skb_tx_timestamp(skb);
	xge_wr_csr(pdata, DMATXCTRL, 1);

	pdata->stats.tx_packets++;
	pdata->stats.tx_bytes += skb->len;

	tx_ring->skbs[tail] = skb;
	tx_ring->pkt_bufs[tail] = pkt_buf;
	tx_ring->tail = (tail + 1) & (XGENE_ENET_NUM_DESC - 1);

out:
	dev_kfree_skb_any(skb);

	return NETDEV_TX_OK;
}

static void xge_txc_poll(struct net_device *ndev, unsigned int budget)
{
	struct xge_pdata *pdata = netdev_priv(ndev);
	struct device *dev = &pdata->pdev->dev;
	struct xge_desc_ring *tx_ring;
	struct xge_raw_desc *raw_desc;
	u64 addr_lo, addr_hi;
	dma_addr_t dma_addr;
	void *pkt_buf;
	bool pktsent;
	u32 data;
	u8 head;
	int i;

	tx_ring = pdata->tx_ring;
	head = tx_ring->head;

	data = xge_rd_csr(pdata, DMATXSTATUS);
	pktsent = data & TX_PKT_SENT;
	if (unlikely(!pktsent))
		return;

	for (i = 0; i < budget; i++) {
		raw_desc = &tx_ring->raw_desc[head];

		if (!GET_BITS(E, le64_to_cpu(raw_desc->m0)))
			break;

		dma_rmb();

		addr_hi = GET_BITS(PKT_ADDRH, le64_to_cpu(raw_desc->m1));
		addr_lo = GET_BITS(PKT_ADDRL, le64_to_cpu(raw_desc->m0));
		dma_addr = (addr_hi << PKT_ADDRL_LEN) | addr_lo;

		pkt_buf = tx_ring->pkt_bufs[head];

		/* clear pktstart address and pktsize */
		raw_desc->m0 = cpu_to_le64(SET_BITS(E, 1) |
					   SET_BITS(PKT_SIZE, 0));
		xge_wr_csr(pdata, DMATXSTATUS, 1);

		dma_free_coherent(dev, XGENE_ENET_STD_MTU, pkt_buf, dma_addr);

		head = (head + 1) & (XGENE_ENET_NUM_DESC - 1);
	}

	tx_ring->head = head;
}

static int xge_rx_poll(struct net_device *ndev, unsigned int budget)
{
	struct xge_pdata *pdata = netdev_priv(ndev);
	struct device *dev = &pdata->pdev->dev;
	dma_addr_t addr_hi, addr_lo, dma_addr;
	struct xge_desc_ring *rx_ring;
	struct xge_raw_desc *raw_desc;
	struct sk_buff *skb;
	int i, npkts, ret = 0;
	bool pktrcvd;
	u32 data;
	u8 head;
	u16 len;

	rx_ring = pdata->rx_ring;
	head = rx_ring->head;

	data = xge_rd_csr(pdata, DMARXSTATUS);
	pktrcvd = data & RXSTATUS_RXPKTRCVD;

	if (unlikely(!pktrcvd))
		return 0;

	npkts = 0;
	for (i = 0; i < budget; i++) {
		raw_desc = &rx_ring->raw_desc[head];

		if (GET_BITS(E, le64_to_cpu(raw_desc->m0)))
			break;

		dma_rmb();

		addr_hi = GET_BITS(PKT_ADDRH, le64_to_cpu(raw_desc->m1));
		addr_lo = GET_BITS(PKT_ADDRL, le64_to_cpu(raw_desc->m0));
		dma_addr = (addr_hi << PKT_ADDRL_LEN) | addr_lo;
		len = GET_BITS(PKT_SIZE, le64_to_cpu(raw_desc->m0));

		dma_unmap_single(dev, dma_addr, XGENE_ENET_STD_MTU,
				 DMA_FROM_DEVICE);

		skb = rx_ring->skbs[head];
		skb_put(skb, len);

		skb->protocol = eth_type_trans(skb, ndev);

		pdata->stats.rx_packets++;
		pdata->stats.rx_bytes += len;
		napi_gro_receive(&pdata->napi, skb);
		npkts++;

		ret = xge_refill_buffers(ndev, 1);
		xge_wr_csr(pdata, DMARXSTATUS, 1);

		if (ret)
			break;

		head = (head + 1) & (XGENE_ENET_NUM_DESC - 1);
	}

	rx_ring->head = head;

	return npkts;
}

static int xge_napi(struct napi_struct *napi, const int budget)
{
	struct net_device *ndev = napi->dev;
	struct xge_pdata *pdata = netdev_priv(ndev);
	int processed;

	pdata = netdev_priv(ndev);

	xge_txc_poll(ndev, budget);
	processed = xge_rx_poll(ndev, budget);

	if (processed < budget) {
		napi_complete(napi);
		xge_intr_enable(pdata);
	}

	return processed;
}

static int xge_set_mac_addr(struct net_device *ndev, void *addr)
{
	struct xge_pdata *pdata = netdev_priv(ndev);
	int ret;

	ret = eth_mac_addr(ndev, addr);
	if (ret)
		return ret;

	xge_mac_set_station_addr(pdata);

	return 0;
}

static void xge_timeout(struct net_device *ndev)
{
	struct xge_pdata *pdata = netdev_priv(ndev);
	struct netdev_queue *txq;

	xge_mac_reset(pdata);

	txq = netdev_get_tx_queue(ndev, 0);
	txq->trans_start = jiffies;
	netif_tx_start_queue(txq);
}

static void xge_get_stats64(struct net_device *ndev,
			    struct rtnl_link_stats64 *storage)
{
	struct xge_pdata *pdata = netdev_priv(ndev);
	struct xge_stats *stats = &pdata->stats;

	storage->tx_packets += stats->tx_packets;
	storage->tx_bytes += stats->tx_bytes;

	storage->rx_packets += stats->rx_packets;
	storage->rx_bytes += stats->rx_bytes;
}

static const struct net_device_ops xgene_ndev_ops = {
	.ndo_open = xge_open,
	.ndo_stop = xge_close,
	.ndo_start_xmit = xge_start_xmit,
	.ndo_set_mac_address = xge_set_mac_addr,
	.ndo_tx_timeout = xge_timeout,
	.ndo_get_stats64 = xge_get_stats64,
};

static int xge_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct net_device *ndev;
	struct xge_pdata *pdata;
	int ret;

	ndev = alloc_etherdev(sizeof(struct xge_pdata));
	if (!ndev)
		return -ENOMEM;

	pdata = netdev_priv(ndev);

	pdata->pdev = pdev;
	pdata->ndev = ndev;
	SET_NETDEV_DEV(ndev, dev);
	platform_set_drvdata(pdev, pdata);
	ndev->netdev_ops = &xgene_ndev_ops;

	ndev->features |= NETIF_F_GSO |
			  NETIF_F_GRO;

	ret = xge_get_resources(pdata);
	if (ret)
		goto err;

	ndev->hw_features = ndev->features;

	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret) {
		netdev_err(ndev, "No usable DMA configuration\n");
		goto err;
	}

	ret = xge_init_hw(ndev);
	if (ret)
		goto err;

	netif_napi_add(ndev, &pdata->napi, xge_napi, NAPI_POLL_WEIGHT);
	ret = register_netdev(ndev);
	if (ret) {
		netdev_err(ndev, "Failed to register netdev\n");
		goto err;
	}

	return 0;

err:
	free_netdev(ndev);

	return ret;
}

static int xge_remove(struct platform_device *pdev)
{
	struct xge_pdata *pdata;
	struct net_device *ndev;

	pdata = platform_get_drvdata(pdev);
	ndev = pdata->ndev;

	rtnl_lock();
	if (netif_running(ndev))
		dev_close(ndev);
	rtnl_unlock();

	unregister_netdev(ndev);
	xge_delete_desc_rings(ndev);
	free_netdev(ndev);

	return 0;
}

static void xge_shutdown(struct platform_device *pdev)
{
	struct xge_pdata *pdata;

	pdata = platform_get_drvdata(pdev);
	if (!pdata)
		return;

	if (!pdata->ndev)
		return;

	xge_remove(pdev);
}

static const struct acpi_device_id xge_acpi_match[] = {
	{ "APMC0D80" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, xge_acpi_match);

static struct platform_driver xge_driver = {
	.driver = {
		   .name = "xgene-enet-v2",
		   .acpi_match_table = ACPI_PTR(xge_acpi_match),
	},
	.probe = xge_probe,
	.remove = xge_remove,
	.shutdown = xge_shutdown,
};
module_platform_driver(xge_driver);

MODULE_DESCRIPTION("APM X-Gene SoC Ethernet v2 driver");
MODULE_AUTHOR("Iyappan Subramanian <isubramanian@apm.com>");
MODULE_VERSION(XGENE_ENET_V2_VERSION);
MODULE_LICENSE("GPL");
