/*
 * Mediatek ALSA SoC AFE platform driver for 2701
 *
 * Copyright (c) 2016 MediaTek Inc.
 * Author: Garlic Tseng <garlic.tseng@mediatek.com>
 *             Koro Chen <koro.chen@mediatek.com>
 *             Hidalgo Huang <hidalgo.huang@mediatek.com>
 *             Ir Lian <ir.lian@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pm_runtime.h>
#include <sound/soc.h>

#include "mt2701-afe-common.h"

#include "mt2701-afe-clock-ctrl.h"
#include "mt2701-irq.h"

#define AFE_BASE_END_OFFSET	8
#define AFE_IRQ_STATUS_BITS	0xff

static const struct snd_pcm_hardware mt2701_afe_hardware = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED
		| SNDRV_PCM_INFO_RESUME | SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE
		   | SNDRV_PCM_FMTBIT_S32_LE,
	.period_bytes_min = 1024,
	.period_bytes_max = 1024 * 256,
	.periods_min = 4,
	.periods_max = 1024,
	.buffer_bytes_max = 1024 * 1024 * 16,
	.fifo_size = 0,
};

static snd_pcm_uframes_t mt2701_afe_pcm_pointer
			 (struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt2701_afe *afe = snd_soc_platform_get_drvdata(rtd->platform);
	int stream_dir = substream->stream;
	int cpu_dai_id = rtd->cpu_dai->id;
	struct mt2701_afe_memif *memif = &afe->memif[cpu_dai_id][stream_dir];
	unsigned int hw_ptr;
	int ret;

	ret = regmap_read(afe->regmap, memif->data->reg_ofs_cur, &hw_ptr);
	if (ret || hw_ptr == 0) {
		dev_err(afe->dev, "%s hw_ptr err\n", __func__);
		hw_ptr = memif->phys_buf_addr;
	}

	return bytes_to_frames(substream->runtime,
			       hw_ptr - memif->phys_buf_addr);
}

static const struct snd_pcm_ops mt2701_afe_pcm_ops = {
	.ioctl = snd_pcm_lib_ioctl,
	.pointer = mt2701_afe_pcm_pointer,
};

static int mt2701_afe_pcm_new(struct snd_soc_pcm_runtime *rtd)
{
	size_t size;
	struct snd_card *card = rtd->card->snd_card;
	struct snd_pcm *pcm = rtd->pcm;

	size = mt2701_afe_hardware.buffer_bytes_max;
	return snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV,
						     card->dev, size, size);
}

static void mt2701_afe_pcm_free(struct snd_pcm *pcm)
{
	snd_pcm_lib_preallocate_free_for_all(pcm);
}

static const struct snd_soc_platform_driver mt2701_afe_pcm_platform = {
	.ops = &mt2701_afe_pcm_ops,
	.pcm_new = mt2701_afe_pcm_new,
	.pcm_free = mt2701_afe_pcm_free,
};

struct mt2701_afe_rate {
	unsigned int rate;
	unsigned int regvalue;
};

static const struct mt2701_afe_rate mt2701_afe_i2s_rates[] = {
	{ .rate = 8000, .regvalue = 0 },
	{ .rate = 12000, .regvalue = 1 },
	{ .rate = 16000, .regvalue = 2 },
	{ .rate = 24000, .regvalue = 3 },
	{ .rate = 32000, .regvalue = 4 },
	{ .rate = 48000, .regvalue = 5 },
	{ .rate = 96000, .regvalue = 6 },
	{ .rate = 192000, .regvalue = 7 },
	{ .rate = 384000, .regvalue = 8 },
	{ .rate = 7350, .regvalue = 16 },
	{ .rate = 11025, .regvalue = 17 },
	{ .rate = 14700, .regvalue = 18 },
	{ .rate = 22050, .regvalue = 19 },
	{ .rate = 29400, .regvalue = 20 },
	{ .rate = 44100, .regvalue = 21 },
	{ .rate = 88200, .regvalue = 22 },
	{ .rate = 176400, .regvalue = 23 },
	{ .rate = 352800, .regvalue = 24 },
};

int mt2701_dai_num_to_i2s(struct mt2701_afe *afe, int num)
{
	int val = num - MT2701_IO_I2S;

	if (val < 0 || val > MT2701_I2S_NUM) {
		dev_err(afe->dev, "%s, num not available, num %d, val %d\n",
			__func__, num, val);
		return -1;
	}
	return val;
}

static int mt2701_afe_i2s_fs(unsigned int sample_rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mt2701_afe_i2s_rates); i++)
		if (mt2701_afe_i2s_rates[i].rate == sample_rate)
			return mt2701_afe_i2s_rates[i].regvalue;

	return -EINVAL;
}

static int mt2701_afe_i2s_startup(struct snd_pcm_substream *substream,
				  struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt2701_afe *afe = snd_soc_platform_get_drvdata(rtd->platform);
	int i2s_num = mt2701_dai_num_to_i2s(afe, dai->id);
	int clk_num = MT2701_AUD_TOP_AUD_I2S1_MCLK + i2s_num;
	int ret = 0;

	/*enable mclk*/
	ret = clk_prepare_enable(afe->clocks[clk_num]);
	if (ret)
		dev_err(afe->dev, "Failed to enable mclk for I2S: %d\n",
			i2s_num);

	return ret;
}

static int mt2701_afe_i2s_path_shutdown(struct snd_pcm_substream *substream,
					struct snd_soc_dai *dai,
					int dir_invert)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt2701_afe *afe = snd_soc_platform_get_drvdata(rtd->platform);
	int i2s_num = mt2701_dai_num_to_i2s(afe, dai->id);
	struct mt2701_i2s_path *i2s_path = &afe->i2s_path[i2s_num];
	const struct mt2701_i2s_data *i2s_data;
	int stream_dir = substream->stream;

	if (dir_invert)	{
		if (stream_dir == SNDRV_PCM_STREAM_PLAYBACK)
			stream_dir = SNDRV_PCM_STREAM_CAPTURE;
		else
			stream_dir = SNDRV_PCM_STREAM_PLAYBACK;
	}
	i2s_data = i2s_path->i2s_data[stream_dir];

	i2s_path->on[stream_dir]--;
	if (i2s_path->on[stream_dir] < 0) {
		dev_warn(afe->dev, "i2s_path->on: %d, dir: %d\n",
			 i2s_path->on[stream_dir], stream_dir);
		i2s_path->on[stream_dir] = 0;
	}
	if (i2s_path->on[stream_dir])
		return 0;

	/*disable i2s*/
	regmap_update_bits(afe->regmap, i2s_data->i2s_ctrl_reg,
			   ASYS_I2S_CON_I2S_EN, 0);
	regmap_update_bits(afe->regmap, AUDIO_TOP_CON4,
			   1 << i2s_data->i2s_pwn_shift,
			   1 << i2s_data->i2s_pwn_shift);
	return 0;
}

static void mt2701_afe_i2s_shutdown(struct snd_pcm_substream *substream,
				    struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt2701_afe *afe = snd_soc_platform_get_drvdata(rtd->platform);
	int i2s_num = mt2701_dai_num_to_i2s(afe, dai->id);
	struct mt2701_i2s_path *i2s_path = &afe->i2s_path[i2s_num];
	int clk_num = MT2701_AUD_TOP_AUD_I2S1_MCLK + i2s_num;

	if (i2s_path->occupied[substream->stream])
		i2s_path->occupied[substream->stream] = 0;
	else
		goto I2S_UNSTART;

	mt2701_afe_i2s_path_shutdown(substream, dai, 0);

	/*need to disable i2s-out path when disable i2s-in*/
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		mt2701_afe_i2s_path_shutdown(substream, dai, 1);

I2S_UNSTART:
	/*disable mclk*/
	clk_disable_unprepare(afe->clocks[clk_num]);
}

static int mt2701_i2s_path_prepare_enable(struct snd_pcm_substream *substream,
					  struct snd_soc_dai *dai,
					  int dir_invert)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt2701_afe *afe = snd_soc_platform_get_drvdata(rtd->platform);
	int i2s_num = mt2701_dai_num_to_i2s(afe, dai->id);
	struct mt2701_i2s_path *i2s_path = &afe->i2s_path[i2s_num];
	const struct mt2701_i2s_data *i2s_data;
	struct snd_pcm_runtime * const runtime = substream->runtime;
	int reg, fs, w_len = 1;
	int stream_dir = substream->stream;
	unsigned int mask = 0, val = 0;

	if (dir_invert) {
		if (stream_dir == SNDRV_PCM_STREAM_PLAYBACK)
			stream_dir = SNDRV_PCM_STREAM_CAPTURE;
		else
			stream_dir = SNDRV_PCM_STREAM_PLAYBACK;
	}
	i2s_data = i2s_path->i2s_data[stream_dir];

	/*no need to enable if already done*/
	i2s_path->on[stream_dir]++;

	if (i2s_path->on[stream_dir] != 1)
		return 0;

	fs = mt2701_afe_i2s_fs(runtime->rate);

	if (i2s_path->div_bck_to_lrck == 32)
		w_len = 0;
	else if (i2s_path->div_bck_to_lrck == 64)
		w_len = 1;
	else
		dev_warn(dai->dev, "%s() bad bit count %d\n", __func__,
			 afe->i2s_path[i2s_num].div_bck_to_lrck);

	mask = ASYS_I2S_CON_FS |
		ASYS_I2S_CON_MULTI_CH | /*0*/
		ASYS_I2S_CON_I2S_COUPLE_MODE | /*0*/
		ASYS_I2S_CON_I2S_MODE |
		ASYS_I2S_CON_WIDE_MODE;

	val = ASYS_I2S_CON_FS_SET(fs) |
	       ASYS_I2S_CON_I2S_MODE |
	       ASYS_I2S_CON_WIDE_MODE_SET(w_len);

	if (stream_dir == SNDRV_PCM_STREAM_CAPTURE) {
		mask |= ASYS_I2S_IN_PHASE_FIX;
		val |= ASYS_I2S_IN_PHASE_FIX;
	}

	regmap_update_bits(afe->regmap, i2s_data->i2s_ctrl_reg, mask, val);

	if (stream_dir == SNDRV_PCM_STREAM_PLAYBACK)
		reg = ASMO_TIMING_CON1;
	else
		reg = ASMI_TIMING_CON1;

	regmap_update_bits(afe->regmap, reg,
		      i2s_data->i2s_asrc_fs_mask << i2s_data->i2s_asrc_fs_shift,
		      fs << i2s_data->i2s_asrc_fs_shift);

	/*enable i2s*/
	regmap_update_bits(afe->regmap, AUDIO_TOP_CON4,
			   1 << i2s_data->i2s_pwn_shift,
			   0 << i2s_data->i2s_pwn_shift);

	/*reset irq hw status before enable*/
	regmap_update_bits(afe->regmap, i2s_data->i2s_ctrl_reg,
			   ASYS_I2S_CON_RESET, ASYS_I2S_CON_RESET);
	udelay(1);
	regmap_update_bits(afe->regmap, i2s_data->i2s_ctrl_reg,
			   ASYS_I2S_CON_RESET, 0);
	udelay(1);
	regmap_update_bits(afe->regmap, i2s_data->i2s_ctrl_reg,
			   ASYS_I2S_CON_I2S_EN, ASYS_I2S_CON_I2S_EN);
	return 0;
}

static int mt2701_afe_i2s_prepare(struct snd_pcm_substream *substream,
				  struct snd_soc_dai *dai)
{
	int clk_domain;

	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt2701_afe *afe = snd_soc_platform_get_drvdata(rtd->platform);
	int i2s_num = mt2701_dai_num_to_i2s(afe, dai->id);
	struct mt2701_i2s_path *i2s_path = &afe->i2s_path[i2s_num];
	const int mclk_rate = i2s_path->mclk_rate;

	if (i2s_path->occupied[substream->stream])
		return -EBUSY;
	i2s_path->occupied[substream->stream] = 1;

	if (MT2701_PLL_DOMAIN_0_RATE % mclk_rate == 0) {
		clk_domain = 0;
	} else if (MT2701_PLL_DOMAIN_1_RATE % mclk_rate == 0) {
		clk_domain = 1;
	} else {
		dev_err(dai->dev, "%s() bad mclk rate %d\n",
			__func__, mclk_rate);
		return -EINVAL;
	}
	mt2701_mclk_configuration(afe, i2s_num, clk_domain, mclk_rate);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		mt2701_i2s_path_prepare_enable(substream, dai, 0);
	} else {
		/*need to enable i2s-out path when enable i2s-in*/
		/*prepare for another direction "out"*/
		mt2701_i2s_path_prepare_enable(substream, dai, 1);
		/*prepare for "in"*/
		mt2701_i2s_path_prepare_enable(substream, dai, 0);
	}

	return 0;
}

static int mt2701_afe_i2s_set_sysclk(struct snd_soc_dai *dai, int clk_id,
				     unsigned int freq, int dir)
{
	struct mt2701_afe *afe = dev_get_drvdata(dai->dev);
	int i2s_num = mt2701_dai_num_to_i2s(afe, dai->id);
	/* mclk */
	if (dir == SND_SOC_CLOCK_IN) {
		dev_warn(dai->dev,
			 "%s() warning: mt2701 doesn't support mclk input\n",
			__func__);
		return -EINVAL;
	}
	afe->i2s_path[i2s_num].mclk_rate = freq;
	return 0;
}

static int mt2701_afe_i2s_set_clkdiv(struct snd_soc_dai *dai, int div_id,
				     int div)
{
	struct mt2701_afe *afe = dev_get_drvdata(dai->dev);
	int i2s_num = mt2701_dai_num_to_i2s(afe, dai->id);

	switch (div_id) {
	case DIV_ID_MCLK_TO_BCK:
		afe->i2s_path[i2s_num].div_mclk_to_bck = div;
		break;
	case DIV_ID_BCK_TO_LRCK:
		afe->i2s_path[i2s_num].div_bck_to_lrck = div;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int mt2701_afe_i2s_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct mt2701_afe *afe = dev_get_drvdata(dai->dev);
	int i2s_num = mt2701_dai_num_to_i2s(afe, dai->id);

	afe = dev_get_drvdata(dai->dev);
	afe->i2s_path[i2s_num].format = fmt;
	return 0;
}

static int mt2701_btmrg_startup(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt2701_afe *afe = snd_soc_platform_get_drvdata(rtd->platform);

	regmap_update_bits(afe->regmap, AUDIO_TOP_CON4,
			   AUDIO_TOP_CON4_PDN_MRGIF, 0);

	afe->mrg_enable[substream->stream] = 1;
	return 0;
}

static int mt2701_btmrg_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params,
				  struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt2701_afe *afe = snd_soc_platform_get_drvdata(rtd->platform);
	int stream_fs;
	u32 val, msk;

	pr_debug("%s() cpu_dai id %d\n", __func__, dai->id);
	stream_fs = params_rate(params);

	if ((stream_fs != 8000) && (stream_fs != 16000)) {
		pr_err("%s() btmgr not supprt this stream_fs %d\n",
		       __func__, stream_fs);
		return -EINVAL;
	}

	regmap_update_bits(afe->regmap,
			   AFE_MRGIF_CON,
			   AFE_MRGIF_CON_I2S_MODE_MASK,
			   AFE_MRGIF_CON_I2S_MODE_32K);

	val = AFE_DAIBT_CON0_BT_FUNC_EN | AFE_DAIBT_CON0_BT_FUNC_RDY
	      | AFE_DAIBT_CON0_MRG_USE;
	msk = val;

	if (stream_fs == 16000)
		val |= AFE_DAIBT_CON0_BT_WIDE_MODE_EN;

	msk |= AFE_DAIBT_CON0_BT_WIDE_MODE_EN;

	regmap_update_bits(afe->regmap, AFE_DAIBT_CON0, msk, val);

	regmap_write(afe->regmap, AFE_BT_SECURITY0, AFE_BT_SECURITY0_INIT_VAL);
	regmap_write(afe->regmap, AFE_BT_SECURITY1, AFE_BT_SECURITY1_INIT_VAL);
	regmap_update_bits(afe->regmap, AFE_DAIBT_CON0, AFE_DAIBT_CON0_DAIBT_EN,
			   AFE_DAIBT_CON0_DAIBT_EN);
	regmap_update_bits(afe->regmap, AFE_MRGIF_CON, AFE_MRGIF_CON_MRG_I2S_EN,
			   AFE_MRGIF_CON_MRG_I2S_EN);
	regmap_update_bits(afe->regmap, AFE_MRGIF_CON, AFE_MRGIF_CON_MRG_EN,
			   AFE_MRGIF_CON_MRG_EN);
	return 0;
}

static void mt2701_btmrg_shutdown(struct snd_pcm_substream *substream,
				  struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt2701_afe *afe = snd_soc_platform_get_drvdata(rtd->platform);

	pr_debug("%s() cpu_dai id %d\n", __func__, dai->id);
	/* if the other direction stream is not occupied */
	if (!afe->mrg_enable[!substream->stream]) {
		regmap_update_bits(afe->regmap, AFE_DAIBT_CON0,
				   AFE_DAIBT_CON0_DAIBT_EN, 0);
		regmap_update_bits(afe->regmap, AFE_MRGIF_CON,
				   AFE_MRGIF_CON_MRG_EN, 0);
		regmap_update_bits(afe->regmap, AFE_MRGIF_CON,
				   AFE_MRGIF_CON_MRG_I2S_EN, 0);
		regmap_update_bits(afe->regmap, AUDIO_TOP_CON4,
				   AUDIO_TOP_CON4_PDN_MRGIF,
				   AUDIO_TOP_CON4_PDN_MRGIF);
	}
	afe->mrg_enable[substream->stream] = 0;
}

static int mt2701_playback_mem_avail(struct mt2701_afe *afe, int memif_num)
{
	struct mt2701_afe_memif *memif_tmp;

	if (memif_num >= MT2701_MEMIF_1 &&
	    memif_num < MT2701_MEMIF_SINGLE_NUM) {
		memif_tmp =
		    &afe->memif[MT2701_MEMIF_M][SNDRV_PCM_STREAM_PLAYBACK];
		if (memif_tmp->substream)
			return 0;
	} else if (memif_num == MT2701_MEMIF_M) {
		int i;

		for (i = MT2701_MEMIF_1; i < MT2701_MEMIF_SINGLE_NUM; ++i) {
			memif_tmp = &afe->memif[i][SNDRV_PCM_STREAM_PLAYBACK];
			if (memif_tmp->substream)
				return 0;
		}
	}
	return 1;
}

static int mt2701_afe_dais_startup(struct snd_pcm_substream *substream,
				   struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt2701_afe *afe = snd_soc_platform_get_drvdata(rtd->platform);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int stream_dir = substream->stream;
	int memif_num = rtd->cpu_dai->id;
	struct mt2701_afe_memif *memif = &afe->memif[memif_num][stream_dir];
	int is_dlm = 0;
	int ret, i;
	struct mt2701_afe_memif *memif_tmp;

	if (memif->substream) {
		dev_warn(afe->dev, "%s memif is occupied, stream_dir %d, memif_num = %d\n",
			 __func__, stream_dir, memif_num);
		return -EBUSY;
	}

	if (stream_dir == SNDRV_PCM_STREAM_PLAYBACK &&
	    !mt2701_playback_mem_avail(afe, memif_num)) {
		dev_warn(afe->dev, "%s memif is not available, stream_dir %d, memif_num %d\n",
			 __func__, stream_dir, memif_num);
		return -EBUSY;
	}

	if (memif_num == MT2701_MEMIF_M &&
	    stream_dir == SNDRV_PCM_STREAM_PLAYBACK)
		is_dlm = 1;

	memif->substream = substream;

	snd_pcm_hw_constraint_step(substream->runtime, 0,
				   SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 16);
	/*enable agent*/
	regmap_update_bits(afe->regmap, AUDIO_TOP_CON5,
			   1 << memif->data->agent_disable_shift,
			   0 << memif->data->agent_disable_shift);
	if (is_dlm) {
		for (i = MT2701_MEMIF_1; i < MT2701_MEMIF_SINGLE_NUM; ++i) {
			memif_tmp = &afe->memif[i][SNDRV_PCM_STREAM_PLAYBACK];
			regmap_update_bits(afe->regmap, AUDIO_TOP_CON5,
				     1 << memif_tmp->data->agent_disable_shift,
				     0 << memif_tmp->data->agent_disable_shift);
		}
	}

	snd_soc_set_runtime_hwparams(substream, &mt2701_afe_hardware);

	/*
	 * Capture cannot use ping-pong buffer since hw_ptr at IRQ may be
	 * smaller than period_size due to AFE's internal buffer.
	 * This easily leads to overrun when avail_min is period_size.
	 * One more period can hold the possible unread buffer.
	 */
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		ret = snd_pcm_hw_constraint_minmax(runtime,
						   SNDRV_PCM_HW_PARAM_PERIODS,
						   3,
					      mt2701_afe_hardware.periods_max);
		if (ret < 0) {
			dev_err(afe->dev, "hw_constraint_minmax failed\n");
			return ret;
		}
	}

	ret = snd_pcm_hw_constraint_integer(runtime,
					    SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		dev_err(afe->dev, "snd_pcm_hw_constraint_integer failed\n");

	/*require irq resource*/
	if (!memif->irq) {
		int irq_id = mt2701_asys_irq_acquire(afe);

		if (irq_id != MT2701_IRQ_ASYS_END) {
			/* link */
			memif->irq = &afe->irqs[irq_id];
			afe->irqs[irq_id].memif = memif;
			afe->irqs[irq_id].isr = mt2701_memif_isr;
		} else {
			dev_err(afe->dev, "%s() error: no more asys irq\n",
				__func__);
			ret = -EBUSY;
		}
	}
	return ret;
}

static void mt2701_afe_dais_shutdown(struct snd_pcm_substream *substream,
				     struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt2701_afe *afe = snd_soc_platform_get_drvdata(rtd->platform);
	int stream_dir = substream->stream;
	struct mt2701_afe_memif *memif =
		&afe->memif[rtd->cpu_dai->id][stream_dir];
	int irq_id, i;
	int is_dlm = 0;
	struct mt2701_afe_memif *memif_tmp;

	irq_id = memif->irq->irq_data->irq_id;
	if (rtd->cpu_dai->id == MT2701_MEMIF_M &&
	    stream_dir == SNDRV_PCM_STREAM_PLAYBACK)
		is_dlm = 1;

	regmap_update_bits(afe->regmap, AUDIO_TOP_CON5,
			   1 << memif->data->agent_disable_shift,
			   1 << memif->data->agent_disable_shift);
	if (is_dlm) {
		for (i = MT2701_MEMIF_1; i < MT2701_MEMIF_SINGLE_NUM; ++i) {
			memif_tmp = &afe->memif[i][SNDRV_PCM_STREAM_PLAYBACK];
			regmap_update_bits(afe->regmap, AUDIO_TOP_CON5,
				     1 << memif_tmp->data->agent_disable_shift,
				     1 << memif_tmp->data->agent_disable_shift);
		}
	}
	mt2701_asys_irq_release(afe, irq_id);
	memif->irq = NULL;
	afe->irqs[irq_id].memif = NULL;
	afe->irqs[irq_id].isr = NULL;
	memif->substream = NULL;
}

static int mt2701_afe_dais_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params,
				      struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt2701_afe *afe = snd_soc_platform_get_drvdata(rtd->platform);
	int stream_dir = substream->stream;
	struct mt2701_afe_memif *memif =
		&afe->memif[rtd->cpu_dai->id][stream_dir];
	int ret, fs, is_dlm = 0;

	ret = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(params));
	if (ret < 0)
		return ret;

	memif->phys_buf_addr = substream->runtime->dma_addr;
	memif->buffer_size = substream->runtime->dma_bytes;

	/* set rate */
	if (memif->data->fs_shift < 0)
		return 0;
	if (rtd->cpu_dai->id != MT2701_MEMIF_BT ||
	    stream_dir != SNDRV_PCM_STREAM_CAPTURE)
		fs = mt2701_afe_i2s_fs(params_rate(params));
	else
		fs = (params_rate(params) == 16000) ? 1 : 0;

	if (fs < 0)
		return -EINVAL;

	regmap_update_bits(afe->regmap, memif->data->fs_reg,
			   0x1f << memif->data->fs_shift,
			   fs << memif->data->fs_shift);
	/* set channel */
	if (memif->data->mono_shift >= 0) {
		unsigned int mono = (params_channels(params) == 1) ? 1 : 0;

		regmap_update_bits(afe->regmap, memif->data->mono_reg,
				   1 << memif->data->mono_shift,
				   mono << memif->data->mono_shift);
	}
	/* start */
	regmap_write(afe->regmap,
		     memif->data->reg_ofs_base, memif->phys_buf_addr);
	/* end */
	regmap_write(afe->regmap,
		     memif->data->reg_ofs_base + AFE_BASE_END_OFFSET,
		     memif->phys_buf_addr + memif->buffer_size - 1);

	if (rtd->cpu_dai->id == MT2701_MEMIF_M &&
	    stream_dir == SNDRV_PCM_STREAM_PLAYBACK)
		is_dlm = 1;

	if (is_dlm) { /*setting for multi-ch playback*/
		int channels = params_channels(params);

		regmap_update_bits(afe->regmap,
				   AFE_MEMIF_PBUF_SIZE,
				   AFE_MEMIF_PBUF_SIZE_DLM_MASK,
				   AFE_MEMIF_PBUF_SIZE_FULL_INTERLEAVE);
		regmap_update_bits(afe->regmap,
				   AFE_MEMIF_PBUF_SIZE,
				   AFE_MEMIF_PBUF_SIZE_DLM_BYTE_MASK,
				   AFE_MEMIF_PBUF_SIZE_DLM_32BYTES);
		regmap_update_bits(afe->regmap,
				   AFE_MEMIF_PBUF_SIZE,
				   AFE_MEMIF_PBUF_SIZE_DLM_CH_MASK,
				   AFE_MEMIF_PBUF_SIZE_DLM_CH(channels));
	} else if (stream_dir == SNDRV_PCM_STREAM_PLAYBACK) {
		regmap_update_bits(afe->regmap,
				   AFE_MEMIF_PBUF_SIZE,
				   AFE_MEMIF_PBUF_SIZE_DLM_MASK,
				   AFE_MEMIF_PBUF_SIZE_PAIR_INTERLEAVE);
	}

	return 0;
}

static int mt2701_afe_dais_hw_free(struct snd_pcm_substream *substream,
				   struct snd_soc_dai *dai)
{
	return snd_pcm_lib_free_pages(substream);
}

static int mt2701_afe_dais_prepare(struct snd_pcm_substream *substream,
				   struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd  = substream->private_data;
	struct mt2701_afe *afe = snd_soc_platform_get_drvdata(rtd->platform);
	int stream_dir = substream->stream;
	struct mt2701_afe_memif *memif =
		&afe->memif[rtd->cpu_dai->id][stream_dir];
	int hd_audio = 0;

	/*set hd mode*/
	switch (substream->runtime->format) {
	case SNDRV_PCM_FORMAT_S16_LE:
		hd_audio = 0;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		hd_audio = 1;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		hd_audio = 1;
		break;
	default:
		dev_err(afe->dev, "%s() error: unsupported format %d\n",
			__func__, substream->runtime->format);
		break;
	}

	regmap_update_bits(afe->regmap, memif->data->hd_reg,
			   1 << memif->data->hd_shift,
			   hd_audio << memif->data->hd_shift);

	return 0;
}

static int mt2701_afe_dais_trigger(struct snd_pcm_substream *substream,
				   int cmd, struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt2701_afe *afe = snd_soc_platform_get_drvdata(rtd->platform);
	int stream_dir = substream->stream;
	struct mt2701_afe_memif *memif =
		&afe->memif[rtd->cpu_dai->id][stream_dir];
	struct mt2701_afe_memif *memif_tmp;
	struct snd_pcm_runtime * const runtime = substream->runtime;
	unsigned int counter = runtime->period_size;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		/*memory interface enable*/
		if (memif->data->enable_shift >= 0)
			regmap_update_bits(afe->regmap, AFE_DAC_CON0,
					   1 << memif->data->enable_shift,
					   1 << memif->data->enable_shift);

		/* set irq counter */
		regmap_update_bits(afe->regmap,
			      memif->irq->irq_data->irq_cnt_reg,
			      memif->irq->irq_data->irq_cnt_maskbit
			      << memif->irq->irq_data->irq_cnt_shift,
			      counter << (memif->irq->irq_data->irq_cnt_shift));
		/* set irq fs */
		if (memif->irq->irq_data->irq_fs_shift >= 0) {
			int fs;

			fs = mt2701_afe_i2s_fs(runtime->rate);
			if (fs < 0)
				return -EINVAL;

			regmap_update_bits(afe->regmap,
				      memif->irq->irq_data->irq_fs_reg,
				      memif->irq->irq_data->irq_fs_maskbit
				      << memif->irq->irq_data->irq_fs_shift,
				      fs << memif->irq->irq_data->irq_fs_shift);
		}

		if (rtd->cpu_dai->id == MT2701_MEMIF_M &&
		    stream_dir == SNDRV_PCM_STREAM_PLAYBACK) {
			memif_tmp = &afe->memif[MT2701_MEMIF_1][stream_dir];
			regmap_update_bits(afe->regmap, AFE_DAC_CON0,
					   1 << memif_tmp->data->enable_shift,
					   1 << memif_tmp->data->enable_shift);
		}
		/* enable interrupt */
		regmap_update_bits(afe->regmap,
				   memif->irq->irq_data->irq_en_reg,
				   1 << memif->irq->irq_data->irq_en_shift,
				   1 << memif->irq->irq_data->irq_en_shift);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		/* disable interrupt */
		regmap_update_bits(afe->regmap,
				   memif->irq->irq_data->irq_en_reg,
				   1 << memif->irq->irq_data->irq_en_shift,
				   0 << memif->irq->irq_data->irq_en_shift);
		/*memory interface disable*/
		if (memif->data->enable_shift >= 0)
			regmap_update_bits(afe->regmap, AFE_DAC_CON0,
					   1 << memif->data->enable_shift, 0);
		if (rtd->cpu_dai->id == MT2701_MEMIF_M &&
		    stream_dir == SNDRV_PCM_STREAM_PLAYBACK) {
			memif_tmp = &afe->memif[MT2701_MEMIF_1][stream_dir];
			regmap_update_bits(afe->regmap, AFE_DAC_CON0,
					   1 << memif_tmp->data->enable_shift,
					   0);
		}
		return 0;
	default:
		return -EINVAL;
	}
}

/* FE DAIs */
static const struct snd_soc_dai_ops mt2701_afe_dai_ops = {
	.startup	= mt2701_afe_dais_startup,
	.shutdown	= mt2701_afe_dais_shutdown,
	.hw_params	= mt2701_afe_dais_hw_params,
	.hw_free	= mt2701_afe_dais_hw_free,
	.prepare	= mt2701_afe_dais_prepare,
	.trigger	= mt2701_afe_dais_trigger,
};

/* I2S BE DAIs */
static const struct snd_soc_dai_ops mt2701_afe_i2s_ops = {
	.startup	= mt2701_afe_i2s_startup,
	.shutdown	= mt2701_afe_i2s_shutdown,
	.prepare	= mt2701_afe_i2s_prepare,
	.set_sysclk	= mt2701_afe_i2s_set_sysclk,
	.set_clkdiv	= mt2701_afe_i2s_set_clkdiv,
	.set_fmt	= mt2701_afe_i2s_set_fmt,
};

/* MRG BE DAIs*/
static struct snd_soc_dai_ops mt2701_btmrg_ops = {
	.startup = mt2701_btmrg_startup,
	.shutdown = mt2701_btmrg_shutdown,
	.hw_params = mt2701_btmrg_hw_params,
};

static int mt2701_afe_runtime_suspend(struct device *dev);
static int mt2701_afe_runtime_resume(struct device *dev);

static int mt2701_afe_dai_suspend(struct snd_soc_dai *dai)
{
	struct mt2701_afe *afe = snd_soc_dai_get_drvdata(dai);
	int i;

	dev_dbg(afe->dev, "%s\n", __func__);
	if (pm_runtime_status_suspended(afe->dev) || afe->suspended)
		return 0;

	for (i = 0; i < ARRAY_SIZE(mt2701_afe_backup_list); i++)
		regmap_read(afe->regmap, mt2701_afe_backup_list[i],
			    &afe->backup_regs[i]);

	afe->suspended = true;
	mt2701_afe_runtime_suspend(afe->dev);
	return 0;
}

static int mt2701_afe_dai_resume(struct snd_soc_dai *dai)
{
	struct mt2701_afe *afe = snd_soc_dai_get_drvdata(dai);
	int i = 0;

	dev_dbg(afe->dev, "%s\n", __func__);
	if (pm_runtime_status_suspended(afe->dev) || !afe->suspended)
		return 0;

	mt2701_afe_runtime_resume(afe->dev);

	for (i = 0; i < ARRAY_SIZE(mt2701_afe_backup_list); i++)
		regmap_write(afe->regmap, mt2701_afe_backup_list[i],
			     afe->backup_regs[i]);

	afe->suspended = false;
	return 0;
}

static struct snd_soc_dai_driver mt2701_afe_pcm_dais[] = {
	/* FE DAIs: memory intefaces to CPU */
	{
		.name = "PCM0",
		.id = MT2701_MEMIF_1,
		.suspend = mt2701_afe_dai_suspend,
		.resume = mt2701_afe_dai_resume,
		.playback = {
			.stream_name = "DL1",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE
				| SNDRV_PCM_FMTBIT_S24_LE
				| SNDRV_PCM_FMTBIT_S32_LE)
		},
		.capture = {
			.stream_name = "UL1",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.ops = &mt2701_afe_dai_ops,
	},
	{
		.name = "PCM_multi",
		.id = MT2701_MEMIF_M,
		.suspend = mt2701_afe_dai_suspend,
		.resume = mt2701_afe_dai_resume,
		.playback = {
			.stream_name = "DLM",
			.channels_min = 1,
			.channels_max = 8,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE
				| SNDRV_PCM_FMTBIT_S24_LE
				| SNDRV_PCM_FMTBIT_S32_LE)

		},
		.ops = &mt2701_afe_dai_ops,
	},
	{
		.name = "PCM1",
		.id = MT2701_MEMIF_2,
		.suspend = mt2701_afe_dai_suspend,
		.resume = mt2701_afe_dai_resume,
		.capture = {
			.stream_name = "UL2",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE
				| SNDRV_PCM_FMTBIT_S24_LE
				| SNDRV_PCM_FMTBIT_S32_LE)

		},
		.ops = &mt2701_afe_dai_ops,
	},
	{
		.name = "PCM_BT",
		.id = MT2701_MEMIF_BT,
		.suspend = mt2701_afe_dai_suspend,
		.resume = mt2701_afe_dai_resume,
		.playback = {
			.stream_name = "DLBT",
			.channels_min = 1,
			.channels_max = 1,
			.rates = (SNDRV_PCM_RATE_8000
				| SNDRV_PCM_RATE_16000),
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.capture = {
			.stream_name = "ULBT",
			.channels_min = 1,
			.channels_max = 1,
			.rates = (SNDRV_PCM_RATE_8000
				| SNDRV_PCM_RATE_16000),
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.ops = &mt2701_afe_dai_ops,
	},
	/* BE DAIs */
	{
		.name = "I2S0",
		.id = MT2701_IO_I2S,
		.playback = {
			.stream_name = "I2S0 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE
				| SNDRV_PCM_FMTBIT_S24_LE
				| SNDRV_PCM_FMTBIT_S32_LE)

		},
		.capture = {
			.stream_name = "I2S0 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE
				| SNDRV_PCM_FMTBIT_S24_LE
				| SNDRV_PCM_FMTBIT_S32_LE)

		},
		.ops = &mt2701_afe_i2s_ops,
		.symmetric_rates = 1,
	},
	{
		.name = "I2S1",
		.id = MT2701_IO_2ND_I2S,
		.playback = {
			.stream_name = "I2S1 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE
				| SNDRV_PCM_FMTBIT_S24_LE
				| SNDRV_PCM_FMTBIT_S32_LE)
			},
		.capture = {
			.stream_name = "I2S1 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE
				| SNDRV_PCM_FMTBIT_S24_LE
				| SNDRV_PCM_FMTBIT_S32_LE)
			},
		.ops = &mt2701_afe_i2s_ops,
		.symmetric_rates = 1,
	},
	{
		.name = "I2S2",
		.id = MT2701_IO_3RD_I2S,
		.playback = {
			.stream_name = "I2S2 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE
				| SNDRV_PCM_FMTBIT_S24_LE
				| SNDRV_PCM_FMTBIT_S32_LE)
			},
		.capture = {
			.stream_name = "I2S2 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE
				| SNDRV_PCM_FMTBIT_S24_LE
				| SNDRV_PCM_FMTBIT_S32_LE)
			},
		.ops = &mt2701_afe_i2s_ops,
		.symmetric_rates = 1,
	},
	{
		.name = "I2S3",
		.id = MT2701_IO_4TH_I2S,
		.playback = {
			.stream_name = "I2S3 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE
				| SNDRV_PCM_FMTBIT_S24_LE
				| SNDRV_PCM_FMTBIT_S32_LE)
			},
		.capture = {
			.stream_name = "I2S3 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE
				| SNDRV_PCM_FMTBIT_S24_LE
				| SNDRV_PCM_FMTBIT_S32_LE)
			},
		.ops = &mt2701_afe_i2s_ops,
		.symmetric_rates = 1,
	},
	{
		.name = "MRG BT",
		.id = MT2701_IO_MRG,
		.playback = {
			.stream_name = "BT Playback",
			.channels_min = 1,
			.channels_max = 1,
			.rates = (SNDRV_PCM_RATE_8000
				| SNDRV_PCM_RATE_16000),
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.capture = {
			.stream_name = "BT Capture",
			.channels_min = 1,
			.channels_max = 1,
			.rates = (SNDRV_PCM_RATE_8000
				| SNDRV_PCM_RATE_16000),
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.ops = &mt2701_btmrg_ops,
		.symmetric_rates = 1,
	}
};

static const struct snd_kcontrol_new mt2701_afe_o00_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I00 Switch", AFE_CONN0, 0, 1, 0),
};

static const struct snd_kcontrol_new mt2701_afe_o01_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I01 Switch", AFE_CONN1, 1, 1, 0),
};

static const struct snd_kcontrol_new mt2701_afe_o02_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I02 Switch", AFE_CONN2, 2, 1, 0),
};

static const struct snd_kcontrol_new mt2701_afe_o03_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I03 Switch", AFE_CONN3, 3, 1, 0),
};

static const struct snd_kcontrol_new mt2701_afe_o14_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I26 Switch", AFE_CONN14, 26, 1, 0),
};

static const struct snd_kcontrol_new mt2701_afe_o15_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I12 Switch", AFE_CONN15, 12, 1, 0),
};

static const struct snd_kcontrol_new mt2701_afe_o16_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I13 Switch", AFE_CONN16, 13, 1, 0),
};

static const struct snd_kcontrol_new mt2701_afe_o17_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I14 Switch", AFE_CONN17, 14, 1, 0),
};

static const struct snd_kcontrol_new mt2701_afe_o18_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I15 Switch", AFE_CONN18, 15, 1, 0),
};

static const struct snd_kcontrol_new mt2701_afe_o19_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I16 Switch", AFE_CONN19, 16, 1, 0),
};

static const struct snd_kcontrol_new mt2701_afe_o20_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I17 Switch", AFE_CONN20, 17, 1, 0),
};

static const struct snd_kcontrol_new mt2701_afe_o21_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I18 Switch", AFE_CONN21, 18, 1, 0),
};

static const struct snd_kcontrol_new mt2701_afe_o22_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I19 Switch", AFE_CONN22, 19, 1, 0),
};

static const struct snd_kcontrol_new mt2701_afe_o23_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I20 Switch", AFE_CONN23, 20, 1, 0),
};

static const struct snd_kcontrol_new mt2701_afe_o24_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I21 Switch", AFE_CONN24, 21, 1, 0),
};

static const struct snd_kcontrol_new mt2701_afe_o31_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("I35 Switch", AFE_CONN41, 9, 1, 0),
};

static const struct snd_kcontrol_new mt2701_afe_i02_mix[] = {
	SOC_DAPM_SINGLE("I2S0 Switch", SND_SOC_NOPM, 0, 1, 0),
};

static const struct snd_kcontrol_new mt2701_afe_multi_ch_out_i2s0[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("Multi ch Out I2s0", ASYS_I2SO1_CON, 26, 1,
				    0),
};

static const struct snd_kcontrol_new mt2701_afe_multi_ch_out_i2s1[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("Multi ch Out I2s1", ASYS_I2SO2_CON, 26, 1,
				    0),
};

static const struct snd_kcontrol_new mt2701_afe_multi_ch_out_i2s2[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("Multi ch Out I2s2", PWR2_TOP_CON, 17, 1,
				    0),
};

static const struct snd_kcontrol_new mt2701_afe_multi_ch_out_i2s3[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("Multi ch Out I2s3", PWR2_TOP_CON, 18, 1,
				    0),
};

static const struct snd_kcontrol_new mt2701_afe_multi_ch_out_i2s4[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("Multi ch Out I2s4", PWR2_TOP_CON, 19, 1,
				    0),
};

static const struct snd_kcontrol_new mt2701_afe_multi_ch_out_asrc0[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("Multi ch asrc out0", AUDIO_TOP_CON4, 14, 1,
				    1),
};

static const struct snd_kcontrol_new mt2701_afe_multi_ch_out_asrc1[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("Multi ch asrc out1", AUDIO_TOP_CON4, 15, 1,
				    1),
};

static const struct snd_kcontrol_new mt2701_afe_multi_ch_out_asrc2[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("Multi ch asrc out2", PWR2_TOP_CON, 6, 1,
				    1),
};

static const struct snd_kcontrol_new mt2701_afe_multi_ch_out_asrc3[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("Multi ch asrc out3", PWR2_TOP_CON, 7, 1,
				    1),
};

static const struct snd_kcontrol_new mt2701_afe_multi_ch_out_asrc4[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("Multi ch asrc out4", PWR2_TOP_CON, 8, 1,
				    1),
};

static const struct snd_soc_dapm_widget mt2701_afe_pcm_widgets[] = {
	/* inter-connections */
	SND_SOC_DAPM_MIXER("I00", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("I01", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("I02", SND_SOC_NOPM, 0, 0, mt2701_afe_i02_mix,
			   ARRAY_SIZE(mt2701_afe_i02_mix)),
	SND_SOC_DAPM_MIXER("I03", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("I12", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("I13", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("I14", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("I15", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("I16", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("I17", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("I18", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("I19", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("I26", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("I35", SND_SOC_NOPM, 0, 0, NULL, 0),

	SND_SOC_DAPM_MIXER("O00", SND_SOC_NOPM, 0, 0, mt2701_afe_o00_mix,
			   ARRAY_SIZE(mt2701_afe_o00_mix)),
	SND_SOC_DAPM_MIXER("O01", SND_SOC_NOPM, 0, 0, mt2701_afe_o01_mix,
			   ARRAY_SIZE(mt2701_afe_o01_mix)),
	SND_SOC_DAPM_MIXER("O02", SND_SOC_NOPM, 0, 0, mt2701_afe_o02_mix,
			   ARRAY_SIZE(mt2701_afe_o02_mix)),
	SND_SOC_DAPM_MIXER("O03", SND_SOC_NOPM, 0, 0, mt2701_afe_o03_mix,
			   ARRAY_SIZE(mt2701_afe_o03_mix)),
	SND_SOC_DAPM_MIXER("O14", SND_SOC_NOPM, 0, 0, mt2701_afe_o14_mix,
			   ARRAY_SIZE(mt2701_afe_o14_mix)),
	SND_SOC_DAPM_MIXER("O15", SND_SOC_NOPM, 0, 0, mt2701_afe_o15_mix,
			   ARRAY_SIZE(mt2701_afe_o15_mix)),
	SND_SOC_DAPM_MIXER("O16", SND_SOC_NOPM, 0, 0, mt2701_afe_o16_mix,
			   ARRAY_SIZE(mt2701_afe_o16_mix)),
	SND_SOC_DAPM_MIXER("O17", SND_SOC_NOPM, 0, 0, mt2701_afe_o17_mix,
			   ARRAY_SIZE(mt2701_afe_o17_mix)),
	SND_SOC_DAPM_MIXER("O18", SND_SOC_NOPM, 0, 0, mt2701_afe_o18_mix,
			   ARRAY_SIZE(mt2701_afe_o18_mix)),
	SND_SOC_DAPM_MIXER("O19", SND_SOC_NOPM, 0, 0, mt2701_afe_o19_mix,
			   ARRAY_SIZE(mt2701_afe_o19_mix)),
	SND_SOC_DAPM_MIXER("O20", SND_SOC_NOPM, 0, 0, mt2701_afe_o20_mix,
			   ARRAY_SIZE(mt2701_afe_o20_mix)),
	SND_SOC_DAPM_MIXER("O21", SND_SOC_NOPM, 0, 0, mt2701_afe_o21_mix,
			   ARRAY_SIZE(mt2701_afe_o21_mix)),
	SND_SOC_DAPM_MIXER("O22", SND_SOC_NOPM, 0, 0, mt2701_afe_o22_mix,
			   ARRAY_SIZE(mt2701_afe_o22_mix)),
	SND_SOC_DAPM_MIXER("O31", SND_SOC_NOPM, 0, 0, mt2701_afe_o31_mix,
			   ARRAY_SIZE(mt2701_afe_o31_mix)),

	SND_SOC_DAPM_MIXER("I12I13", SND_SOC_NOPM, 0, 0,
			   mt2701_afe_multi_ch_out_i2s0,
			   ARRAY_SIZE(mt2701_afe_multi_ch_out_i2s0)),
	SND_SOC_DAPM_MIXER("I14I15", SND_SOC_NOPM, 0, 0,
			   mt2701_afe_multi_ch_out_i2s1,
			   ARRAY_SIZE(mt2701_afe_multi_ch_out_i2s1)),
	SND_SOC_DAPM_MIXER("I16I17", SND_SOC_NOPM, 0, 0,
			   mt2701_afe_multi_ch_out_i2s2,
			   ARRAY_SIZE(mt2701_afe_multi_ch_out_i2s2)),
	SND_SOC_DAPM_MIXER("I18I19", SND_SOC_NOPM, 0, 0,
			   mt2701_afe_multi_ch_out_i2s3,
			   ARRAY_SIZE(mt2701_afe_multi_ch_out_i2s3)),

	SND_SOC_DAPM_MIXER("ASRC_O0", SND_SOC_NOPM, 0, 0,
			   mt2701_afe_multi_ch_out_asrc0,
			   ARRAY_SIZE(mt2701_afe_multi_ch_out_asrc0)),
	SND_SOC_DAPM_MIXER("ASRC_O1", SND_SOC_NOPM, 0, 0,
			   mt2701_afe_multi_ch_out_asrc1,
			   ARRAY_SIZE(mt2701_afe_multi_ch_out_asrc1)),
	SND_SOC_DAPM_MIXER("ASRC_O2", SND_SOC_NOPM, 0, 0,
			   mt2701_afe_multi_ch_out_asrc2,
			   ARRAY_SIZE(mt2701_afe_multi_ch_out_asrc2)),
	SND_SOC_DAPM_MIXER("ASRC_O3", SND_SOC_NOPM, 0, 0,
			   mt2701_afe_multi_ch_out_asrc3,
			   ARRAY_SIZE(mt2701_afe_multi_ch_out_asrc3)),
};

static const struct snd_soc_dapm_route mt2701_afe_pcm_routes[] = {
	{"I12", NULL, "DL1"},
	{"I13", NULL, "DL1"},
	{"I35", NULL, "DLBT"},

	{"I2S0 Playback", NULL, "O15"},
	{"I2S0 Playback", NULL, "O16"},

	{"I2S1 Playback", NULL, "O17"},
	{"I2S1 Playback", NULL, "O18"},
	{"I2S2 Playback", NULL, "O19"},
	{"I2S2 Playback", NULL, "O20"},
	{"I2S3 Playback", NULL, "O21"},
	{"I2S3 Playback", NULL, "O22"},
	{"BT Playback", NULL, "O31"},

	{"UL1", NULL, "O00"},
	{"UL1", NULL, "O01"},
	{"UL2", NULL, "O02"},
	{"UL2", NULL, "O03"},
	{"ULBT", NULL, "O14"},

	{"I00", NULL, "I2S0 Capture"},
	{"I01", NULL, "I2S0 Capture"},

	{"I02", NULL, "I2S1 Capture"},
	{"I03", NULL, "I2S1 Capture"},
	/*I02,03 link to UL2, also need to open I2S0*/
	{"I02", "I2S0 Switch", "I2S0 Capture"},

	{"I26", NULL, "BT Capture"},

	{"ASRC_O0", "Multi ch asrc out0", "DLM"},
	{"ASRC_O1", "Multi ch asrc out1", "DLM"},
	{"ASRC_O2", "Multi ch asrc out2", "DLM"},
	{"ASRC_O3", "Multi ch asrc out3", "DLM"},

	{"I12I13", "Multi ch Out I2s0", "ASRC_O0"},
	{"I14I15", "Multi ch Out I2s1", "ASRC_O1"},
	{"I16I17", "Multi ch Out I2s2", "ASRC_O2"},
	{"I18I19", "Multi ch Out I2s3", "ASRC_O3"},

	{ "I12", NULL, "I12I13" },
	{ "I13", NULL, "I12I13" },
	{ "I14", NULL, "I14I15" },
	{ "I15", NULL, "I14I15" },
	{ "I16", NULL, "I16I17" },
	{ "I17", NULL, "I16I17" },
	{ "I18", NULL, "I18I19" },
	{ "I19", NULL, "I18I19" },

	{ "O00", "I00 Switch", "I00" },
	{ "O01", "I01 Switch", "I01" },
	{ "O02", "I02 Switch", "I02" },
	{ "O03", "I03 Switch", "I03" },
	{ "O14", "I26 Switch", "I26" },
	{ "O15", "I12 Switch", "I12" },
	{ "O16", "I13 Switch", "I13" },
	{ "O17", "I14 Switch", "I14" },
	{ "O18", "I15 Switch", "I15" },
	{ "O19", "I16 Switch", "I16" },
	{ "O20", "I17 Switch", "I17" },
	{ "O21", "I18 Switch", "I18" },
	{ "O22", "I19 Switch", "I19" },
	{ "O31", "I35 Switch", "I35" },

};

static const struct snd_soc_component_driver mt2701_afe_pcm_dai_component = {
	.name = "mt2701-afe-pcm-dai",
	.dapm_widgets = mt2701_afe_pcm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(mt2701_afe_pcm_widgets),
	.dapm_routes = mt2701_afe_pcm_routes,
	.num_dapm_routes = ARRAY_SIZE(mt2701_afe_pcm_routes),
};

static const struct mt2701_afe_memif_data
		    memif_data[MT2701_MEMIF_NUM][MT2701_STREAM_DIR_NUM] = {
	{
		{
			.name = "DL1",
			.id = MT2701_MEMIF_1,
			.reg_ofs_base = AFE_DL1_BASE,
			.reg_ofs_cur = AFE_DL1_CUR,
			.fs_reg = AFE_DAC_CON1,
			.fs_shift = 0,
			.mono_reg = AFE_DAC_CON3,
			.mono_shift = 16,
			.enable_shift = 1,
			.hd_reg = AFE_MEMIF_HD_CON0,
			.hd_shift = 0,
			.agent_disable_shift = 6,
		},
		{
			.name = "UL1",
			.id = MT2701_MEMIF_1,
			.reg_ofs_base = AFE_VUL_BASE,
			.reg_ofs_cur = AFE_VUL_CUR,
			.fs_reg = AFE_DAC_CON2,
			.fs_shift = 0,
			.mono_reg = AFE_DAC_CON4,
			.mono_shift = 0,
			.enable_shift = 10,
			.hd_reg = AFE_MEMIF_HD_CON1,
			.hd_shift = 0,
			.agent_disable_shift = 0,
		}
	},
	{
		{
			.name = "DL2",
			.id = MT2701_MEMIF_2,
			.reg_ofs_base = AFE_DL2_BASE,
			.reg_ofs_cur = AFE_DL2_CUR,
			.fs_reg = AFE_DAC_CON1,
			.fs_shift = 5,
			.mono_reg = AFE_DAC_CON3,
			.mono_shift = 17,
			.enable_shift = 2,
			.hd_reg = AFE_MEMIF_HD_CON0,
			.hd_shift = 2,
			.agent_disable_shift = 7,
		},
		{
			.name = "UL2",
			.id = MT2701_MEMIF_2,
			.reg_ofs_base = AFE_UL2_BASE,
			.reg_ofs_cur = AFE_UL2_CUR,
			.fs_reg = AFE_DAC_CON2,
			.fs_shift = 5,
			.mono_reg = AFE_DAC_CON4,
			.mono_shift = 2,
			.enable_shift = 11,
			.hd_reg = AFE_MEMIF_HD_CON1,
			.hd_shift = 2,
			.agent_disable_shift = 1,
		}
	},
	{
		{
			.name = "DL3",
			.id = MT2701_MEMIF_3,
			.reg_ofs_base = AFE_DL3_BASE,
			.reg_ofs_cur = AFE_DL3_CUR,
			.fs_reg = AFE_DAC_CON1,
			.fs_shift = 10,
			.mono_reg = AFE_DAC_CON3,
			.mono_shift = 18,
			.enable_shift = 3,
			.hd_reg = AFE_MEMIF_HD_CON0,
			.hd_shift = 4,
			.agent_disable_shift = 8,
		},
		{
			.name = "UL3",
			.id = MT2701_MEMIF_3,
			.reg_ofs_base = AFE_UL3_BASE,
			.reg_ofs_cur = AFE_UL3_CUR,
			.fs_reg = AFE_DAC_CON2,
			.fs_shift = 10,
			.mono_reg = AFE_DAC_CON4,
			.mono_shift = 4,
			.enable_shift = 12,
			.hd_reg = AFE_MEMIF_HD_CON0,
			.hd_shift = 0,
			.agent_disable_shift = 2,
		}
	},
	{
		{
			.name = "DL4",
			.id = MT2701_MEMIF_4,
			.reg_ofs_base = AFE_DL4_BASE,
			.reg_ofs_cur = AFE_DL4_CUR,
			.fs_reg = AFE_DAC_CON1,
			.fs_shift = 15,
			.mono_reg = AFE_DAC_CON3,
			.mono_shift = 19,
			.enable_shift = 4,
			.hd_reg = AFE_MEMIF_HD_CON0,
			.hd_shift = 6,
			.agent_disable_shift = 9,
		},
		{
			.name = "UL4",
			.id = MT2701_MEMIF_4,
			.reg_ofs_base = AFE_UL4_BASE,
			.reg_ofs_cur = AFE_UL4_CUR,
			.fs_reg = AFE_DAC_CON2,
			.fs_shift = 15,
			.mono_reg = AFE_DAC_CON4,
			.mono_shift = 6,
			.enable_shift = 13,
			.hd_reg = AFE_MEMIF_HD_CON0,
			.hd_shift = 6,
			.agent_disable_shift = 3,
		}
	},
	{
		{
			.name = "DL5",
			.id = MT2701_MEMIF_5,
			.reg_ofs_base = AFE_DL5_BASE,
			.reg_ofs_cur = AFE_DL5_CUR,
			.fs_reg = AFE_DAC_CON1,
			.fs_shift = 20,
			.mono_reg = AFE_DAC_CON3,
			.mono_shift = 20,
			.enable_shift = 5,
			.hd_reg = AFE_MEMIF_HD_CON0,
			.hd_shift = 8,
			.agent_disable_shift = 10,
		},
		{
			.name = "UL5",
			.id = MT2701_MEMIF_5,
			.reg_ofs_base = AFE_UL5_BASE,
			.reg_ofs_cur = AFE_UL5_CUR,
			.fs_reg = AFE_DAC_CON2,
			.fs_shift = 20,
			.mono_reg = AFE_DAC_CON4,
			.mono_shift = 8,
			.enable_shift = 14,
			.hd_reg = AFE_MEMIF_HD_CON0,
			.hd_shift = 8,
			.agent_disable_shift = 4,
		}
	},
	{
		{
			.name = "DLM",
			.id = MT2701_MEMIF_M,
			.reg_ofs_base = AFE_DLMCH_BASE,
			.reg_ofs_cur = AFE_DLMCH_CUR,
			.fs_reg = AFE_DAC_CON1,
			.fs_shift = 0,
			.mono_reg = -1,
			.mono_shift = -1,
			.enable_shift = 7,
			.hd_reg = AFE_MEMIF_PBUF_SIZE,
			.hd_shift = 28,
			.agent_disable_shift = 12,
		},
		{
			/*no UL multi channel support*/
		}
	},
	{
		{
			.name = "DLBT",
			.id = MT2701_MEMIF_BT,
			.reg_ofs_base = AFE_ARB1_BASE,
			.reg_ofs_cur = AFE_ARB1_CUR,
			.fs_reg = AFE_DAC_CON3,
			.fs_shift = 10,
			.mono_reg = AFE_DAC_CON3,
			.mono_shift = 22,
			.enable_shift = 8,
			.hd_reg = AFE_MEMIF_HD_CON0,
			.hd_shift = 14,
			.agent_disable_shift = 13,
		},
		{
			.name = "ULBT",
			.id = MT2701_MEMIF_BT,
			.reg_ofs_base = AFE_DAI_BASE,
			.reg_ofs_cur = AFE_DAI_CUR,
			.fs_reg = AFE_DAC_CON2,
			.fs_shift = 30,
			.mono_reg = -1,
			.mono_shift = -1,
			.enable_shift = 17,
			.hd_reg = AFE_MEMIF_HD_CON1,
			.hd_shift = 20,
			.agent_disable_shift = 16,
		}
	}
};

static const struct mt2701_afe_irq_data irq_data[MT2701_IRQ_ASYS_END] = {
	{
		.irq_id = MT2701_IRQ_ASYS_IRQ1,
		.irq_cnt_reg = ASYS_IRQ1_CON,
		.irq_cnt_shift = 0,
		.irq_cnt_maskbit = 0xffffff,
		.irq_fs_reg = ASYS_IRQ1_CON,
		.irq_fs_shift = 24,
		.irq_fs_maskbit = 0x1f,
		.irq_en_reg = ASYS_IRQ1_CON,
		.irq_en_shift = 31,
	},
	{
		.irq_id = MT2701_IRQ_ASYS_IRQ2,
		.irq_cnt_reg = ASYS_IRQ2_CON,
		.irq_cnt_shift = 0,
		.irq_cnt_maskbit = 0xffffff,
		.irq_fs_reg = ASYS_IRQ2_CON,
		.irq_fs_shift = 24,
		.irq_fs_maskbit = 0x1f,
		.irq_en_reg = ASYS_IRQ2_CON,
		.irq_en_shift = 31,
	},
	{
		.irq_id = MT2701_IRQ_ASYS_IRQ3,
		.irq_cnt_reg = ASYS_IRQ3_CON,
		.irq_cnt_shift = 0,
		.irq_cnt_maskbit = 0xffffff,
		.irq_fs_reg = ASYS_IRQ3_CON,
		.irq_fs_shift = 24,
		.irq_fs_maskbit = 0x1f,
		.irq_en_reg = ASYS_IRQ3_CON,
		.irq_en_shift = 31,
	}
};

static const struct mt2701_i2s_data mt2701_i2s_data[MT2701_I2S_NUM][2] = {
	{
		{
			.i2s_ctrl_reg = ASYS_I2SO1_CON,
			.i2s_pwn_shift = 6,
			.i2s_asrc_fs_shift = 0,
			.i2s_asrc_fs_mask = 0x1f,

		},
		{
			.i2s_ctrl_reg = ASYS_I2SIN1_CON,
			.i2s_pwn_shift = 0,
			.i2s_asrc_fs_shift = 0,
			.i2s_asrc_fs_mask = 0x1f,

		},
	},
	{
		{
			.i2s_ctrl_reg = ASYS_I2SO2_CON,
			.i2s_pwn_shift = 7,
			.i2s_asrc_fs_shift = 5,
			.i2s_asrc_fs_mask = 0x1f,

		},
		{
			.i2s_ctrl_reg = ASYS_I2SIN2_CON,
			.i2s_pwn_shift = 1,
			.i2s_asrc_fs_shift = 5,
			.i2s_asrc_fs_mask = 0x1f,

		},
	},
	{
		{
			.i2s_ctrl_reg = ASYS_I2SO3_CON,
			.i2s_pwn_shift = 8,
			.i2s_asrc_fs_shift = 10,
			.i2s_asrc_fs_mask = 0x1f,

		},
		{
			.i2s_ctrl_reg = ASYS_I2SIN3_CON,
			.i2s_pwn_shift = 2,
			.i2s_asrc_fs_shift = 10,
			.i2s_asrc_fs_mask = 0x1f,

		},
	},
	{
		{
			.i2s_ctrl_reg = ASYS_I2SO4_CON,
			.i2s_pwn_shift = 9,
			.i2s_asrc_fs_shift = 15,
			.i2s_asrc_fs_mask = 0x1f,

		},
		{
			.i2s_ctrl_reg = ASYS_I2SIN4_CON,
			.i2s_pwn_shift = 3,
			.i2s_asrc_fs_shift = 15,
			.i2s_asrc_fs_mask = 0x1f,

		},
	},
};

static const struct regmap_config mt2701_afe_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = AFE_END_ADDR,
	.cache_type = REGCACHE_NONE,
};

static irqreturn_t mt2701_asys_isr(int irq_id, void *dev)
{
	int id;
	struct mt2701_afe *afe = dev;
	struct mt2701_afe_irq *irq;
	u32 status;

	status = mt2701_asys_irq_status(afe);
	mt2701_asys_irq_clear(afe, status);

	for (id = MT2701_IRQ_ASYS_START; id < MT2701_IRQ_ASYS_END; ++id) {
		irq = &afe->irqs[id];
		if (status & (0x1 << (id - MT2701_IRQ_ASYS_START)) && irq->isr)
			irq->isr(afe, irq->memif);
	}
	return IRQ_HANDLED;
}

static int mt2701_afe_runtime_suspend(struct device *dev)
{
	struct mt2701_afe *afe = dev_get_drvdata(dev);

	mt2701_afe_enable_clock(afe, 0);
	return 0;
}

static int mt2701_afe_runtime_resume(struct device *dev)
{
	struct mt2701_afe *afe = dev_get_drvdata(dev);

	pr_warn("%s\n", __func__);
	mt2701_afe_enable_clock(afe, 1);
	return 0;
}

static int mt2701_afe_pcm_dev_probe(struct platform_device *pdev)
{
	int ret, i, j;
	unsigned int irq_id;
	struct mt2701_afe *afe;
	struct resource *res;

	ret = 0;
	afe = devm_kzalloc(&pdev->dev, sizeof(*afe), GFP_KERNEL);
	if (!afe)
		return -ENOMEM;

	afe->dev = &pdev->dev;

	irq_id = platform_get_irq(pdev, 0);
	if (!irq_id) {
		dev_err(afe->dev, "%s no irq found\n",
			afe->dev->of_node->name);
		return -ENXIO;
	}
	ret = devm_request_irq(afe->dev, irq_id, mt2701_asys_isr,
			       IRQF_TRIGGER_NONE, "asys-isr", (void *)afe);
	if (ret) {
		dev_err(afe->dev, "could not request_irq for asys-isr\n");
		return ret;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	afe->base_addr = devm_ioremap_resource(&pdev->dev, res);

	if (IS_ERR(afe->base_addr))
		return PTR_ERR(afe->base_addr);

	afe->regmap = devm_regmap_init_mmio(&pdev->dev, afe->base_addr,
		&mt2701_afe_regmap_config);
	if (IS_ERR(afe->regmap))
		return PTR_ERR(afe->regmap);

	for (i = 0; i < MT2701_MEMIF_NUM; i++)
		for (j = 0; j < MT2701_STREAM_DIR_NUM; ++j)
			afe->memif[i][j].data = &memif_data[i][j];
	for (i = 0; i < MT2701_IRQ_ASYS_END; i++)
		afe->irqs[i].irq_data = &irq_data[i];

	for (i = 0; i < MT2701_I2S_NUM; i++) {
		afe->i2s_path[i].i2s_data[I2S_OUT]
			= &mt2701_i2s_data[i][I2S_OUT];
		afe->i2s_path[i].i2s_data[I2S_IN]
			= &mt2701_i2s_data[i][I2S_IN];
	}

	/* initial audio related clock */
	mt2701_init_clock(afe);

	platform_set_drvdata(pdev, afe);

	ret = snd_soc_register_platform(&pdev->dev, &mt2701_afe_pcm_platform);
	if (ret) {
		dev_warn(afe->dev, "err_platform\n");
		goto err_platform;
	}

	ret = snd_soc_register_component(&pdev->dev,
					 &mt2701_afe_pcm_dai_component,
					 mt2701_afe_pcm_dais,
					 ARRAY_SIZE(mt2701_afe_pcm_dais));
	if (ret) {
		dev_warn(afe->dev, "err_dai_component\n");
		goto err_dai_component;
	}
	/*enable afe clock*/
	mt2701_afe_enable_clock(afe, 1);

	return 0;
err_platform:
	snd_soc_unregister_platform(&pdev->dev);
err_dai_component:
	snd_soc_unregister_component(&pdev->dev);
	return ret;
}

static int mt2701_afe_pcm_dev_remove(struct platform_device *pdev)
{
	struct mt2701_afe *afe = platform_get_drvdata(pdev);

	if (!pm_runtime_status_suspended(&pdev->dev))
		mt2701_afe_runtime_suspend(&pdev->dev);

	snd_soc_unregister_component(&pdev->dev);
	snd_soc_unregister_platform(&pdev->dev);
	/*disable afe clock*/
	mt2701_afe_enable_clock(afe, 0);
	return 0;
}

static const struct of_device_id mt2701_afe_pcm_dt_match[] = {
	{ .compatible = "mediatek,mt2701-audio", },
	{},
};
MODULE_DEVICE_TABLE(of, mt2701_afe_pcm_dt_match);

static const struct dev_pm_ops mt2701_afe_pm_ops = {
	SET_RUNTIME_PM_OPS(mt2701_afe_runtime_suspend,
			   mt2701_afe_runtime_resume, NULL)
};

static struct platform_driver mt2701_afe_pcm_driver = {
	.driver = {
		   .name = "mt2701-audio",
		   .of_match_table = mt2701_afe_pcm_dt_match,
#ifdef CONFIG_PM
		   .pm = &mt2701_afe_pm_ops,
#endif
	},
	.probe = mt2701_afe_pcm_dev_probe,
	.remove = mt2701_afe_pcm_dev_remove,
};

module_platform_driver(mt2701_afe_pcm_driver);

MODULE_DESCRIPTION("Mediatek ALSA SoC AFE platform driver for 2701");
MODULE_AUTHOR("Garlic Tseng <garlic.tseng@mediatek.com>");
MODULE_LICENSE("GPL v2");
