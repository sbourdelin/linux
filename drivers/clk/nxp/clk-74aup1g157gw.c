// SPDX-License-Identifier: GPL-2.0
//
// Copyright (C) 2018 Renesas Solutions Corp.
// Kuninori Morimoto <kuninori.morimoto.gx@renesas.com>
//
#include <linux/module.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#define CLK_I_NUM 2
struct clk_priv {
	struct clk_hw hw;
	struct device *dev;
	struct clk *i[CLK_I_NUM];
	struct gpio_desc *sel;
	long (*round_rate)(struct clk_hw *hw, unsigned long rate,
			   unsigned long *parent_rate);
};
#define hw_to_priv(_hw) container_of(_hw, struct clk_priv, hw)
#define priv_to_dev(priv) priv->dev
#define for_each_iclk(i)			\
	for ((i) = 0; (i) < CLK_I_NUM; (i)++)

static int clk74_set_rate(struct clk_hw *hw, unsigned long rate,
			  unsigned long parent_rate)
{
	struct clk_priv *priv = hw_to_priv(hw);
	struct device *dev = priv_to_dev(priv);
	int i;

	for_each_iclk(i) {
		if (rate == clk_get_rate(priv->i[i])) {
			dev_dbg(dev, "set rate %lu as i%d\n", rate, i);
			gpiod_set_value_cansleep(priv->sel, i);
			return 0;
		}
	}

	dev_err(dev, "unsupported rate %lu\n", rate);
	return -EIO;
}

static long clk74_round_rate_close(struct clk_hw *hw, unsigned long rate,
			     unsigned long *parent_rate)
{
	struct clk_priv *priv = hw_to_priv(hw);
	struct device *dev = priv_to_dev(priv);
	unsigned long min = ~0;
	unsigned long ret = 0;
	int i;

	/*
	 * select closest rate
	 */
	for_each_iclk(i) {
		unsigned long irate = clk_get_rate(priv->i[i]);
		unsigned long diff = abs(rate - irate);

		if (min > diff) {
			min = diff;
			ret = irate;
		}
	}

	dev_dbg(dev, "(close)round rate %lu\n", ret);

	return ret;
}

static long clk74_round_rate_audio(struct clk_hw *hw, unsigned long rate,
			     unsigned long *parent_rate)
{
	struct clk_priv *priv = hw_to_priv(hw);
	struct device *dev = priv_to_dev(priv);
	unsigned long ret = 0;
	int is_8k = 0;
	int i;

	/*
	 * select 48kHz or 44.1kHz category rate
	 */
	if (!(rate % 8000))
		is_8k = 1;

	for_each_iclk(i) {
		unsigned long irate = clk_get_rate(priv->i[i]);

		if (( is_8k && !(irate % 8000)) ||
		    (!is_8k &&  (irate % 8000))) {
			ret = irate;
		}
	}

	dev_dbg(dev, "(audio)round rate %lu\n", ret);

	return ret;
}

static long clk74_round_rate(struct clk_hw *hw, unsigned long rate,
			     unsigned long *parent_rate)
{
	struct clk_priv *priv = hw_to_priv(hw);

	return priv->round_rate(hw, rate, parent_rate);
}

static unsigned long clk74_recalc_rate(struct clk_hw *hw,
				     unsigned long parent_rate)
{
	struct clk_priv *priv = hw_to_priv(hw);
	struct device *dev = priv_to_dev(priv);
	unsigned long rate;
	int i = gpiod_get_raw_value_cansleep(priv->sel);

	rate = clk_get_rate(priv->i[i]);

	dev_dbg(dev, "recalc rate %lu as i%d\n", rate, i);

	return rate;
}

static u8 clk74_get_parent(struct clk_hw *hw)
{
	struct clk_priv *priv = hw_to_priv(hw);

	return gpiod_get_raw_value_cansleep(priv->sel);
}

static const struct clk_ops clk74_ops = {
	.set_rate	= clk74_set_rate,
	.round_rate	= clk74_round_rate,
	.recalc_rate	= clk74_recalc_rate,
	.get_parent	= clk74_get_parent,
};

static const char * const clk74_in_name[CLK_I_NUM] = {
	"i0",
	"i1",
};

static int clk74_probe(struct platform_device *pdev)
{
	struct clk *clk;
	struct clk_init_data init;
	struct clk_priv *priv;
	struct device *dev = &pdev->dev;
	const char *parent_names[CLK_I_NUM];
	int i, ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENODEV;

	for_each_iclk(i) {
		clk = devm_clk_get(dev, clk74_in_name[i]);
		if (IS_ERR(clk))
			return -EPROBE_DEFER;
		priv->i[i] = clk;
		parent_names[i] = __clk_get_name(clk);
	}

	memset(&init, 0, sizeof(init));
	init.name		= "74aup1g157gw";
	init.ops		= &clk74_ops;
	init.parent_names	= parent_names;
	init.num_parents	= CLK_I_NUM;

	priv->hw.init		= &init;
	priv->dev		= dev;
	priv->round_rate	= of_device_get_match_data(dev);
	priv->sel		= devm_gpiod_get(dev, "sel", 0);
	if (IS_ERR(priv->sel))
		return -EPROBE_DEFER;

	gpiod_direction_output(priv->sel, 0);

	ret = devm_clk_hw_register(dev, &priv->hw);
	if (ret < 0)
		return ret;

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get, &priv->hw);
	if (ret < 0)
		return ret;

	platform_set_drvdata(pdev, priv);

	dev_info(dev, "probed\n");

	return 0;
}

#define OF_ID(name, func) { .compatible = name, .data = func }
static const struct of_device_id clk74_of_match[] = {
	OF_ID("nxp,74aup1g157gw-clk",		clk74_round_rate_close),
	OF_ID("nxp,74aup1g157gw-audio-clk",	clk74_round_rate_audio),
	{ }
};
MODULE_DEVICE_TABLE(of, clk74_of_match);

static struct platform_driver clk74_driver = {
	.driver = {
		.name = "74aup1g157gw",
		.of_match_table = clk74_of_match,
	},
	.probe = clk74_probe,
};
builtin_platform_driver(clk74_driver);
