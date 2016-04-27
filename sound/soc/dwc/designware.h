/*
 * ALSA SoC Synopsys Audio Layer
 *
 * sound/soc/dwc/designware.h
 *
 * Copyright (C) 2016 Synopsys
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef __DESIGNWARE_H
#define __DESIGNWARE_H

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/kconfig.h>
#include <sound/designware_i2s.h>
#include <sound/dmaengine_pcm.h>

struct dw_pcm_binfo {
	struct snd_pcm_substream *stream;
	unsigned char *dma_base;
	unsigned char *dma_pointer;
	unsigned int period_size_frames;
	unsigned int size;
	snd_pcm_uframes_t period_pointer;
	unsigned int total_periods;
	unsigned int current_period;
};

union dw_i2s_snd_dma_data {
	struct i2s_dma_data pd;
	struct snd_dmaengine_dai_dma_data dt;
};

struct dw_i2s_dev {
	void __iomem *i2s_base;
	struct clk *clk;
	int active;
	unsigned int capability;
	unsigned int quirks;
	unsigned int i2s_reg_comp1;
	unsigned int i2s_reg_comp2;
	struct device *dev;
	u32 ccr;
	u32 xfer_resolution;
	u32 fifo_th;

	/* data related to DMA transfers b/w i2s and DMAC */
	bool use_dmaengine;
	union dw_i2s_snd_dma_data play_dma_data;
	union dw_i2s_snd_dma_data capture_dma_data;
	struct i2s_clk_config_data config;
	int (*i2s_clk_cfg)(struct i2s_clk_config_data *config);
	struct dw_pcm_binfo binfo;
};

#if IS_ENABLED(CONFIG_SND_DESIGNWARE_PCM)
int dw_pcm_transfer(u32 *lsample, u32 *rsample, int bytes, int buf_size,
		struct dw_pcm_binfo *bi);
#else
int dw_pcm_transfer(u32 *lsample, u32 *rsample, int bytes, int buf_size,
		struct dw_pcm_binfo *bi)
{
	return 0;
}
#endif

#endif
