/*
 * ALSA SoC codec for HDMI encoder drivers
 * Copyright (C) 2015 Texas Instruments Incorporated - http://www.ti.com/
 * Author: Jyri Sarha <jsarha@ti.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * General Public License for more details.
 */
#include <linux/module.h>
#include <linux/string.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>
#include <sound/pcm_drm_eld.h>
#include <sound/hdmi-codec.h>
#include <sound/pcm_iec958.h>

#include <drm/drm_crtc.h> /* This is only to get MAX_ELD_BYTES */

struct hdmi_device {
	struct device *dev;
	struct list_head list;
	int cnt;
};
#define pos_to_hdmi_device(pos)	container_of((pos), struct hdmi_device, list)
LIST_HEAD(hdmi_device_list);

#define DAI_NAME_SIZE 16
#define HDMI_MAX_SPEAKERS  8

struct hdmi_codec_channel_map_table {
	unsigned char map;	/* ALSA API channel map position */
	unsigned long spk_mask;		/* speaker position bit mask */
};

/*
 * CEA speaker placement for HDMI 1.4:
 *
 *  FL  FLC   FC   FRC   FR   FRW
 *
 *                                  LFE
 *
 *  RL  RLC   RC   RRC   RR
 *
 *  Speaker placement has to be extended to support HDMI 2.0
 */
enum hdmi_codec_cea_spk_placement {
	FL  = BIT(0),	/* Front Left           */
	FC  = BIT(1),	/* Front Center         */
	FR  = BIT(2),	/* Front Right          */
	FLC = BIT(3),	/* Front Left Center    */
	FRC = BIT(4),	/* Front Right Center   */
	RL  = BIT(5),	/* Rear Left            */
	RC  = BIT(6),	/* Rear Center          */
	RR  = BIT(7),	/* Rear Right           */
	RLC = BIT(8),	/* Rear Left Center     */
	RRC = BIT(9),	/* Rear Right Center    */
	LFE = BIT(10),	/* Low Frequency Effect */
};

static const struct hdmi_codec_channel_map_table hdmi_codec_map_table[] = {
	{ SNDRV_CHMAP_FL,	FL },
	{ SNDRV_CHMAP_FR,	FR },
	{ SNDRV_CHMAP_RL,	RL },
	{ SNDRV_CHMAP_RR,	RR },
	{ SNDRV_CHMAP_LFE,	LFE },
	{ SNDRV_CHMAP_FC,	FC },
	{ SNDRV_CHMAP_RLC,	RLC },
	{ SNDRV_CHMAP_RRC,	RRC },
	{ SNDRV_CHMAP_RC,	RC },
	{ SNDRV_CHMAP_FLC,	FLC },
	{ SNDRV_CHMAP_FRC,	FRC },
	{} /* terminator */
};

/*
 * cea Speaker allocation structure
 */
struct hdmi_codec_cea_spk_alloc {
	const int ca_id;
	const unsigned long speakers[HDMI_MAX_SPEAKERS];

	/* Derived values, computed during init */
	unsigned int channels;
	unsigned long spks_mask;
	unsigned long spk_na_mask;
};

/* default HDMI channel maps is stereo */
const struct snd_pcm_chmap_elem hdmi_codec_stereo_chmaps[] = {
	{ .channels = 2,
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR } },
	{ }
};

/*
 * hdmi_codec_channel_alloc: speaker configuration available for CEA
 *
 * This is an ordered list!
 * The preceding ones have better chances to be selected by
 * hdmi_codec_get_ch_alloc_table_idx().
 */
static struct hdmi_codec_cea_spk_alloc hdmi_codec_channel_alloc[] = {
{ .ca_id = 0x00,  .speakers = {   0,    0,   0,   0,   0,    0,  FR,  FL } },
/* 2.1 */
{ .ca_id = 0x01,  .speakers = {   0,    0,   0,   0,   0,  LFE,  FR,  FL } },
/* Dolby Surround */
{ .ca_id = 0x02,  .speakers = {   0,    0,   0,   0,  FC,    0,  FR,  FL } },
/* surround51 */
{ .ca_id = 0x0b,  .speakers = {   0,    0,  RR,  RL,  FC,  LFE,  FR,  FL } },
/* surround40 */
{ .ca_id = 0x08,  .speakers = {   0,    0,  RR,  RL,   0,    0,  FR,  FL } },
/* surround41 */
{ .ca_id = 0x09,  .speakers = {   0,    0,  RR,  RL,   0,  LFE,  FR,  FL } },
/* surround50 */
{ .ca_id = 0x0a,  .speakers = {   0,    0,  RR,  RL,  FC,    0,  FR,  FL } },
/* 6.1 */
{ .ca_id = 0x0f,  .speakers = {   0,   RC,  RR,  RL,  FC,  LFE,  FR,  FL } },
/* surround71 */
{ .ca_id = 0x13,  .speakers = { RRC,  RLC,  RR,  RL,  FC,  LFE,  FR,  FL } },

{ .ca_id = 0x03,  .speakers = {   0,    0,   0,   0,  FC,  LFE,  FR,  FL } },
{ .ca_id = 0x04,  .speakers = {   0,    0,   0,  RC,   0,    0,  FR,  FL } },
{ .ca_id = 0x05,  .speakers = {   0,    0,   0,  RC,   0,  LFE,  FR,  FL } },
{ .ca_id = 0x06,  .speakers = {   0,    0,   0,  RC,  FC,    0,  FR,  FL } },
{ .ca_id = 0x07,  .speakers = {   0,    0,   0,  RC,  FC,  LFE,  FR,  FL } },
{ .ca_id = 0x0c,  .speakers = {   0,   RC,  RR,  RL,   0,    0,  FR,  FL } },
{ .ca_id = 0x0d,  .speakers = {   0,   RC,  RR,  RL,   0,  LFE,  FR,  FL } },
{ .ca_id = 0x0e,  .speakers = {   0,   RC,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_id = 0x10,  .speakers = { RRC,  RLC,  RR,  RL,   0,    0,  FR,  FL } },
{ .ca_id = 0x11,  .speakers = { RRC,  RLC,  RR,  RL,   0,  LFE,  FR,  FL } },
{ .ca_id = 0x12,  .speakers = { RRC,  RLC,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_id = 0x14,  .speakers = { FRC,  FLC,   0,   0,   0,    0,  FR,  FL } },
{ .ca_id = 0x15,  .speakers = { FRC,  FLC,   0,   0,   0,  LFE,  FR,  FL } },
{ .ca_id = 0x16,  .speakers = { FRC,  FLC,   0,   0,  FC,    0,  FR,  FL } },
{ .ca_id = 0x17,  .speakers = { FRC,  FLC,   0,   0,  FC,  LFE,  FR,  FL } },
{ .ca_id = 0x18,  .speakers = { FRC,  FLC,   0,  RC,   0,    0,  FR,  FL } },
{ .ca_id = 0x19,  .speakers = { FRC,  FLC,   0,  RC,   0,  LFE,  FR,  FL } },
{ .ca_id = 0x1a,  .speakers = { FRC,  FLC,   0,  RC,  FC,    0,  FR,  FL } },
{ .ca_id = 0x1b,  .speakers = { FRC,  FLC,   0,  RC,  FC,  LFE,  FR,  FL } },
{ .ca_id = 0x1c,  .speakers = { FRC,  FLC,  RR,  RL,   0,    0,  FR,  FL } },
{ .ca_id = 0x1d,  .speakers = { FRC,  FLC,  RR,  RL,   0,  LFE,  FR,  FL } },
{ .ca_id = 0x1e,  .speakers = { FRC,  FLC,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_id = 0x1f,  .speakers = { FRC,  FLC,  RR,  RL,  FC,  LFE,  FR,  FL } },
};

struct hdmi_codec_priv {
	struct hdmi_codec_pdata hcd;
	struct snd_soc_dai_driver *daidrv;
	struct hdmi_codec_daifmt daifmt[2];
	struct mutex current_stream_lock;
	struct snd_pcm_substream *current_stream;
	struct snd_pcm_hw_constraint_list ratec;
	uint8_t eld[MAX_ELD_BYTES];
	struct snd_pcm_chmap_elem *chmap_tlv;
	struct snd_pcm_chmap *chmap_info;
};

static const struct snd_soc_dapm_widget hdmi_widgets[] = {
	SND_SOC_DAPM_OUTPUT("TX"),
};

static const struct snd_soc_dapm_route hdmi_routes[] = {
	{ "TX", NULL, "Playback" },
};

enum {
	DAI_ID_I2S = 0,
	DAI_ID_SPDIF,
};

static int hdmi_eld_ctl_info(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_info *uinfo)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct hdmi_codec_priv *hcp = snd_soc_component_get_drvdata(component);

	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count = sizeof(hcp->eld);

	return 0;
}

static int hdmi_eld_ctl_get(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct hdmi_codec_priv *hcp = snd_soc_component_get_drvdata(component);

	memcpy(ucontrol->value.bytes.data, hcp->eld, sizeof(hcp->eld));

	return 0;
}

static unsigned long hdmi_codec_spk_mask_from_alloc(int spk_alloc)
{
	int i;
	const unsigned long hdmi_codec_eld_spk_alloc_bits[] = {
		[0] = FL | FR, [1] = LFE, [2] = FC, [3] = RL | RR,
		[4] = RC, [5] = FLC | FRC, [6] = RLC | RRC,
	};
	unsigned long spk_mask = 0;

	for (i = 0; i < ARRAY_SIZE(hdmi_codec_eld_spk_alloc_bits); i++) {
		if (spk_alloc & (1 << i))
			spk_mask |= hdmi_codec_eld_spk_alloc_bits[i];
	}

	return spk_mask;
}

/* From speaker bit mask to ALSA API channel position */
static int snd_hdac_spk_to_chmap(int spk)
{
	const struct hdmi_codec_channel_map_table *t = hdmi_codec_map_table;

	for (; t->map; t++) {
		if (t->spk_mask == spk)
			return t->map;
	}

	return 0;
}

/**
 * hdmi_codec_cea_init_channel_alloc:
 * Compute derived values in hdmi_codec_channel_alloc[].
 * spk_na_mask is used to store unused channels in mid of the channel
 * allocations. These particular channels are then considered as active channels
 * For instance:
 *    CA_ID 0x02: CA =  (FL, FR, 0, FC) => spk_na_mask = 0x04, channels = 4
 *    CA_ID 0x04: CA =  (FL, FR, 0, 0, RC) => spk_na_mask = 0x03C, channels = 5
 */
static void hdmi_codec_cea_init_channel_alloc(void)
{
	int i, j, k, last;
	struct hdmi_codec_cea_spk_alloc *p;

	/* Test if not already done by another instance */
	if (hdmi_codec_channel_alloc[0].channels)
		return;

	for (i = 0; i < ARRAY_SIZE(hdmi_codec_channel_alloc); i++) {
		p = hdmi_codec_channel_alloc + i;
		p->spks_mask = 0;
		p->spk_na_mask = 0;
		last = HDMI_MAX_SPEAKERS;
		for (j = 0, k = 7; j < HDMI_MAX_SPEAKERS; j++, k--) {
			if (p->speakers[j]) {
				p->spks_mask |= p->speakers[j];
				if (last == HDMI_MAX_SPEAKERS)
					last = j;
			} else if (last != HDMI_MAX_SPEAKERS) {
				p->spk_na_mask |= 1 << k;
			}
		}
		p->channels = 8 - last;
	}
}

static int hdmi_codec_get_ch_alloc_table_idx(struct hdmi_codec_priv *hcp,
					     unsigned char channels)
{
	int i;
	u8 spk_alloc;
	unsigned long spk_mask;
	struct hdmi_codec_cea_spk_alloc *cap = hdmi_codec_channel_alloc;

	spk_alloc = drm_eld_get_spk_alloc(hcp->eld);
	spk_mask = hdmi_codec_spk_mask_from_alloc(spk_alloc);

	for (i = 0; i < ARRAY_SIZE(hdmi_codec_channel_alloc); i++, cap++) {
		/* If spk_alloc == 0, HDMI is unplugged return stereo config*/
		if (!spk_alloc && cap->ca_id == 0)
			return i;
		if (cap->channels != channels)
			continue;
		if (!(cap->spks_mask == (spk_mask & cap->spks_mask)))
			continue;
		return i;
	}

	return -EINVAL;
}

static void hdmi_cea_alloc_to_tlv_spks(struct hdmi_codec_cea_spk_alloc *cap,
				       unsigned char *chmap)
{
	int count = 0;
	int c, spk;

	/* Detect unused channels in cea caps, tag them as N/A channel in TLV */
	for (c = 0; c < HDMI_MAX_SPEAKERS; c++) {
		spk = cap->speakers[7 - c];
		if (cap->spk_na_mask & BIT(c))
			chmap[count++] = SNDRV_CHMAP_NA;
		else
			chmap[count++] = snd_hdac_spk_to_chmap(spk);
	}
}

static void hdmi_cea_alloc_to_tlv_chmap(struct hdmi_codec_priv *hcp,
					struct hdmi_codec_cea_spk_alloc *cap)
{
	unsigned int chs, count = 0;
	struct snd_pcm_chmap *info = hcp->chmap_info;
	struct snd_pcm_chmap_elem *chmap = info->chmap;
	unsigned long max_chs = info->max_channels;
	int num_ca = ARRAY_SIZE(hdmi_codec_channel_alloc);
	int spk_alloc, spk_mask;

	spk_alloc = drm_eld_get_spk_alloc(hcp->eld);
	spk_mask = hdmi_codec_spk_mask_from_alloc(spk_alloc);

	for (chs = 2; chs <= max_chs; chs++) {
		int i;
		struct hdmi_codec_cea_spk_alloc *cap;

		cap = hdmi_codec_channel_alloc;
		for (i = 0; i < num_ca; i++, cap++) {
			if (cap->channels != chs)
				continue;

			if (!(cap->spks_mask == (spk_mask & cap->spks_mask)))
				continue;

			chmap[count].channels = cap->channels;
			hdmi_cea_alloc_to_tlv_spks(cap, chmap[count].map);
			count++;
		}
	}

	/* Force last one to 0 to indicate end of available allocations */
	chmap[count].channels = 0;
}

static const struct snd_kcontrol_new hdmi_controls[] = {
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.iface = SNDRV_CTL_ELEM_IFACE_PCM,
		.name = "ELD",
		.info = hdmi_eld_ctl_info,
		.get = hdmi_eld_ctl_get,
	},
};

static int hdmi_codec_new_stream(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{
	struct hdmi_codec_priv *hcp = snd_soc_dai_get_drvdata(dai);
	int ret = 0;

	mutex_lock(&hcp->current_stream_lock);
	if (!hcp->current_stream) {
		hcp->current_stream = substream;
	} else if (hcp->current_stream != substream) {
		dev_err(dai->dev, "Only one simultaneous stream supported!\n");
		ret = -EINVAL;
	}
	mutex_unlock(&hcp->current_stream_lock);

	return ret;
}

static int hdmi_codec_startup(struct snd_pcm_substream *substream,
			      struct snd_soc_dai *dai)
{
	struct hdmi_codec_priv *hcp = snd_soc_dai_get_drvdata(dai);
	int ret = 0;

	dev_dbg(dai->dev, "%s()\n", __func__);

	ret = hdmi_codec_new_stream(substream, dai);
	if (ret)
		return ret;

	if (hcp->hcd.ops->audio_startup) {
		ret = hcp->hcd.ops->audio_startup(dai->dev->parent, hcp->hcd.data);
		if (ret) {
			mutex_lock(&hcp->current_stream_lock);
			hcp->current_stream = NULL;
			mutex_unlock(&hcp->current_stream_lock);
			return ret;
		}
	}

	if (hcp->hcd.ops->get_eld) {
		ret = hcp->hcd.ops->get_eld(dai->dev->parent, hcp->hcd.data,
					    hcp->eld, sizeof(hcp->eld));

		if (!ret) {
			ret = snd_pcm_hw_constraint_eld(substream->runtime,
							hcp->eld);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static void hdmi_codec_shutdown(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct hdmi_codec_priv *hcp = snd_soc_dai_get_drvdata(dai);

	dev_dbg(dai->dev, "%s()\n", __func__);

	WARN_ON(hcp->current_stream != substream);

	hcp->hcd.ops->audio_shutdown(dai->dev->parent, hcp->hcd.data);

	mutex_lock(&hcp->current_stream_lock);
	hcp->current_stream = NULL;
	mutex_unlock(&hcp->current_stream_lock);
}

static int hdmi_codec_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct hdmi_codec_priv *hcp = snd_soc_dai_get_drvdata(dai);
	struct hdmi_codec_params hp = {
		.iec = {
			.status = { 0 },
			.subcode = { 0 },
			.pad = 0,
			.dig_subframe = { 0 },
		}
	};
	int ret, idx;

	dev_dbg(dai->dev, "%s() width %d rate %d channels %d\n", __func__,
		params_width(params), params_rate(params),
		params_channels(params));

	if (params_width(params) > 24)
		params->msbits = 24;

	ret = snd_pcm_create_iec958_consumer_hw_params(params, hp.iec.status,
						       sizeof(hp.iec.status));
	if (ret < 0) {
		dev_err(dai->dev, "Creating IEC958 channel status failed %d\n",
			ret);
		return ret;
	}

	ret = hdmi_codec_new_stream(substream, dai);
	if (ret)
		return ret;

	hdmi_audio_infoframe_init(&hp.cea);
	hp.cea.channels = params_channels(params);
	hp.cea.coding_type = HDMI_AUDIO_CODING_TYPE_STREAM;
	hp.cea.sample_size = HDMI_AUDIO_SAMPLE_SIZE_STREAM;
	hp.cea.sample_frequency = HDMI_AUDIO_SAMPLE_FREQUENCY_STREAM;

	/* Select a channel allocation that matches with ELD and pcm channels */
	idx = hdmi_codec_get_ch_alloc_table_idx(hcp, hp.cea.channels);
	if (idx < 0) {
		dev_err(dai->dev, "Not able to map channels to speakers (%d)\n",
			idx);
		return idx;
	}
	hp.cea.channel_allocation = hdmi_codec_channel_alloc[idx].ca_id;
	hdmi_cea_alloc_to_tlv_chmap(hcp, &hdmi_codec_channel_alloc[idx]);

	hp.sample_width = params_width(params);
	hp.sample_rate = params_rate(params);
	hp.channels = params_channels(params);

	return hcp->hcd.ops->hw_params(dai->dev->parent, hcp->hcd.data,
				       &hcp->daifmt[dai->id], &hp);
}

static int hdmi_codec_set_fmt(struct snd_soc_dai *dai,
			      unsigned int fmt)
{
	struct hdmi_codec_priv *hcp = snd_soc_dai_get_drvdata(dai);
	struct hdmi_codec_daifmt cf = { 0 };
	int ret = 0;

	dev_dbg(dai->dev, "%s()\n", __func__);

	if (dai->id == DAI_ID_SPDIF) {
		cf.fmt = HDMI_SPDIF;
	} else {
		switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
		case SND_SOC_DAIFMT_CBM_CFM:
			cf.bit_clk_master = 1;
			cf.frame_clk_master = 1;
			break;
		case SND_SOC_DAIFMT_CBS_CFM:
			cf.frame_clk_master = 1;
			break;
		case SND_SOC_DAIFMT_CBM_CFS:
			cf.bit_clk_master = 1;
			break;
		case SND_SOC_DAIFMT_CBS_CFS:
			break;
		default:
			return -EINVAL;
		}

		switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
		case SND_SOC_DAIFMT_NB_NF:
			break;
		case SND_SOC_DAIFMT_NB_IF:
			cf.frame_clk_inv = 1;
			break;
		case SND_SOC_DAIFMT_IB_NF:
			cf.bit_clk_inv = 1;
			break;
		case SND_SOC_DAIFMT_IB_IF:
			cf.frame_clk_inv = 1;
			cf.bit_clk_inv = 1;
			break;
		}

		switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
		case SND_SOC_DAIFMT_I2S:
			cf.fmt = HDMI_I2S;
			break;
		case SND_SOC_DAIFMT_DSP_A:
			cf.fmt = HDMI_DSP_A;
			break;
		case SND_SOC_DAIFMT_DSP_B:
			cf.fmt = HDMI_DSP_B;
			break;
		case SND_SOC_DAIFMT_RIGHT_J:
			cf.fmt = HDMI_RIGHT_J;
			break;
		case SND_SOC_DAIFMT_LEFT_J:
			cf.fmt = HDMI_LEFT_J;
			break;
		case SND_SOC_DAIFMT_AC97:
			cf.fmt = HDMI_AC97;
			break;
		default:
			dev_err(dai->dev, "Invalid DAI interface format\n");
			return -EINVAL;
		}
	}

	hcp->daifmt[dai->id] = cf;

	return ret;
}

static int hdmi_codec_digital_mute(struct snd_soc_dai *dai, int mute)
{
	struct hdmi_codec_priv *hcp = snd_soc_dai_get_drvdata(dai);

	dev_dbg(dai->dev, "%s()\n", __func__);

	if (hcp->hcd.ops->digital_mute)
		return hcp->hcd.ops->digital_mute(dai->dev->parent,
						  hcp->hcd.data, mute);

	return 0;
}

static const struct snd_soc_dai_ops hdmi_dai_ops = {
	.startup	= hdmi_codec_startup,
	.shutdown	= hdmi_codec_shutdown,
	.hw_params	= hdmi_codec_hw_params,
	.set_fmt	= hdmi_codec_set_fmt,
	.digital_mute	= hdmi_codec_digital_mute,
};


#define HDMI_RATES	(SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |\
			 SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 |\
			 SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 |\
			 SNDRV_PCM_RATE_192000)

#define SPDIF_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S16_BE |\
			 SNDRV_PCM_FMTBIT_S20_3LE | SNDRV_PCM_FMTBIT_S20_3BE |\
			 SNDRV_PCM_FMTBIT_S24_3LE | SNDRV_PCM_FMTBIT_S24_3BE |\
			 SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S24_BE)

/*
 * This list is only for formats allowed on the I2S bus. So there is
 * some formats listed that are not supported by HDMI interface. For
 * instance allowing the 32-bit formats enables 24-precision with CPU
 * DAIs that do not support 24-bit formats. If the extra formats cause
 * problems, we should add the video side driver an option to disable
 * them.
 */
#define I2S_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S16_BE |\
			 SNDRV_PCM_FMTBIT_S20_3LE | SNDRV_PCM_FMTBIT_S20_3BE |\
			 SNDRV_PCM_FMTBIT_S24_3LE | SNDRV_PCM_FMTBIT_S24_3BE |\
			 SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S24_BE |\
			 SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_S32_BE)

static int hdmi_codec_pcm_new(struct snd_soc_pcm_runtime *rtd,
			      struct snd_soc_dai *dai)
{
	struct snd_soc_dai_driver *drv = dai->driver;
	struct hdmi_codec_priv *hcp = snd_soc_dai_get_drvdata(dai);
	int ret;

	dev_dbg(dai->dev, "%s()\n", __func__);

	ret =  snd_pcm_add_chmap_ctls(rtd->pcm, SNDRV_PCM_STREAM_PLAYBACK,
				      NULL, drv->playback.channels_max, 0,
				      &hcp->chmap_info);
	if (ret < 0)
		return ret;

	hcp->chmap_tlv = devm_kcalloc(dai->dev,
				      ARRAY_SIZE(hdmi_codec_channel_alloc),
				      sizeof(*hcp->chmap_tlv), GFP_KERNEL);
	if (!hcp->chmap_tlv)
		return -ENOMEM;

	/* Initialize mapping to stereo as default config supported */
	memcpy(hcp->chmap_tlv, hdmi_codec_stereo_chmaps,
	       ARRAY_SIZE(hdmi_codec_stereo_chmaps) * sizeof(*hcp->chmap_tlv));

	hcp->chmap_info->chmap = hcp->chmap_tlv;

	return 0;
}

static struct snd_soc_dai_driver hdmi_i2s_dai = {
	.id = DAI_ID_I2S,
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 8,
		.rates = HDMI_RATES,
		.formats = I2S_FORMATS,
		.sig_bits = 24,
	},
	.ops = &hdmi_dai_ops,
	.pcm_new = hdmi_codec_pcm_new,
};

static const struct snd_soc_dai_driver hdmi_spdif_dai = {
	.id = DAI_ID_SPDIF,
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = HDMI_RATES,
		.formats = SPDIF_FORMATS,
	},
	.ops = &hdmi_dai_ops,
	.pcm_new = hdmi_codec_pcm_new,
};

static char hdmi_dai_name[][DAI_NAME_SIZE] = {
	"hdmi-hifi.0",
	"hdmi-hifi.1",
	"hdmi-hifi.2",
	"hdmi-hifi.3",
};

static int hdmi_of_xlate_dai_name(struct snd_soc_component *component,
				  struct of_phandle_args *args,
				  const char **dai_name)
{
	int id;

	if (args->args_count)
		id = args->args[0];
	else
		id = 0;

	if (id < ARRAY_SIZE(hdmi_dai_name)) {
		*dai_name = hdmi_dai_name[id];
		return 0;
	}

	return -EAGAIN;
}

static struct snd_soc_codec_driver hdmi_codec = {
	.component_driver = {
		.controls		= hdmi_controls,
		.num_controls		= ARRAY_SIZE(hdmi_controls),
		.dapm_widgets		= hdmi_widgets,
		.num_dapm_widgets	= ARRAY_SIZE(hdmi_widgets),
		.dapm_routes		= hdmi_routes,
		.num_dapm_routes	= ARRAY_SIZE(hdmi_routes),
		.of_xlate_dai_name	= hdmi_of_xlate_dai_name,
	},
};

static int hdmi_codec_probe(struct platform_device *pdev)
{
	struct hdmi_codec_pdata *hcd = pdev->dev.platform_data;
	struct device *dev = &pdev->dev;
	struct hdmi_codec_priv *hcp;
	struct hdmi_device *hd;
	struct list_head *pos;
	int dai_count, i = 0;
	int ret;

	dev_dbg(dev, "%s()\n", __func__);

	if (!hcd) {
		dev_err(dev, "%s: No plalform data\n", __func__);
		return -EINVAL;
	}

	dai_count = hcd->i2s + hcd->spdif;
	if (dai_count < 1 || !hcd->ops || !hcd->ops->hw_params ||
	    !hcd->ops->audio_shutdown) {
		dev_err(dev, "%s: Invalid parameters\n", __func__);
		return -EINVAL;
	}

	hcp = devm_kzalloc(dev, sizeof(*hcp), GFP_KERNEL);
	if (!hcp)
		return -ENOMEM;

	hd = NULL;
	list_for_each(pos, &hdmi_device_list) {
		struct hdmi_device *tmp = pos_to_hdmi_device(pos);

		if (tmp->dev == dev->parent) {
			hd = tmp;
			break;
		}
	}

	if (!hd) {
		hd = devm_kzalloc(dev, sizeof(*hd), GFP_KERNEL);
		if (!hd)
			return -ENOMEM;

		hd->dev = dev->parent;

		list_add_tail(&hd->list, &hdmi_device_list);
	}

	if (hd->cnt >= ARRAY_SIZE(hdmi_dai_name)) {
		dev_err(dev, "too many hdmi codec are deteced\n");
		return -EINVAL;
	}

	hcp->hcd = *hcd;
	mutex_init(&hcp->current_stream_lock);

	hcp->daidrv = devm_kzalloc(dev, dai_count * sizeof(*hcp->daidrv),
				   GFP_KERNEL);
	if (!hcp->daidrv)
		return -ENOMEM;

	if (hcd->i2s) {
		hcp->daidrv[i] = hdmi_i2s_dai;
		hcp->daidrv[i].playback.channels_max =
			hcd->max_i2s_channels;
		hcp->daidrv[i].name = hdmi_dai_name[hd->cnt++];
		i++;
	}

	if (hcd->spdif) {
		hcp->daidrv[i] = hdmi_spdif_dai;
		hcp->daidrv[i].name = hdmi_dai_name[hd->cnt++];
	}

	hdmi_codec_cea_init_channel_alloc();

	ret = snd_soc_register_codec(dev, &hdmi_codec, hcp->daidrv,
				     dai_count);
	if (ret) {
		dev_err(dev, "%s: snd_soc_register_codec() failed (%d)\n",
			__func__, ret);
		return ret;
	}

	dev_set_drvdata(dev, hcp);
	return 0;
}

static int hdmi_codec_remove(struct platform_device *pdev)
{
	struct hdmi_codec_priv *hcp;

	hcp = dev_get_drvdata(&pdev->dev);
	kfree(hcp->chmap_info);
	snd_soc_unregister_codec(&pdev->dev);
	return 0;
}

static struct platform_driver hdmi_codec_driver = {
	.driver = {
		.name = HDMI_CODEC_DRV_NAME,
	},
	.probe = hdmi_codec_probe,
	.remove = hdmi_codec_remove,
};

module_platform_driver(hdmi_codec_driver);

MODULE_AUTHOR("Jyri Sarha <jsarha@ti.com>");
MODULE_DESCRIPTION("HDMI Audio Codec Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" HDMI_CODEC_DRV_NAME);
