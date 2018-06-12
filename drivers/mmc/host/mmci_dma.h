/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) STMicroelectronics 2018 - All Rights Reserved
 * Author: Ludovic.barre@st.com for STMicroelectronics.
 */
#ifndef __MMC_DMA_H__
#define __MMC_DMA_H__

struct mmci_dma_ops {
	int (*setup)(struct mmci_host *host);
	void (*release)(struct mmci_host *host);
	void (*pre_req)(struct mmci_host *host, struct mmc_data *data);
	int (*start)(struct mmci_host *host, unsigned int datactrl);
	void (*finalize)(struct mmci_host *host, struct mmc_data *data);
	void (*post_req)(struct mmci_host *host,
			 struct mmc_data *data, int err);
	void (*error)(struct mmci_host *host);
	void (*get_next_data)(struct mmci_host *host, struct mmc_data *data);
};

int mmci_dma_setup(struct mmci_host *host);
int mmci_dma_start(struct mmci_host *host, unsigned int datactrl);
void mmci_dma_release(struct mmci_host *host);
void mmci_dma_pre_req(struct mmci_host *host, struct mmc_data *data);
void mmci_dma_finalize(struct mmci_host *host, struct mmc_data *data);
void mmci_dma_post_req(struct mmci_host *host,
		       struct mmc_data *data, int err);
void mmci_dma_error(struct mmci_host *host);
void mmci_dma_get_next_data(struct mmci_host *host, struct mmc_data *data);

int sdmmc_idma_validate_data(struct mmci_host *host,
			     struct mmc_data *data);
#endif /* __MMC_DMA_H__ */
