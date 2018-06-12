// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) STMicroelectronics 2018 - All Rights Reserved
 * Author: Ludovic.barre@st.com for STMicroelectronics.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/of.h>
#include <linux/scatterlist.h>

#include "mmci.h"
#include "mmci_dma.h"

int mmci_dma_setup(struct mmci_host *host)
{
	struct mmci_dma_ops *mmci_dma = host->variant->mmci_dma;

	if (mmci_dma && mmci_dma->setup)
		return mmci_dma->setup(host);

	return 0;
}

void mmci_dma_release(struct mmci_host *host)
{
	struct mmci_dma_ops *mmci_dma = host->variant->mmci_dma;

	if (mmci_dma && mmci_dma->release)
		mmci_dma->release(host);
}

void mmci_dma_pre_req(struct mmci_host *host, struct mmc_data *data)
{
	struct mmci_dma_ops *mmci_dma = host->variant->mmci_dma;

	if (mmci_dma && mmci_dma->pre_req)
		mmci_dma->pre_req(host, data);
}

int mmci_dma_start(struct mmci_host *host, unsigned int datactrl)
{
	struct mmci_dma_ops *mmci_dma = host->variant->mmci_dma;

	if (mmci_dma && mmci_dma->start)
		return mmci_dma->start(host, datactrl);

	return -EINVAL;
}

void mmci_dma_finalize(struct mmci_host *host, struct mmc_data *data)
{
	struct mmci_dma_ops *mmci_dma = host->variant->mmci_dma;

	if (mmci_dma && mmci_dma->finalize)
		mmci_dma->finalize(host, data);
}

void mmci_dma_post_req(struct mmci_host *host,
		       struct mmc_data *data, int err)
{
	struct mmci_dma_ops *mmci_dma = host->variant->mmci_dma;

	if (mmci_dma && mmci_dma->post_req)
		mmci_dma->post_req(host, data, err);
}

void mmci_dma_error(struct mmci_host *host)
{
	struct mmci_dma_ops *mmci_dma = host->variant->mmci_dma;

	if (mmci_dma && mmci_dma->error)
		mmci_dma->error(host);
}

void mmci_dma_get_next_data(struct mmci_host *host, struct mmc_data *data)
{
	struct mmci_dma_ops *mmci_dma = host->variant->mmci_dma;

	if (mmci_dma && mmci_dma->get_next_data)
		mmci_dma->get_next_data(host, data);
}

#ifdef CONFIG_DMA_ENGINE
struct dmaengine_next {
	struct dma_async_tx_descriptor *dma_desc;
	struct dma_chan	*dma_chan;
	s32 cookie;
};

struct dmaengine_priv {
	struct dma_chan	*dma_current;
	struct dma_chan	*dma_rx_channel;
	struct dma_chan	*dma_tx_channel;
	struct dma_async_tx_descriptor	*dma_desc_current;
	struct dmaengine_next next_data;
	bool dma_in_progress;
};

#define dma_inprogress(dmae) ((dmae)->dma_in_progress)

#ifdef CONFIG_MMC_QCOM_DML
void dml_start_xfer(struct mmci_host *host, struct mmc_data *data)
{
	u32 config;
	void __iomem *base = host->base + DML_OFFSET;

	if (data->flags & MMC_DATA_READ) {
		/* Read operation: configure DML for producer operation */
		/* Set producer CRCI-x and disable consumer CRCI */
		config = readl_relaxed(base + DML_CONFIG);
		config = (config & ~PRODUCER_CRCI_MSK) | PRODUCER_CRCI_X_SEL;
		config = (config & ~CONSUMER_CRCI_MSK) | CONSUMER_CRCI_DISABLE;
		writel_relaxed(config, base + DML_CONFIG);

		/* Set the Producer BAM block size */
		writel_relaxed(data->blksz, base + DML_PRODUCER_BAM_BLOCK_SIZE);

		/* Set Producer BAM Transaction size */
		writel_relaxed(data->blocks * data->blksz,
			       base + DML_PRODUCER_BAM_TRANS_SIZE);
		/* Set Producer Transaction End bit */
		config = readl_relaxed(base + DML_CONFIG);
		config |= PRODUCER_TRANS_END_EN;
		writel_relaxed(config, base + DML_CONFIG);
		/* Trigger producer */
		writel_relaxed(1, base + DML_PRODUCER_START);
	} else {
		/* Write operation: configure DML for consumer operation */
		/* Set consumer CRCI-x and disable producer CRCI*/
		config = readl_relaxed(base + DML_CONFIG);
		config = (config & ~CONSUMER_CRCI_MSK) | CONSUMER_CRCI_X_SEL;
		config = (config & ~PRODUCER_CRCI_MSK) | PRODUCER_CRCI_DISABLE;
		writel_relaxed(config, base + DML_CONFIG);
		/* Clear Producer Transaction End bit */
		config = readl_relaxed(base + DML_CONFIG);
		config &= ~PRODUCER_TRANS_END_EN;
		writel_relaxed(config, base + DML_CONFIG);
		/* Trigger consumer */
		writel_relaxed(1, base + DML_CONSUMER_START);
	}

	/* make sure the dml is configured before dma is triggered */
	wmb();
}

static int of_get_dml_pipe_index(struct device_node *np, const char *name)
{
	int index;
	struct of_phandle_args dma_spec;

	index = of_property_match_string(np, "dma-names", name);

	if (index < 0)
		return -ENODEV;

	if (of_parse_phandle_with_args(np, "dmas", "#dma-cells", index,
				       &dma_spec))
		return -ENODEV;

	if (dma_spec.args_count)
		return dma_spec.args[0];

	return -ENODEV;
}

/* Initialize the dml hardware connected to SD Card controller */
int dml_hw_init(struct mmci_host *host, struct device_node *np)
{
	u32 config;
	void __iomem *base;
	int consumer_id, producer_id;

	consumer_id = of_get_dml_pipe_index(np, "tx");
	producer_id = of_get_dml_pipe_index(np, "rx");

	if (producer_id < 0 || consumer_id < 0)
		return -ENODEV;

	base = host->base + DML_OFFSET;

	/* Reset the DML block */
	writel_relaxed(1, base + DML_SW_RESET);

	/* Disable the producer and consumer CRCI */
	config = (PRODUCER_CRCI_DISABLE | CONSUMER_CRCI_DISABLE);
	/*
	 * Disable the bypass mode. Bypass mode will only be used
	 * if data transfer is to happen in PIO mode and don't
	 * want the BAM interface to connect with SDCC-DML.
	 */
	config &= ~BYPASS;
	/*
	 * Disable direct mode as we don't DML to MASTER the AHB bus.
	 * BAM connected with DML should MASTER the AHB bus.
	 */
	config &= ~DIRECT_MODE;
	/*
	 * Disable infinite mode transfer as we won't be doing any
	 * infinite size data transfers. All data transfer will be
	 * of finite data size.
	 */
	config &= ~INFINITE_CONS_TRANS;
	writel_relaxed(config, base + DML_CONFIG);

	/*
	 * Initialize the logical BAM pipe size for producer
	 * and consumer.
	 */
	writel_relaxed(PRODUCER_PIPE_LOGICAL_SIZE,
		       base + DML_PRODUCER_PIPE_LOGICAL_SIZE);
	writel_relaxed(CONSUMER_PIPE_LOGICAL_SIZE,
		       base + DML_CONSUMER_PIPE_LOGICAL_SIZE);

	/* Initialize Producer/consumer pipe id */
	writel_relaxed(producer_id | (consumer_id << CONSUMER_PIPE_ID_SHFT),
		       base + DML_PIPE_ID);

	/* Make sure dml initialization is finished */
	mb();

	return 0;
}
#else
static inline int dml_hw_init(struct mmci_host *host, struct device_node *np)
{
	return -EINVAL;
}

static inline void dml_start_xfer(struct mmci_host *host, struct mmc_data *data)
{
}
#endif /* CONFIG_MMC_QCOM_DML */

static int dmaengine_setup(struct mmci_host *host)
{
	const char *rxname, *txname;
	struct variant_data *variant = host->variant;
	struct dmaengine_priv *dmae;

	dmae = devm_kzalloc(mmc_dev(host->mmc), sizeof(*dmae), GFP_KERNEL);
	if (!dmae)
		return -ENOMEM;

	host->dma_priv = dmae;

	dmae->dma_rx_channel = dma_request_slave_channel(mmc_dev(host->mmc),
							 "rx");
	dmae->dma_tx_channel = dma_request_slave_channel(mmc_dev(host->mmc),
							 "tx");

	/* initialize pre request cookie */
	dmae->next_data.cookie = 1;

	/*
	 * If only an RX channel is specified, the driver will
	 * attempt to use it bidirectionally, however if it is
	 * is specified but cannot be located, DMA will be disabled.
	 */
	if (dmae->dma_rx_channel && !dmae->dma_tx_channel)
		dmae->dma_tx_channel = dmae->dma_rx_channel;

	if (dmae->dma_rx_channel)
		rxname = dma_chan_name(dmae->dma_rx_channel);
	else
		rxname = "none";

	if (dmae->dma_tx_channel)
		txname = dma_chan_name(dmae->dma_tx_channel);
	else
		txname = "none";

	dev_info(mmc_dev(host->mmc), "DMA channels RX %s, TX %s\n",
		 rxname, txname);

	/*
	 * Limit the maximum segment size in any SG entry according to
	 * the parameters of the DMA engine device.
	 */
	if (dmae->dma_tx_channel) {
		struct device *dev = dmae->dma_tx_channel->device->dev;
		unsigned int max_seg_size = dma_get_max_seg_size(dev);

		if (max_seg_size < host->mmc->max_seg_size)
			host->mmc->max_seg_size = max_seg_size;
	}
	if (dmae->dma_rx_channel) {
		struct device *dev = dmae->dma_rx_channel->device->dev;
		unsigned int max_seg_size = dma_get_max_seg_size(dev);

		if (max_seg_size < host->mmc->max_seg_size)
			host->mmc->max_seg_size = max_seg_size;
	}

	if (variant->qcom_dml && dmae->dma_rx_channel && dmae->dma_tx_channel)
		if (dml_hw_init(host, host->mmc->parent->of_node))
			variant->qcom_dml = false;

	return 0;
}

static inline void dmaengine_release(struct mmci_host *host)
{
	struct dmaengine_priv *dmae = host->dma_priv;

	if (dmae->dma_rx_channel)
		dma_release_channel(dmae->dma_rx_channel);
	if (dmae->dma_tx_channel)
		dma_release_channel(dmae->dma_tx_channel);

	dmae->dma_rx_channel = dmae->dma_tx_channel = NULL;
}

static void dmaengine_unmap(struct mmci_host *host, struct mmc_data *data)
{
	struct dmaengine_priv *dmae = host->dma_priv;
	struct dma_chan *chan;

	if (data->flags & MMC_DATA_READ)
		chan = dmae->dma_rx_channel;
	else
		chan = dmae->dma_tx_channel;

	dma_unmap_sg(chan->device->dev, data->sg, data->sg_len,
		     mmc_get_dma_dir(data));
}

static void dmaengine_error(struct mmci_host *host)
{
	struct dmaengine_priv *dmae = host->dma_priv;

	if (!dma_inprogress(dmae))
		return;

	dev_err(mmc_dev(host->mmc), "error during DMA transfer!\n");
	dmaengine_terminate_all(dmae->dma_current);
	dmae->dma_in_progress = false;
	dmae->dma_current = NULL;
	dmae->dma_desc_current = NULL;
	host->data->host_cookie = 0;

	dmaengine_unmap(host, host->data);
}

static void dmaengine_finalize(struct mmci_host *host, struct mmc_data *data)
{
	struct dmaengine_priv *dmae = host->dma_priv;
	u32 status;
	int i;

	if (!dma_inprogress(dmae))
		return;

	/* Wait up to 1ms for the DMA to complete */
	for (i = 0; ; i++) {
		status = readl(host->base + MMCISTATUS);
		if (!(status & MCI_RXDATAAVLBLMASK) || i >= 100)
			break;
		udelay(10);
	}

	/*
	 * Check to see whether we still have some data left in the FIFO -
	 * this catches DMA controllers which are unable to monitor the
	 * DMALBREQ and DMALSREQ signals while allowing us to DMA to non-
	 * contiguous buffers.  On TX, we'll get a FIFO underrun error.
	 */
	if (status & MCI_RXDATAAVLBLMASK) {
		dmaengine_error(host);
		if (!data->error)
			data->error = -EIO;
	}

	if (!data->host_cookie)
		dmaengine_unmap(host, data);

	/*
	 * Use of DMA with scatter-gather is impossible.
	 * Give up with DMA and switch back to PIO mode.
	 */
	if (status & MCI_RXDATAAVLBLMASK) {
		dev_err(mmc_dev(host->mmc),
			"buggy DMA detected. Taking evasive action.\n");
		dmaengine_release(host);
	}

	dmae->dma_in_progress = false;
	dmae->dma_current = NULL;
	dmae->dma_desc_current = NULL;
}

/* prepares DMA channel and DMA descriptor, returns non-zero on failure */
static int __dmaengine_prep_data(struct mmci_host *host, struct mmc_data *data,
				 struct dma_chan **dma_chan,
				 struct dma_async_tx_descriptor **dma_desc)
{
	struct dmaengine_priv *dmae = host->dma_priv;
	struct variant_data *variant = host->variant;
	struct dma_slave_config conf = {
		.src_addr = host->phybase + MMCIFIFO,
		.dst_addr = host->phybase + MMCIFIFO,
		.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
		.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
		.src_maxburst = variant->fifohalfsize >> 2, /* # of words */
		.dst_maxburst = variant->fifohalfsize >> 2, /* # of words */
		.device_fc = false,
	};
	struct dma_chan *chan;
	struct dma_device *device;
	struct dma_async_tx_descriptor *desc;
	int nr_sg;
	unsigned long flags = DMA_CTRL_ACK;

	if (data->flags & MMC_DATA_READ) {
		conf.direction = DMA_DEV_TO_MEM;
		chan = dmae->dma_rx_channel;
	} else {
		conf.direction = DMA_MEM_TO_DEV;
		chan = dmae->dma_tx_channel;
	}

	/* If there's no DMA channel, fall back to PIO */
	if (!chan)
		return -EINVAL;

	/* If less than or equal to the fifo size, don't bother with DMA */
	if (data->blksz * data->blocks <= variant->fifosize)
		return -EINVAL;

	device = chan->device;
	nr_sg = dma_map_sg(device->dev, data->sg, data->sg_len,
			   mmc_get_dma_dir(data));
	if (nr_sg == 0)
		return -EINVAL;

	if (host->variant->qcom_dml)
		flags |= DMA_PREP_INTERRUPT;

	dmaengine_slave_config(chan, &conf);
	desc = dmaengine_prep_slave_sg(chan, data->sg, nr_sg,
				       conf.direction, flags);
	if (!desc)
		goto unmap_exit;

	*dma_chan = chan;
	*dma_desc = desc;

	return 0;

 unmap_exit:
	dmaengine_unmap(host, data);
	return -ENOMEM;
}

static inline int dmaengine_prep_data(struct mmci_host *host,
				      struct mmc_data *data)
{
	struct dmaengine_priv *dmae = host->dma_priv;

	/* Check if next job is already prepared. */
	if (dmae->dma_current && dmae->dma_desc_current)
		return 0;

	/* No job were prepared thus do it now. */
	return __dmaengine_prep_data(host, data, &dmae->dma_current,
				     &dmae->dma_desc_current);
}

static inline int dmaengine_prep_next(struct mmci_host *host,
				      struct mmc_data *data)
{
	struct dmaengine_priv *dmae = host->dma_priv;
	struct dmaengine_next *nd = &dmae->next_data;

	return __dmaengine_prep_data(host, data, &nd->dma_chan, &nd->dma_desc);
}

static int dmaengine_start(struct mmci_host *host, unsigned int datactrl)
{
	struct dmaengine_priv *dmae = host->dma_priv;
	struct mmc_data *data = host->data;
	int ret;

	ret = dmaengine_prep_data(host, host->data);
	if (ret)
		return ret;

	/* Okay, go for it. */
	dev_vdbg(mmc_dev(host->mmc),
		 "Submit MMCI DMA job, sglen %d blksz %04x blks %04x flags %08x\n",
		 data->sg_len, data->blksz, data->blocks, data->flags);
	dmae->dma_in_progress = true;
	dmaengine_submit(dmae->dma_desc_current);
	dma_async_issue_pending(dmae->dma_current);

	if (host->variant->qcom_dml)
		dml_start_xfer(host, data);

	datactrl |= MCI_DPSM_DMAENABLE;

	/* Trigger the DMA transfer */
	mmci_write_datactrlreg(host, datactrl);

	/*
	 * Let the MMCI say when the data is ended and it's time
	 * to fire next DMA request. When that happens, MMCI will
	 * call mmci_data_end()
	 */
	writel(readl(host->base + MMCIMASK0) | MCI_DATAENDMASK,
	       host->base + MMCIMASK0);
	return 0;
}

static void dmaengine_get_next_data(struct mmci_host *host,
				    struct mmc_data *data)
{
	struct dmaengine_priv *dmae = host->dma_priv;
	struct dmaengine_next *next = &dmae->next_data;

	WARN_ON(data->host_cookie && data->host_cookie != next->cookie);
	WARN_ON(!data->host_cookie && (next->dma_desc || next->dma_chan));

	dmae->dma_desc_current = next->dma_desc;
	dmae->dma_current = next->dma_chan;
	next->dma_desc = NULL;
	next->dma_chan = NULL;
}

static void dmaengine_pre_req(struct mmci_host *host, struct mmc_data *data)
{
	struct dmaengine_priv *dmae = host->dma_priv;
	struct dmaengine_next *nd = &dmae->next_data;

	if (!dmaengine_prep_next(host, data))
		data->host_cookie = ++nd->cookie < 0 ? 1 : nd->cookie;
}

static void dmaengine_post_req(struct mmci_host *host,
			       struct mmc_data *data, int err)
{
	struct dmaengine_priv *dmae = host->dma_priv;

	dmaengine_unmap(host, data);

	if (err) {
		struct dmaengine_next *next = &dmae->next_data;
		struct dma_chan *chan;

		if (data->flags & MMC_DATA_READ)
			chan = dmae->dma_rx_channel;
		else
			chan = dmae->dma_tx_channel;
		dmaengine_terminate_all(chan);

		if (dmae->dma_desc_current == next->dma_desc)
			dmae->dma_desc_current = NULL;

		if (dmae->dma_current == next->dma_chan) {
			dmae->dma_in_progress = false;
			dmae->dma_current = NULL;
		}

		next->dma_desc = NULL;
		next->dma_chan = NULL;
		data->host_cookie = 0;
	}
}

struct mmci_dma_ops dmaengine = {
	.setup = dmaengine_setup,
	.release = dmaengine_release,
	.pre_req = dmaengine_pre_req,
	.start = dmaengine_start,
	.finalize = dmaengine_finalize,
	.post_req = dmaengine_post_req,
	.error = dmaengine_error,
	.get_next_data = dmaengine_get_next_data,
};
#else
struct mmci_dma_ops dmaengine = {};
#endif

#define SDMMC_LLI_BUF_LEN	PAGE_SIZE
#define SDMMC_IDMA_BURST	BIT(MMCI_STM32_IDMABNDT_SHIFT)

struct sdmmc_lli_desc {
	u32 idmalar;
	u32 idmabase;
	u32 idmasize;
};

struct sdmmc_next {
	s32 cookie;
};

struct sdmmc_priv {
	dma_addr_t sg_dma;
	void *sg_cpu;
	struct sdmmc_next next_data;
};

static int __sdmmc_idma_prep_data(struct mmci_host *host, struct mmc_data *data)
{
	int n_elem;

	n_elem = dma_map_sg(mmc_dev(host->mmc),
			    data->sg,
			    data->sg_len,
			    mmc_get_dma_dir(data));

	if (!n_elem) {
		dev_err(mmc_dev(host->mmc), "dma_map_sg failed\n");
		return -EINVAL;
	}

	return 0;
}

int sdmmc_idma_validate_data(struct mmci_host *host,
			     struct mmc_data *data)
{
	struct sdmmc_priv *idma = host->dma_priv;
	struct sdmmc_next *nd = &idma->next_data;
	struct scatterlist *sg;
	int ret, i;

	/* Check if next job is not already prepared. */
	if (data->host_cookie != nd->cookie) {
		ret = __sdmmc_idma_prep_data(host, data);
		if (ret)
			return ret;
	}

	/*
	 * idma has constraints on idmabase & idmasize for each element
	 * excepted the last element which has no constraint on idmasize
	 */
	for_each_sg(data->sg, sg, data->sg_len - 1, i) {
		if (!IS_ALIGNED(sg_dma_address(data->sg), sizeof(u32)) ||
		    !IS_ALIGNED(sg_dma_len(data->sg), SDMMC_IDMA_BURST)) {
			dev_err(mmc_dev(host->mmc),
				"unaligned scatterlist: ofst:%x length:%d\n",
				data->sg->offset, data->sg->length);
			return -EINVAL;
		}
	}

	if (!IS_ALIGNED(sg_dma_address(data->sg), sizeof(u32))) {
		dev_err(mmc_dev(host->mmc),
			"unaligned last scatterlist: ofst:%x length:%d\n",
			data->sg->offset, data->sg->length);
		return -EINVAL;
	}

	return 0;
}

static void sdmmc_idma_pre_req(struct mmci_host *host, struct mmc_data *data)
{
	struct sdmmc_priv *idma = host->dma_priv;
	struct sdmmc_next *nd = &idma->next_data;

	if (!__sdmmc_idma_prep_data(host, data))
		data->host_cookie = ++nd->cookie < 0 ? 1 : nd->cookie;
}

static void sdmmc_idma_post_req(struct mmci_host *host, struct mmc_data *data,
				int err)
{
	if (!data || !data->host_cookie)
		return;

	dma_unmap_sg(mmc_dev(host->mmc), data->sg, data->sg_len,
		     mmc_get_dma_dir(data));

	data->host_cookie = 0;
}

static int sdmmc_idma_setup(struct mmci_host *host)
{
	struct sdmmc_priv *idma;

	idma = devm_kzalloc(mmc_dev(host->mmc), sizeof(*idma), GFP_KERNEL);
	if (!idma)
		return -ENOMEM;

	host->dma_priv = idma;

	if (host->variant->dma_lli) {
		idma->sg_cpu = dmam_alloc_coherent(mmc_dev(host->mmc),
						   SDMMC_LLI_BUF_LEN,
						   &idma->sg_dma, GFP_KERNEL);
		if (!idma->sg_cpu) {
			dev_err(mmc_dev(host->mmc),
				"Failed to alloc IDMA descriptor\n");
			return -ENOMEM;
		}
		host->mmc->max_segs = SDMMC_LLI_BUF_LEN /
			sizeof(struct sdmmc_lli_desc);
		host->mmc->max_seg_size = host->variant->stm32_idmabsize_mask;
	} else {
		host->mmc->max_segs = 1;
		host->mmc->max_seg_size = host->mmc->max_req_size;
	}

	/* initialize pre request cookie */
	idma->next_data.cookie = 1;

	return 0;
}

static int sdmmc_idma_start(struct mmci_host *host, unsigned int datactrl)

{
	struct sdmmc_priv *idma = host->dma_priv;
	struct sdmmc_lli_desc *desc = (struct sdmmc_lli_desc *)idma->sg_cpu;
	struct mmc_data *data = host->data;
	struct scatterlist *sg;
	int i;

	if (!host->variant->dma_lli || data->sg_len == 1) {
		writel_relaxed(sg_dma_address(data->sg),
			       host->base + MMCI_STM32_IDMABASE0R);
		writel_relaxed(MMCI_STM32_IDMAEN,
			       host->base + MMCI_STM32_IDMACTRLR);
		goto out;
	}

	for_each_sg(data->sg, sg, data->sg_len, i) {
		desc[i].idmalar = (i + 1) * sizeof(struct sdmmc_lli_desc);
		desc[i].idmalar |= MMCI_STM32_ULA | MMCI_STM32_ULS
			| MMCI_STM32_ABR;
		desc[i].idmabase = sg_dma_address(sg);
		desc[i].idmasize = sg_dma_len(sg);
	}

	/* notice the end of link list */
	desc[data->sg_len - 1].idmalar &= ~MMCI_STM32_ULA;

	dma_wmb();
	writel_relaxed(idma->sg_dma, host->base + MMCI_STM32_IDMABAR);
	writel_relaxed(desc[0].idmalar, host->base + MMCI_STM32_IDMALAR);
	writel_relaxed(desc[0].idmabase, host->base + MMCI_STM32_IDMABASE0R);
	writel_relaxed(desc[0].idmasize, host->base + MMCI_STM32_IDMABSIZER);
	writel_relaxed(MMCI_STM32_IDMAEN | MMCI_STM32_IDMALLIEN,
		       host->base + MMCI_STM32_IDMACTRLR);

	/* mask & datactrl */
out:
	mmci_write_datactrlreg(host, datactrl);
	writel(readl(host->base + MMCIMASK0) | MCI_DATAENDMASK,
	       host->base + MMCIMASK0);

	return 0;
}

static void sdmmc_idma_finalize(struct mmci_host *host, struct mmc_data *data)
{
	writel_relaxed(0, host->base + MMCI_STM32_IDMACTRLR);
}

static void sdmmc_idma_get_next_data(struct mmci_host *host,
				     struct mmc_data *data)
{
	struct sdmmc_priv *idma = host->dma_priv;
	struct sdmmc_next *next = &idma->next_data;

	WARN_ON(data->host_cookie && data->host_cookie != next->cookie);
}

struct mmci_dma_ops sdmmc_idma = {
	.setup = sdmmc_idma_setup,
	.pre_req = sdmmc_idma_pre_req,
	.start = sdmmc_idma_start,
	.finalize = sdmmc_idma_finalize,
	.post_req = sdmmc_idma_post_req,
	.get_next_data = sdmmc_idma_get_next_data,
};
