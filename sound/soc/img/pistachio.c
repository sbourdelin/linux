/*
 * Pistachio audio card driver
 *
 * Copyright (C) 2015 Imagination Technologies Ltd.
 *
 * Author: Damien Horsley <Damien.Horsley@imgtec.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */


#include <linux/clk.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <sound/jack.h>
#include <sound/soc.h>
#include <dt-bindings/sound/pistachio-audio.h>

#define PISTACHIO_PLL_RATE_A		147456000
#define PISTACHIO_PLL_RATE_B		135475200
#define PISTACHIO_MAX_DIV		256
#define PISTACHIO_MIN_MCLK_FREQ		(135475200 / 256)

#define PISTACHIO_CLOCK_MASTER_EXT	-1
#define PISTACHIO_CLOCK_MASTER_LOOPBACK	-2

#define PISTACHIO_MAX_I2S_CODECS	12

#define PISTACHIO_MAX_FS_RATES		20

#define PISTACHIO_I2S_MCLK_MAX_FREQ	200000000
#define PISTACHIO_DAC_MCLK_MAX_FREQ	200000000

#define PISTACHIO_INTERNAL_DAC_PREFIX	"internal-dac"
#define PISTACHIO_I2S_OUT_PREFIX	"i2s-out"
#define PISTACHIO_I2S_IN_PREFIX		"i2s-in"

#define PISTACHIO_I2S_MCLK_NAME		"i2s_mclk"
#define PISTACHIO_DAC_MCLK_NAME		"dac_mclk"

#define PISTACHIO_I2S_OUTPUT_NAME	"I2S OUTPUT"
#define PISTACHIO_I2S_INPUT_NAME	"I2S INPUT"

#define PISTACHIO_I2S_LOOPBACK_REG		0x88
#define PISTACHIO_I2S_LOOPBACK_CLK_MASK		0x3

#define PISTACHIO_I2S_LOOPBACK_CLK_NONE		0
#define PISTACHIO_I2S_LOOPBACK_CLK_LOCAL	2

#define PISTACHIO_MAX_DAPM_ROUTES		6

struct pistachio_audio_output {
	unsigned int active_rate;
};

struct pistachio_parallel_out {
	struct pistachio_audio_output output;
	struct snd_soc_dai_link_component component;
};

struct pistachio_mclk {
	char *name;
	struct clk *mclk;
	unsigned int cur_rate;
	unsigned int max_rate;
};

struct pistachio_i2s_mclk {
	struct pistachio_mclk *mclk;
	unsigned int *fs_rates;
	unsigned int num_fs_rates;
	unsigned int min_rate;
	unsigned int max_rate;
};

struct pistachio_codec_i2s {
	struct pistachio_mclk *mclk;
	struct snd_soc_dai *dai;
	unsigned int mclk_index;
};

struct pistachio_i2s {
	struct pistachio_i2s_mclk mclk_a;
	struct pistachio_i2s_mclk mclk_b;
	struct pistachio_codec_i2s *codecs;
	struct snd_soc_dai_link_component *components;
	unsigned int num_codecs;
};

struct pistachio_i2s_out {
	struct pistachio_i2s i2s;
	struct pistachio_audio_output output;
};

struct pistachio_i2s_in {
	struct pistachio_i2s i2s;
	unsigned int active_rate;
	unsigned int fmt;
	int frame_master;
	int bitclock_master;
};

struct pistachio_i2s_codec_info_s {
	const char *prefix;
	const char *dai_name;
	struct device_node *np;
	struct pistachio_mclk *mclk;
	unsigned int mclk_index;
};

struct pistachio_i2s_codec_info {
	unsigned int total_codecs;
	unsigned int unique_codecs;
	int bitclock_master_idx;
	int frame_master_idx;
	struct pistachio_i2s_codec_info_s codecs[PISTACHIO_MAX_I2S_CODECS];
};

struct pistachio_i2s_mclk_info {
	unsigned int fs_rates[PISTACHIO_MAX_FS_RATES];
	unsigned int num_fs_rates;
	unsigned int min_rate;
	unsigned int max_rate;
};

struct pistachio_card {
	struct pistachio_audio_output *spdif_out;
	struct pistachio_parallel_out *parallel_out;
	struct pistachio_i2s_out *i2s_out;
	struct pistachio_i2s_in *i2s_in;
	bool spdif_in;
	struct snd_soc_card card;
	struct snd_soc_jack hp_jack;
	struct snd_soc_jack_pin hp_jack_pin;
	struct snd_soc_jack_gpio hp_jack_gpio;
	unsigned int mute_gpio;
	bool mute_gpio_inverted;
	struct mutex rate_mutex;
	struct clk *audio_pll;
	unsigned int audio_pll_rate;
	struct pistachio_mclk i2s_mclk;
	struct pistachio_mclk dac_mclk;
	struct regmap *periph_regs;
	struct notifier_block i2s_clk_notifier;
	struct snd_soc_dapm_route routes[PISTACHIO_MAX_DAPM_ROUTES];
};

static const struct snd_soc_dapm_widget pistachio_card_widgets[] = {
	SND_SOC_DAPM_CLOCK_SUPPLY(PISTACHIO_I2S_MCLK_NAME),
	SND_SOC_DAPM_CLOCK_SUPPLY(PISTACHIO_DAC_MCLK_NAME),
	SND_SOC_DAPM_OUTPUT(PISTACHIO_I2S_OUTPUT_NAME),
	SND_SOC_DAPM_INPUT(PISTACHIO_I2S_INPUT_NAME)
};

static int pistachio_card_set_sysclk_s(struct pistachio_codec_i2s *codec,
				       unsigned int rate, struct device *dev)
{
	int ret;

	ret = snd_soc_dai_set_sysclk(codec->dai, codec->mclk_index,
				     rate, SND_SOC_CLOCK_IN);
	if (ret)
		dev_err(dev, "snd_soc_dai_set_sysclk failed: %d", ret);

	return ret;
}

static int pistachio_card_set_sysclk(struct pistachio_i2s *i2s,
				     struct pistachio_mclk *mclk,
				     struct device *dev)
{
	int i, ret;
	struct pistachio_codec_i2s *codec;
	unsigned int rate = mclk->cur_rate;

	for (i = 0; i < i2s->num_codecs; i++) {
		codec = &i2s->codecs[i];
		if (codec->mclk == mclk) {
			ret = pistachio_card_set_sysclk_s(codec, rate, dev);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int pistachio_card_set_mclk_codecs(struct pistachio_card *pbc,
					  struct pistachio_mclk *mclk)
{
	int ret;
	struct device *dev = pbc->card.dev;

	if (pbc->i2s_out) {
		ret = pistachio_card_set_sysclk(&pbc->i2s_out->i2s, mclk, dev);
		if (ret)
			return ret;
	}

	if (pbc->i2s_in) {
		ret = pistachio_card_set_sysclk(&pbc->i2s_in->i2s, mclk, dev);
		if (ret)
			return ret;
	}

	return 0;
}

static bool pistachio_card_mclk_active(struct pistachio_card *pbc,
				       struct pistachio_mclk *mclk)
{
	if (pbc->i2s_out && pbc->i2s_out->output.active_rate) {
		if (pbc->i2s_out->i2s.mclk_a.mclk == mclk)
			return true;
		if (pbc->i2s_out->i2s.mclk_b.mclk == mclk)
			return true;
	}

	if (pbc->i2s_in && pbc->i2s_in->active_rate) {
		if (pbc->i2s_in->i2s.mclk_a.mclk == mclk)
			return true;
		if (pbc->i2s_in->i2s.mclk_b.mclk == mclk)
			return true;
	}

	return false;
}

static int pistachio_card_set_mclk(struct pistachio_card *pbc,
				   struct pistachio_mclk *mclk,
				   unsigned int rate)
{
	int ret;
	unsigned int old_rate = mclk->cur_rate;
	struct device *dev = pbc->card.dev;

	if (pistachio_card_mclk_active(pbc, mclk)) {
		dev_err(dev, "%s in use, cannot change rate\n", mclk->name);
		return -EBUSY;
	}

	/*
	 * Set cur_rate before the clk_set_rate call to stop the i2s
	 * mclk rate change callback rejecting the change
	 */
	mclk->cur_rate = rate;
	ret = clk_set_rate(mclk->mclk, rate);
	if (ret) {
		dev_err(dev, "clk_set_rate(%s, %u) failed: %d\n",
			mclk->name, rate, ret);
		mclk->cur_rate = old_rate;
		return ret;
	}

	return pistachio_card_set_mclk_codecs(pbc, mclk);
}

static int pistachio_card_set_pll_rate(struct pistachio_card *pbc,
				       unsigned int rate)
{
	int ret;
	unsigned int old_i2s_rate;
	struct device *dev = pbc->card.dev;

	/*
	 * If any configured streams are currently using a clock derived
	 * from the audio pll, a pll rate change cannot take place
	 */
	if ((pbc->spdif_out && pbc->spdif_out->active_rate) ||
	    (pbc->parallel_out && pbc->parallel_out->output.active_rate) ||
	    (pbc->i2s_out && pbc->i2s_out->output.active_rate) ||
	    (pbc->i2s_in && pbc->i2s_in->active_rate &&
	    pbc->i2s_in->i2s.mclk_a.mclk)) {
		dev_err(dev, "audio pll in use, cannot change rate\n");
		return -EBUSY;
	}

	/*
	 * Set cur_rate before the clk_set_rate call to stop the i2s
	 * mclk rate change callback rejecting the change
	 */
	old_i2s_rate = pbc->i2s_mclk.cur_rate;
	pbc->i2s_mclk.cur_rate = rate / (pbc->audio_pll_rate / old_i2s_rate);

	ret = clk_set_rate(pbc->audio_pll, rate);
	if (ret) {
		dev_err(dev, "clk_set_rate(audio_pll, %u) failed: %d\n",
			rate, ret);
		pbc->i2s_mclk.cur_rate = old_i2s_rate;
		return ret;
	}

	pbc->audio_pll_rate = rate;

	pbc->dac_mclk.cur_rate = rate / (pbc->audio_pll_rate /
					 pbc->dac_mclk.cur_rate);

	ret = pistachio_card_set_mclk_codecs(pbc, &pbc->i2s_mclk);
	if (ret)
		return ret;

	return pistachio_card_set_mclk_codecs(pbc, &pbc->dac_mclk);
}

static void pistachio_card_rate_err(struct pistachio_card *pbc,
			struct pistachio_i2s_mclk *mclk_a,
			struct pistachio_i2s_mclk *mclk_b,
			unsigned int rate_a, unsigned int rate_b)
{
	char *dir_a, *dir_b;
	struct device *dev = pbc->card.dev;

	if (pbc->i2s_out && ((mclk_a == &pbc->i2s_out->i2s.mclk_a) ||
			     (mclk_a == &pbc->i2s_out->i2s.mclk_b))) {
		dir_a = "I2S out";
	} else {
		dir_a = "I2S in";
	}

	if (pbc->i2s_out && ((mclk_b == &pbc->i2s_out->i2s.mclk_a) ||
			     (mclk_b == &pbc->i2s_out->i2s.mclk_b))) {
		dir_b = "I2S out";
	} else {
		dir_b = "I2S in";
	}

	if (!mclk_b) {
		dev_err(dev, "No valid rate for %s (%s sample rate %u)\n",
			mclk_a->mclk->name, dir_a, rate_a);
	} else {
		dev_err(dev,
			"No valid rate for %s (%s sample rate %u, %s sample rate %u)\n",
			mclk_a->mclk->name, dir_a, rate_a, dir_b, rate_b);
	}
}

static bool pistachio_card_mclk_ok(struct pistachio_i2s_mclk *mclk,
				   unsigned int rate)
{
	int i;
	unsigned int mclk_rate;

	if (!mclk)
		return true;

	mclk_rate = mclk->mclk->cur_rate;

	if ((mclk_rate < mclk->min_rate) || (mclk_rate > mclk->max_rate))
		return false;

	for (i = 0; i < mclk->num_fs_rates; i++)
		if ((rate * mclk->fs_rates[i]) == mclk_rate)
			return true;

	return false;
}

static int pistachio_card_get_mclk_rate(struct pistachio_card *pbc,
	struct pistachio_i2s_mclk *mclk_a, struct pistachio_i2s_mclk *mclk_b,
	unsigned int rate_a, unsigned int rate_b, unsigned int *p_mclk_rate)
{
	int i, j;
	unsigned int div, total_div, mclk_rate;

	/*
	 * If the current system clock rate is sufficient for the given
	 * sample rates, do not change the rate.
	 */
	if (pistachio_card_mclk_ok(mclk_a, rate_a) &&
	    pistachio_card_mclk_ok(mclk_b, rate_b)) {
		*p_mclk_rate = mclk_a->mclk->cur_rate;
		return 0;
	}

	/* Calculate total divide (internal divide and Nfs combined) */
	total_div = pbc->audio_pll_rate / rate_a;

	/* Attempt to find an mclk rate that satisfies the constraints */
	for (i = 0; i < mclk_a->num_fs_rates; i++) {

		div = total_div / mclk_a->fs_rates[i];

		if (div > PISTACHIO_MAX_DIV)
			continue;

		mclk_rate = pbc->audio_pll_rate / div;

		if ((mclk_rate < mclk_a->min_rate) ||
		    (mclk_rate > mclk_a->max_rate) ||
		    (mclk_rate > mclk_a->mclk->max_rate))
			continue;

		if ((rate_a * mclk_a->fs_rates[i] * div) !=
		     pbc->audio_pll_rate)
			continue;

		if (!mclk_b)
			break;

		if ((mclk_rate < mclk_b->min_rate) ||
		    (mclk_rate > mclk_b->max_rate))
			continue;

		for (j = 0; j < mclk_b->num_fs_rates; j++)
			if ((rate_b * mclk_b->fs_rates[j] * div) ==
			     pbc->audio_pll_rate)
				break;

		if (j != mclk_b->num_fs_rates)
			break;
	}

	if (i == mclk_a->num_fs_rates) {
		pistachio_card_rate_err(pbc, mclk_a, mclk_b, rate_a, rate_b);
		return -EINVAL;
	}

	*p_mclk_rate = mclk_rate;

	return 0;
}

static int pistachio_card_update_mclk(struct pistachio_card *pbc,
	struct pistachio_i2s_mclk *mclk_a, struct pistachio_i2s_mclk *mclk_b,
	unsigned int rate_a, unsigned int rate_b)
{
	struct pistachio_mclk *mclk = mclk_a->mclk;
	unsigned int mclk_rate;
	int ret;

	ret = pistachio_card_get_mclk_rate(pbc, mclk_a, mclk_b, rate_a,
					   rate_b, &mclk_rate);
	if (ret)
		return ret;

	if (mclk->cur_rate != mclk_rate)
		return pistachio_card_set_mclk(pbc, mclk, mclk_rate);

	return 0;
}

static int pistachio_card_update_mclk_single(struct pistachio_card *pbc,
		      struct pistachio_i2s_mclk *mclk, unsigned int rate)
{
	return pistachio_card_update_mclk(pbc, mclk, NULL, rate, 0);
}

static int pistachio_card_get_pll_rate(unsigned int rate, struct device *dev)
{
	switch (rate) {
	case 8000:
	case 16000:
	case 32000:
	case 48000:
	case 64000:
	case 96000:
	case 192000:
		return PISTACHIO_PLL_RATE_A;
	case 11025:
	case 22050:
	case 44100:
	case 88200:
	case 176400:
		return PISTACHIO_PLL_RATE_B;
	default:
		dev_err(dev, "No suitable pll rate for sample rate %u\n", rate);
		return -EINVAL;
	}
}

static int _pistachio_card_change_rate(struct pistachio_card *pbc,
				       unsigned int rate,
				       struct pistachio_i2s *i2s)
{
	int ret;
	unsigned int pll_rate;

	ret = pistachio_card_get_pll_rate(rate, pbc->card.dev);
	if (ret < 0)
		return ret;

	pll_rate = ret;

	if (pbc->audio_pll_rate != pll_rate) {
		ret = pistachio_card_set_pll_rate(pbc, pll_rate);
		if (ret)
			return ret;
	}

	/*
	 * Nothing more to do if an mclk is not used. The individual
	 * cpu-dai drivers will make the required clock changes
	 */
	if (!i2s)
		return 0;

	ret = pistachio_card_update_mclk_single(pbc, &i2s->mclk_a, rate);
	if (ret)
		return ret;

	if (!i2s->mclk_b.mclk)
		return 0;

	return pistachio_card_update_mclk_single(pbc, &i2s->mclk_b, rate);
}

static int pistachio_card_change_rate(struct pistachio_card *pbc,
			unsigned int rate, struct pistachio_i2s *i2s,
			unsigned int *active_rate)
{
	int ret;

	mutex_lock(&pbc->rate_mutex);
	*active_rate = 0;
	ret = _pistachio_card_change_rate(pbc, rate, i2s);
	if (!ret)
		*active_rate = rate;
	mutex_unlock(&pbc->rate_mutex);

	return ret;
}

static int pistachio_card_i2s_link_init(struct pistachio_i2s *i2s,
					struct snd_soc_pcm_runtime *rtd,
					struct device *dev)
{
	int ret, i;
	unsigned long rate;
	struct pistachio_codec_i2s *codec;

	for (i = 0; i < i2s->num_codecs; i++) {
		codec = &i2s->codecs[i];
		codec->dai = rtd->codec_dais[i];
		if (codec->mclk) {
			rate = codec->mclk->cur_rate;
			ret = pistachio_card_set_sysclk_s(codec, rate, dev);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int pistachio_card_i2s_out_link_init(struct snd_soc_pcm_runtime *rtd)
{
	struct pistachio_card *pbc = snd_soc_card_get_drvdata(rtd->card);
	struct device *dev = pbc->card.dev;

	return pistachio_card_i2s_link_init(&pbc->i2s_out->i2s, rtd, dev);
}

static int pistachio_card_i2s_in_link_init(struct snd_soc_pcm_runtime *rtd)
{
	struct pistachio_card *pbc = snd_soc_card_get_drvdata(rtd->card);
	int ret, i;
	unsigned int fmt;
	struct device *dev = pbc->card.dev;
	u32 val;

	ret = pistachio_card_i2s_link_init(&pbc->i2s_in->i2s, rtd, dev);
	if (ret)
		return ret;

	if (pbc->i2s_in->frame_master == PISTACHIO_CLOCK_MASTER_LOOPBACK)
		val = PISTACHIO_I2S_LOOPBACK_CLK_LOCAL;
	else
		val = PISTACHIO_I2S_LOOPBACK_CLK_NONE;

	ret = regmap_update_bits(pbc->periph_regs, PISTACHIO_I2S_LOOPBACK_REG,
				 PISTACHIO_I2S_LOOPBACK_CLK_MASK, val);
	if (ret) {
		dev_err(pbc->card.dev, "regmap_update_bits failed: %d\n", ret);
		return ret;
	}

	fmt = pbc->i2s_in->fmt | SND_SOC_DAIFMT_CBM_CFM;
	ret = snd_soc_dai_set_fmt(rtd->cpu_dai, fmt);
	if (ret) {
		dev_err(dev, "snd_soc_dai_set_fmt (cpu) failed: %d\n", ret);
		return ret;
	}

	for (i = 0; i < pbc->i2s_in->i2s.num_codecs; i++) {
		fmt = pbc->i2s_in->fmt;

		if (i == pbc->i2s_in->frame_master)
			if (i == pbc->i2s_in->bitclock_master)
				fmt |= SND_SOC_DAIFMT_CBM_CFM;
			else
				fmt |= SND_SOC_DAIFMT_CBS_CFM;
		else
			if (i == pbc->i2s_in->bitclock_master)
				fmt |= SND_SOC_DAIFMT_CBM_CFS;
			else
				fmt |= SND_SOC_DAIFMT_CBS_CFS;

		ret = snd_soc_dai_set_fmt(rtd->codec_dais[i], fmt);
		if (ret) {
			dev_err(dev, "snd_soc_dai_set_fmt failed: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static void pistachio_card_parallel_out_shutdown(struct snd_pcm_substream *st)
{
	struct snd_soc_pcm_runtime *rtd = st->private_data;
	struct pistachio_card *pbc = snd_soc_card_get_drvdata(rtd->card);

	pbc->parallel_out->output.active_rate = 0;
}

static int pistachio_card_parallel_out_hw_params(struct snd_pcm_substream *st,
					      struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = st->private_data;
	struct pistachio_card *pbc = snd_soc_card_get_drvdata(rtd->card);

	return pistachio_card_change_rate(pbc, params_rate(params), NULL,
				   &pbc->parallel_out->output.active_rate);
}

static struct snd_soc_ops pistachio_card_parallel_out_ops = {
	.shutdown = pistachio_card_parallel_out_shutdown,
	.hw_params = pistachio_card_parallel_out_hw_params
};

static void pistachio_card_spdif_out_shutdown(struct snd_pcm_substream *st)
{
	struct snd_soc_pcm_runtime *rtd = st->private_data;
	struct pistachio_card *pbc = snd_soc_card_get_drvdata(rtd->card);

	pbc->spdif_out->active_rate = 0;
}

static int pistachio_card_spdif_out_hw_params(struct snd_pcm_substream *st,
					      struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = st->private_data;
	struct pistachio_card *pbc = snd_soc_card_get_drvdata(rtd->card);

	return pistachio_card_change_rate(pbc, params_rate(params), NULL,
					  &pbc->spdif_out->active_rate);
}

static struct snd_soc_ops pistachio_card_spdif_out_ops = {
	.shutdown = pistachio_card_spdif_out_shutdown,
	.hw_params = pistachio_card_spdif_out_hw_params
};

static void pistachio_card_i2s_out_shutdown(struct snd_pcm_substream *st)
{
	struct snd_soc_pcm_runtime *rtd = st->private_data;
	struct pistachio_card *pbc = snd_soc_card_get_drvdata(rtd->card);

	pbc->i2s_out->output.active_rate = 0;
}

static int pistachio_card_i2s_out_hw_params(struct snd_pcm_substream *st,
					    struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = st->private_data;
	struct pistachio_card *pbc = snd_soc_card_get_drvdata(rtd->card);

	return pistachio_card_change_rate(pbc, params_rate(params),
					  &pbc->i2s_out->i2s,
					  &pbc->i2s_out->output.active_rate);
}

static struct snd_soc_ops pistachio_card_i2s_out_ops = {
	.shutdown = pistachio_card_i2s_out_shutdown,
	.hw_params = pistachio_card_i2s_out_hw_params
};

static void pistachio_card_i2s_in_shutdown(struct snd_pcm_substream *st)
{
	struct snd_soc_pcm_runtime *rtd = st->private_data;
	struct pistachio_card *pbc = snd_soc_card_get_drvdata(rtd->card);

	pbc->i2s_in->active_rate = 0;
}

static int pistachio_card_i2s_in_hw_params(struct snd_pcm_substream *st,
					   struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = st->private_data;
	struct pistachio_card *pbc = snd_soc_card_get_drvdata(rtd->card);

	return pistachio_card_change_rate(pbc, params_rate(params),
					  &pbc->i2s_in->i2s,
					  &pbc->i2s_in->active_rate);
}

static struct snd_soc_ops pistachio_card_i2s_in_ops = {
	.shutdown = pistachio_card_i2s_in_shutdown,
	.hw_params = pistachio_card_i2s_in_hw_params
};

static int pistachio_card_parse_spdif_out(struct device_node *node,
					  struct pistachio_card *pbc,
					  struct snd_soc_dai_link *link)
{
	struct device_node *np;
	struct device *dev = pbc->card.dev;

	pbc->spdif_out = devm_kzalloc(pbc->card.dev, sizeof(*pbc->spdif_out),
				      GFP_KERNEL);
	if (!pbc->spdif_out)
		return -ENOMEM;

	link->name = link->stream_name = "pistachio-spdif-out";

	np = of_parse_phandle(node, "cpu-dai", 0);
	if (!np) {
		dev_err(dev, "Failed to parse cpu-dai (%s)\n", node->name);
		return -EINVAL;
	}

	link->cpu_of_node = np;
	link->platform_of_node = np;
	link->codec_dai_name = "snd-soc-dummy-dai";
	link->codec_name = "snd-soc-dummy";
	link->ops = &pistachio_card_spdif_out_ops;

	return 0;
}

static int pistachio_card_parse_spdif_in(struct device_node *node,
					 struct pistachio_card *pbc,
					 struct snd_soc_dai_link *link)
{
	struct device_node *np;
	struct device *dev = pbc->card.dev;

	pbc->spdif_in = true;

	link->name = link->stream_name = "pistachio-spdif-in";

	np = of_parse_phandle(node, "cpu-dai", 0);
	if (!np) {
		dev_err(dev, "Failed to parse cpu-dai (%s)\n", node->name);
		return -EINVAL;
	}

	link->cpu_of_node = np;
	link->platform_of_node = np;
	link->codec_dai_name = "snd-soc-dummy-dai";
	link->codec_name = "snd-soc-dummy";

	return 0;
}

static int pistachio_card_parse_parallel_out(struct device_node *node,
					     struct pistachio_card *pbc,
					     struct snd_soc_dai_link *link)
{
	struct device_node *np;
	int ret;
	struct device *dev = pbc->card.dev;

	pbc->parallel_out = devm_kzalloc(pbc->card.dev,
					 sizeof(*pbc->parallel_out),
					 GFP_KERNEL);
	if (!pbc->parallel_out)
		return -ENOMEM;

	link->name = link->stream_name = "pistachio-parallel-out";

	np = of_parse_phandle(node, "cpu-dai", 0);
	if (!np) {
		dev_err(dev, "Failed to parse cpu-dai (%s)\n", node->name);
		return -EINVAL;
	}

	link->cpu_of_node = np;
	link->platform_of_node = np;
	link->codecs = &pbc->parallel_out->component;

	np = of_parse_phandle(node, "sound-dai", 0);
	if (!np) {
		dev_err(dev, "Failed to parse sound-dai (%s)\n", node->name);
		return -EINVAL;
	}
	link->codecs[0].of_node = np;
	link->num_codecs = 1;
	ret = snd_soc_of_get_dai_name(node, &link->codecs[0].dai_name);
	if (ret) {
		dev_err(dev, "snd_soc_of_get_dai_name failed (%s): %d\n",
			node->name, ret);
		return ret;
	}

	link->ops = &pistachio_card_parallel_out_ops;

	return 0;
}

static int pistachio_card_parse_i2s_mclk(struct pistachio_card *pbc,
		struct device_node *np, struct pistachio_mclk *mclk,
		struct pistachio_i2s_mclk_info *fs)
{
	int ret, i, j, k, num_fs_rates;
	u32 min_fs_rate, max_fs_rate, max_rate;
	u32 fs_rates[PISTACHIO_MAX_FS_RATES];
	struct device *dev = pbc->card.dev;

	ret = of_property_read_u32(np, "mclk-min-fs-freq", &min_fs_rate);
	if (ret) {
		dev_err(dev, "Failed to read mclk-min-fs-freq (%s): %d\n",
			np->name, ret);
		return ret;
	}

	ret = of_property_read_u32(np, "mclk-max-fs-freq", &max_fs_rate);
	if (ret) {
		dev_err(dev, "Failed to read mclk-max-fs-freq (%s): %d\n",
			np->name, ret);
		return ret;
	}

	ret = of_property_read_u32(np, "mclk-max-freq", &max_rate);
	if (ret) {
		dev_err(dev, "Failed to read mclk-max-freq (%s): %d\n",
			np->name, ret);
		return ret;
	}

	if ((max_fs_rate < PISTACHIO_MIN_MCLK_FREQ) ||
	    (max_fs_rate > max_rate) ||
	    (max_fs_rate < min_fs_rate)) {
		dev_err(dev, "Invalid min/max rate (%s)\n", np->name);
		return -EINVAL;
	}

	if (min_fs_rate > fs->min_rate)
		fs->min_rate = min_fs_rate;

	if (max_fs_rate < fs->max_rate)
		fs->max_rate = max_fs_rate;

	if (max_rate < mclk->max_rate)
		mclk->max_rate = max_rate;

	if (fs->min_rate > fs->max_rate) {
		dev_err(dev, "No valid frequency range remaining for %s\n",
			mclk->name);
		return -EINVAL;
	}

	num_fs_rates = of_property_count_u32_elems(np, "mclk-fs");
	if (num_fs_rates < 0) {
		dev_err(dev, "of_property_count_u32_elems failed: %d (%s)\n",
			num_fs_rates, np->name);
		return num_fs_rates;
	}

	if (!num_fs_rates || (num_fs_rates > PISTACHIO_MAX_FS_RATES)) {
		dev_err(dev, "Invalid fs-rates count: %d (%s)\n",
			num_fs_rates, np->name);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(np, "mclk-fs", fs_rates,
					 num_fs_rates);
	if (ret) {
		dev_err(dev, "of_property_read_u32_array failed: %d (%s)\n",
			ret, np->name);
		return ret;
	}

	/*
	 * If this is the first fs-rates list for this combination
	 * of {i2s direction, mclk}, this list defines the
	 * current fs-rate list for this combination. Else, this list
	 * subtracts any fs-rates that are not present in both lists
	 */
	if (!fs->num_fs_rates) {
		/* Remove any duplicates while copying */
		for (i = 0, k = 0; i < num_fs_rates; i++) {
			for (j = 0; j < k; j++)
				if (fs->fs_rates[j] == fs_rates[i])
					break;
			if (j == k)
				fs->fs_rates[k++] = fs_rates[i];
		}
		fs->num_fs_rates = k;
	} else {
		for (j = 0; j < fs->num_fs_rates; j++) {
			for (i = 0; i < num_fs_rates; i++)
				if (fs->fs_rates[j] == fs_rates[i])
					break;

			if (i == num_fs_rates) {
				for (k = j; k < (fs->num_fs_rates - 1); k++)
					fs->fs_rates[k] = fs->fs_rates[k + 1];
				fs->num_fs_rates--;

				if (!fs->num_fs_rates) {
					dev_err(dev, "No fs rates remaining for %s\n",
						mclk->name);
					return -EINVAL;
				}

				j--;
			}
		}
	}

	return 0;
}

static int pistachio_card_parse_i2s_codec(struct device_node *np,
	struct device_node *subnode, int index, struct pistachio_card *pbc,
	struct device_node *codec, struct pistachio_i2s_codec_info *codec_info,
	struct pistachio_mclk *mclk)
{
	int i, ret;
	struct pistachio_i2s_codec_info_s *info;
	struct device *dev = pbc->card.dev;

	if (codec_info->total_codecs == PISTACHIO_MAX_I2S_CODECS) {
		dev_err(dev, "Too many codecs\n");
		of_node_put(codec);
		return -EINVAL;
	}

	for (i = 0; i < codec_info->total_codecs; i++)
		if (codec_info->codecs[i].np == codec)
			break;

	if (i == codec_info->total_codecs)
		codec_info->unique_codecs++;

	info = &codec_info->codecs[codec_info->total_codecs++];
	info->np = codec;
	info->prefix = subnode->name;
	info->mclk = mclk;

	ret = of_property_read_u32(subnode, "mclk-index", &info->mclk_index);
	if (ret)
		info->mclk_index = 0;

	ret = snd_soc_of_get_dai_name(subnode, &info->dai_name);
	if (ret) {
		dev_err(dev, "snd_soc_of_get_dai_name failed: %d (%s)\n",
			ret, subnode->name);
		return ret;
	}

	if (of_property_read_bool(subnode, "frame-master")) {
		if (codec_info->frame_master_idx != -1) {
			dev_err(dev, "Multiple frame clock masters (%s)\n",
				np->name);
			return -EINVAL;
		}
		codec_info->frame_master_idx = index;
	}

	if (of_property_read_bool(subnode, "bitclock-master")) {
		if (codec_info->bitclock_master_idx != -1) {
			dev_err(dev, "Multiple bit clock masters (%s)\n",
				np->name);
			return -EINVAL;
		}
		codec_info->bitclock_master_idx = index;
	}

	return 0;
}

static int pistachio_card_parse_i2s_codecs(struct device_node *np,
			struct pistachio_card *pbc,
			struct pistachio_i2s_codec_info *codec_info,
			struct pistachio_i2s_mclk_info *i2s_fs_info,
			struct pistachio_i2s_mclk_info *dac_fs_info)
{
	int i, ret;
	struct device_node *subnode, *codec;
	u32 mclk_id;
	struct pistachio_mclk *mclk;
	struct pistachio_i2s_mclk_info *fs_info;
	struct device *dev = pbc->card.dev;

	i = 0;
	for_each_child_of_node(np, subnode) {

		ret = of_property_read_u32(subnode, "mclk", &mclk_id);
		if (ret) {
			mclk = NULL;
		} else {
			switch (mclk_id) {
			case PISTACHIO_MCLK_I2S:
				mclk = &pbc->i2s_mclk;
				fs_info = i2s_fs_info;
				break;
			case PISTACHIO_MCLK_DAC:
				mclk = &pbc->dac_mclk;
				fs_info = dac_fs_info;
				break;
			default:
				dev_err(dev, "Invalid mclk id: %u (%s)\n",
					mclk_id, subnode->name);
				ret = -EINVAL;
				goto err_subnode;
			}
			ret = pistachio_card_parse_i2s_mclk(pbc, subnode, mclk,
							    fs_info);
			if (ret)
				goto err_subnode;
		}

		codec = of_parse_phandle(subnode, "sound-dai", 0);
		if (!codec)
			continue;

		ret = pistachio_card_parse_i2s_codec(np, subnode, i, pbc,
						     codec, codec_info, mclk);
		if (ret)
			goto err_subnode;

		i++;
	}

	return 0;

err_subnode:
	of_node_put(subnode);

	return ret;
}

static int pistachio_card_mclk_copy(struct pistachio_mclk *mclk,
	struct pistachio_i2s_mclk *mclk_i2s, struct pistachio_card *pbc,
	struct pistachio_i2s_mclk_info *mclk_info)
{
	unsigned int size;

	mclk_i2s->mclk = mclk;
	mclk_i2s->num_fs_rates = mclk_info->num_fs_rates;

	size = sizeof(*mclk_i2s->fs_rates) * mclk_i2s->num_fs_rates;
	mclk_i2s->fs_rates = devm_kzalloc(pbc->card.dev, size, GFP_KERNEL);
	if (!mclk_i2s->fs_rates)
		return -ENOMEM;

	memcpy(mclk_i2s->fs_rates, mclk_info->fs_rates, size);

	mclk_i2s->min_rate = mclk_info->min_rate;
	mclk_i2s->max_rate = mclk_info->max_rate;

	return 0;
}

static int pistachio_card_parse_i2s_common(struct device_node *node,
		struct pistachio_card *pbc, struct pistachio_i2s *i2s,
		struct snd_soc_dai_link *link,
		struct pistachio_i2s_codec_info *codec_info,
		struct pistachio_i2s_mclk_info *i2s_mclk_info,
		struct pistachio_i2s_mclk_info *dac_mclk_info)
{
	int ret, i;
	unsigned int initial_codecs = codec_info->total_codecs, size;
	struct pistachio_i2s_codec_info_s *codecs;
	struct pistachio_i2s_mclk *mclk;

	codecs = &codec_info->codecs[initial_codecs];

	ret = pistachio_card_parse_i2s_codecs(node, pbc, codec_info,
					      i2s_mclk_info, dac_mclk_info);
	i2s->num_codecs = codec_info->total_codecs - initial_codecs;
	if (ret)
		goto err_codec_info;

	mclk = &i2s->mclk_a;

	if (i2s_mclk_info->num_fs_rates) {
		ret = pistachio_card_mclk_copy(&pbc->i2s_mclk, mclk,
					       pbc, i2s_mclk_info);
		if (ret)
			goto err_codec_info;

		mclk = &i2s->mclk_b;
	}

	if (dac_mclk_info->num_fs_rates) {
		ret = pistachio_card_mclk_copy(&pbc->dac_mclk, mclk,
					       pbc, dac_mclk_info);
		if (ret)
			goto err_codec_info;
	}

	/* Use the dummy codec if there are no codec drivers in this link */
	if (!i2s->num_codecs) {
		link->codec_dai_name = "snd-soc-dummy-dai";
		link->codec_name = "snd-soc-dummy";
		return 0;
	}

	size = sizeof(*i2s->codecs) * i2s->num_codecs;
	i2s->codecs = devm_kzalloc(pbc->card.dev, size, GFP_KERNEL);
	if (!i2s->codecs) {
		ret = -ENOMEM;
		goto err_codec_info;
	}

	for (i = 0; i < i2s->num_codecs; i++) {
		i2s->codecs[i].mclk = codecs[i].mclk;
		i2s->codecs[i].mclk_index = codecs[i].mclk_index;
	}

	size = sizeof(*i2s->components) * i2s->num_codecs;
	i2s->components = devm_kzalloc(pbc->card.dev, size, GFP_KERNEL);
	if (!i2s->components) {
		ret = -ENOMEM;
		goto err_codec_info;
	}

	for (i = 0; i < i2s->num_codecs; i++) {
		i2s->components[i].dai_name = codecs[i].dai_name;
		i2s->components[i].of_node = codecs[i].np;
	}

	link->codecs = i2s->components;
	link->num_codecs = i2s->num_codecs;

	return 0;

err_codec_info:
	for (i = 0; i < i2s->num_codecs; i++)
		of_node_put(codecs[i].np);

	return ret;
}

static void pistachio_card_add_i2s_clk_route(struct pistachio_card *pbc,
					     struct pistachio_i2s_mclk *mclk,
					     char *cpu_dai_wname)
{
	struct snd_soc_dapm_route *route;

	/*
	 * Add a route connecting the clock supply widget to the i2s
	 * Playback/Capture widget if the mclk is used in this path
	 */
	if (!mclk->mclk)
		return;

	route = &pbc->routes[pbc->card.num_dapm_routes++];
	route->source = mclk->mclk->name;
	route->sink = cpu_dai_wname;
}

static void pistachio_card_add_i2s_routes(struct pistachio_card *pbc,
					  struct pistachio_i2s *i2s)
{
	char *cpu_dai_wname;
	struct snd_soc_dapm_route *route;

	route = &pbc->routes[pbc->card.num_dapm_routes++];

	/*
	 * dapm requires a full path (source to sink) for the clock supply
	 * widgets to turn on/off as expected. Create routes linking the
	 * i2s Playback/Capture widgets to Inputs/Outputs as required to
	 * create these paths
	 */
	if (i2s == &pbc->i2s_out->i2s) {
		cpu_dai_wname = PISTACHIO_I2S_OUT_PREFIX " Playback";
		route->source = cpu_dai_wname;
		route->sink = PISTACHIO_I2S_OUTPUT_NAME;
	} else {
		cpu_dai_wname = PISTACHIO_I2S_IN_PREFIX " Capture";
		route->source = PISTACHIO_I2S_INPUT_NAME;
		route->sink = cpu_dai_wname;
	}

	pistachio_card_add_i2s_clk_route(pbc, &i2s->mclk_a, cpu_dai_wname);

	pistachio_card_add_i2s_clk_route(pbc, &i2s->mclk_b, cpu_dai_wname);
}

static int pistachio_card_parse_i2s_out(struct device_node *i2s_out_np,
		struct pistachio_card *pbc, struct snd_soc_dai_link *link,
		struct pistachio_i2s_codec_info *codec_info)
{
	struct device *dev = pbc->card.dev;
	struct device_node *np;
	unsigned int fmt;
	int ret;
	struct pistachio_i2s_mclk_info i2s_mclk_info;
	struct pistachio_i2s_mclk_info dac_mclk_info;

	pbc->i2s_out = devm_kzalloc(dev, sizeof(*pbc->i2s_out), GFP_KERNEL);
	if (!pbc->i2s_out)
		return -ENOMEM;

	link->name = link->stream_name = "pistachio-i2s-out";

	np = of_parse_phandle(i2s_out_np, "cpu-dai", 0);
	if (!np) {
		dev_err(dev, "Failed to parse cpu-dai (%s)", i2s_out_np->name);
		return -EINVAL;
	}

	link->cpu_of_node = np;
	link->platform_of_node = np;

	fmt = snd_soc_of_parse_daifmt(i2s_out_np, NULL, NULL, NULL);
	fmt &= ~SND_SOC_DAIFMT_MASTER_MASK;
	fmt |= SND_SOC_DAIFMT_CBS_CFS;
	link->dai_fmt = fmt;

	/*
	 * Internal i2s out controller uses i2s_mclk and
	 * accepts 256fs,384fs
	 */
	i2s_mclk_info.fs_rates[0] = 256;
	i2s_mclk_info.fs_rates[1] = 384;
	i2s_mclk_info.num_fs_rates = 2;
	i2s_mclk_info.min_rate = 0;
	i2s_mclk_info.max_rate = PISTACHIO_I2S_MCLK_MAX_FREQ;
	dac_mclk_info.num_fs_rates = 0;
	dac_mclk_info.min_rate = 0;
	dac_mclk_info.max_rate = PISTACHIO_DAC_MCLK_MAX_FREQ;

	codec_info->bitclock_master_idx = 0;
	codec_info->frame_master_idx = 0;

	ret = pistachio_card_parse_i2s_common(i2s_out_np, pbc,
			&pbc->i2s_out->i2s, link, codec_info,
			&i2s_mclk_info, &dac_mclk_info);
	if (ret)
		return ret;

	pistachio_card_add_i2s_routes(pbc, &pbc->i2s_out->i2s);

	link->init = pistachio_card_i2s_out_link_init;
	link->ops = &pistachio_card_i2s_out_ops;

	return 0;
}

static int pistachio_card_parse_i2s_in(struct device_node *i2s_in_np,
		struct pistachio_card *pbc, struct snd_soc_dai_link *link,
		bool i2s_loopback, struct pistachio_i2s_codec_info *codec_info)
{
	int ret;
	struct device *dev = pbc->card.dev;
	unsigned int fmt;
	struct device_node *np;
	struct pistachio_i2s_mclk_info i2s_mclk_info;
	struct pistachio_i2s_mclk_info dac_mclk_info;

	pbc->i2s_in = devm_kzalloc(dev, sizeof(*pbc->i2s_in), GFP_KERNEL);
	if (!pbc->i2s_in)
		return -ENOMEM;

	link->name = link->stream_name = "pistachio-i2s-in";

	np = of_parse_phandle(i2s_in_np, "cpu-dai", 0);
	if (!np) {
		dev_err(dev, "Failed to parse cpu-dai (%s)", i2s_in_np->name);
		return -EINVAL;
	}

	link->cpu_of_node = np;
	link->platform_of_node = np;

	fmt = snd_soc_of_parse_daifmt(i2s_in_np, NULL, NULL, NULL);
	fmt &= ~SND_SOC_DAIFMT_MASTER_MASK;
	pbc->i2s_in->fmt = fmt;

	i2s_mclk_info.num_fs_rates = 0;
	i2s_mclk_info.min_rate = 0;
	i2s_mclk_info.max_rate = PISTACHIO_I2S_MCLK_MAX_FREQ;
	dac_mclk_info.num_fs_rates = 0;
	dac_mclk_info.min_rate = 0;
	dac_mclk_info.max_rate = PISTACHIO_DAC_MCLK_MAX_FREQ;

	codec_info->bitclock_master_idx = -1;
	codec_info->frame_master_idx = -1;

	ret = pistachio_card_parse_i2s_common(i2s_in_np, pbc,
			&pbc->i2s_in->i2s, link, codec_info,
			&i2s_mclk_info, &dac_mclk_info);
	if (ret)
		return ret;

	if (i2s_loopback) {
		pbc->i2s_in->frame_master = PISTACHIO_CLOCK_MASTER_LOOPBACK;
		pbc->i2s_in->bitclock_master = PISTACHIO_CLOCK_MASTER_LOOPBACK;
	} else if ((codec_info->bitclock_master_idx == -1) ||
		   (codec_info->frame_master_idx == -1)) {
		pbc->i2s_in->frame_master = PISTACHIO_CLOCK_MASTER_EXT;
		pbc->i2s_in->bitclock_master = PISTACHIO_CLOCK_MASTER_EXT;
	} else {
		pbc->i2s_in->frame_master = codec_info->frame_master_idx;
		pbc->i2s_in->bitclock_master = codec_info->bitclock_master_idx;
	}

	pistachio_card_add_i2s_routes(pbc, &pbc->i2s_in->i2s);

	link->init = pistachio_card_i2s_in_link_init;

	/*
	 * If no mclks are used by i2s in, there is nothing for
	 * the ops callbacks to do, so leave this as NULL
	 */
	if (pbc->i2s_in->i2s.mclk_a.mclk)
		link->ops = &pistachio_card_i2s_in_ops;

	return 0;
}

static int pistachio_card_prefixes(struct pistachio_card *pbc,
	struct pistachio_i2s_codec_info *codec_info,
	struct snd_soc_dai_link *i2s_out, struct snd_soc_dai_link *i2s_in,
	struct snd_soc_dai_link *parallel_out)
{
	int i, j, n;
	unsigned int size;
	struct pistachio_i2s_codec_info_s *codecs;
	struct snd_soc_codec_conf *conf, *c;

	n = codec_info->unique_codecs;

	if (parallel_out)
		n++;

	if (i2s_out)
		n++;

	if (i2s_in)
		n++;

	codecs = codec_info->codecs;

	size = sizeof(*pbc->card.codec_conf) * n;
	pbc->card.codec_conf = devm_kzalloc(pbc->card.dev, size, GFP_KERNEL);
	if (!pbc->card.codec_conf)
		return -ENOMEM;

	conf = pbc->card.codec_conf;

	/* Create prefixes for unique codecs only */
	for (i = 0; i < codec_info->total_codecs; i++) {
		for (j = 0; j < i; j++)
			if (codecs[j].np == codecs[i].np)
				break;
		if (j == i) {
			conf->of_node = codecs[i].np;
			conf->name_prefix = codecs[i].prefix;
			conf++;
		}
	}

	if (i2s_out) {
		conf->of_node = i2s_out->cpu_of_node;
		conf->name_prefix = PISTACHIO_I2S_OUT_PREFIX;
		conf++;
	}

	if (i2s_in) {
		conf->of_node = i2s_in->cpu_of_node;
		conf->name_prefix = PISTACHIO_I2S_IN_PREFIX;
		conf++;
	}

	if (parallel_out) {
		conf->of_node = parallel_out->codecs[0].of_node;
		conf->name_prefix = PISTACHIO_INTERNAL_DAC_PREFIX;
		conf++;
	}

	pbc->card.num_configs = n;

	/* Check for prefix clashes */
	for (i = 0; i < n; i++) {
		conf = &pbc->card.codec_conf[i];
		for (j = i + 1; j < n; j++) {
			c = &pbc->card.codec_conf[j];
			if (!strcasecmp(conf->name_prefix, c->name_prefix)) {
				dev_err(pbc->card.dev, "Prefix clash: %s\n",
					conf->name_prefix);
				return -EINVAL;
			}
		}
	}

	return 0;
}

static int pistachio_card_parse_of(struct device_node *node,
				   struct pistachio_card *pbc)
{
	int ret = 0;
	unsigned int size, gpio;
	struct device_node *spdif_out_np, *spdif_in_np, *parallel_out_np;
	struct device_node *i2s_out_np, *i2s_in_np;
	struct snd_soc_dai_link *link, *prl_out, *i2s_out, *i2s_in;
	enum of_gpio_flags flags;
	struct pistachio_i2s_codec_info codec_info;
	bool i2s_loopback;
	struct device *dev = pbc->card.dev;

	pbc->periph_regs = syscon_regmap_lookup_by_phandle(node, "img,cr-periph");
	if (IS_ERR(pbc->periph_regs)) {
		dev_err(dev, "syscon_regmap_lookup_by_phandle failed: %ld\n",
			PTR_ERR(pbc->periph_regs));
		return PTR_ERR(pbc->periph_regs);
	}

	if (of_property_read_bool(node, "img,widgets")) {
		ret = snd_soc_of_parse_audio_simple_widgets(&pbc->card, "img,widgets");
		if (ret) {
			dev_err(dev, "img,widgets parse failed: %d\n", ret);
			return ret;
		}
	}

	if (of_property_read_bool(node, "img,routing")) {
		ret = snd_soc_of_parse_audio_routing(&pbc->card, "img,routing");
		if (ret) {
			dev_err(dev, "img,routing parse failed: %d\n", ret);
			return ret;
		}
	}

	spdif_out_np = of_get_child_by_name(node, "spdif-out");
	if (spdif_out_np)
		pbc->card.num_links++;

	spdif_in_np = of_get_child_by_name(node, "spdif-in");
	if (spdif_in_np)
		pbc->card.num_links++;

	parallel_out_np = of_get_child_by_name(node, "parallel-out");
	if (parallel_out_np)
		pbc->card.num_links++;

	i2s_out_np = of_get_child_by_name(node, "i2s-out");
	if (i2s_out_np)
		pbc->card.num_links++;

	i2s_in_np = of_get_child_by_name(node, "i2s-in");
	if (i2s_in_np)
		pbc->card.num_links++;

	i2s_loopback = of_property_read_bool(node, "img,i2s-clk-loopback");
	if (i2s_loopback && (!i2s_out_np || !i2s_in_np)) {
		dev_err(dev, "img,i2s-clk-loopback specified when i2s-out/i2s-in are not present\n");
		ret = -EINVAL;
		goto end;
	}

	if (!pbc->card.num_links) {
		dev_err(dev, "No dai links on card\n");
		ret = -EINVAL;
		goto end;
	}

	size = sizeof(*pbc->card.dai_link) * pbc->card.num_links;
	pbc->card.dai_link = devm_kzalloc(pbc->card.dev, size, GFP_KERNEL);
	if (!pbc->card.dai_link) {
		ret = -ENOMEM;
		goto end;
	}

	codec_info.total_codecs = 0;
	codec_info.unique_codecs = 0;

	link = pbc->card.dai_link;

	if (spdif_out_np) {
		ret = pistachio_card_parse_spdif_out(spdif_out_np, pbc, link);
		if (ret)
			goto end;
		link++;
	}

	if (spdif_in_np) {
		ret = pistachio_card_parse_spdif_in(spdif_in_np, pbc, link);
		if (ret)
			goto end;
		link++;
	}

	if (parallel_out_np) {
		ret = pistachio_card_parse_parallel_out(parallel_out_np, pbc,
							link);
		if (ret)
			goto end;

		prl_out = link++;
	} else {
		prl_out = NULL;
	}

	if (i2s_out_np) {
		ret = pistachio_card_parse_i2s_out(i2s_out_np, pbc, link,
						   &codec_info);
		if (ret)
			goto end;

		i2s_out = link++;
	} else {
		i2s_out = NULL;
	}

	if (i2s_in_np) {
		ret = pistachio_card_parse_i2s_in(i2s_in_np, pbc, link,
						  i2s_loopback, &codec_info);
		if (ret)
			goto end;

		i2s_in = link;
	} else {
		i2s_in = NULL;
	}

	ret = pistachio_card_prefixes(pbc, &codec_info, i2s_out,
				      i2s_in, prl_out);
	if (ret)
		goto end;

	gpio = of_get_named_gpio_flags(node, "img,hp-det-gpio", 0, &flags);
	pbc->hp_jack_gpio.gpio = gpio;
	pbc->hp_jack_gpio.invert = !!(flags & OF_GPIO_ACTIVE_LOW);
	if (pbc->hp_jack_gpio.gpio == -EPROBE_DEFER) {
		ret = -EPROBE_DEFER;
		goto end;
	}

	gpio = of_get_named_gpio_flags(node, "img,mute-gpio", 0, &flags);
	pbc->mute_gpio = gpio;
	pbc->mute_gpio_inverted = !!(flags & OF_GPIO_ACTIVE_LOW);
	if (pbc->mute_gpio == -EPROBE_DEFER)
		ret = -EPROBE_DEFER;

end:
	if (spdif_out_np)
		of_node_put(spdif_out_np);
	if (spdif_in_np)
		of_node_put(spdif_in_np);
	if (parallel_out_np)
		of_node_put(parallel_out_np);
	if (i2s_out_np)
		of_node_put(i2s_out_np);
	if (i2s_in_np)
		of_node_put(i2s_in_np);

	return ret;
}

static void pistachio_card_unref(struct pistachio_card *pbc)
{
	int i, j;
	struct snd_soc_dai_link *link;

	link = pbc->card.dai_link;
	if (!link)
		return;

	for (i = 0; i < pbc->card.num_links; i++, link++) {
		if (link->cpu_of_node)
			of_node_put(link->cpu_of_node);
		for (j = 0; j < link->num_codecs; j++)
			of_node_put(link->codecs[j].of_node);
	}
}

static int pistachio_card_init_clk(struct device *dev, char *name,
				   struct clk **pclk, unsigned int rate)
{
	struct clk *clk;
	int ret;

	clk = devm_clk_get(dev, name);
	if (IS_ERR(clk)) {
		ret = PTR_ERR(clk);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "devm_clk_get failed for %s: %d",
				name, ret);
		return ret;
	}

	ret = clk_set_rate(clk, rate);
	if (ret) {
		dev_err(dev, "clk_set_rate(%s, %u) failed: %d",
			name, rate, ret);
		return ret;
	}

	*pclk = clk;

	return 0;
}

static int pistachio_card_get_mute(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct pistachio_card *pbc = snd_soc_card_get_drvdata(card);
	int ret;

	ret = gpio_get_value_cansleep(pbc->mute_gpio);
	if (ret < 0)
		return ret;

	if (pbc->mute_gpio_inverted)
		ucontrol->value.integer.value[0] = !ret;
	else
		ucontrol->value.integer.value[0] = !!ret;

	return 0;
}

static int pistachio_card_set_mute(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct pistachio_card *pbc = snd_soc_card_get_drvdata(card);
	int val;

	if (pbc->mute_gpio_inverted)
		val = !ucontrol->value.integer.value[0];
	else
		val = ucontrol->value.integer.value[0];

	gpio_set_value_cansleep(pbc->mute_gpio, val);

	return 0;
}

static int pistachio_card_info_sample_rates(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 2;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 192000;

	return 0;
}

static int pistachio_card_set_sample_rates_mclk(struct pistachio_card *pbc,
		    struct pistachio_mclk *mclk, unsigned int i2s_out_rate,
		    unsigned int i2s_in_rate)
{
	struct pistachio_i2s_mclk *mclk_a, *mclk_b;
	unsigned int rate_a, rate_b;

	mclk_a = NULL;
	mclk_b = NULL;
	rate_a = i2s_out_rate;
	rate_b = i2s_in_rate;

	if (i2s_out_rate) {
		if (pbc->i2s_out->i2s.mclk_a.mclk == mclk)
			mclk_a = &pbc->i2s_out->i2s.mclk_a;
		else if (pbc->i2s_out->i2s.mclk_b.mclk == mclk)
			mclk_a = &pbc->i2s_out->i2s.mclk_b;
	}
	if (i2s_in_rate) {
		if (pbc->i2s_in->i2s.mclk_a.mclk == mclk)
			mclk_b = &pbc->i2s_in->i2s.mclk_a;
		else if (pbc->i2s_in->i2s.mclk_b.mclk == mclk)
			mclk_b = &pbc->i2s_in->i2s.mclk_b;
	}

	if (!mclk_a) {
		mclk_a = mclk_b;
		rate_a = rate_b;
		mclk_b = NULL;
	}

	if (mclk_a)
		return pistachio_card_update_mclk(pbc, mclk_a, mclk_b,
						  rate_a, rate_b);

	return 0;
}

static int pistachio_card_set_sample_rates(struct snd_kcontrol *kcontrol,
					   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct pistachio_card *pbc = snd_soc_card_get_drvdata(card);
	int ret;
	unsigned int pll_rate, i2s_out_rate, i2s_in_rate;
	struct device *dev = pbc->card.dev;

	i2s_out_rate = 0;
	i2s_in_rate = 0;

	if (pbc->i2s_out)
		i2s_out_rate = ucontrol->value.integer.value[0];

	if (pbc->i2s_in && pbc->i2s_in->i2s.mclk_a.mclk)
		i2s_in_rate = ucontrol->value.integer.value[1];

	if (!i2s_out_rate && !i2s_in_rate)
		return 0;

	pll_rate = 0;

	if (i2s_out_rate) {
		ret = pistachio_card_get_pll_rate(i2s_out_rate, dev);
		if (ret < 0)
			return ret;

		pll_rate = ret;
	}

	if (i2s_in_rate) {
		ret = pistachio_card_get_pll_rate(i2s_in_rate, dev);
		if (ret < 0)
			return ret;

		if (pll_rate && (ret != pll_rate)) {
			dev_err(dev, "Conflicting pll rate requirements\n");
			return -EINVAL;
		}

		pll_rate = ret;
	}

	mutex_lock(&pbc->rate_mutex);

	if (pbc->audio_pll_rate != pll_rate) {
		ret = pistachio_card_set_pll_rate(pbc, pll_rate);
		if (ret) {
			mutex_unlock(&pbc->rate_mutex);
			return ret;
		}
	}

	ret = pistachio_card_set_sample_rates_mclk(pbc, &pbc->i2s_mclk,
						   i2s_out_rate, i2s_in_rate);
	if (ret) {
		mutex_unlock(&pbc->rate_mutex);
		return ret;
	}

	ret = pistachio_card_set_sample_rates_mclk(pbc, &pbc->dac_mclk,
						   i2s_out_rate, i2s_in_rate);

	mutex_unlock(&pbc->rate_mutex);

	return ret;
}

static struct snd_kcontrol_new pistachio_controls[] = {
	{
		.access = SNDRV_CTL_ELEM_ACCESS_WRITE,
		.iface = SNDRV_CTL_ELEM_IFACE_CARD,
		.name = "I2S Rates",
		.info = pistachio_card_info_sample_rates,
		.put = pistachio_card_set_sample_rates
	},
};

static int pistachio_card_i2s_clk_cb(struct notifier_block *nb,
				     unsigned long event, void *data)
{
	struct clk_notifier_data *ndata = data;
	struct pistachio_card *pbc;

	pbc = container_of(nb, struct pistachio_card, i2s_clk_notifier);

	switch (event) {
	case PRE_RATE_CHANGE:
		/* Allow changes made by the card driver only */
		if (ndata->new_rate == pbc->i2s_mclk.cur_rate)
			return NOTIFY_OK;
		else
			return NOTIFY_STOP;
	case POST_RATE_CHANGE:
	case ABORT_RATE_CHANGE:
		return NOTIFY_OK;
	default:
		return NOTIFY_DONE;
	}
}

#ifdef DEBUG

static void pistachio_card_info_fmt(struct pistachio_card *pbc,
				    unsigned int fmt)
{
	struct device *dev = pbc->card.dev;
	char *text_a, *text_b;

	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) == SND_SOC_DAIFMT_I2S)
		text_a = "I2S";
	else
		text_a = "Left Justified";
	dev_dbg(dev, "    Format: %s\n", text_a);

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		text_a = "No";
		text_b = "No";
		break;
	case SND_SOC_DAIFMT_NB_IF:
		text_a = "Yes";
		text_b = "No";
		break;
	case SND_SOC_DAIFMT_IB_NF:
		text_a = "No";
		text_b = "Yes";
		break;
	default:
		text_a = "Yes";
		text_b = "Yes";
		break;
	}
	dev_dbg(dev, "    Frame Clock Inverted: %s\n", text_a);
	dev_dbg(dev, "    Bit Clock Inverted: %s\n", text_b);

	if ((fmt & SND_SOC_DAIFMT_CLOCK_MASK) == SND_SOC_DAIFMT_CONT)
		text_a = "Yes";
	else
		text_a = "No";
	dev_dbg(dev, "    Continuous Clock: %s\n", text_a);
}

static void pistachio_card_info_mclk(struct pistachio_card *pbc,
				     struct pistachio_i2s_mclk *mclk)
{
	struct device *dev = pbc->card.dev;
	int i;

	dev_dbg(dev, "        Min FS Freq: %u\n", mclk->min_rate);
	dev_dbg(dev, "        Max FS Freq: %u\n", mclk->max_rate);
	dev_dbg(dev, "        FS Rates:\n");

	for (i = 0; i < mclk->num_fs_rates; i++)
		dev_dbg(dev, "            %u\n", mclk->fs_rates[i]);
}

static void pistachio_card_info_mclks(struct pistachio_card *pbc,
				      struct pistachio_i2s *i2s)
{
	struct pistachio_i2s_mclk *i2s_mclk;
	struct pistachio_i2s_mclk *dac_mclk;
	struct device *dev = pbc->card.dev;

	if (i2s->mclk_a.mclk == &pbc->i2s_mclk)
		i2s_mclk = &i2s->mclk_a;
	else if (pbc->i2s_in->i2s.mclk_b.mclk == &pbc->i2s_mclk)
		i2s_mclk = &i2s->mclk_b;
	else
		i2s_mclk = NULL;

	if (i2s_mclk) {
		dev_dbg(dev, "    I2S MCLK\n");
		pistachio_card_info_mclk(pbc, i2s_mclk);
	} else {
		dev_dbg(dev, "    I2S MCLK NOT USED\n");
	}

	dev_dbg(dev, "\n");

	if (i2s->mclk_a.mclk == &pbc->dac_mclk)
		dac_mclk = &i2s->mclk_a;
	else if (i2s->mclk_b.mclk == &pbc->dac_mclk)
		dac_mclk = &i2s->mclk_b;
	else
		dac_mclk = NULL;

	if (dac_mclk) {
		dev_dbg(dev, "    DAC MCLK\n");
		pistachio_card_info_mclk(pbc, dac_mclk);
	} else {
		dev_dbg(dev, "    DAC MCLK NOT USED\n");
	}
}

static void pistachio_card_info_i2s_out(struct pistachio_card *pbc,
					struct snd_soc_dai_link *link)
{
	int i, j;
	struct snd_soc_dai_link_component *components;
	struct snd_soc_codec_conf *confs;
	struct device *dev = pbc->card.dev;
	char *text;

	components = pbc->i2s_out->i2s.components;
	confs = pbc->card.codec_conf;

	dev_dbg(dev, "I2S OUT\n");
	dev_dbg(dev, "\n");
	if (pbc->i2s_in && (pbc->i2s_in->frame_master ==
			    PISTACHIO_CLOCK_MASTER_LOOPBACK))
		text = "(Dual Frame + Bit Clock Master)";
	else
		text = "(Frame + Bit Clock Master)";
	dev_dbg(dev, "    CPU DAI\n");
	dev_dbg(dev, "        i2s-out (%s) %s\n",
		link->cpu_of_node->name, text);
	dev_dbg(dev, "\n");
	dev_dbg(dev, "    CODECS\n");

	for (i = 0; i < pbc->i2s_out->i2s.num_codecs; i++) {
		for (j = 0; j < pbc->card.num_configs; j++)
			if (confs[j].of_node == components[i].of_node)
				break;

		dev_dbg(dev, "        %s (%s) (%s)\n", confs[j].name_prefix,
			confs[j].of_node->name, components[i].dai_name);
	}
	dev_dbg(dev, "\n");

	pistachio_card_info_mclks(pbc, &pbc->i2s_out->i2s);

	dev_dbg(dev, "\n");

	pistachio_card_info_fmt(pbc, link->dai_fmt);

	dev_dbg(dev, "\n");
}

static void pistachio_card_info_i2s_in(struct pistachio_card *pbc,
				       struct snd_soc_dai_link *link)
{
	int i, j;
	struct snd_soc_dai_link_component *components;
	struct snd_soc_codec_conf *confs;
	char *text;
	struct device *dev = pbc->card.dev;

	components = pbc->i2s_in->i2s.components;
	confs = pbc->card.codec_conf;

	dev_dbg(dev, "I2S IN\n");
	dev_dbg(dev, "\n");
	dev_dbg(dev, "    CPU DAI\n");
	dev_dbg(dev, "        i2s-in (%s)\n", link->cpu_of_node->name);
	dev_dbg(dev, "\n");
	dev_dbg(dev, "    CODECS\n");

	for (i = 0; i < pbc->i2s_in->i2s.num_codecs; i++) {
		for (j = 0; j < pbc->card.num_configs; j++)
			if (confs[j].of_node == components[i].of_node)
				break;

		if (i == pbc->i2s_in->frame_master)
			if (i == pbc->i2s_in->bitclock_master)
				text = "(Frame + Bit Clock Master)";
			else
				text = "(Frame Master)";
		else
			if (i == pbc->i2s_in->bitclock_master)
				text = "(Bitclock Master)";
			else
				text = "";

		dev_dbg(dev, "        %s (%s) (%s) %s\n", confs[j].name_prefix,
			confs[j].of_node->name, components[i].dai_name, text);
	}
	dev_dbg(dev, "\n");

	pistachio_card_info_mclks(pbc, &pbc->i2s_in->i2s);

	dev_dbg(dev, "\n");

	pistachio_card_info_fmt(pbc, pbc->i2s_in->fmt);

	dev_dbg(dev, "\n");
}

static void pistachio_card_info(struct pistachio_card *pbc)
{
	struct device *dev = pbc->card.dev;
	struct snd_soc_codec_conf *conf;
	struct snd_soc_dai_link *link;
	char *text;

	link = pbc->card.dai_link;

	dev_dbg(dev, "\n");
	dev_dbg(dev, "####################################################\n");
	dev_dbg(dev, "\n");
	dev_dbg(dev, "Pistachio Audio Card\n");
	dev_dbg(dev, "\n");

	if (pbc->spdif_out) {
		dev_dbg(dev, "SPDIF OUT\n");
		dev_dbg(dev, "\n");
		dev_dbg(dev, "    CPU DAI\n");
		dev_dbg(dev, "        spdif-out (%s)\n",
			link->cpu_of_node->name);
		dev_dbg(dev, "\n");
		link++;
	}
	if (pbc->spdif_in) {
		dev_dbg(dev, "SPDIF IN\n");
		dev_dbg(dev, "\n");
		dev_dbg(dev, "    CPU DAI\n");
		dev_dbg(dev, "        spdif-in (%s)\n",
			link->cpu_of_node->name);
		dev_dbg(dev, "\n");
		link++;
	}
	if (pbc->parallel_out) {
		dev_dbg(dev, "PARALLEL OUT\n");
		dev_dbg(dev, "\n");
		dev_dbg(dev, "    CPU DAI\n");
		dev_dbg(dev, "        parallel-out (%s)\n",
			link->cpu_of_node->name);
		dev_dbg(dev, "\n");
		dev_dbg(dev, "    CODEC\n");
		conf = &pbc->card.codec_conf[pbc->card.num_configs - 1];
		dev_dbg(dev, "        %s (%s) (%s)\n", conf->name_prefix,
			conf->of_node->name,
			pbc->parallel_out->component.dai_name);
		dev_dbg(dev, "\n");
		link++;
	}
	if (pbc->i2s_out) {
		pistachio_card_info_i2s_out(pbc, link);
		link++;
	}
	if (pbc->i2s_in)
		pistachio_card_info_i2s_in(pbc, link);

	dev_dbg(dev, "I2S MCLK Max Freq: %u\n", pbc->i2s_mclk.max_rate);
	dev_dbg(dev, "DAC MCLK Max Freq: %u\n", pbc->dac_mclk.max_rate);
	dev_dbg(dev, "\n");

	if (gpio_is_valid(pbc->mute_gpio)) {
		if (pbc->mute_gpio_inverted)
			text = "(Active Low)";
		else
			text = "(Active High)";
		dev_dbg(dev, "Mute: GPIO %u %s\n", pbc->mute_gpio, text);
	}
	if (gpio_is_valid(pbc->hp_jack_gpio.gpio)) {
		if (pbc->hp_jack_gpio.invert)
			text = "(Active Low)";
		else
			text = "(Active High)";
		dev_dbg(dev, "Headphone-Detect: GPIO %u %s\n",
				pbc->hp_jack_gpio.gpio, text);
	}
	dev_dbg(dev, "\n");
	dev_dbg(dev, "####################################################\n");
	dev_dbg(dev, "\n");
}

#endif

static int pistachio_card_probe(struct platform_device *pdev)
{
	struct pistachio_card *pbc;
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	int ret;
	unsigned long gpio_flags;
	struct snd_kcontrol_new *control;

	if (!np || !of_device_is_available(np))
		return -EINVAL;

	pbc = devm_kzalloc(dev, sizeof(*pbc), GFP_KERNEL);
	if (!pbc)
		return -ENOMEM;

	snd_soc_card_set_drvdata(&pbc->card, pbc);

	pbc->card.owner = THIS_MODULE;
	pbc->card.dev = dev;
	pbc->card.name = "pistachio-card";

	pbc->i2s_mclk.name = PISTACHIO_I2S_MCLK_NAME;
	pbc->i2s_mclk.max_rate = PISTACHIO_I2S_MCLK_MAX_FREQ;
	pbc->dac_mclk.name = PISTACHIO_DAC_MCLK_NAME;
	pbc->dac_mclk.max_rate = PISTACHIO_DAC_MCLK_MAX_FREQ;

	mutex_init(&pbc->rate_mutex);

	pbc->hp_jack_gpio.gpio = -ENOENT;
	pbc->mute_gpio = -ENOENT;

	pbc->card.dapm_widgets = pistachio_card_widgets;
	pbc->card.num_dapm_widgets = ARRAY_SIZE(pistachio_card_widgets);

	pbc->card.dapm_routes = pbc->routes;

	ret = pistachio_card_parse_of(np, pbc);
	if (ret)
		goto err;

	pbc->audio_pll_rate = PISTACHIO_PLL_RATE_B;
	ret = pistachio_card_init_clk(dev, "audio_pll", &pbc->audio_pll,
				      pbc->audio_pll_rate);
	if (ret)
		goto err;

	pbc->i2s_mclk.cur_rate = PISTACHIO_MIN_MCLK_FREQ;
	ret = pistachio_card_init_clk(dev, PISTACHIO_I2S_MCLK_NAME,
				      &pbc->i2s_mclk.mclk,
				      pbc->i2s_mclk.cur_rate);
	if (ret)
		goto err;

	pbc->dac_mclk.cur_rate = PISTACHIO_MIN_MCLK_FREQ;
	ret = pistachio_card_init_clk(dev, PISTACHIO_DAC_MCLK_NAME,
				      &pbc->dac_mclk.mclk,
				      pbc->dac_mclk.cur_rate);
	if (ret)
		goto err;

	pbc->i2s_clk_notifier.notifier_call = pistachio_card_i2s_clk_cb;
	ret = clk_notifier_register(pbc->i2s_mclk.mclk,
				    &pbc->i2s_clk_notifier);
	if (ret) {
		dev_err(dev, "clk_notifier_register failed: %d", ret);
		goto err;
	}

	ret = devm_snd_soc_register_card(dev, &pbc->card);
	if (ret) {
		dev_err(dev, "devm_snd_soc_register_card failed: %d", ret);
		goto err_notifier;
	}

	ret = snd_soc_add_card_controls(&pbc->card, pistachio_controls,
					ARRAY_SIZE(pistachio_controls));
	if (ret) {
		dev_err(dev, "snd_soc_add_card_controls failed: %d", ret);
		goto err_notifier;
	}

	if (gpio_is_valid(pbc->hp_jack_gpio.gpio)) {
		pbc->hp_jack_pin.pin = "Headphones";
		pbc->hp_jack_pin.mask = SND_JACK_HEADPHONE;
		pbc->hp_jack_gpio.name = "Headphone detection";
		pbc->hp_jack_gpio.report = SND_JACK_HEADPHONE;
		pbc->hp_jack_gpio.debounce_time = 150;
		ret = snd_soc_card_jack_new(&pbc->card, "Headphones",
					    SND_JACK_HEADPHONE, &pbc->hp_jack,
					    &pbc->hp_jack_pin, 1);
		if (ret) {
			dev_err(dev, "snd_soc_card_jack_new failed: %d", ret);
			goto err_notifier;
		}
		ret = snd_soc_jack_add_gpios(&pbc->hp_jack, 1,
					     &pbc->hp_jack_gpio);
		if (ret) {
			dev_err(dev, "snd_soc_jack_add_gpios failed: %d", ret);
			goto err_notifier;
		}
	}

	if (gpio_is_valid(pbc->mute_gpio)) {
		if (pbc->mute_gpio_inverted)
			gpio_flags = GPIOF_OUT_INIT_HIGH;
		else
			gpio_flags = GPIOF_OUT_INIT_LOW;
		ret = gpio_request_one(pbc->mute_gpio, gpio_flags, "Mute");
		if (ret) {
			dev_err(dev, "gpio_request_one failed: %d", ret);
			goto err_jack;
		}
		control = devm_kzalloc(dev, sizeof(*control), GFP_KERNEL);
		if (!control) {
			ret = -ENOMEM;
			goto err_mute;
		}
		control->access = SNDRV_CTL_ELEM_ACCESS_READWRITE;
		control->iface = SNDRV_CTL_ELEM_IFACE_CARD;
		control->name = "Mute Switch";
		control->info = snd_ctl_boolean_mono_info;
		control->get = pistachio_card_get_mute;
		control->put = pistachio_card_set_mute;
		ret = snd_soc_add_card_controls(&pbc->card, control, 1);
		if (ret) {
			dev_err(dev, "mute control add failed: %d", ret);
			goto err_mute;
		}
	}

#ifdef	DEBUG
	pistachio_card_info(pbc);
#endif

	return 0;

err_mute:
	if (gpio_is_valid(pbc->mute_gpio))
		gpio_free(pbc->mute_gpio);
err_jack:
	if (gpio_is_valid(pbc->hp_jack_gpio.gpio))
		snd_soc_jack_free_gpios(&pbc->hp_jack, 1, &pbc->hp_jack_gpio);
err_notifier:
	clk_notifier_unregister(pbc->i2s_mclk.mclk, &pbc->i2s_clk_notifier);
err:
	pistachio_card_unref(pbc);

	return ret;
}

static int pistachio_card_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct pistachio_card *pbc = snd_soc_card_get_drvdata(card);

	if (gpio_is_valid(pbc->mute_gpio))
		gpio_free(pbc->mute_gpio);
	if (gpio_is_valid(pbc->hp_jack_gpio.gpio))
		snd_soc_jack_free_gpios(&pbc->hp_jack, 1, &pbc->hp_jack_gpio);
	clk_notifier_unregister(pbc->i2s_mclk.mclk, &pbc->i2s_clk_notifier);
	pistachio_card_unref(pbc);

	return 0;
}

static const struct of_device_id pistachio_card_of_match[] = {
	{ .compatible = "img,pistachio-audio" },
	{},
};
MODULE_DEVICE_TABLE(of, pistachio_card_of_match);

static struct platform_driver pistachio_card = {
	.driver = {
		.name = "pistachio-card",
		.of_match_table = pistachio_card_of_match,
	},
	.probe = pistachio_card_probe,
	.remove = pistachio_card_remove,
};
module_platform_driver(pistachio_card);

MODULE_DESCRIPTION("Pistachio audio card driver");
MODULE_AUTHOR("Damien Horsley <Damien.Horsley@imgtec.com>");
MODULE_LICENSE("GPL v2");
