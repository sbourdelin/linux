// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 */

#include <linux/module.h>
#include <linux/component.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/atomic.h>
#include <linux/of_device.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <linux/soc/qcom/apr.h>
#include "qdsp6/q6afe.h"

#define DEFAULT_SAMPLE_RATE_48K		48000
#define DEFAULT_MCLK_RATE		24576000
#define DEFAULT_BCLK_RATE		1536000

struct sdm845_snd_data {
	struct snd_soc_card *card;
	struct regulator *vdd_supply;
	struct snd_soc_dai_link dai_link[];
};

static struct mutex pri_mi2s_res_lock;
static struct mutex quat_tdm_res_lock;
static atomic_t pri_mi2s_clk_count;
static atomic_t quat_tdm_clk_count;

static unsigned int tdm_slot_offset[8] = {0, 4, 8, 12, 16, 20, 24, 28};

static int sdm845_tdm_snd_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int ret = 0;
	int channels, slot_width;

	channels = params_channels(params);
	if (channels < 1 || channels > 8) {
		pr_err("%s: invalid param channels %d\n",
				__func__, channels);
		return -EINVAL;
	}

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S32_LE:
	case SNDRV_PCM_FORMAT_S24_LE:
	case SNDRV_PCM_FORMAT_S16_LE:
		slot_width = 32;
		break;
	default:
		pr_err("%s: invalid param format 0x%x\n",
				__func__, params_format(params));
		return -EINVAL;
	}

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ret = snd_soc_dai_set_tdm_slot(cpu_dai, 0, 0x3,
				channels, slot_width);
		if (ret < 0) {
			pr_err("%s: failed to set tdm slot, err:%d\n",
					__func__, ret);
			goto end;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, NULL,
				channels, tdm_slot_offset);
		if (ret < 0) {
			pr_err("%s: failed to set channel map, err:%d\n",
					__func__, ret);
			goto end;
		}
	} else {
		ret = snd_soc_dai_set_tdm_slot(cpu_dai, 0xf, 0,
				channels, slot_width);
		if (ret < 0) {
			pr_err("%s: failed to set tdm slot, err:%d\n",
					__func__, ret);
			goto end;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai, channels,
				tdm_slot_offset, 0, NULL);
		if (ret < 0) {
			pr_err("%s: failed to set channel map, err:%d\n",
					__func__, ret);
			goto end;
		}
	}
end:
	return ret;
}

static int sdm845_snd_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int ret = 0;

	switch (cpu_dai->id) {
	case QUATERNARY_TDM_RX_0:
	case QUATERNARY_TDM_TX_0:
		ret = sdm845_tdm_snd_hw_params(substream, params);
		break;
	default:
		pr_err("%s: invalid dai id 0x%x\n", __func__, cpu_dai->id);
		break;
	}
	return ret;
}

static int sdm845_snd_startup(struct snd_pcm_substream *substream)
{
	unsigned int fmt = SND_SOC_DAIFMT_CBS_CFS;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;

	pr_debug("%s: dai_id: 0x%x\n", __func__, cpu_dai->id);
	switch (cpu_dai->id) {
	case PRIMARY_MI2S_RX:
	case PRIMARY_MI2S_TX:
		mutex_lock(&pri_mi2s_res_lock);
		if (atomic_inc_return(&pri_mi2s_clk_count) == 1) {
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_MCLK_1,
				DEFAULT_MCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_PRI_MI2S_IBIT,
				DEFAULT_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		}
		mutex_unlock(&pri_mi2s_res_lock);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		break;
	case QUATERNARY_TDM_RX_0:
	case QUATERNARY_TDM_TX_0:
		mutex_lock(&quat_tdm_res_lock);
		if (atomic_inc_return(&quat_tdm_clk_count) == 1) {
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_QUAD_TDM_IBIT,
				DEFAULT_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		}
		mutex_unlock(&quat_tdm_res_lock);
		break;
	default:
		pr_err("%s: invalid dai id 0x%x\n", __func__, cpu_dai->id);
		break;
	}
	return 0;
}

static void  sdm845_snd_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;

	pr_debug("%s: dai_id: 0x%x\n", __func__, cpu_dai->id);
	switch (cpu_dai->id) {
	case PRIMARY_MI2S_RX:
	case PRIMARY_MI2S_TX:
		mutex_lock(&pri_mi2s_res_lock);
		if (!atomic_dec_return(&pri_mi2s_clk_count)) {
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_MCLK_1,
				0, SNDRV_PCM_STREAM_PLAYBACK);
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_PRI_MI2S_IBIT,
				0, SNDRV_PCM_STREAM_PLAYBACK);
		};
		mutex_unlock(&pri_mi2s_res_lock);
		break;
	case QUATERNARY_TDM_RX_0:
	case QUATERNARY_TDM_TX_0:
		mutex_lock(&quat_tdm_res_lock);
		if (!atomic_dec_return(&quat_tdm_clk_count)) {
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_QUAD_TDM_IBIT,
				0, SNDRV_PCM_STREAM_PLAYBACK);
		}
		mutex_unlock(&quat_tdm_res_lock);
		break;
	default:
		pr_err("%s: invalid dai id 0x%x\n", __func__, cpu_dai->id);
		break;
	}
}

static struct snd_soc_ops sdm845_be_ops = {
	.hw_params = sdm845_snd_hw_params,
	.startup = sdm845_snd_startup,
	.shutdown = sdm845_snd_shutdown,
};

static int sdm845_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	rate->min = rate->max = DEFAULT_SAMPLE_RATE_48K;
	channels->min = channels->max = 2;

	return 0;
}

static const struct snd_soc_dapm_widget sdm845_widgets[] = {
	SND_SOC_DAPM_PINCTRL("Pri-mi2s-gpio", "pri_active", "pri_sleep"),
	SND_SOC_DAPM_PINCTRL("Quat-tdm-gpio", "quat_active", "quat_sleep"),
};

static int sdm845_sbc_parse_of(struct snd_soc_card *card)
{
	struct device *dev = card->dev;
	struct snd_soc_dai_link *link;
	struct device_node *np, *codec, *platform, *cpu, *node;
	int ret, num_links;
	struct sdm845_snd_data *data;

	ret = snd_soc_of_parse_card_name(card, "qcom,model");
	if (ret) {
		dev_err(dev, "Error parsing card name: %d\n", ret);
		return ret;
	}

	node = dev->of_node;

	/* DAPM routes */
	if (of_property_read_bool(node, "qcom,audio-routing")) {
		ret = snd_soc_of_parse_audio_routing(card,
					"qcom,audio-routing");
		if (ret)
			return ret;
	}

	/* Populate links */
	num_links = of_get_child_count(node);

	dev_info(dev, "Found %d child audio dai links..\n", num_links);
	/* Allocate the private data and the DAI link array */
	data = kzalloc(sizeof(*data) + sizeof(*link) * num_links,
			GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	card->dai_link = &data->dai_link[0];
	card->num_links = num_links;
	card->dapm_widgets = sdm845_widgets;
	card->num_dapm_widgets = ARRAY_SIZE(sdm845_widgets);

	link = data->dai_link;
	data->card = card;

	for_each_child_of_node(node, np) {
		cpu = of_get_child_by_name(np, "cpu");
		platform = of_get_child_by_name(np, "platform");
		codec = of_get_child_by_name(np, "codec");

		if (!cpu) {
			dev_err(dev, "Can't find cpu DT node\n");
			ret = -EINVAL;
			goto fail;
		}

		link->cpu_of_node = of_parse_phandle(cpu, "sound-dai", 0);
		if (!link->cpu_of_node) {
			dev_err(card->dev, "error getting cpu phandle\n");
			ret = -EINVAL;
			goto fail;
		}

		link->platform_of_node = of_parse_phandle(platform,
				"sound-dai", 0);
		if (!link->platform_of_node) {
			dev_err(card->dev, "error getting platform phandle\n");
			ret = -EINVAL;
			goto fail;
		}

		ret = snd_soc_of_get_dai_name(cpu, &link->cpu_dai_name);
		if (ret) {
			dev_err(card->dev, "error getting cpu dai name\n");
			goto fail;
		}

		if (codec) {
			ret = snd_soc_of_get_dai_link_codecs(dev, codec, link);
			if (ret < 0) {
				dev_err(card->dev, "error getting codec dai name\n");
				goto fail;
			}
			link->no_pcm = 1;
			link->ignore_suspend = 1;
			link->ignore_pmdown_time = 1;
			link->ops = &sdm845_be_ops;
			link->be_hw_params_fixup = sdm845_be_hw_params_fixup;
		} else {
			link->dynamic = 1;
			link->codec_dai_name = "snd-soc-dummy-dai";
			link->codec_name = "snd-soc-dummy";
		}

		ret = of_property_read_string(np, "link-name", &link->name);
		if (ret) {
			dev_err(card->dev,
				"error getting codec dai_link name\n");
			goto fail;
		}

		link->dpcm_playback = 1;
		link->dpcm_capture = 1;
		link->stream_name = link->name;
		link++;
	}
	dev_set_drvdata(dev, card);
	snd_soc_card_set_drvdata(card, data);

	return ret;
fail:
	kfree(data);
	return ret;
}

static void sdm845_init_supplies(struct device *dev)
{
	struct snd_soc_card *card = dev_get_drvdata(dev);
	struct sdm845_snd_data *data = snd_soc_card_get_drvdata(card);

	data->vdd_supply = regulator_get(dev, "cdc-vdd");
	if (IS_ERR(data->vdd_supply)) {
		dev_err(dev, "Unable to get regulator supplies\n");
		data->vdd_supply = NULL;
		return;
	}

	if (regulator_enable(data->vdd_supply))
		dev_err(dev, "Unable to enable vdd supply\n");
}

static void sdm845_deinit_supplies(struct device *dev)
{
	struct snd_soc_card *card = dev_get_drvdata(dev);
	struct sdm845_snd_data *data = snd_soc_card_get_drvdata(card);

	if (!data->vdd_supply)
		return;

	regulator_disable(data->vdd_supply);
	regulator_put(data->vdd_supply);
}

static int sdm845_bind(struct device *dev)
{
	struct snd_soc_card *card;
	int ret;

	card = kzalloc(sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	ret = component_bind_all(dev, card);
	if (ret) {
		dev_err(dev, "Audio components bind failed: %d\n", ret);
		goto bind_fail;
	}

	card->dev = dev;
	ret = sdm845_sbc_parse_of(card);
	if (ret) {
		dev_err(dev, "Error parsing OF data\n");
		goto parse_dt_fail;
	}
	sdm845_init_supplies(dev);

	mutex_init(&pri_mi2s_res_lock);
	mutex_init(&quat_tdm_res_lock);
	atomic_set(&pri_mi2s_clk_count, 0);
	atomic_set(&quat_tdm_clk_count, 0);
	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(dev, "Sound card registration failed\n");
		goto register_card_fail;
	}
	return ret;

register_card_fail:
	mutex_destroy(&pri_mi2s_res_lock);
	mutex_destroy(&quat_tdm_res_lock);
	sdm845_deinit_supplies(dev);
	kfree(snd_soc_card_get_drvdata(card));
parse_dt_fail:
	component_unbind_all(dev, card);
bind_fail:
	kfree(card);
	return ret;
}

static void sdm845_unbind(struct device *dev)
{
	struct snd_soc_card *card = dev_get_drvdata(dev);
	struct sdm845_snd_data *data = snd_soc_card_get_drvdata(card);

	mutex_destroy(&pri_mi2s_res_lock);
	mutex_destroy(&quat_tdm_res_lock);
	if (data->vdd_supply)
		regulator_put(data->vdd_supply);
	component_unbind_all(dev, card);
	snd_soc_unregister_card(card);
	kfree(data);
	kfree(card);
}

static const struct component_master_ops sdm845_ops = {
	.bind = sdm845_bind,
	.unbind = sdm845_unbind,
};

static int sdm845_runtime_resume(struct device *dev)
{
	struct snd_soc_card *card = dev_get_drvdata(dev);
	struct sdm845_snd_data *data = snd_soc_card_get_drvdata(card);

	if (!data->vdd_supply) {
		dev_dbg(dev, "no supplies defined\n");
		return 0;
	}

	if (regulator_enable(data->vdd_supply))
		dev_err(dev, "Enable regulator supply failed\n");

	return 0;
}

static int sdm845_runtime_suspend(struct device *dev)
{
	struct snd_soc_card *card = dev_get_drvdata(dev);
	struct sdm845_snd_data *data = snd_soc_card_get_drvdata(card);

	if (!data->vdd_supply) {
		dev_dbg(dev, "no supplies defined\n");
		return 0;
	}

	if (regulator_disable(data->vdd_supply))
		dev_err(dev, "Disable regulator supply failed\n");

	return 0;
}

static const struct dev_pm_ops sdm845_pm_ops = {
	SET_RUNTIME_PM_OPS(sdm845_runtime_suspend,
			sdm845_runtime_resume, NULL)
};

static int sdm845_compare_of(struct device *dev, void *data)
{
	return dev->of_node == data;
}

static void sdm845_release_of(struct device *dev, void *data)
{
	of_node_put(data);
}

static int add_audio_components(struct device *dev,
				struct component_match **matchptr)
{
	struct device_node *np, *platform, *cpu, *node, *dai_node;

	node = dev->of_node;

	for_each_child_of_node(node, np) {
		cpu = of_get_child_by_name(np, "cpu");
		if (cpu) {
			dai_node = of_parse_phandle(cpu, "sound-dai", 0);
			of_node_get(dai_node);
			component_match_add_release(dev, matchptr,
					sdm845_release_of,
					sdm845_compare_of,
					dai_node);
		}

		platform = of_get_child_by_name(np, "platform");
		if (platform) {
			dai_node = of_parse_phandle(platform, "sound-dai", 0);
			component_match_add_release(dev, matchptr,
					sdm845_release_of,
					sdm845_compare_of,
					dai_node);
		}
	}

	return 0;
}

static int sdm845_snd_platform_probe(struct platform_device *pdev)
{
	struct component_match *match = NULL;
	int ret;

	ret = add_audio_components(&pdev->dev, &match);
	if (ret)
		return ret;

	return component_master_add_with_match(&pdev->dev, &sdm845_ops, match);
}

static int sdm845_snd_platform_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &sdm845_ops);
	return 0;
}

static const struct of_device_id sdm845_snd_device_id[]  = {
	{ .compatible = "qcom,sdm845-sndcard" },
	{},
};
MODULE_DEVICE_TABLE(of, sdm845_snd_device_id);

static struct platform_driver sdm845_snd_driver = {
	.probe = sdm845_snd_platform_probe,
	.remove = sdm845_snd_platform_remove,
	.driver = {
		.name = "msm-snd-sdm845",
		.pm = &sdm845_pm_ops,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(sdm845_snd_device_id),
	},
};
module_platform_driver(sdm845_snd_driver);

MODULE_DESCRIPTION("sdm845 ASoC Machine Driver");
MODULE_LICENSE("GPL v2");
