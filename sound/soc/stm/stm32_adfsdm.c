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

#include <linux/mfd/stm32-dfsdm.h>

/*
 * Set data output resolution to 23 bits max to keep 1 extra bit for sign,
 * as filter output is symmetric +/-2^(n-1).
 */
#define STM32_ADFSDM_DATA_RES BIT(23)
#define STM32_ADFSDM_MAX_RES BIT(31)
#define STM32_ADFSDM_DATAR_DATA_MASK	GENMASK(31, 8)

struct stm32_adfsdm_data {
	unsigned int rate;	/* SNDRV_PCM_RATE value */
	unsigned int freq;	/* frequency in Hz */
	unsigned int fosr;	/* filter over sampling ratio */
	unsigned int iosr;	/* integrator over sampling ratio */
	unsigned int fast;	/* filter fast mode */
	unsigned long res;	/* output data resolution */
	int shift;		/* shift on data output */
	bool h_res_found;	/* preferred resolution higher than expected */
};

static const struct stm32_adfsdm_data stm32_dfsdm_filter[] = {
	{ .rate = SNDRV_PCM_RATE_8000,  .freq = 8000 },
	{ .rate = SNDRV_PCM_RATE_16000, .freq = 16000 },
	{ .rate = SNDRV_PCM_RATE_32000, .freq = 32000 },
};

static const unsigned int stm32_dfsdm_sr_val[] = {
	8000,
	16000,
	32000,
};

struct stm32_adfsdm_priv {
	struct snd_soc_dai_driver dai;
	struct snd_dmaengine_dai_dma_data dma_data;
	struct snd_pcm_substream *substream;
	struct stm32_dfsdm_sinc_filter fl;
	struct stm32_dfsdm_channel channel;
	struct stm32_dfsdm_ch_cfg ch_cfg;
	struct stm32_dfsdm *dfsdm;
	struct stm32_adfsdm_data *f_param;
	struct device *dev;
	struct snd_pcm_hw_constraint_list rates_const;
	unsigned long dmic_clk;
	unsigned int input_id;
	unsigned int fl_id;
	unsigned int order; /* filter order */
	int synchro;
};

static const struct snd_pcm_hardware stm32_adfsdm_pcm_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
	    SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_MMAP |
	    SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE,

	.rate_min = 8000,
	.rate_max = 48000,

	.channels_min = 1,
	.channels_max = 1,

	.periods_min = 2,
	.periods_max = 48,

	.period_bytes_min = 40, /* 8 khz 5 ms */
	.period_bytes_max = 4 * PAGE_SIZE,
	.buffer_bytes_max = 16 * PAGE_SIZE
};

static inline void stm32_adfsdm_get_param(struct stm32_adfsdm_priv *priv,
					  unsigned int rate,
					  struct stm32_adfsdm_data **fparam)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(stm32_dfsdm_filter); i++) {
		if (rate == priv->f_param[i].freq) {
			*fparam = &priv->f_param[i];
			break;
		}
	}
}

static int stm32_adfsdm_compute_shift(struct stm32_adfsdm_priv *priv,
				      struct stm32_adfsdm_data *param)
{
	int shift = 0;
	u32 r = param->res;

	if (!r) {
		dev_err(priv->dev, "%s: resolution undefined\n", __func__);
		return -EINVAL;
	}

	/*
	 * If filter resolution is higher than data output resolution
	 * compute right shift required to match data resolution.
	 * Otherwise compute left shift to align MSB on data resolution.
	 */
	if (r >= STM32_ADFSDM_DATA_RES)
		while ((r >> -shift) >= STM32_ADFSDM_DATA_RES)
			shift--;
	else
		while ((r << shift) < STM32_ADFSDM_DATA_RES)
			shift++;

	param->shift = shift;
	dev_dbg(priv->dev, "%s: output shift: %d\n", __func__, shift);

	return 0;
}

static int stm32_adfsdm_get_best_osr(struct stm32_adfsdm_priv *priv,
				     unsigned int decim, bool fast,
				     struct stm32_adfsdm_data *param)
{
	unsigned int i, d, fosr, iosr;
	u64 res;
	s64 delta;
	unsigned int m = 1;	/* multiplication factor */
	unsigned int p = priv->order;	/* filter order (ford) */

	/*
	 * Decimation d depends on the filter order and the oversampling ratios.
	 * ford: filter order
	 * fosr: filter over sampling ratio
	 * iosr: integrator over sampling ratio
	 */
	dev_dbg(priv->dev, "%s: decim = %d fast = %d\n", __func__, decim, fast);
	if (priv->order == DFSDM_FASTSINC_ORDER) {
		m = 2;
		p = 2;
	}

	/*
	 * Looks for filter and integrator oversampling ratios which allow
	 * to reach 24 bits data output resolution.
	 * Leave at once if exact resolution if reached.
	 * Otherwise the higher resolution below 32 bits is kept.
	 */
	for (fosr = 1; fosr <= DFSDM_MAX_FL_OVERSAMPLING; fosr++) {
		for (iosr = 1; iosr <= DFSDM_MAX_INT_OVERSAMPLING; iosr++) {
			if (fast)
				d = fosr * iosr;
			else if (priv->order == DFSDM_FASTSINC_ORDER)
				d = fosr * (iosr + 3) + 2;
			else
				d = fosr * (iosr - 1 + p) + p;

			if (d > decim)
				break;
			else if (d != decim)
				continue;
			/*
			 * Check resolution (limited to signed 32 bits)
			 *   res <= 2^31
			 * Sincx filters:
			 *   res = m * fosr^p x iosr (with m=1, p=ford)
			 * FastSinc filter
			 *   res = m * fosr^p x iosr (with m=2, p=2)
			 */
			res = fosr;
			for (i = p - 1; i > 0; i--) {
				res = res * (u64)fosr;
				if (res > STM32_ADFSDM_MAX_RES)
					break;
			}
			if (res > STM32_ADFSDM_MAX_RES)
				continue;
			res = res * (u64)m * (u64)iosr;
			if (res > STM32_ADFSDM_MAX_RES)
				continue;

			delta = res - STM32_ADFSDM_DATA_RES;

			if (res >= param->res) {
				param->res = res;
				param->fosr = fosr;
				param->iosr = iosr;
				param->fast = fast;
			}

			if (!delta)
				return 0;
		}
	}

	if (!param->fosr)
		return -EINVAL;

	return 0;
}

static int stm32_adfsdm_get_supported_rates(struct stm32_adfsdm_priv *priv,
					    unsigned int *rates)
{
	unsigned long fs = priv->dmic_clk;
	unsigned int i, decim;
	int ret;

	*rates = 0;

	for (i = 0; i < ARRAY_SIZE(stm32_dfsdm_filter); i++) {
		/* check that clkout_freq is compatible */
		if ((fs % priv->f_param[i].freq) != 0)
			continue;

		decim = fs / priv->f_param[i].freq;

		/*
		 * Try to find one solution for filter and integrator
		 * oversampling ratio with fast mode ON or OFF.
		 * Fast mode on is the preferred solution.
		 */
		ret = stm32_adfsdm_get_best_osr(priv, decim, 0,
						&priv->f_param[i]);
		ret &= stm32_adfsdm_get_best_osr(priv, decim, 1,
						 &priv->f_param[i]);
		if (!ret) {
			ret = stm32_adfsdm_compute_shift(priv,
							 &priv->f_param[i]);
			if (ret)
				continue;

			*rates |= 1 << i;
			dev_dbg(priv->dev, "%s: %d rate supported\n", __func__,
				priv->f_param[i].freq);
		}
	}

	if (!*rates) {
		dev_err(priv->dev, "%s: no matched rate found\n", __func__);
		return -EINVAL;
	}

	return 0;
}

static void stm32_dfsdm_xrun(struct stm32_dfsdm *dfsdm, int flt_id,
			     enum stm32_dfsdm_events ev, unsigned int param,
			     void *context)
{
	struct stm32_adfsdm_priv *priv = context;

	snd_pcm_stream_lock(priv->substream);
	dev_err(priv->dev, "%s:unexpected underrun\n", __func__);
	/* Stop the player */
	stm32_dfsdm_unregister_fl_event(priv->dfsdm, priv->fl_id,
					DFSDM_EVENT_REG_XRUN, 0);
	snd_pcm_stop(priv->substream, SNDRV_PCM_STATE_XRUN);
	snd_pcm_stream_unlock(priv->substream);
}

static int stm32_adfsdm_copy(struct snd_pcm_substream *substream, int channel,
			     snd_pcm_uframes_t pos,
			     void __user *buf, snd_pcm_uframes_t count)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(rtd->cpu_dai);
	struct stm32_adfsdm_data *f_param;
	int *ptr = (int *)(runtime->dma_area + frames_to_bytes(runtime, pos));
	char *hwbuf = runtime->dma_area + frames_to_bytes(runtime, pos);
	ssize_t bytes = frames_to_bytes(runtime, count);
	ssize_t sample_cnt = bytes_to_samples(runtime, bytes);

	stm32_adfsdm_get_param(priv, runtime->rate, &f_param);

	/*
	 * Audio samples are available on 24 MSBs of the DFSDM DATAR register.
	 * We need to mask 8 LSB control bits...
	 * Additionnaly precision depends on decimation and can need shift
	 * to be aligned on 32-bit word MSB.
	 */
	if (f_param->shift > 0) {
		do {
			*ptr <<= f_param->shift & STM32_ADFSDM_DATAR_DATA_MASK;
			ptr++;
		} while (--sample_cnt);
	} else {
		do {
			*ptr &= STM32_ADFSDM_DATAR_DATA_MASK;
			ptr++;
		} while (--sample_cnt);
	}

	return copy_to_user(buf, hwbuf, bytes);
}

static int stm32_adfsdm_startup(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(dai);

	priv->substream = substream;

	/* Fix available rate depending on CLKOUT or CKIN value */
	return snd_pcm_hw_constraint_list(substream->runtime, 0,
					  SNDRV_PCM_HW_PARAM_RATE,
					  &priv->rates_const);
}

static void stm32_adfsdm_shutdown(struct snd_pcm_substream *substream,
				  struct snd_soc_dai *dai)
{
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(dai);

	priv->substream = NULL;
}

static int stm32_adfsdm_dai_hw_params(struct snd_pcm_substream *substream,
				      struct snd_pcm_hw_params *params,
				      struct snd_soc_dai *dai)
{
	struct snd_dmaengine_dai_dma_data *dma_data;

	dev_dbg(dai->dev, "%s: enter\n", __func__);
	dma_data = snd_soc_dai_get_dma_data(dai, substream);
	dma_data->maxburst = 1;

	return 0;
}

static int stm32_adfsdm_prepare(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(dai);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct stm32_adfsdm_data *f_param;
	struct stm32_dfsdm_filter filter;
	struct stm32_dfsdm_regular params;
	int ret;

	dev_dbg(dai->dev, "%s: enter\n", __func__);

	stm32_adfsdm_get_param(priv, runtime->rate, &f_param);

	memset(&filter, 0, sizeof(filter));
	memset(&params, 0, sizeof(params));

	params.ch_src = priv->channel.id;
	params.dma_mode = 1;
	params.cont_mode = 1;
	params.fast_mode = f_param->fast;
	params.sync_mode = priv->synchro ?
	    DFSDM_FILTER_RSYNC_ON : DFSDM_FILTER_RSYNC_OFF;
	filter.reg_params = &params;
	filter.sinc_params.order = priv->order;
	filter.sinc_params.oversampling = f_param->fosr;
	filter.int_oversampling = f_param->iosr;

	filter.event.cb = stm32_dfsdm_xrun;
	filter.event.context = priv;

	ret = stm32_dfsdm_configure_filter(priv->dfsdm, priv->fl_id, &filter);
	if (ret < 0)
		return ret;

	ret = stm32_dfsdm_register_fl_event(priv->dfsdm, priv->fl_id,
					    DFSDM_EVENT_REG_XRUN, 0);
	if (ret < 0)
		dev_err(priv->dev, "Failed to register xrun event\n");

	return ret;
}

static int stm32_adfsdm_start(struct snd_pcm_substream *substream,
			      struct snd_soc_dai *dai)
{
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(dai);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct stm32_adfsdm_data *f_param;
	int ret;

	dev_dbg(dai->dev, "%s: enter\n", __func__);

	stm32_adfsdm_get_param(priv, runtime->rate, &f_param);
	if (f_param->shift < 0)
		priv->ch_cfg.right_bit_shift = -f_param->shift;

	ret = stm32_dfsdm_start_channel(priv->dfsdm, priv->channel.id,
					&priv->ch_cfg);
	if (ret < 0)
		return ret;

	stm32_dfsdm_start_filter(priv->dfsdm, priv->fl_id,
				 DFSDM_FILTER_REG_CONV);

	return 0;
}

static void stm32_adfsdm_stop(struct snd_pcm_substream *substream,
			      struct snd_soc_dai *dai)
{
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(dai);

	dev_dbg(dai->dev, "%s: enter\n", __func__);

	stm32_dfsdm_unregister_fl_event(priv->dfsdm, priv->fl_id,
					DFSDM_EVENT_REG_XRUN, 0);
	stm32_dfsdm_stop_filter(priv->dfsdm, priv->fl_id);
	stm32_dfsdm_stop_channel(priv->dfsdm, priv->channel.id);
}

static int stm32_adfsdm_trigger(struct snd_pcm_substream *substream, int cmd,
				struct snd_soc_dai *dai)
{
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		return stm32_adfsdm_start(substream, dai);
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_STOP:
		stm32_adfsdm_stop(substream, dai);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int stm32_adfsdm_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(dai);
	unsigned int inv = fmt & SND_SOC_DAIFMT_INV_MASK;
	unsigned int cb = fmt & SND_SOC_DAIFMT_MASTER_MASK;
	int ret;

	dev_dbg(dai->dev, "%s: enter\n", __func__);

	/* DAI clock strobing */
	if ((inv == SND_SOC_DAIFMT_IB_NF) || (inv == SND_SOC_DAIFMT_IB_IF)) {
		priv->channel.serial_if.type = DFSDM_CHANNEL_SPI_FALLING;
		priv->channel.serial_if.pins = DFSDM_CHANNEL_NEXT_CHANNEL_PINS;
		/*
		 * if data on falling egde SPI connected to channel n - 1.
		 * if data on rising egde  SPI connected to channel n.
		 */
		if (priv->input_id)
			priv->channel.id = priv->input_id - 1;
		else
			priv->channel.id = priv->dfsdm->max_channels - 1;
	} else {
		priv->channel.serial_if.type = DFSDM_CHANNEL_SPI_RISING;
		priv->channel.serial_if.pins = DFSDM_CHANNEL_SAME_CHANNEL_PINS;
		priv->channel.id = priv->input_id;
	}

	dev_dbg(dai->dev, "%s: channel %d on input %d\n", __func__,
		priv->channel.id, priv->input_id);

	if ((cb == SND_SOC_DAIFMT_CBS_CFM) || (cb == SND_SOC_DAIFMT_CBS_CFS)) {
		/* Digital microphone is clocked by CLKOUT */
		stm32_dfsdm_get_clk_out_rate(priv->dfsdm, &priv->dmic_clk);
	} else {
		/* Digital microphone is clocked by external clock */
		if (!priv->dmic_clk) {
			dev_err(priv->dev,
				"system-clock-frequency not defined\n");
			return -EINVAL;
		}
	}

	priv->rates_const.count = ARRAY_SIZE(stm32_dfsdm_sr_val);
	priv->rates_const.list = stm32_dfsdm_sr_val;
	ret = stm32_adfsdm_get_supported_rates(priv, &priv->rates_const.mask);
	if (ret < 0)
		return ret;

	return stm32_dfsdm_get_channel(priv->dfsdm, &priv->channel);
}

static int stm32_adfsdm_set_sysclk(struct snd_soc_dai *dai, int clk_id,
				   unsigned int freq, int dir)
{
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(dai);

	dev_dbg(dai->dev, "%s: enter for dai %d\n", __func__, dai->id);
	if (dir == SND_SOC_CLOCK_IN)
		priv->dmic_clk = freq;

	return 0;
}

static const struct snd_soc_dai_ops stm32_adfsdm_dai_ops = {
	.startup = stm32_adfsdm_startup,
	.shutdown = stm32_adfsdm_shutdown,
	.hw_params = stm32_adfsdm_dai_hw_params,
	.set_fmt = stm32_adfsdm_set_dai_fmt,
	.set_sysclk = stm32_adfsdm_set_sysclk,
	.prepare = stm32_adfsdm_prepare,
	.trigger = stm32_adfsdm_trigger,
};

static int stm32_adfsdm_dai_probe(struct snd_soc_dai *dai)
{
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(dai);
	struct snd_dmaengine_dai_dma_data *dma = &priv->dma_data;
	int ret;

	dev_dbg(dai->dev, "%s: enter for dai %d\n", __func__, dai->id);

	/* filter settings */
	ret = stm32_dfsdm_get_filter(priv->dfsdm, priv->fl_id);
	if (ret < 0)
		return -EBUSY;

	/* DMA settings */
	snd_soc_dai_init_dma_data(dai, NULL, dma);
	dma->addr = stm32_dfsdm_get_filter_dma_phy_addr(priv->dfsdm,
							priv->fl_id,
							DFSDM_FILTER_REG_CONV);
	dma->addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;

	return 0;
}

static int stm32_adfsdm_dai_remove(struct snd_soc_dai *dai)
{
	struct stm32_adfsdm_priv *priv = snd_soc_dai_get_drvdata(dai);

	dev_dbg(dai->dev, "%s: enter for dai %d\n", __func__, dai->id);

	stm32_dfsdm_release_filter(priv->dfsdm, priv->fl_id);
	stm32_dfsdm_release_channel(priv->dfsdm, priv->channel.id);

	return 0;
}

static const struct snd_soc_dai_driver stm32_adfsdm_dai = {
	.capture = {
		    .channels_min = 1,
		    .channels_max = 1,
		    .formats = SNDRV_PCM_FMTBIT_S24_LE |
			       SNDRV_PCM_FMTBIT_S32_LE,
		    .rates = SNDRV_PCM_RATE_8000_48000,
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
	struct device_node *np = pdev->dev.of_node;
	char *_name;
	int ret;

	dev_dbg(&pdev->dev, "%s: enter for node %s\n", __func__,
		np->name);

	if (!np) {
		dev_err(&pdev->dev, "No DT found\n");
		return -EINVAL;
	}

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;
	priv->dfsdm = dev_get_drvdata(pdev->dev.parent);

	if (of_property_read_u32(np, "reg", &priv->fl_id)) {
		dev_err(&pdev->dev, "missing reg property\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "st,dai-filter-order", &priv->order);
	if (ret < 0) {
		dev_warn(&pdev->dev, "Default filter order selected\n");
		priv->order = DFSDM_SINC5_ORDER;
	}

	ret = of_property_read_u32(np, "st,input-id", &priv->input_id);
	if (ret < 0) {
		dev_err(&pdev->dev, "st,input-id property missing\n");
		return ret;
	}

	ret = of_property_read_u32(np, "st,dai0-synchronized", &priv->synchro);
	if (ret < 0)
		/* default case if property not defined */
		priv->synchro = 0;

	priv->channel.type.DataPacking = DFSDM_CHANNEL_STANDARD_MODE;
	priv->channel.type.source = DFSDM_CHANNEL_EXTERNAL_INPUTS;
	priv->channel.serial_if.spi_clk = DFSDM_CHANNEL_SPI_CLOCK_INTERNAL;

	/* DAI settings */
	_name = devm_kzalloc(&pdev->dev, sizeof("dfsdm_pdm_0"), GFP_KERNEL);
	if (!_name)
		return -ENOMEM;

	priv->dai = stm32_adfsdm_dai;

	priv->f_param = devm_kcalloc(&pdev->dev,
				     ARRAY_SIZE(stm32_dfsdm_filter),
				     sizeof(stm32_dfsdm_filter[0]), GFP_KERNEL);
	if (!priv->f_param)
		return -ENOMEM;

	memcpy(priv->f_param, stm32_dfsdm_filter,
	       ARRAY_SIZE(stm32_dfsdm_filter) * sizeof(stm32_dfsdm_filter[0]));

	snprintf(_name, sizeof("dfsdm_pdm_0"), "dfsdm_pdm_%i", priv->fl_id);
	priv->dai.name = _name;
	priv->dai.capture.stream_name = _name;

	dev_set_drvdata(&pdev->dev, priv);

	ret = devm_snd_soc_register_component(&pdev->dev,
					      &stm32_adfsdm_dai_component,
					      &priv->dai, 1);
	if (ret < 0)
		return ret;

	ret = devm_snd_dmaengine_pcm_register(&pdev->dev,
					      &dmaengine_pcm_config, 0);
	if (ret < 0)
		dev_err(&pdev->dev, "failed to register dma pcm config\n");

	return ret;
}

static const struct of_device_id snd_soc_dfsdm_match[] = {
	{.compatible = "st,stm32-dfsdm-audio"},
	{},
};

static struct platform_driver stm32_adfsdm_driver = {
	.driver = {
		   .name = "stm32-dfsdm-audio",
		   .of_match_table = snd_soc_dfsdm_match,
		   },
	.probe = stm32_adfsdm_probe,
};

module_platform_driver(stm32_adfsdm_driver);

MODULE_DESCRIPTION("stm32 DFSDM DAI driver");
MODULE_AUTHOR("Arnaud Pouliquen <arnaud.pouliquen@st.com>");
MODULE_LICENSE("GPL v2");
