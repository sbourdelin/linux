/*
 * Synopsys DesignWare Ethernet Driver
 *
 * Copyright (c) 2014-2016 Synopsys, Inc. (www.synopsys.com)
 *
 * This file is free software; you may copy, redistribute and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or (at
 * your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *     The Synopsys DWC ETHER XGMAC Software Driver and documentation
 *     (hereinafter "Software") is an unsupported proprietary work of Synopsys,
 *     Inc. unless otherwise expressly agreed to in writing between Synopsys
 *     and you.
 *
 *     The Software IS NOT an item of Licensed Software or Licensed Product
 *     under any End User Software License Agreement or Agreement for Licensed
 *     Product with Synopsys or any supplement thereto.  Permission is hereby
 *     granted, free of charge, to any person obtaining a copy of this software
 *     annotated with this license and the Software, to deal in the Software
 *     without restriction, including without limitation the rights to use,
 *     copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 *     of the Software, and to permit persons to whom the Software is furnished
 *     to do so, subject to the following conditions:
 *
 *     The above copyright notice and this permission notice shall be included
 *     in all copies or substantial portions of the Software.
 *
 *     THIS SOFTWARE IS BEING DISTRIBUTED BY SYNOPSYS SOLELY ON AN "AS IS"
 *     BASIS AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 *     TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *     PARTICULAR PURPOSE ARE HEREBY DISCLAIMED. IN NO EVENT SHALL SYNOPSYS
 *     BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *     CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *     SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *     INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *     CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *     ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 *     THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "dwc-eth.h"
#include "dwc-eth-regacc.h"

static void dwc_eth_unmap_desc_data(struct dwc_eth_pdata *pdata,
				    struct dwc_eth_desc_data *desc_data)
{
	if (desc_data->skb_dma) {
		if (desc_data->mapped_as_page) {
			dma_unmap_page(pdata->dev, desc_data->skb_dma,
				       desc_data->skb_dma_len, DMA_TO_DEVICE);
		} else {
			dma_unmap_single(pdata->dev, desc_data->skb_dma,
					 desc_data->skb_dma_len, DMA_TO_DEVICE);
		}
		desc_data->skb_dma = 0;
		desc_data->skb_dma_len = 0;
	}

	if (desc_data->skb) {
		dev_kfree_skb_any(desc_data->skb);
		desc_data->skb = NULL;
	}

	if (desc_data->rx.hdr.pa.pages)
		put_page(desc_data->rx.hdr.pa.pages);

	if (desc_data->rx.hdr.pa_unmap.pages) {
		dma_unmap_page(pdata->dev, desc_data->rx.hdr.pa_unmap.pages_dma,
			       desc_data->rx.hdr.pa_unmap.pages_len,
			       DMA_FROM_DEVICE);
		put_page(desc_data->rx.hdr.pa_unmap.pages);
	}

	if (desc_data->rx.buf.pa.pages)
		put_page(desc_data->rx.buf.pa.pages);

	if (desc_data->rx.buf.pa_unmap.pages) {
		dma_unmap_page(pdata->dev, desc_data->rx.buf.pa_unmap.pages_dma,
			       desc_data->rx.buf.pa_unmap.pages_len,
			       DMA_FROM_DEVICE);
		put_page(desc_data->rx.buf.pa_unmap.pages);
	}

	memset(&desc_data->tx, 0, sizeof(desc_data->tx));
	memset(&desc_data->rx, 0, sizeof(desc_data->rx));

	desc_data->mapped_as_page = 0;

	if (desc_data->state_saved) {
		desc_data->state_saved = 0;
		desc_data->state.skb = NULL;
		desc_data->state.len = 0;
		desc_data->state.error = 0;
	}
}

static void dwc_eth_free_ring(struct dwc_eth_pdata *pdata,
			      struct dwc_eth_ring *ring)
{
	struct dwc_eth_desc_data *desc_data;
	unsigned int i;

	TRACE("-->");

	if (!ring)
		return;

	if (ring->desc_data_head) {
		for (i = 0; i < ring->dma_desc_count; i++) {
			desc_data = DWC_ETH_GET_DESC_DATA(ring, i);
			dwc_eth_unmap_desc_data(pdata, desc_data);
		}

		kfree(ring->desc_data_head);
		ring->desc_data_head = NULL;
	}

	if (ring->rx_hdr_pa.pages) {
		dma_unmap_page(pdata->dev, ring->rx_hdr_pa.pages_dma,
			       ring->rx_hdr_pa.pages_len, DMA_FROM_DEVICE);
		put_page(ring->rx_hdr_pa.pages);

		ring->rx_hdr_pa.pages = NULL;
		ring->rx_hdr_pa.pages_len = 0;
		ring->rx_hdr_pa.pages_offset = 0;
		ring->rx_hdr_pa.pages_dma = 0;
	}

	if (ring->rx_buf_pa.pages) {
		dma_unmap_page(pdata->dev, ring->rx_buf_pa.pages_dma,
			       ring->rx_buf_pa.pages_len, DMA_FROM_DEVICE);
		put_page(ring->rx_buf_pa.pages);

		ring->rx_buf_pa.pages = NULL;
		ring->rx_buf_pa.pages_len = 0;
		ring->rx_buf_pa.pages_offset = 0;
		ring->rx_buf_pa.pages_dma = 0;
	}

	if (ring->dma_desc_head) {
		dma_free_coherent(pdata->dev,
				  (sizeof(struct dwc_eth_dma_desc) *
				  ring->dma_desc_count),
				  ring->dma_desc_head,
				  ring->dma_desc_head_addr);
		ring->dma_desc_head = NULL;
	}

	TRACE("<--");
}

static int dwc_eth_init_ring(struct dwc_eth_pdata *pdata,
			     struct dwc_eth_ring *ring,
			     unsigned int dma_desc_count)
{
	TRACE("-->");

	if (!ring)
		return 0;

	/* Descriptors */
	ring->dma_desc_count = dma_desc_count;
	ring->dma_desc_head = dma_alloc_coherent(pdata->dev,
					(sizeof(struct dwc_eth_dma_desc) *
					 dma_desc_count),
					&ring->dma_desc_head_addr,
					GFP_KERNEL);
	if (!ring->dma_desc_head)
		return -ENOMEM;

	/* Array of descriptor data */
	ring->desc_data_head = kcalloc(dma_desc_count,
					sizeof(struct dwc_eth_desc_data),
					GFP_KERNEL);
	if (!ring->desc_data_head)
		return -ENOMEM;

	netif_dbg(pdata, drv, pdata->netdev,
		  "dma_desc_head=%p, dma_desc_head_addr=%pad, desc_data_head=%p\n",
		ring->dma_desc_head,
		&ring->dma_desc_head_addr,
		ring->desc_data_head);

	TRACE("<--");

	return 0;
}

static void dwc_eth_free_rings(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;

	TRACE("-->");

	if (!pdata->channel_head)
		return;

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		dwc_eth_free_ring(pdata, channel->tx_ring);
		dwc_eth_free_ring(pdata, channel->rx_ring);
	}

	TRACE("<--");
}

static int dwc_eth_alloc_rings(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel;
	unsigned int i;
	int ret;

	TRACE("-->");

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		netif_dbg(pdata, drv, pdata->netdev, "%s - Tx ring:\n",
			  channel->name);

		ret = dwc_eth_init_ring(pdata, channel->tx_ring,
					pdata->tx_desc_count);

		if (ret) {
			netdev_alert(pdata->netdev,
				     "error initializing Tx ring");
			goto err_ring;
		}

		netif_dbg(pdata, drv, pdata->netdev, "%s - Rx ring:\n",
			  channel->name);

		ret = dwc_eth_init_ring(pdata, channel->rx_ring,
					pdata->rx_desc_count);
		if (ret) {
			netdev_alert(pdata->netdev,
				     "error initializing Rx ring\n");
			goto err_ring;
		}
	}

	TRACE("<--");

	return 0;

err_ring:
	dwc_eth_free_rings(pdata);

	return ret;
}

static void dwc_eth_free_channels(struct dwc_eth_pdata *pdata)
{
	if (!pdata->channel_head)
		return;

	kfree(pdata->channel_head->tx_ring);
	pdata->channel_head->tx_ring = NULL;

	kfree(pdata->channel_head->rx_ring);
	pdata->channel_head->rx_ring = NULL;

	kfree(pdata->channel_head);

	pdata->channel_head = NULL;
	pdata->channel_count = 0;
}

static int dwc_eth_alloc_channels(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_channel *channel_head, *channel;
	struct dwc_eth_ring *tx_ring, *rx_ring;
	unsigned int i;
	int ret = -ENOMEM;

	TRACE("-->");

	channel_head = kcalloc(pdata->channel_count,
			       sizeof(struct dwc_eth_channel), GFP_KERNEL);
	if (!channel_head)
		goto err_channel;

	netif_dbg(pdata, drv, pdata->netdev,
		  "channel_head=%p\n", channel_head);

	tx_ring = kcalloc(pdata->tx_ring_count, sizeof(struct dwc_eth_ring),
			  GFP_KERNEL);
	if (!tx_ring)
		goto err_tx_ring;

	rx_ring = kcalloc(pdata->rx_ring_count, sizeof(struct dwc_eth_ring),
			  GFP_KERNEL);
	if (!rx_ring)
		goto err_rx_ring;

	for (i = 0, channel = channel_head; i < pdata->channel_count;
		i++, channel++) {
		snprintf(channel->name, sizeof(channel->name), "channel-%u", i);
		channel->pdata = pdata;
		channel->queue_index = i;
		channel->dma_regs = pdata->mac_regs + DMA_CH_BASE +
				    (DMA_CH_INC * i);

		if (pdata->per_channel_irq) {
			/* Get the per DMA interrupt */
			ret = pdata->channel_irq[i];
			if (ret < 0) {
				netdev_err(pdata->netdev,
					   "get_irq %u failed\n",
					   i + 1);
				goto err_irq;
			}
			channel->dma_irq = ret;
		}

		if (i < pdata->tx_ring_count)
			channel->tx_ring = tx_ring++;

		if (i < pdata->rx_ring_count)
			channel->rx_ring = rx_ring++;

		netif_dbg(pdata, drv, pdata->netdev,
			  "%s: dma_regs=%p, tx_ring=%p, rx_ring=%p\n",
			  channel->name, channel->dma_regs,
			  channel->tx_ring, channel->rx_ring);
	}

	pdata->channel_head = channel_head;

	TRACE("<--");

	return 0;

err_irq:
	kfree(rx_ring);

err_rx_ring:
	kfree(tx_ring);

err_tx_ring:
	kfree(channel_head);

err_channel:
	return ret;
}

static void dwc_eth_free_channels_and_rings(struct dwc_eth_pdata *pdata)
{
	dwc_eth_free_rings(pdata);

	dwc_eth_free_channels(pdata);
}

static int dwc_eth_alloc_channels_and_rings(struct dwc_eth_pdata *pdata)
{
	int ret;

	ret = dwc_eth_alloc_channels(pdata);
	if (ret)
		goto err_alloc;

	ret = dwc_eth_alloc_rings(pdata);
	if (ret)
		goto err_alloc;

	return 0;

err_alloc:
	dwc_eth_free_channels_and_rings(pdata);

	return ret;
}

static int dwc_eth_alloc_pages(struct dwc_eth_pdata *pdata,
			       struct dwc_eth_page_alloc *pa,
			       gfp_t gfp, int order)
{
	struct page *pages = NULL;
	dma_addr_t pages_dma;
	int ret;

	/* Try to obtain pages, decreasing order if necessary */
	gfp |= __GFP_COLD | __GFP_COMP | __GFP_NOWARN;
	while (order >= 0) {
		pages = alloc_pages(gfp, order);
		if (pages)
			break;

		order--;
	}
	if (!pages)
		return -ENOMEM;

	/* Map the pages */
	pages_dma = dma_map_page(pdata->dev, pages, 0,
				 PAGE_SIZE << order, DMA_FROM_DEVICE);
	ret = dma_mapping_error(pdata->dev, pages_dma);
	if (ret) {
		put_page(pages);
		return ret;
	}

	pa->pages = pages;
	pa->pages_len = PAGE_SIZE << order;
	pa->pages_offset = 0;
	pa->pages_dma = pages_dma;

	return 0;
}

static void dwc_eth_set_buffer_data(struct dwc_eth_buffer_data *bd,
				    struct dwc_eth_page_alloc *pa,
				    unsigned int len)
{
	get_page(pa->pages);
	bd->pa = *pa;

	bd->dma_base = pa->pages_dma;
	bd->dma_off = pa->pages_offset;
	bd->dma_len = len;

	pa->pages_offset += len;
	if ((pa->pages_offset + len) > pa->pages_len) {
		/* This data descriptor is responsible for unmapping page(s) */
		bd->pa_unmap = *pa;

		/* Get a new allocation next time */
		pa->pages = NULL;
		pa->pages_len = 0;
		pa->pages_offset = 0;
		pa->pages_dma = 0;
	}
}

static int dwc_eth_map_rx_buffer(struct dwc_eth_pdata *pdata,
				 struct dwc_eth_ring *ring,
				 struct dwc_eth_desc_data *desc_data)
{
	int order, ret;

	if (!ring->rx_hdr_pa.pages) {
		ret = dwc_eth_alloc_pages(pdata, &ring->rx_hdr_pa,
					  GFP_ATOMIC, 0);
		if (ret)
			return ret;
	}

	if (!ring->rx_buf_pa.pages) {
		order = max_t(int, PAGE_ALLOC_COSTLY_ORDER - 1, 0);
		ret = dwc_eth_alloc_pages(pdata, &ring->rx_buf_pa,
					  GFP_ATOMIC, order);
		if (ret)
			return ret;
	}

	/* Set up the header page info */
	dwc_eth_set_buffer_data(&desc_data->rx.hdr, &ring->rx_hdr_pa,
				pdata->skb_alloc_size);

	/* Set up the buffer page info */
	dwc_eth_set_buffer_data(&desc_data->rx.buf, &ring->rx_buf_pa,
				pdata->rx_buf_size);

	return 0;
}

static void dwc_eth_tx_desc_init(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;
	struct dwc_eth_channel *channel;
	struct dwc_eth_ring *ring;
	struct dwc_eth_dma_desc *dma_desc;
	struct dwc_eth_desc_data *desc_data;
	dma_addr_t dma_desc_addr;
	unsigned int i, j;

	TRACE("-->");

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		ring = channel->tx_ring;
		if (!ring)
			break;

		dma_desc = ring->dma_desc_head;
		dma_desc_addr = ring->dma_desc_head_addr;

		for (j = 0; j < ring->dma_desc_count; j++) {
			desc_data = DWC_ETH_GET_DESC_DATA(ring, j);

			desc_data->dma_desc = dma_desc;
			desc_data->dma_desc_addr = dma_desc_addr;

			dma_desc++;
			dma_desc_addr += sizeof(struct dwc_eth_dma_desc);
		}

		ring->cur = 0;
		ring->dirty = 0;
		memset(&ring->tx, 0, sizeof(ring->tx));

		hw_ops->tx_desc_init(channel);
	}

	TRACE("<--");
}

static void dwc_eth_rx_desc_init(struct dwc_eth_pdata *pdata)
{
	struct dwc_eth_hw_ops *hw_ops = &pdata->hw_ops;
	struct dwc_eth_channel *channel;
	struct dwc_eth_ring *ring;
	struct dwc_eth_dma_desc *dma_desc;
	struct dwc_eth_desc_data *desc_data;
	dma_addr_t dma_desc_addr;
	unsigned int i, j;

	TRACE("-->");

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		ring = channel->rx_ring;
		if (!ring)
			break;

		dma_desc = ring->dma_desc_head;
		dma_desc_addr = ring->dma_desc_head_addr;

		for (j = 0; j < ring->dma_desc_count; j++) {
			desc_data = DWC_ETH_GET_DESC_DATA(ring, j);

			desc_data->dma_desc = dma_desc;
			desc_data->dma_desc_addr = dma_desc_addr;

			if (dwc_eth_map_rx_buffer(pdata, ring, desc_data))
				break;

			dma_desc++;
			dma_desc_addr += sizeof(struct dwc_eth_dma_desc);
		}

		ring->cur = 0;
		ring->dirty = 0;

		hw_ops->rx_desc_init(channel);
	}

	TRACE("<--");
}

static int dwc_eth_map_tx_skb(struct dwc_eth_channel *channel,
			      struct sk_buff *skb)
{
	struct dwc_eth_pdata *pdata = channel->pdata;
	struct dwc_eth_ring *ring = channel->tx_ring;
	struct dwc_eth_desc_data *desc_data;
	struct dwc_eth_pkt_info *pkt_info;
	struct skb_frag_struct *frag;
	dma_addr_t skb_dma;
	unsigned int start_index, cur_index;
	unsigned int offset, tso, vlan, datalen, len;
	unsigned int i;

	TRACE("-->");
	DBGPR("  cur = %d\n", ring->cur);

	offset = 0;
	start_index = ring->cur;
	cur_index = ring->cur;

	pkt_info = &ring->pkt_info;
	pkt_info->desc_count = 0;
	pkt_info->length = 0;

	tso = DWC_ETH_GET_BITS(pkt_info->attributes, TX_PACKET_ATTRIBUTES,
			       TSO_ENABLE);
	vlan = DWC_ETH_GET_BITS(pkt_info->attributes, TX_PACKET_ATTRIBUTES,
				VLAN_CTAG);

	/* Save space for a context descriptor if needed */
	if ((tso && (pkt_info->mss != ring->tx.cur_mss)) ||
	    (vlan && (pkt_info->vlan_ctag != ring->tx.cur_vlan_ctag)))
		cur_index++;
	desc_data = DWC_ETH_GET_DESC_DATA(ring, cur_index);

	if (tso) {
		/* Map the TSO header */
		skb_dma = dma_map_single(pdata->dev, skb->data,
					 pkt_info->header_len, DMA_TO_DEVICE);
		if (dma_mapping_error(pdata->dev, skb_dma)) {
			netdev_alert(pdata->netdev, "dma_map_single failed\n");
			goto err_out;
		}
		desc_data->skb_dma = skb_dma;
		desc_data->skb_dma_len = pkt_info->header_len;
		netif_dbg(pdata, tx_queued, pdata->netdev,
			  "skb header: index=%u, dma=%pad, len=%u\n",
			  cur_index, &skb_dma, pkt_info->header_len);

		offset = pkt_info->header_len;

		pkt_info->length += pkt_info->header_len;

		cur_index++;
		desc_data = DWC_ETH_GET_DESC_DATA(ring, cur_index);
	}

	/* Map the (remainder of the) packet */
	for (datalen = skb_headlen(skb) - offset; datalen; ) {
		len = min_t(unsigned int, datalen, pdata->tx_max_buf_size);

		skb_dma = dma_map_single(pdata->dev, skb->data + offset, len,
					 DMA_TO_DEVICE);
		if (dma_mapping_error(pdata->dev, skb_dma)) {
			netdev_alert(pdata->netdev, "dma_map_single failed\n");
			goto err_out;
		}
		desc_data->skb_dma = skb_dma;
		desc_data->skb_dma_len = len;
		netif_dbg(pdata, tx_queued, pdata->netdev,
			  "skb data: index=%u, dma=%pad, len=%u\n",
			  cur_index, &skb_dma, len);

		datalen -= len;
		offset += len;

		pkt_info->length += len;

		cur_index++;
		desc_data = DWC_ETH_GET_DESC_DATA(ring, cur_index);
	}

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		netif_dbg(pdata, tx_queued, pdata->netdev,
			  "mapping frag %u\n", i);

		frag = &skb_shinfo(skb)->frags[i];
		offset = 0;

		for (datalen = skb_frag_size(frag); datalen; ) {
			len = min_t(unsigned int, datalen,
				    pdata->tx_max_buf_size);

			skb_dma = skb_frag_dma_map(pdata->dev, frag, offset,
						   len, DMA_TO_DEVICE);
			if (dma_mapping_error(pdata->dev, skb_dma)) {
				netdev_alert(pdata->netdev,
					     "skb_frag_dma_map failed\n");
				goto err_out;
			}
			desc_data->skb_dma = skb_dma;
			desc_data->skb_dma_len = len;
			desc_data->mapped_as_page = 1;
			netif_dbg(pdata, tx_queued, pdata->netdev,
				  "skb frag: index=%u, dma=%pad, len=%u\n",
				  cur_index, &skb_dma, len);

			datalen -= len;
			offset += len;

			pkt_info->length += len;

			cur_index++;
			desc_data = DWC_ETH_GET_DESC_DATA(ring, cur_index);
		}
	}

	/* Save the skb address in the last entry. We always have some data
	 * that has been mapped so desc_data is always advanced past the last
	 * piece of mapped data - use the entry pointed to by cur_index - 1.
	 */
	desc_data = DWC_ETH_GET_DESC_DATA(ring, cur_index - 1);
	desc_data->skb = skb;

	/* Save the number of descriptor entries used */
	pkt_info->desc_count = cur_index - start_index;

	DBGPR("  count=%u\n", pkt_info->desc_count);
	TRACE("<--");

	return pkt_info->desc_count;

err_out:
	while (start_index < cur_index) {
		desc_data = DWC_ETH_GET_DESC_DATA(ring, start_index++);
		dwc_eth_unmap_desc_data(pdata, desc_data);
	}

	DBGPR("  count=0\n");
	TRACE("<--");

	return 0;
}

void dwc_eth_init_desc_ops(struct dwc_eth_desc_ops *desc_ops)
{
	desc_ops->alloc_channles_and_rings = dwc_eth_alloc_channels_and_rings;
	desc_ops->free_channels_and_rings = dwc_eth_free_channels_and_rings;
	desc_ops->map_tx_skb = dwc_eth_map_tx_skb;
	desc_ops->map_rx_buffer = dwc_eth_map_rx_buffer;
	desc_ops->unmap_desc_data = dwc_eth_unmap_desc_data;
	desc_ops->tx_desc_init = dwc_eth_tx_desc_init;
	desc_ops->rx_desc_init = dwc_eth_rx_desc_init;
}
