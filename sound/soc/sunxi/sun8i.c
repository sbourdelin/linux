/*
 * ALSA SoC driver for Allwinner sun8i SoC
 *
 * Copyright (C) 2016 Mylène Josserand <mylene.josserand@free-electrons.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/firmware.h>
#include <linux/module.h>

#include <sound/soc.h>

static struct snd_soc_aux_dev sun8i_audio_prcm_aux_devs[] = {
	{
		.name = "sun8i-codec-analog",
		.codec_name = "sun8i-codec-analog.0",
	},
};

static struct snd_soc_dai_link sun8i_dai_link = {
	.name           = "sun4i-i2s",
	.stream_name    = "Playback",
	.codec_dai_name = "sun8i",
	.dai_fmt        = SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S |
			SND_SOC_DAIFMT_CBM_CFM,
};

static struct snd_soc_card sun8i_card = {
	.name           = "sun8i-card",
	.owner          = THIS_MODULE,
	.dai_link       = &sun8i_dai_link,
	.num_links      = 1,
	.aux_dev	= sun8i_audio_prcm_aux_devs,
	.num_aux_devs	= ARRAY_SIZE(sun8i_audio_prcm_aux_devs),
};

static int sun8i_probe(struct platform_device *pdev)
{
	struct snd_soc_dai_link *link = &sun8i_dai_link;
	struct device_node *np = pdev->dev.of_node;
	int ret;

	/* register the soc card */
	sun8i_card.dev = &pdev->dev;

	/* Retrieve the audio-codec from DT */
	link->codec_of_node = of_parse_phandle(np, "allwinner,audio-codec", 0);
	if (!link->codec_of_node) {
		dev_err(&pdev->dev, "Missing audio codec\n");
		return -EINVAL;
	}

	/* Retrieve DAI from DT */
	link->cpu_of_node = of_parse_phandle(np, "allwinner,i2s-controller", 0);
	if (!link->cpu_of_node) {
		dev_err(&pdev->dev, "Missing I2S controller\n");
		return -EINVAL;
	}

	link->platform_of_node = link->cpu_of_node;

	/* Register the sound card */
	ret = devm_snd_soc_register_card(&pdev->dev, &sun8i_card);
	if (ret) {
		dev_err(&pdev->dev,
			"Soc register card failed %d\n", ret);
		return ret;
	}

	return ret;
}

static const struct of_device_id sun8i_of_match[] = {
	{ .compatible = "allwinner,sun8i-audio", },
	{},
};

MODULE_DEVICE_TABLE(of, sun8i_of_match);

static struct platform_driver sun8i_card_driver = {
	.probe = sun8i_probe,
	.driver = {
		.name   = "sun8i-audio",
		.of_match_table = sun8i_of_match,
	},
};

module_platform_driver(sun8i_card_driver);

MODULE_AUTHOR("Mylène Josserand <mylene.josserand@free-electrons.com>");
MODULE_DESCRIPTION("Allwinner sun8i machine ASoC driver");
MODULE_LICENSE("GPL v2");
