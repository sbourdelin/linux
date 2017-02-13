/*
 * This file is part of STM32 DFSDM ASoC DAI driver
 *
 * Copyright (C) 2017, STMicroelectronics - All Rights Reserved
 * Authors: Arnaud Pouliquen <arnaud.pouliquen@st.com>
 *          Olivier Moysan <olivier.moysan@st.com>
 *
 * License type: GPLv2
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <sound/dmaengine_pcm.h>
#include <sound/stm32-adfsdm.h>

#define STM32_ADFSDM_DATA_MASK	GENMASK(31, 8)

struct stm32_adfsdm_priv {
	struct snd_soc_dai_driver dai_drv;
	struct stm32_adfsdm_pdata *pdata; /* platform data set by IIO driver */
	struct snd_dmaengine_dai_dma_data dma_data;  /* dma config */
	struct snd_pcm_substream *substream;  
	struct snd_pcm_hw_constraint_list rates_const;
	unsigned long dmic_clk; /* SPI or manchester input clock frequency */
	unsigned int fl_id;   /* filter instance ID */
	unsigned int order; /* filter order */
	unsigned int max_scaling;  /* max scaling for audio samples */
};

struct stm32_adfsdm_data {
	unsigned int rate;	/* SNDRV_PCM_RATE value */
	unsigned int freq;	/* frequency in Hz */
};

static const struct stm32_adfsdm_data stm32_dfsdm_filter[] = {
	{ .rate = SNDRV_PCM_RATE_8000,  .freq = 8000 },
	{ .rate = SNDRV_PCM_RATE_16000, .freq = 16000 },
	{ .rate = SNDRV_PCM_RATE_32000, .freq = 32000 },
};

static const struct snd_pcm_hardware stm32_adfsdm_pcm_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
	    SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_MMAP |
	    SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S24_LE,

	.rate_min = 8000,
	.rate_max = 32000,

	.channels_min = 1,
	.channels_max = 1,

	.periods_min = 2,
	.periods_max = 48,

	.period_bytes_min = 40, /* 8 khz 5 ms */
	.period_bytes_max = 4 * PAGE_SIZE,
	.buffer_bytes_max = 16 * PAGE_SIZE
};

static int stm32_adfsdm_get_supported_rates(struct snd_soc_dai *dai,
					    unsigned int *rates)
{
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(dai);
	struct stm32_adfsdm_pdata *pdata = priv->pdata;
	struct stm32_dfsdm_hw_param params;
	unsigned int max_scaling, i;
	int ret;

	*rates = 0;

	for (i = 0; i < ARRAY_SIZE(stm32_dfsdm_filter); i++) {
		/* 
		 * Check that clkout_freq is compatible
		 * Try to find one solution for filter and integrator
		 * oversampling ratio.
		 */

		params.rate = stm32_dfsdm_filter[i].freq;
		params.sample_bits = 24;
		params.max_scaling = &max_scaling;

		ret = pdata->ops->set_hwparam(pdata->adc, &params);
		if (!ret) {
			*rates |= 1 << i;
			dev_err(dai->dev, "%s: %d rate supported\n", __func__,
				stm32_dfsdm_filter[i].freq);
		}
	}

	if (!*rates) {
		dev_err(dai->dev, "%s: no matched rate found\n", __func__);
		return -EINVAL;
	}

	return 0;
}

static int stm32_adfsdm_copy(struct snd_pcm_substream *substream, int channel,
			     snd_pcm_uframes_t pos,
			     void __user *buf, snd_pcm_uframes_t count)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(rtd->cpu_dai);
	int *ptr = (int *)(runtime->dma_area + frames_to_bytes(runtime, pos));
	char *hwbuf = runtime->dma_area + frames_to_bytes(runtime, pos);
	ssize_t bytes = frames_to_bytes(runtime, count);
	ssize_t sample_cnt = bytes_to_samples(runtime, bytes);
	unsigned int shift = 24 -priv->max_scaling;
	
	/*
	 * Audio samples are available on 24 MSBs of the DFSDM DATAR register.
	 * We need to mask 8 LSB control bits...
	 * Additionnaly sample scaling depends on decimation and can need shift
	 * to be aligned on 32-bit word MSB.
	 */
	if (shift > 0) {
		do {
			*ptr <<= shift & STM32_ADFSDM_DATA_MASK;
			ptr++;
		} while (--sample_cnt);
	} else {
		do {
			*ptr &= STM32_ADFSDM_DATA_MASK;
			ptr++;
		} while (--sample_cnt);
	}

	return copy_to_user(buf, hwbuf, bytes);
}

static void stm32_dfsdm_xrun(void *context)
{
	struct snd_soc_dai *dai = context;
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(dai);

	snd_pcm_stream_lock(priv->substream);
	dev_dbg(dai->dev, "%s:unexpected overrun\n", __func__);
	/* Stop the player */
	snd_pcm_stop(priv->substream, SNDRV_PCM_STATE_XRUN);
	snd_pcm_stream_unlock(priv->substream);
}

static int stm32_adfsdm_startup(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(dai);

	priv->substream = substream;

	dev_dbg(dai->dev, "%s: enter\n", __func__);
	return 0;
	return snd_pcm_hw_constraint_list(substream->runtime, 0,
					  SNDRV_PCM_HW_PARAM_RATE,
					  &priv->rates_const);
}

static void stm32_adfsdm_shutdown(struct snd_pcm_substream *substream,
				  struct snd_soc_dai *dai)
{
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(dai);

	dev_dbg(dai->dev, "%s: enter\n", __func__);
	priv->substream = NULL;
}

static int stm32_adfsdm_dai_hw_params(struct snd_pcm_substream *substream,
				      struct snd_pcm_hw_params *params,
				      struct snd_soc_dai *dai)
{
	struct snd_dmaengine_dai_dma_data *dma_data;
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(dai);
	struct stm32_adfsdm_pdata *pdata = priv->pdata;
	struct stm32_dfsdm_hw_param df_params;

	dev_dbg(dai->dev, "%s: enter\n", __func__);
	dma_data = snd_soc_dai_get_dma_data(dai, substream);
	dma_data->maxburst = 1;

	df_params.rate = substream->runtime->rate;
	df_params.sample_bits = substream->runtime->sample_bits;
	df_params.max_scaling = &priv->max_scaling;

	return pdata->ops->set_hwparam(pdata->adc, &df_params);
}

static int stm32_adfsdm_trigger(struct snd_pcm_substream *substream, int cmd,
				struct snd_soc_dai *dai)
{
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(dai);
	struct stm32_adfsdm_pdata *pdata = priv->pdata;

	dev_dbg(dai->dev, "%s: enter\n", __func__);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		return pdata->ops->audio_startup(pdata->adc);
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_STOP:
		pdata->ops->audio_shutdown(pdata->adc);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int stm32_adfsdm_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(dai);
	unsigned int cb = fmt & SND_SOC_DAIFMT_MASTER_MASK;

	dev_dbg(dai->dev, "%s: enter\n", __func__);

	if ((cb == SND_SOC_DAIFMT_CBM_CFM) || (cb == SND_SOC_DAIFMT_CBM_CFS)) {
		/* Digital microphone is clocked by external clock */
		if (!priv->dmic_clk) {
			dev_err(dai->dev,
				"system-clock-frequency not defined\n");
			return -EINVAL;
		}
	}

	return 0;
}

static int stm32_adfsdm_set_sysclk(struct snd_soc_dai *dai, int clk_id,
				   unsigned int freq, int dir)
{
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(dai);
	struct stm32_adfsdm_pdata *pdata = priv->pdata;

	dev_dbg(dai->dev, "%s: enter for dai %d\n", __func__, dai->id);
	if (dir == SND_SOC_CLOCK_IN) {
		pdata->ops->set_sysclk(pdata->adc, freq);
		priv->dmic_clk = freq;
	}

	/* Determine supported rate which depends on SPI/manchester clock */
	return stm32_adfsdm_get_supported_rates(dai, &priv->rates_const.mask);
}

static const struct snd_soc_dai_ops stm32_adfsdm_dai_ops = {
	.startup = stm32_adfsdm_startup,
	.shutdown = stm32_adfsdm_shutdown,
	.hw_params = stm32_adfsdm_dai_hw_params,
	.set_fmt = stm32_adfsdm_set_dai_fmt,
	.set_sysclk = stm32_adfsdm_set_sysclk,
	.trigger = stm32_adfsdm_trigger,
};

static int stm32_adfsdm_dai_probe(struct snd_soc_dai *dai)
{
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(dai);
	struct snd_dmaengine_dai_dma_data *dma = &priv->dma_data;
	struct stm32_adfsdm_pdata *pdata = priv->pdata;

	dev_dbg(dai->dev, "%s: enter for dai %d\n", __func__, dai->id);

	/* DMA settings */
	snd_soc_dai_init_dma_data(dai, NULL, dma);
	dma->addr = pdata->ops->get_dma_source(pdata->adc);
	dma->addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;

	pdata->ops->register_xrun_cb(priv->pdata->adc, stm32_dfsdm_xrun, dai);

	return 0;
}

static int stm32_adfsdm_dai_remove(struct snd_soc_dai *dai)
{
	dev_dbg(dai->dev, "%s: enter for dai %d\n", __func__, dai->id);

	return 0;
}

static const struct snd_soc_dai_driver stm32_adfsdm_dai = {
	.capture = {
		    .channels_min = 1,
		    .channels_max = 1,
		    .formats = SNDRV_PCM_FMTBIT_S24_LE,
		    .rates = (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
			      SNDRV_PCM_RATE_32000),
		    },
	.probe = stm32_adfsdm_dai_probe,
	.remove = stm32_adfsdm_dai_remove,
	.ops = &stm32_adfsdm_dai_ops,
};

static const struct snd_soc_component_driver stm32_adfsdm_dai_component = {
	.name = "sti_cpu_dai",
};

static const struct snd_dmaengine_pcm_config dmaengine_pcm_config = {
	.pcm_hardware = &stm32_adfsdm_pcm_hw,
	.prepare_slave_config = snd_dmaengine_pcm_prepare_slave_config,
	.copy = stm32_adfsdm_copy,
};

static int stm32_adfsdm_probe(struct platform_device *pdev)
{
	struct stm32_adfsdm_priv *priv;
	struct stm32_adfsdm_pdata *pdata = pdev->dev.platform_data;
	int ret;

	dev_dbg(&pdev->dev, "%s: enter for node %p\n", __func__,
		pdev->dev.parent->of_node->name);

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->pdata = pdata;

	priv->dai_drv = stm32_adfsdm_dai;
	priv->dai_drv.name = pdev->dev.parent->of_node->name;
	priv->dai_drv.capture.stream_name = pdev->dev.parent->of_node->name;

	dev_set_drvdata(&pdev->dev, priv);

	ret = devm_snd_soc_register_component(&pdev->dev,
					      &stm32_adfsdm_dai_component,
					      &priv->dai_drv, 1);
	if (ret < 0)
		return ret;

	ret = devm_snd_dmaengine_pcm_register(pdev->dev.parent,
					      &dmaengine_pcm_config, 0);
	if (ret < 0)
		dev_err(&pdev->dev, "failed to register dma pcm config\n");

	return ret;
}

static struct platform_driver stm32_adfsdm_driver = {
	.driver = {
		   .name = STM32_ADFSDM_DRV_NAME,
		   },
	.probe = stm32_adfsdm_probe,
};

module_platform_driver(stm32_adfsdm_driver);

MODULE_DESCRIPTION("stm32 DFSDM DAI driver");
MODULE_AUTHOR("Arnaud Pouliquen <arnaud.pouliquen@st.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" STM32_ADFSDM_DRV_NAME);
