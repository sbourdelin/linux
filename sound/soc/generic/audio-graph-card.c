/*
 * ASoC audio graph sound card support
 *
 * Copyright (C) 2016 Renesas Solutions Corp.
 * Kuninori Morimoto <kuninori.morimoto.gx@renesas.com>
 *
 * based on ${LINUX}/sound/soc/generic/simple-card.c
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

struct graph_card_data {
	struct snd_soc_card snd_card;
	struct graph_dai_props {
		struct asoc_simple_dai cpu_dai;
		struct asoc_simple_dai codec_dai;
	} *dai_props;
	struct snd_soc_dai_link *dai_link;
};

#define graph_priv_to_dev(priv) ((priv)->snd_card.dev)
#define graph_priv_to_link(priv, i) ((priv)->snd_card.dai_link + (i))
#define graph_priv_to_props(priv, i) ((priv)->dai_props + (i))

static int asoc_graph_card_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct graph_card_data *priv =	snd_soc_card_get_drvdata(rtd->card);
	struct graph_dai_props *dai_props =
		graph_priv_to_props(priv, rtd->num);
	int ret;

	ret = clk_prepare_enable(dai_props->cpu_dai.clk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(dai_props->codec_dai.clk);
	if (ret)
		clk_disable_unprepare(dai_props->cpu_dai.clk);

	return ret;
}

static void asoc_graph_card_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct graph_card_data *priv =	snd_soc_card_get_drvdata(rtd->card);
	struct graph_dai_props *dai_props =
		graph_priv_to_props(priv, rtd->num);

	clk_disable_unprepare(dai_props->cpu_dai.clk);

	clk_disable_unprepare(dai_props->codec_dai.clk);
}

static struct snd_soc_ops asoc_graph_card_ops = {
	.startup = asoc_graph_card_startup,
	.shutdown = asoc_graph_card_shutdown,
};

static int asoc_graph_card_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	struct graph_card_data *priv =	snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *codec = rtd->codec_dai;
	struct snd_soc_dai *cpu = rtd->cpu_dai;
	struct graph_dai_props *dai_props =
		graph_priv_to_props(priv, rtd->num);
	int ret;

	ret = asoc_simple_card_init_dai(codec, &dai_props->codec_dai);
	if (ret < 0)
		return ret;

	ret = asoc_simple_card_init_dai(cpu, &dai_props->cpu_dai);
	if (ret < 0)
		return ret;

	return 0;
}

static int asoc_graph_card_dai_link_of(struct device_node *cpu_port,
					struct graph_card_data *priv,
					int idx)
{
	struct device *dev = graph_priv_to_dev(priv);
	struct snd_soc_dai_link *dai_link = graph_priv_to_link(priv, idx);
	struct graph_dai_props *dai_props = graph_priv_to_props(priv, idx);
	struct asoc_simple_dai *cpu_dai = &dai_props->cpu_dai;
	struct asoc_simple_dai *codec_dai = &dai_props->codec_dai;
	struct device_node *cpu_ep    = of_get_next_child(cpu_port, NULL);
	struct device_node *codec_ep = of_graph_get_remote_endpoint(cpu_ep);
	struct device_node *rcpu_ep = of_graph_get_remote_endpoint(codec_ep);
	int ret;

	if (rcpu_ep != cpu_ep) {
		dev_err(dev, "remote-endpoint missmatch (%s/%s/%s)\n",
			cpu_ep->name, codec_ep->name, rcpu_ep->name);
		return -EINVAL;
	}

	ret = asoc_simple_card_parse_daifmt(dev, cpu_ep, codec_ep,
					    NULL, &dai_link->dai_fmt);
	if (ret < 0)
		goto dai_link_of_err;

	/*
	 * we need to consider "mclk-fs" around here
	 * see simple-card
	 */

	ret = asoc_simple_card_parse_graph_cpu(cpu_ep, dai_link);
	if (ret < 0)
		goto dai_link_of_err;

	ret = asoc_simple_card_parse_graph_codec(codec_ep, dai_link);
	if (ret < 0)
		goto dai_link_of_err;

	ret = snd_soc_of_parse_tdm_slot(cpu_ep,
					&cpu_dai->tx_slot_mask,
					&cpu_dai->rx_slot_mask,
					&cpu_dai->slots,
					&cpu_dai->slot_width);
	if (ret < 0)
		goto dai_link_of_err;

	ret = snd_soc_of_parse_tdm_slot(codec_ep,
					&codec_dai->tx_slot_mask,
					&codec_dai->rx_slot_mask,
					&codec_dai->slots,
					&codec_dai->slot_width);
	if (ret < 0)
		goto dai_link_of_err;

	ret = asoc_simple_card_parse_clk_cpu(dev, cpu_ep, dai_link, cpu_dai);
	if (ret < 0)
		goto dai_link_of_err;

	ret = asoc_simple_card_parse_clk_codec(dev, codec_ep, dai_link, codec_dai);
	if (ret < 0)
		goto dai_link_of_err;

	ret = asoc_simple_card_canonicalize_dailink(dai_link);
	if (ret < 0)
		goto dai_link_of_err;

	ret = asoc_simple_card_set_dailink_name(dev, dai_link,
						"%s-%s",
						dai_link->cpu_dai_name,
						dai_link->codec_dai_name);
	if (ret < 0)
		goto dai_link_of_err;

	dai_link->ops = &asoc_graph_card_ops;
	dai_link->init = asoc_graph_card_dai_init;

	dev_dbg(dev, "\tname : %s\n", dai_link->stream_name);
	dev_dbg(dev, "\tformat : %04x\n", dai_link->dai_fmt);
	dev_dbg(dev, "\tcpu : %s / %d\n",
		dai_link->cpu_dai_name,
		dai_props->cpu_dai.sysclk);
	dev_dbg(dev, "\tcodec : %s / %d\n",
		dai_link->codec_dai_name,
		dai_props->codec_dai.sysclk);

	asoc_simple_card_canonicalize_cpu(dai_link,
					  priv->snd_card.num_links == 1);

dai_link_of_err:
	of_node_put(codec_ep);

	return ret;
}

static int asoc_graph_card_parse_of(struct device *dev,
				     struct graph_card_data *priv)
{
	struct device_node *node = dev->of_node;
	struct snd_soc_card *card = &priv->snd_card;
	struct device_node *np;
	int idx = 0;
	int ret;

	/*
	 * we need to consider "widgets", "routing", "mclk-fs" around here
	 * see simple-card
	 */

	while ((np = of_parse_phandle(node, "dais", idx))) {
		ret = asoc_graph_card_dai_link_of(np, priv, idx++);
		if (ret < 0)
			return ret;
	}

	return asoc_simple_card_parse_card_name(card, NULL);
}

static int asoc_graph_get_dais_count(struct device *dev)
{
	struct device_node *node = dev->of_node;
	int count = 0;

	if (of_get_property(node, "dais", NULL)) {
		while (of_parse_phandle(node, "dais", count))
			count++;
	} else {
		count = 1;
	}

	return count;
}

static int asoc_graph_card_probe(struct platform_device *pdev)
{
	struct graph_card_data *priv;
	struct snd_soc_dai_link *dai_link;
	struct graph_dai_props *dai_props;
	struct device *dev = &pdev->dev;
	int num, ret;

	/* Allocate the private data and the DAI link array */
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	num = asoc_graph_get_dais_count(dev);

	dai_props = devm_kzalloc(dev, sizeof(*dai_props) * num, GFP_KERNEL);
	dai_link  = devm_kzalloc(dev, sizeof(*dai_link)  * num, GFP_KERNEL);
	if (!dai_props || !dai_link)
		return -ENOMEM;

	priv->dai_props			= dai_props;
	priv->dai_link			= dai_link;

	/* Init snd_soc_card */
	priv->snd_card.owner		= THIS_MODULE;
	priv->snd_card.dev		= dev;
	priv->snd_card.dai_link		= dai_link;
	priv->snd_card.num_links	= num;

	ret = asoc_graph_card_parse_of(dev, priv);
	if (ret < 0) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "parse error %d\n", ret);
		goto err;
	}

	snd_soc_card_set_drvdata(&priv->snd_card, priv);

	ret = devm_snd_soc_register_card(dev, &priv->snd_card);
	if (ret >= 0)
		return ret;
err:
	asoc_simple_card_clean_reference(&priv->snd_card);

	return ret;
}

static int asoc_graph_card_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct graph_card_data *priv = snd_soc_card_get_drvdata(card);

	return asoc_simple_card_clean_reference(&priv->snd_card);
}

static const struct of_device_id asoc_graph_of_match[] = {
	{ .compatible = "audio-graph-card", },
	{},
};
MODULE_DEVICE_TABLE(of, asoc_graph_of_match);

static struct platform_driver asoc_graph_card = {
	.driver = {
		.name = "asoc-audio-graph-card",
		.of_match_table = asoc_graph_of_match,
	},
	.probe = asoc_graph_card_probe,
	.remove = asoc_graph_card_remove,
};
module_platform_driver(asoc_graph_card);

MODULE_ALIAS("platform:asoc-audio-graph-card");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("ASoC Audio Graph Sound Card");
MODULE_AUTHOR("Kuninori Morimoto <kuninori.morimoto.gx@renesas.com>");
