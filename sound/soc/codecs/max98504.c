/*
 * MAX98504 ALSA SoC Audio driver
 *
 * Copyright 2013 - 2014 Maxim Integrated Products
 * Copyright 2016 Samsung Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <sound/soc.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include "max98504.h"

static struct reg_default max98504_reg_defaults[] = {
	{ 0x01,	0},
	{ 0x02,	0},
	{ 0x03,	0},
	{ 0x04,	0},
	{ 0x10, 0},
	{ 0x11, 0},
	{ 0x12, 0},
	{ 0x13, 0},
	{ 0x14, 0},
	{ 0x15, 0},
	{ 0x16, 0},
	{ 0x17, 0},
	{ 0x18, 0},
	{ 0x19, 0},
	{ 0x1A, 0},
	{ 0x20, 0},
	{ 0x21, 0},
	{ 0x22, 0},
	{ 0x23, 0},
	{ 0x24, 0},
	{ 0x25, 0},
	{ 0x26, 0},
	{ 0x27, 0},
	{ 0x28, 0},
	{ 0x30, 0},
	{ 0x31, 0},
	{ 0x32, 0},
	{ 0x33, 0},
	{ 0x34, 0},
	{ 0x35, 0},
	{ 0x36, 0},
	{ 0x37, 0},
	{ 0x38, 0},
	{ 0x39, 0},
	{ 0x40, 0},
	{ 0x41, 0},
};

static bool max98504_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case MAX98504_REG_INTERRUPT_STATUS:
	case MAX98504_REG_INTERRUPT_FLAGS:
	case MAX98504_REG_INTERRUPT_FLAG_CLEARS:
	case MAX98504_REG_WATCHDOG_CLEAR:
	case MAX98504_REG_GLOBAL_ENABLE:
	case MAX98504_REG_SOFTWARE_RESET:
		return true;
	default:
		return false;
	}
}

static bool max98504_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case MAX98504_REG_SOFTWARE_RESET:
	case MAX98504_REG_WATCHDOG_CLEAR:
	case MAX98504_REG_INTERRUPT_FLAG_CLEARS:
		return false;
	default:
		return true;
	}
}

static int max98504_probe(struct max98504_priv *max98504)
{
	struct regmap *map = max98504->regmap;

	/* Software reset */
	regmap_write(map, MAX98504_REG_SOFTWARE_RESET, 0x01);
	msleep(20);

	/* DAI PCM/PDM Rx configuration */
	switch (max98504->rx_path) {
	case MODE_RX_PCM:
		regmap_write(map, MAX98504_REG_PCM_RX_ENABLE,
			     max98504->rx_ch_enable);
		break;
	case MODE_RX_PDM0...MODE_RX_PDM1:
		regmap_write(map, MAX98504_REG_PDM_RX_ENABLE,
			     M98504_PDM_RX_EN_MASK);
		break;
	default:
		/* Analog input, disable PCM, PDM Rx */
		regmap_write(map, MAX98504_REG_PCM_RX_ENABLE, 0);
		regmap_write(map, MAX98504_REG_PDM_RX_ENABLE, 0);
	}

	regmap_write(map, MAX98504_REG_SPEAKER_SOURCE_SELECT,
		     max98504->rx_path & M98504_SPK_SRC_SEL_MASK);

	/* DAI PCM/PDM Tx configuration */
	switch (max98504->tx_path) {
	case MODE_TX_PCM:
		regmap_write(map, MAX98504_REG_PCM_TX_ENABLE,
			     max98504->tx_ch_enable);
		regmap_write(map, MAX98504_REG_PCM_TX_CHANNEL_SOURCES,
			     max98504->tx_ch_source);
		break;
	default:
		/* PDM Tx */
		regmap_write(map, MAX98504_REG_PDM_TX_ENABLE,
			     max98504->tx_ch_enable);
		regmap_write(map, MAX98504_REG_PDM_TX_CONTROL,
			     max98504->tx_ch_source);
	}

	regmap_write(map, MAX98504_REG_MEASUREMENT_ENABLE, 0x3);

	/* Brownout protection */
	regmap_write(map, MAX98504_REG_PVDD_BROWNOUT_ENABLE, 0x1);
	regmap_write(map, MAX98504_REG_PVDD_BROWNOUT_CONFIG(1), 0x33);
	regmap_write(map, MAX98504_REG_PVDD_BROWNOUT_CONFIG(2), 0x0a);
	regmap_write(map, MAX98504_REG_PVDD_BROWNOUT_CONFIG(3), 0xff);
	regmap_write(map, MAX98504_REG_PVDD_BROWNOUT_CONFIG(4), 0xff);

	return 0;
}

static const struct snd_soc_dapm_route max98504_dapm_routes[] = {
	{ "SPKOUT", NULL, "Global Enable" },
};

static const struct snd_soc_dapm_widget max98504_dapm_widgets[] = {
	SND_SOC_DAPM_SUPPLY("Global Enable", MAX98504_REG_GLOBAL_ENABLE,
		0, 0, NULL, 0),
	SND_SOC_DAPM_REG(snd_soc_dapm_spk, "SPKOUT",
		MAX98504_REG_SPEAKER_ENABLE, 0, 1, 1, 0),
};

static const struct snd_soc_component_driver max98504_component_driver = {
	.dapm_widgets		= max98504_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(max98504_dapm_widgets),
	.dapm_routes		= max98504_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(max98504_dapm_routes),
};

static const struct regmap_config max98504_regmap = {
	.reg_bits		= 16,
	.val_bits		= 8,
	.max_register		= MAX98504_MAX_REGISTER,
	.reg_defaults		= max98504_reg_defaults,
	.num_reg_defaults	= ARRAY_SIZE(max98504_reg_defaults),
	.volatile_reg		= max98504_volatile_register,
	.readable_reg		= max98504_readable_register,
	.cache_type		= REGCACHE_RBTREE,
};

static void max98504_parse_dt(struct max98504_priv *max98504,
			      struct device_node *of_node)
{
	of_property_read_u32(of_node, "maxim,rx-path",
			     &max98504->rx_path);

	of_property_read_u32(of_node, "maxim,tx-path",
			     &max98504->tx_path);

	of_property_read_u32(of_node, "maxim,rx-channel-mask",
			     &max98504->rx_ch_enable);

	of_property_read_u32(of_node, "maxim,tx-channel-mask",
			     &max98504->tx_ch_enable);

	of_property_read_u32(of_node, "maxim,tx-channel-source",
			     &max98504->tx_ch_source);
}

static int max98504_i2c_probe(struct i2c_client *client,
			      const struct i2c_device_id *id)
{
	struct max98504_priv *max98504;
	int ret;

	max98504 = devm_kzalloc(&client->dev, sizeof(*max98504), GFP_KERNEL);
	if (!max98504)
		return -ENOMEM;

	if (client->dev.of_node)
		max98504_parse_dt(max98504, client->dev.of_node);

	max98504->regmap = devm_regmap_init_i2c(client, &max98504_regmap);
	if (IS_ERR(max98504->regmap)) {
		ret = PTR_ERR(max98504->regmap);
		dev_err(&client->dev, "regmap initialization failed: %d\n", ret);
		return ret;
	}

	ret = max98504_probe(max98504);
	if (ret < 0)
		return ret;

	return devm_snd_soc_register_component(&client->dev,
				&max98504_component_driver, NULL, 0);
}

#ifdef CONFIG_OF
static const struct of_device_id max98504_of_match[] = {
	{ .compatible = "maxim,max98504", },
	{ },
};
MODULE_DEVICE_TABLE(of, rt5651_of_match);
#endif

static const struct i2c_device_id max98504_i2c_id[] = {
	{ "max98504" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, max98504_i2c_id);

static struct i2c_driver max98504_i2c_driver = {
	.driver = {
		.name = "max98504",
		.of_match_table = of_match_ptr(max98504_of_match),
	},
	.probe = max98504_i2c_probe,
	.id_table = max98504_i2c_id,
};
module_i2c_driver(max98504_i2c_driver);

MODULE_DESCRIPTION("ASoC MAX98504 driver");
MODULE_LICENSE("GPL");
