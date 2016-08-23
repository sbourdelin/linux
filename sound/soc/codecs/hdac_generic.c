/*
 *  hdac_generic.c - ASoc HDA generic codec driver
 *
 *  Copyright (C) 2016-2017 Intel Corp
 *  Author: Subhransu S. Prusty <subhransu.s.prusty@intel.com>
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include <linux/init.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>
#include <sound/hdaudio_ext.h>
#include <sound/hda_regmap.h>
#include "../../hda/local.h"
#include "../../hda/ext/hdac_codec.h"
#include "hdac_generic.h"

#define HDA_MAX_CVTS	10

struct hdac_generic_dai_map {
	struct hdac_codec_widget *cvt;
};

struct hdac_generic_priv {
	struct hdac_generic_dai_map dai_map[HDA_MAX_CVTS];
	unsigned int num_pins;
	unsigned int num_adcs;
	unsigned int num_dacs;
	unsigned int num_dapm_widgets;
};

static void hdac_generic_set_power_state(struct hdac_ext_device *edev,
			hda_nid_t nid, unsigned int pwr_state)
{
	/* TODO: check D0sup bit before setting this */
	if (!snd_hdac_check_power_state(&edev->hdac, nid, pwr_state))
		snd_hdac_codec_write(&edev->hdac, nid, 0,
				AC_VERB_SET_POWER_STATE, pwr_state);
}

static void hdac_generic_calc_dapm_widgets(struct hdac_ext_device *edev)
{
	struct hdac_generic_priv *hdac_priv = edev->private_data;
	struct hdac_codec_widget *wid;

	if (list_empty(&edev->hdac.widget_list))
		return;

	/*
	 * PIN widget with output capable are represented with an additional
	 * virtual mux widgets.
	 */
	list_for_each_entry(wid, &edev->hdac.widget_list, head) {
		switch (wid->type) {
		case AC_WID_AUD_IN:
			hdac_priv->num_dapm_widgets++;
			hdac_priv->num_adcs++;
			break;

		case AC_WID_AUD_OUT:
			hdac_priv->num_dapm_widgets++;
			hdac_priv->num_dacs++;
			break;

		case AC_WID_PIN:
			hdac_priv->num_pins++;
			/*
			 * PIN widgets are represented with dapm_pga and
			 * dapm_output.
			 */
			hdac_priv->num_dapm_widgets += 2;

			if (is_input_pin(&edev->hdac, wid->nid))
				continue;

			/*
			 * PIN widget with output capable are represented
			 * with an additional virtual mux widgets.
			 */
			if (wid->num_inputs > 1)
				hdac_priv->num_dapm_widgets++;

			break;

		case AC_WID_AUD_MIX:
			hdac_priv->num_dapm_widgets++;
			break;

		case AC_WID_AUD_SEL:
			hdac_priv->num_dapm_widgets++;
			break;

		case AC_WID_POWER:
			hdac_priv->num_dapm_widgets++;
			break;

		case AC_WID_BEEP:
			/*
			 * Beep widgets are represented with a siggen and
			 * pga dapm widgets
			 */
			hdac_priv->num_dapm_widgets += 2;
			break;

		default:
			dev_warn(&edev->hdac.dev, "no dapm widget for type: %d\n",
						wid->type);
			break;
		}
	}
}

static int hdac_generic_set_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *hparams, struct snd_soc_dai *dai)
{
	struct hdac_ext_device *edev = snd_soc_dai_get_drvdata(dai);
	struct hdac_generic_priv *hdac_priv = edev->private_data;
	struct hdac_generic_dai_map *dai_map = &hdac_priv->dai_map[dai->id];
	u32 format;

	format = snd_hdac_calc_stream_format(params_rate(hparams),
			params_channels(hparams), params_format(hparams),
			32, 0);

	snd_hdac_codec_write(&edev->hdac, dai_map->cvt->nid, 0,
				AC_VERB_SET_STREAM_FORMAT, format);

	return 0;
}

static int hdac_generic_set_tdm_slot(struct snd_soc_dai *dai,
		unsigned int tx_mask, unsigned int rx_mask,
		int slots, int slot_width)
{
	struct hdac_ext_device *edev = snd_soc_dai_get_drvdata(dai);
	struct hdac_generic_priv *hdac_priv = edev->private_data;
	struct hdac_generic_dai_map *dai_map = &hdac_priv->dai_map[dai->id];
	int val;

	dev_dbg(&edev->hdac.dev, "%s: strm_tag: %d\n", __func__, tx_mask);

	val = snd_hdac_codec_read(&edev->hdac, dai_map->cvt->nid, 0,
					AC_VERB_GET_CONV, 0);
	snd_hdac_codec_write(&edev->hdac, dai_map->cvt->nid, 0,
				AC_VERB_SET_CHANNEL_STREAMID,
				(val & 0xf0) | (tx_mask << 4));

	return 0;
}

static int hdac_codec_set_bias_level(struct snd_soc_codec *codec,
			enum snd_soc_bias_level level)
{
	struct hdac_ext_device *edev = snd_soc_codec_get_drvdata(codec);
	struct hdac_device *hdac = &edev->hdac;

	dev_dbg(&edev->hdac.dev, "%s: level: %d\n", __func__, level); 

	switch (level) {
	case SND_SOC_BIAS_PREPARE:
		hdac_generic_set_power_state(edev, hdac->afg, AC_PWRST_D0);
		break;

	case SND_SOC_BIAS_OFF:
		hdac_generic_set_power_state(edev, hdac->afg, AC_PWRST_D3);
		break;

	default:
		dev_info(&edev->hdac.dev, "Bias level %d not handled\n", level);
		break;
	}

	return 0;
}

static int hdac_codec_probe(struct snd_soc_codec *codec)
{
	struct hdac_ext_device *edev = snd_soc_codec_get_drvdata(codec);
	struct snd_soc_dapm_context *dapm =
		snd_soc_component_get_dapm(&codec->component);

	edev->scodec = codec;

	/* TODO: create widget, route and controls */
	/* TODO: jack sense */

	/* Imp: Store the card pointer in hda_codec */
	edev->card = dapm->card->snd_card;

	/* TODO: runtime PM */
	return 0;
}

static int hdac_codec_remove(struct snd_soc_codec *codec)
{
	/* TODO: disable runtime pm */
	return 0;
}

static struct snd_soc_codec_driver hdac_generic_codec = {
	.probe		= hdac_codec_probe,
	.remove		= hdac_codec_remove,
	.set_bias_level = hdac_codec_set_bias_level,
};

static struct snd_soc_dai_ops hdac_generic_ops = {
	.hw_params = hdac_generic_set_hw_params,
	.set_tdm_slot = hdac_generic_set_tdm_slot,
};

static int hdac_generic_create_dais(struct hdac_ext_device *edev,
		struct snd_soc_dai_driver **dais, int num_dais)
{
	struct hdac_device *hdac = &edev->hdac;
	struct hdac_generic_priv *hdac_priv = edev->private_data;
	struct snd_soc_dai_driver *codec_dais;
	char stream_name[HDAC_GENERIC_NAME_SIZE];
	char dai_name[HDAC_GENERIC_NAME_SIZE];
	struct hdac_codec_widget *widget;
	int i = 0;
	u32 rates, bps;
	unsigned int rate_max = 192000, rate_min = 8000;
	u64 formats;
	int ret;

	codec_dais = devm_kzalloc(&hdac->dev,
			(sizeof(*codec_dais) * num_dais),
			GFP_KERNEL);
	if (!codec_dais)
		return -ENOMEM;

	/* Iterate over the input adc and dac list to create DAIs */
	list_for_each_entry(widget, &edev->hdac.widget_list, head) {

		if ((widget->type != AC_WID_AUD_IN) &&
				(widget->type != AC_WID_AUD_OUT))
			continue;

		ret = snd_hdac_query_supported_pcm(hdac, widget->nid,
				&rates,	&formats, &bps);
		if (ret)
			return ret;

		sprintf(dai_name, "%x-aif%d", hdac->vendor_id, widget->nid);

		codec_dais[i].name = devm_kstrdup(&hdac->dev, dai_name,
							GFP_KERNEL);
		if (!codec_dais[i].name)
			return -ENOMEM;

		codec_dais[i].ops = &hdac_generic_ops;
		codec_dais[i].dobj.private = widget;
		hdac_priv->dai_map[i].cvt = widget;

		switch (widget->type) {
		case AC_WID_AUD_IN:
			snprintf(stream_name, sizeof(stream_name),
					"Analog Capture-%d", widget->nid);
			codec_dais[i].capture.stream_name =
					devm_kstrdup(&hdac->dev, stream_name,
								GFP_KERNEL);
			if (!codec_dais[i].capture.stream_name)
				return -ENOMEM;

			 /*
			  * Set caps based on capability queried from the
			  * converter.
			  */
			codec_dais[i].capture.formats = formats;
			codec_dais[i].capture.rates = rates;
			codec_dais[i].capture.rate_max = rate_max;
			codec_dais[i].capture.rate_min = rate_min;
			codec_dais[i].capture.channels_min = 2;
			codec_dais[i].capture.channels_max = 2;

			i++;
			break;

		case AC_WID_AUD_OUT:
			if (widget->caps & AC_WCAP_DIGITAL)
				snprintf(stream_name, sizeof(stream_name),
					"Digital Playback-%d", widget->nid);
			else
				snprintf(stream_name, sizeof(stream_name),
					"Analog Playback-%d", widget->nid);

			codec_dais[i].playback.stream_name =
					devm_kstrdup(&hdac->dev, stream_name,
								GFP_KERNEL);
			if (!codec_dais[i].playback.stream_name)
				return -ENOMEM;

			/*
			 * Set caps based on capability queried from the
			 * converter.
			 */
			codec_dais[i].playback.formats = formats;
			codec_dais[i].playback.rates = rates;
			codec_dais[i].playback.rate_max = rate_max;
			codec_dais[i].playback.rate_min = rate_min;
			codec_dais[i].playback.channels_min = 2;
			codec_dais[i].playback.channels_max = 2;

			i++;

			break;
		default:
			dev_warn(&hdac->dev, "Invalid widget type: %d\n",
						widget->type);
			break;
		}
	}

	*dais = codec_dais;

	return 0;
}

static int hdac_generic_dev_probe(struct hdac_ext_device *edev)
{
	struct hdac_device *codec = &edev->hdac;
	struct hdac_generic_priv *hdac_priv;
	struct snd_soc_dai_driver *codec_dais = NULL;
	int num_dais = 0;
	int ret = 0;

	hdac_priv = devm_kzalloc(&codec->dev, sizeof(*hdac_priv), GFP_KERNEL);
	if (hdac_priv == NULL)
		return -ENOMEM;

	ret = snd_hdac_codec_init(codec);
	if (ret < 0)
		return ret;

	edev->private_data = hdac_priv;
	dev_set_drvdata(&codec->dev, edev);

	ret = snd_hdac_parse_widgets(codec);
	if (ret < 0) {
		dev_err(&codec->dev, "Failed to parse widgets with err: %d\n",
							ret);
		return ret;
	}

	hdac_generic_calc_dapm_widgets(edev);

	if (!hdac_priv->num_pins || ((!hdac_priv->num_adcs) &&
					 (!hdac_priv->num_dacs))) {

		dev_err(&codec->dev, "No port widgets or cvt widgets");
		return -EIO;
	}

	num_dais = hdac_priv->num_adcs + hdac_priv->num_dacs;

	ret = hdac_generic_create_dais(edev, &codec_dais, num_dais);
	if (ret < 0) {
		dev_err(&codec->dev, "Failed to create dais with err: %d\n",
							ret);
		return ret;
	}

	/* ASoC specific initialization */
	return snd_soc_register_codec(&codec->dev, &hdac_generic_codec,
			codec_dais, num_dais);
}

static int hdac_generic_dev_remove(struct hdac_ext_device *edev)
{
	snd_hdac_codec_cleanup(&edev->hdac);
	return 0;
}

/*
 * TODO:
 * Driver_data will be used to perform any vendor specific init, register
 * specific dai ops.
 * Driver will implement it's own match function to retrieve driver data.
 */
static const struct hda_device_id codec_list[] = {
	HDA_CODEC_EXT_ENTRY(0x10ec0286, 0x100002, "ALC286", 0),
	{}
};

MODULE_DEVICE_TABLE(hdaudio, codec_list);

static struct hdac_ext_driver hdac_codec_driver = {
	. hdac = {
		.driver = {
			.name   = "HDA ASoC Codec",
			/* Add PM */
		},
		.id_table       = codec_list,
	},
	.probe          = hdac_generic_dev_probe,
	.remove         = hdac_generic_dev_remove,
};

static int __init hdac_generic_init(void)
{
	return snd_hda_ext_driver_register(&hdac_codec_driver);
}

static void __exit hdac_generic_exit(void)
{
	snd_hda_ext_driver_unregister(&hdac_codec_driver);
}

module_init(hdac_generic_init);
module_exit(hdac_generic_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("HDA ASoC codec");
MODULE_AUTHOR("Subhransu S. Prusty<subhransu.s.prusty@intel.com>");
