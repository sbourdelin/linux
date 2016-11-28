/*
 * ASoC simple graph SCU sound card support
 *
 * Copyright (C) 2016 Renesas Solutions Corp.
 * Kuninori Morimoto <kuninori.morimoto.gx@renesas.com>
 *
 * based on
 *	${LINUX}/sound/soc/generic/simple-graph-card.c
 *	${LINUX}/sound/soc/generic/simple-scu-card.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <sound/jack.h>
#include <sound/simple_card_utils.h>

struct simple_card_data {
	struct snd_soc_card snd_card;
	struct snd_soc_codec_conf codec_conf;
	struct asoc_simple_dai *dai_props;
	struct snd_soc_dai_link *dai_link;
	u32 convert_rate;
	u32 convert_channels;
};

#define simple_priv_to_dev(priv) ((priv)->snd_card.dev)
#define simple_priv_to_link(priv, i) ((priv)->snd_card.dai_link + (i))
#define simple_priv_to_props(priv, i) ((priv)->dai_props + (i))

#define PREFIX	"simple-audio-card,"

static int asoc_simple_card_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct simple_card_data *priv =	snd_soc_card_get_drvdata(rtd->card);
	struct asoc_simple_dai *dai_props =
		simple_priv_to_props(priv, rtd->num);

	return clk_prepare_enable(dai_props->clk);
}

static void asoc_simple_card_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct simple_card_data *priv =	snd_soc_card_get_drvdata(rtd->card);
	struct asoc_simple_dai *dai_props =
		simple_priv_to_props(priv, rtd->num);

	clk_disable_unprepare(dai_props->clk);
}

static struct snd_soc_ops asoc_simple_card_ops = {
	.startup = asoc_simple_card_startup,
	.shutdown = asoc_simple_card_shutdown,
};

static int asoc_simple_card_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	struct simple_card_data *priv =	snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *dai;
	struct snd_soc_dai_link *dai_link;
	struct asoc_simple_dai *dai_props;
	int num = rtd->num;

	dai_link	= simple_priv_to_link(priv, num);
	dai_props	= simple_priv_to_props(priv, num);
	dai		= dai_link->dynamic ?
				rtd->cpu_dai :
				rtd->codec_dai;

	return asoc_simple_card_init_dai(dai, dai_props);
}

static int asoc_simple_card_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					       struct snd_pcm_hw_params *params)
{
	struct simple_card_data *priv = snd_soc_card_get_drvdata(rtd->card);
	struct snd_interval *rate = hw_param_interval(params,
						      SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
							  SNDRV_PCM_HW_PARAM_CHANNELS);

	if (priv->convert_rate)
		rate->min =
		rate->max = priv->convert_rate;

	if (priv->convert_channels)
		channels->min =
		channels->max = priv->convert_channels;

	return 0;
}

static int asoc_simple_card_dai_link_of(struct device_node *port,
					struct device_node *ep,
					struct simple_card_data *priv,
					unsigned int daifmt,
					int idx, int is_fe)
{
	struct device *dev = simple_priv_to_dev(priv);
	struct snd_soc_dai_link *dai_link = simple_priv_to_link(priv, idx);
	struct asoc_simple_dai *dai_props = simple_priv_to_props(priv, idx);
	int ret;

	if (is_fe) {
		/* BE is dummy */
		dai_link->codec_of_node		= NULL;
		dai_link->codec_dai_name	= "snd-soc-dummy-dai";
		dai_link->codec_name		= "snd-soc-dummy";

		/* FE settings */
		dai_link->dynamic		= 1;
		dai_link->dpcm_merged_format	= 1;

		ret = asoc_simple_card_parse_graph_cpu(ep, dai_link);
		if (ret)
			return ret;

		ret = asoc_simple_card_parse_clk_cpu(ep, dai_link, dai_props);
		if (ret < 0)
			return ret;

		ret = asoc_simple_card_set_dailink_name(dev, dai_link,
							"fe.%s",
							dai_link->cpu_dai_name);
		if (ret < 0)
			return ret;

		/* snd_card.num_links includes Codec */
		asoc_simple_card_canonicalize_cpu(dai_link,
					(priv->snd_card.num_links - 1) == 1);
	} else {
		/* FE is dummy */
		dai_link->cpu_of_node		= NULL;
		dai_link->cpu_dai_name		= "snd-soc-dummy-dai";
		dai_link->cpu_name		= "snd-soc-dummy";

		/* BE settings */
		dai_link->no_pcm		= 1;
		dai_link->be_hw_params_fixup	= asoc_simple_card_be_hw_params_fixup;

		ret = asoc_simple_card_parse_graph_codec(ep, dai_link);
		if (ret < 0)
			return ret;

		ret = asoc_simple_card_parse_clk_codec(ep, dai_link, dai_props);
		if (ret < 0)
			return ret;

		ret = asoc_simple_card_set_dailink_name(dev, dai_link,
							"be.%s",
							dai_link->codec_dai_name);
		if (ret < 0)
			return ret;

		snd_soc_of_parse_audio_prefix_from_node(&priv->snd_card,
							port->parent,
							&priv->codec_conf,
							dai_link->codec_of_node,
							PREFIX "prefix");
	}

	ret = snd_soc_of_parse_tdm_slot(ep,
					&dai_props->tx_slot_mask,
					&dai_props->rx_slot_mask,
					&dai_props->slots,
					&dai_props->slot_width);
	if (ret)
		return ret;

	ret = asoc_simple_card_canonicalize_dailink(dai_link);
	if (ret < 0)
		return ret;

	dai_link->dai_fmt		= daifmt;
	dai_link->dpcm_playback		= 1;
	dai_link->dpcm_capture		= 1;
	dai_link->ops			= &asoc_simple_card_ops;
	dai_link->init			= asoc_simple_card_dai_init;

	dev_dbg(dev, "\t%s / %04x / %d\n",
		dai_link->name,
		dai_link->dai_fmt,
		dai_props->sysclk);

	return 0;
}

static int asoc_simple_card_parse_of(struct device_node *node,
				     struct simple_card_data *priv)
{
	struct device *dev = simple_priv_to_dev(priv);
	struct device *cpu_dev = dev->parent;
	struct device_node *ports = of_graph_get_top_port(cpu_dev);
	struct snd_soc_card *card = &priv->snd_card;
	struct device_node *port, *cpu_ep, *r_cpu_ep, *codec_ep;
	unsigned int daifmt = 0;
	int i, ret, done;

	if (!node)
		return -EINVAL;

	ret = snd_soc_of_parse_audio_routing_from_node(&priv->snd_card,
						       ports, PREFIX "routing");
	if (ret)
		return ret;

	/* sampling rate convert */
	of_property_read_u32(ports, PREFIX "convert-rate",
			     &priv->convert_rate);

	/* channels transfer */
	of_property_read_u32(ports, PREFIX "convert-channels",
			     &priv->convert_channels);

	/*
	 * it supports multi CPU, single CODEC only here.
	 */

	/* find 1st codec */
	done = 0;
	for_each_of_port(node, port) {
		/* keep for_each for of_node_get/of_node_put */
		if (done)
			continue;

		if (!of_graph_port_type_is_sound(port))
			continue;

		for_each_of_endpoint_in_port(port, cpu_ep) {
			/* keep for_each for of_node_get/of_node_put */
			if (done)
				continue;

			codec_ep = of_graph_get_remote_endpoint(cpu_ep);
			r_cpu_ep = of_graph_get_remote_endpoint(codec_ep);
			of_node_put(codec_ep);
			of_node_put(r_cpu_ep);
			if (cpu_ep != r_cpu_ep) {
				ret = -EINVAL;
				goto parse_of_err;
			}

			ret = asoc_simple_card_parse_daifmt(dev,
							    ports, codec_ep,
							    PREFIX, &daifmt);
			if (ret < 0)
				goto parse_of_err;

			done = 1;
		}
	}

	/* Front-End (= CPU) */
	i = 0;
	for_each_of_port(node, port) {
		if (!of_graph_port_type_is_sound(port))
			continue;

		for_each_of_endpoint_in_port(port, cpu_ep) {
			ret = asoc_simple_card_dai_link_of(
				port, cpu_ep, priv, daifmt, i, 1);
			if (ret < 0)
				goto parse_of_err;
			i++;
		}
	}

	/* Back-End (= Codec) */
	done = 0;
	for_each_of_port(node, port) {
		if (!of_graph_port_type_is_sound(port))
			continue;

		/* keep for_each for of_node_get/of_node_put */
		if (done)
			continue;

		for_each_of_endpoint_in_port(port, cpu_ep) {
			/* keep for_each for of_node_get/of_node_put */
			if (done)
				continue;

			codec_ep = of_graph_get_remote_endpoint(cpu_ep);
			r_cpu_ep = of_graph_get_remote_endpoint(codec_ep);
			of_node_put(codec_ep);
			of_node_put(r_cpu_ep);
			if (cpu_ep != r_cpu_ep) {
				ret = -EINVAL;
				goto parse_of_err;
			}

			ret = asoc_simple_card_dai_link_of(
				port, codec_ep, priv, daifmt, i, 0);
			if (ret < 0)
				goto parse_of_err;
			i++;

			done = 1;
		}
	}

	ret = asoc_simple_card_parse_card_name(card, ports, PREFIX);
	if (ret)
		return ret;

	dev_dbg(dev, "New card: %s\n",
		priv->snd_card.name ? priv->snd_card.name : "");
	dev_dbg(dev, "convert_rate     %d\n", priv->convert_rate);
	dev_dbg(dev, "convert_channels %d\n", priv->convert_channels);

	return 0;
parse_of_err:
	of_node_put(port);
	of_node_put(cpu_ep);

	return ret;
}

static int asoc_get_sound_port_count(struct device_node *cpu_node)
{
	int num;

	/*
	 * it supports multi CPU, single CODEC only here.
	 */

	/* CPU */
	num = of_graph_get_sound_endpoint_count(cpu_node);

	/* Codec */
	num++;

	return num;
}

static int asoc_simple_card_probe(struct platform_device *pdev)
{
	struct simple_card_data *priv;
	struct snd_soc_dai_link *dai_link;
	struct asoc_simple_dai *dai_props;
	struct device *dev = &pdev->dev;
	struct device *cpu_dev = pdev->dev.parent;
	struct device_node *cpu_node = cpu_dev->of_node;
	int num, ret;

	/* Allocate the private data and the DAI link array */
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	num = asoc_get_sound_port_count(cpu_node);

	dai_props = devm_kzalloc(dev, sizeof(*dai_props) * num, GFP_KERNEL);
	dai_link  = devm_kzalloc(dev, sizeof(*dai_link)  * num, GFP_KERNEL);
	if (!dai_props || !dai_link)
		return -ENOMEM;

	priv->dai_props			= dai_props;
	priv->dai_link			= dai_link;

	/* Init snd_soc_card */
	priv->snd_card.owner		= THIS_MODULE;
	priv->snd_card.dev		= dev;
	priv->snd_card.dai_link		= priv->dai_link;
	priv->snd_card.num_links	= num;
	priv->snd_card.codec_conf	= &priv->codec_conf;
	priv->snd_card.num_configs	= 1;

	ret = asoc_simple_card_parse_of(cpu_node, priv);
	if (ret < 0) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "parse error %d\n", ret);
		goto err;
	}

	/*
	 * FIXME
	 *
	 * This driver is assuming that it will be called from
	 * asoc_simple_card_try_to_probe_graph_card() which
	 * is using platform_device_register_full().
	 * This means it is not came from DT. But this driver itself
	 * will be used as part of ALSA SoC (= sound card).
	 * Because of these background, it might fail in
	 * snd_pcm_lib_malloc_pages() on .hw_params.
	 * Because, noone cares its dma_ops, and result of get_dma_ops()
	 * is based on its architecture.
	 * So, it should call arch_setup_dma_ops() from somewhere,
	 * otherwise, for example, ARM is no problem, but ARM64 will be fail.
	 * But, of_platform_device_xxx() are not good solution today.
	 * This driver calls it by itself here. Please fixme
	 * see also
	 * linux/sound/soc/generic/simple-card-utils.c ::
	 *	asoc_simple_card_try_to_probe_graph_card()
	 */
	of_dma_configure(dev, dev->of_node);

	snd_soc_card_set_drvdata(&priv->snd_card, priv);

	ret = devm_snd_soc_register_card(dev, &priv->snd_card);
	if (ret >= 0)
		return ret;
err:
	asoc_simple_card_clean_reference(&priv->snd_card);

	return ret;
}

static int asoc_simple_card_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct simple_card_data *priv = snd_soc_card_get_drvdata(card);

	return asoc_simple_card_clean_reference(&priv->snd_card);
}

static struct platform_driver asoc_simple_card = {
	.driver = {
		.name = "asoc-simple-graph-scu-card",
	},
	.probe = asoc_simple_card_probe,
	.remove = asoc_simple_card_remove,
};
module_platform_driver(asoc_simple_card);

MODULE_ALIAS("platform:asoc-simple-graph-scu-card");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("ASoC Simple Graph SCU Sound Card");
MODULE_AUTHOR("Kuninori Morimoto <kuninori.morimoto.gx@renesas.com>");
