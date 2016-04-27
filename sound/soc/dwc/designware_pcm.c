/*
 * Synopsys I2S PCM Driver
 *
 * Copyright (C) 2016 Synopsys
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/dmaengine_pcm.h>
#include "designware.h"

#define BUFFER_BYTES_MAX	384000
#define PERIOD_BYTES_MIN	2048
#define PERIODS_MIN		8

static const struct snd_pcm_hardware dw_pcm_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.rates = SNDRV_PCM_RATE_32000 |
		SNDRV_PCM_RATE_44100 |
		SNDRV_PCM_RATE_48000,
	.rate_min = 32000,
	.rate_max = 48000,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = BUFFER_BYTES_MAX,
	.period_bytes_min = PERIOD_BYTES_MIN,
	.period_bytes_max = BUFFER_BYTES_MAX / PERIODS_MIN,
	.periods_min = PERIODS_MIN,
	.periods_max = BUFFER_BYTES_MAX / PERIOD_BYTES_MIN,
};

int dw_pcm_transfer(u32 *lsample, u32 *rsample, int bytes, int buf_size,
		struct dw_pcm_binfo *bi)
{
	struct snd_pcm_runtime *rt = bi->stream->runtime;
	int dir = bi->stream->stream;
	int i;

	for (i = 0; i < buf_size; i++) {
		if (dir == SNDRV_PCM_STREAM_PLAYBACK) {
			memcpy(&lsample[i], bi->dma_pointer, bytes);
			bi->dma_pointer += bytes;
			memcpy(&rsample[i], bi->dma_pointer, bytes);
			bi->dma_pointer += bytes;
		} else {
			memcpy(bi->dma_pointer, &lsample[i], bytes);
			bi->dma_pointer += bytes;
			memcpy(bi->dma_pointer, &rsample[i], bytes);
			bi->dma_pointer += bytes;
		}
	}

	bi->period_pointer += bytes_to_frames(rt, bytes * 2 * buf_size);

	if (bi->period_pointer >= (bi->period_size_frames * bi->current_period)) {
		bi->current_period++;
		if (bi->current_period > bi->total_periods) {
			bi->dma_pointer = bi->dma_base;
			bi->period_pointer = 0;
			bi->current_period = 1;
		}
		snd_pcm_period_elapsed(bi->stream);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(dw_pcm_transfer);

static int dw_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_pcm_runtime *rt = substream->runtime;
	struct dw_i2s_dev *dev = snd_soc_dai_get_drvdata(rtd->cpu_dai);

	snd_soc_set_runtime_hwparams(substream, &dw_pcm_hardware);
	snd_pcm_hw_constraint_integer(rt, SNDRV_PCM_HW_PARAM_PERIODS);

	dev->binfo.stream = substream;
	rt->private_data = &dev->binfo;
	return 0;
}

static int dw_pcm_close(struct snd_pcm_substream *substream)
{
	return 0;
}

static int dw_pcm_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *hw_params)
{
	struct snd_pcm_runtime *rt = substream->runtime;
	struct dw_pcm_binfo *bi = rt->private_data;
	int ret;

	ret = snd_pcm_lib_alloc_vmalloc_buffer(substream,
			params_buffer_bytes(hw_params));
	if (ret < 0)
		return ret;

	memset(rt->dma_area, 0, params_buffer_bytes(hw_params));
	bi->dma_base = rt->dma_area;
	bi->dma_pointer = bi->dma_base;

	return 0;
}

static int dw_pcm_hw_free(struct snd_pcm_substream *substream)
{
	return snd_pcm_lib_free_vmalloc_buffer(substream);
}

static int dw_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *rt = substream->runtime;
	struct dw_pcm_binfo *bi = rt->private_data;
	u32 buffer_size_frames = 0;

	bi->period_size_frames = bytes_to_frames(rt,
			snd_pcm_lib_period_bytes(substream));
	bi->size = snd_pcm_lib_buffer_bytes(substream);
	buffer_size_frames = bytes_to_frames(rt, bi->size);
	bi->total_periods = buffer_size_frames / bi->period_size_frames;
	bi->current_period = 1;

	if ((buffer_size_frames % bi->period_size_frames) != 0)
		return -EINVAL;
	if ((bi->size % (snd_pcm_format_width(rt->format) / 8)) != 0)
		return -EINVAL;
	return 0;
}

static int dw_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static snd_pcm_uframes_t dw_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *rt = substream->runtime;
	struct dw_pcm_binfo *bi = rt->private_data;

	return bi->period_pointer;
}

static struct snd_pcm_ops dw_pcm_ops = {
	.open = dw_pcm_open,
	.close = dw_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = dw_pcm_hw_params,
	.hw_free = dw_pcm_hw_free,
	.prepare = dw_pcm_prepare,
	.trigger = dw_pcm_trigger,
	.pointer = dw_pcm_pointer,
	.page = snd_pcm_lib_get_vmalloc_page,
	.mmap = snd_pcm_lib_mmap_vmalloc,
};

static int dw_pcm_new(struct snd_soc_pcm_runtime *runtime)
{
	struct snd_pcm *pcm = runtime->pcm;

	return snd_pcm_lib_preallocate_pages_for_all(pcm,
			SNDRV_DMA_TYPE_CONTINUOUS,
			snd_dma_continuous_data(GFP_KERNEL), BUFFER_BYTES_MAX,
			BUFFER_BYTES_MAX);
}

static void dw_pcm_free(struct snd_pcm *pcm)
{
	snd_pcm_lib_preallocate_free_for_all(pcm);
}

static struct snd_soc_platform_driver dw_pcm_platform = {
	.pcm_new = dw_pcm_new,
	.pcm_free = dw_pcm_free,
	.ops = &dw_pcm_ops,
};

static int dw_pcm_probe(struct platform_device *pdev)
{
	return devm_snd_soc_register_platform(&pdev->dev, &dw_pcm_platform);
}

#ifdef CONFIG_OF
static const struct of_device_id dw_pcm_of[] = {
	{ .compatible = "snps,designware-pcm" },
	{ }
};
MODULE_DEVICE_TABLE(of, dw_pcm_of);
#endif

static struct platform_driver dw_pcm_driver = {
	.driver = {
		.name = "designware-pcm",
		.of_match_table = of_match_ptr(dw_pcm_of),
	},
	.probe = dw_pcm_probe,
};
module_platform_driver(dw_pcm_driver);

MODULE_AUTHOR("Jose Abreu <joabreu@synopsys.com>, Tiago Duarte");
MODULE_DESCRIPTION("Synopsys Designware PCM Driver");
MODULE_LICENSE("GPL v2");
