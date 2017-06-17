/*
 * Copyright (C) 2017 Sanechips Technology Co., Ltd.
 * Copyright 2017 Linaro Ltd.
 *
 * Author: Baoyou Xie <baoyou.xie@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <sound/tlv.h>

#define AUD96P22_RESET			0x00
#define AUD96P22_I2S1_CONFIG_0		0x03
#define I2S1_MS_MODE			BIT(3)
#define I2S1_MODE_MASK			0x7
#define I2S1_MODE_RIGHT_J		0x0
#define I2S1_MODE_I2S			0x1
#define I2S1_MODE_LEFT_J		0x2
#define AUD96P22_PD_0			0x15
#define PD_0_PDZ			BIT(0)
#define AUD96P22_PD_1			0x16
#define AUD96P22_PD_3			0x18
#define AUD96P22_PD_4			0x19
#define AUD96P22_MUTE_0			0x1d
#define AUD96P22_MUTE_2			0x1f
#define AUD96P22_MUTE_4			0x21
#define AUD96P22_RECVOL_0		0x24
#define AUD96P22_RECVOL_1		0x25
#define AUD96P22_PGA1VOL_0		0x26
#define AUD96P22_PGA1VOL_1		0x27
#define AUD96P22_LMVOL_0		0x34
#define AUD96P22_LMVOL_1		0x35
#define AUD96P22_HS1VOL_0		0x38
#define AUD96P22_HS1VOL_1		0x39
#define AUD96P22_PGA1SEL_0		0x47
#define AUD96P22_PGA1SEL_1		0x48
#define AUD96P22_LDR1SEL_0		0x59
#define AUD96P22_LDR1SEL_1		0x60
#define AUD96P22_LDR2SEL_0		0x5d
#define AUD96P22_REG_MAX		0xfb

struct aud96p22_priv {
	struct regmap *regmap;
	struct gpio_desc *pa_gpio;
};

static int lineout_driver_event(struct snd_soc_dapm_widget *w,
				struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);
	struct aud96p22_priv *priv = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		gpiod_set_value_cansleep(priv->pa_gpio, 1);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		gpiod_set_value_cansleep(priv->pa_gpio, 0);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const DECLARE_TLV_DB_SCALE(lm_tlv, -11550, 50, 0);
static const DECLARE_TLV_DB_SCALE(hs_tlv, -3900, 300, 0);
static const DECLARE_TLV_DB_SCALE(rec_tlv, -9550, 50, 0);
static const DECLARE_TLV_DB_SCALE(pga_tlv, -1800, 100, 0);

static const struct snd_kcontrol_new aud96p22_snd_controls[] = {
	/* Volume control */
	SOC_DOUBLE_R_TLV("Playback Master Volume", AUD96P22_LMVOL_0,
			 AUD96P22_LMVOL_1, 0, 0xff, 0, lm_tlv),
	SOC_DOUBLE_R_TLV("Headset Volume", AUD96P22_HS1VOL_0,
			 AUD96P22_HS1VOL_1, 0, 0xf, 0, hs_tlv),
	SOC_DOUBLE_R_TLV("Recording Master Volume", AUD96P22_RECVOL_0,
			 AUD96P22_RECVOL_1, 0, 0xff, 0, rec_tlv),
	SOC_DOUBLE_R_TLV("Recording PGA Volume", AUD96P22_PGA1VOL_0,
			 AUD96P22_PGA1VOL_1, 0, 0x37, 0, pga_tlv),

	/* Mute control */
	SOC_DOUBLE("Playback Master Mute", AUD96P22_MUTE_2, 0, 1, 1, 0),
	SOC_DOUBLE("Headset Mute", AUD96P22_MUTE_2, 4, 5, 1, 0),
	SOC_DOUBLE("Lineout1 Mute", AUD96P22_MUTE_4, 0, 1, 1, 0),
	SOC_DOUBLE("Lineout2 Mute", AUD96P22_MUTE_4, 2, 3, 1, 0),
	SOC_DOUBLE("Recording Master Mute", AUD96P22_MUTE_0, 0, 1, 1, 0),
	SOC_DOUBLE("Recording PGA Mute", AUD96P22_MUTE_0, 2, 3, 1, 0),
};

/* Input mux kcontrols */
static const unsigned int ain_mux_values[] = {
	0, 1, 3, 4, 5,
};

static const char * const ainl_mux_texts[] = {
	"AINL1 differential",
	"AINL1 single-ended",
	"AINL3 single-ended",
	"AINL2 differential",
	"AINL2 single-ended",
};

static const char * const ainr_mux_texts[] = {
	"AINR1 differential",
	"AINR1 single-ended",
	"AINR3 single-ended",
	"AINR2 differential",
	"AINR2 single-ended",
};

static SOC_VALUE_ENUM_SINGLE_DECL(ainl_mux_enum, AUD96P22_PGA1SEL_0,
				  0, 0x7, ainl_mux_texts, ain_mux_values);
static SOC_VALUE_ENUM_SINGLE_DECL(ainr_mux_enum, AUD96P22_PGA1SEL_1,
				  0, 0x7, ainr_mux_texts, ain_mux_values);

static const struct snd_kcontrol_new ainl_mux_kcontrol =
			SOC_DAPM_ENUM("AIN Left Path", ainl_mux_enum);
static const struct snd_kcontrol_new ainr_mux_kcontrol =
			SOC_DAPM_ENUM("AIN Right Path", ainr_mux_enum);

/* Output mixer kcontrols */
static const struct snd_kcontrol_new ld1_left_kcontrols[] = {
	SOC_DAPM_SINGLE("LD1L DACL Switch", AUD96P22_LDR1SEL_0, 0, 1, 0),
	SOC_DAPM_SINGLE("LD1L AINL Switch", AUD96P22_LDR1SEL_0, 1, 1, 0),
	SOC_DAPM_SINGLE("LD1L AINR Switch", AUD96P22_LDR1SEL_0, 2, 1, 0),
};

static const struct snd_kcontrol_new ld1_right_kcontrols[] = {
	SOC_DAPM_SINGLE("LD1R DACR Switch", AUD96P22_LDR1SEL_1, 8, 1, 0),
	SOC_DAPM_SINGLE("LD1R AINR Switch", AUD96P22_LDR1SEL_1, 9, 1, 0),
	SOC_DAPM_SINGLE("LD1R AINL Switch", AUD96P22_LDR1SEL_1, 10, 1, 0),
};

static const struct snd_kcontrol_new ld2_kcontrols[] = {
	SOC_DAPM_SINGLE("LD2 DACL Switch", AUD96P22_LDR2SEL_0, 0, 1, 0),
	SOC_DAPM_SINGLE("LD2 AINL Switch", AUD96P22_LDR2SEL_0, 1, 1, 0),
	SOC_DAPM_SINGLE("LD2 DACR Switch", AUD96P22_LDR2SEL_0, 2, 1, 0),
};

/* Headset power kcontrols */
static const struct snd_kcontrol_new hs1l_kcontrol =
		SOC_DAPM_SINGLE("HS1L Switch", AUD96P22_PD_3, 4, 1, 0);
static const struct snd_kcontrol_new hs1r_kcontrol =
		SOC_DAPM_SINGLE("HS1R Switch", AUD96P22_PD_3, 5, 1, 0);

static const struct snd_soc_dapm_widget aud96p22_dapm_widgets[] = {
	/* Input pins */
	SND_SOC_DAPM_INPUT("AINL1P"),
	SND_SOC_DAPM_INPUT("AINL2P"),
	SND_SOC_DAPM_INPUT("AINL3"),
	SND_SOC_DAPM_INPUT("AINL1N"),
	SND_SOC_DAPM_INPUT("AINL2N"),
	SND_SOC_DAPM_INPUT("AINR2N"),
	SND_SOC_DAPM_INPUT("AINR1N"),
	SND_SOC_DAPM_INPUT("AINR3"),
	SND_SOC_DAPM_INPUT("AINR2P"),
	SND_SOC_DAPM_INPUT("AINR1P"),

	/* Input muxes */
	SND_SOC_DAPM_MUX("AINLMUX", AUD96P22_PD_1, 2, 0, &ainl_mux_kcontrol),
	SND_SOC_DAPM_MUX("AINRMUX", AUD96P22_PD_1, 3, 0, &ainr_mux_kcontrol),

	/* ADCs */
	SND_SOC_DAPM_ADC("ADCL", "Capture Left", AUD96P22_PD_1, 0, 0),
	SND_SOC_DAPM_ADC("ADCR", "Capture Right", AUD96P22_PD_1, 1, 0),

	/* DACs */
	SND_SOC_DAPM_DAC("DACL", "Playback Left", AUD96P22_PD_3, 0, 0),
	SND_SOC_DAPM_DAC("DACR", "Playback Right", AUD96P22_PD_3, 1, 0),

	/* Output mixers */
	SND_SOC_DAPM_MIXER("LD1L", AUD96P22_PD_3, 6, 0, ld1_left_kcontrols,
			   ARRAY_SIZE(ld1_left_kcontrols)),
	SND_SOC_DAPM_MIXER("LD1R", AUD96P22_PD_3, 7, 0, ld1_right_kcontrols,
			   ARRAY_SIZE(ld1_right_kcontrols)),
	SND_SOC_DAPM_MIXER("LD2", AUD96P22_PD_4, 2, 0, ld2_kcontrols,
			   ARRAY_SIZE(ld2_kcontrols)),

	/* Headset power switch */
	SND_SOC_DAPM_SWITCH("HS1L", AUD96P22_PD_3, 4, 0, &hs1l_kcontrol),
	SND_SOC_DAPM_SWITCH("HS1R", AUD96P22_PD_3, 5, 0, &hs1r_kcontrol),

	/* Output pins */
	SND_SOC_DAPM_OUTPUT("HSOUTL"),
	SND_SOC_DAPM_OUTPUT("LINEOUTL"),
	SND_SOC_DAPM_OUTPUT("LINEOUTMP"),
	SND_SOC_DAPM_OUTPUT("LINEOUTMN"),
	SND_SOC_DAPM_OUTPUT("LINEOUTR"),
	SND_SOC_DAPM_OUTPUT("HSOUTR"),

	/* Lineout driver */
	SND_SOC_DAPM_OUT_DRV_E("LINEOUTDRV", SND_SOC_NOPM, 0, 0, NULL, 0,
			       lineout_driver_event,
			       SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
};

static const struct snd_soc_dapm_route aud96p22_dapm_routes[] = {
	{ "AINLMUX", "AINL1 differential", "AINL1N" },
	{ "AINLMUX", "AINL1 single-ended", "AINL1P" },
	{ "AINLMUX", "AINL3 single-ended", "AINL3" },
	{ "AINLMUX", "AINL2 differential", "AINL2N" },
	{ "AINLMUX", "AINL2 single-ended", "AINL2P" },

	{ "AINRMUX", "AINR1 differential", "AINR1N" },
	{ "AINRMUX", "AINR1 single-ended", "AINR1P" },
	{ "AINRMUX", "AINR3 single-ended", "AINR3" },
	{ "AINRMUX", "AINR2 differential", "AINR2N" },
	{ "AINRMUX", "AINR2 single-ended", "AINR2P" },

	{ "ADCL", NULL, "AINLMUX" },
	{ "ADCR", NULL, "AINRMUX" },

	{ "LD1L", "LD1L DACL Switch", "DACL" },
	{ "LD1L", "LD1L AINL Switch", "AINLMUX" },
	{ "LD1L", "LD1L AINR Switch", "AINRMUX" },

	{ "LD1R", "LD1R DACR Switch", "DACR" },
	{ "LD1R", "LD1R AINR Switch", "AINRMUX" },
	{ "LD1R", "LD1R AINL Switch", "AINLMUX" },

	{ "LD2", "LD2 DACL Switch", "DACL" },
	{ "LD2", "LD2 AINL Switch", "AINLMUX" },
	{ "LD2", "LD2 DACR Switch", "DACR" },

	{ "HS1L", NULL, "LD1L" },
	{ "HS1R", NULL, "LD1R" },
	{ "HSOUTL", NULL, "HS1L" },
	{ "HSOUTR", NULL, "HS1R" },

	{ "LINEOUTL", NULL, "LD1L" },
	{ "LINEOUTR", NULL, "LD1R" },
	{ "LINEOUTDRV", NULL, "LINEOUTL" },
	{ "LINEOUTDRV", NULL, "LINEOUTR" },

	{ "LINEOUTMP", NULL, "LD2" },
	{ "LINEOUTMN", NULL, "LD2" },
};

static struct snd_soc_codec_driver aud96p22_driver = {
	.component_driver = {
		.controls = aud96p22_snd_controls,
		.num_controls = ARRAY_SIZE(aud96p22_snd_controls),
		.dapm_widgets = aud96p22_dapm_widgets,
		.num_dapm_widgets = ARRAY_SIZE(aud96p22_dapm_widgets),
		.dapm_routes = aud96p22_dapm_routes,
		.num_dapm_routes = ARRAY_SIZE(aud96p22_dapm_routes),
	},
};

static int aud96p22_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct aud96p22_priv *priv = snd_soc_codec_get_drvdata(dai->codec);
	struct regmap *regmap = priv->regmap;
	unsigned int val;

	/* Master/slave mode */
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		val = 0;
		break;
	case SND_SOC_DAIFMT_CBM_CFM:
		val = I2S1_MS_MODE;
		break;
	default:
		return -EINVAL;
	}

	regmap_update_bits(regmap, AUD96P22_I2S1_CONFIG_0, I2S1_MS_MODE, val);

	/* Audio format */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_RIGHT_J:
		val = I2S1_MODE_RIGHT_J;
		break;
	case SND_SOC_DAIFMT_I2S:
		val = I2S1_MODE_I2S;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		val = I2S1_MODE_LEFT_J;
		break;
	default:
		return -EINVAL;
	}

	regmap_update_bits(regmap, AUD96P22_I2S1_CONFIG_0, I2S1_MODE_MASK, val);

	return 0;
}

static int aud96p22_startup(struct snd_pcm_substream *substream,
			    struct snd_soc_dai *dai)
{
	struct aud96p22_priv *priv = snd_soc_codec_get_drvdata(dai->codec);
	struct regmap *regmap = priv->regmap;

	/* Overall power-up */
	regmap_update_bits(regmap, AUD96P22_PD_0, PD_0_PDZ, PD_0_PDZ);

	/* Reset ADC and DAC path */
	regmap_write(regmap, AUD96P22_RESET, 0x0);
	regmap_write(regmap, AUD96P22_RESET, 0x3);

	return 0;
}

static void aud96p22_shutdown(struct snd_pcm_substream *substream,
			      struct snd_soc_dai *dai)
{
	struct aud96p22_priv *priv = snd_soc_codec_get_drvdata(dai->codec);
	struct regmap *regmap = priv->regmap;

	/* Overall power-down */
	regmap_update_bits(regmap, AUD96P22_PD_0, PD_0_PDZ, 0);
}

static struct snd_soc_dai_ops aud96p22_dai_ops = {
	.set_fmt = aud96p22_set_fmt,
	.startup = aud96p22_startup,
	.shutdown = aud96p22_shutdown,
};

#define AUD96P22_RATES	SNDRV_PCM_RATE_8000_192000
#define AUD96P22_FORMATS (\
		SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S18_3LE | \
		SNDRV_PCM_FMTBIT_S20_3LE | SNDRV_PCM_FMTBIT_S24_LE)

static struct snd_soc_dai_driver aud96p22_dai = {
	.name = "aud96p22-dai",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = AUD96P22_RATES,
		.formats = AUD96P22_FORMATS,
	},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = AUD96P22_RATES,
		.formats = AUD96P22_FORMATS,
	},
	.ops = &aud96p22_dai_ops,
};

static const struct regmap_config aud96p22_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AUD96P22_REG_MAX,
	.cache_type = REGCACHE_RBTREE,
};

static int aud96p22_i2c_probe(struct i2c_client *i2c,
			      const struct i2c_device_id *id)
{
	struct device *dev = &i2c->dev;
	struct aud96p22_priv *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (priv == NULL)
		return -ENOMEM;

	priv->regmap = devm_regmap_init_i2c(i2c, &aud96p22_regmap);
	if (IS_ERR(priv->regmap)) {
		ret = PTR_ERR(priv->regmap);
		dev_err(dev, "failed to init i2c regmap: %d\n", ret);
		return ret;
	}

	priv->pa_gpio = devm_gpiod_get_optional(dev, "zte,pa", GPIOD_OUT_LOW);
	if (IS_ERR(priv->pa_gpio)) {
		ret = PTR_ERR(priv->pa_gpio);
		dev_err(dev, "failed to get pa gpio: %d\n", ret);
		return ret;
	}

	i2c_set_clientdata(i2c, priv);

	ret = snd_soc_register_codec(dev, &aud96p22_driver, &aud96p22_dai, 1);
	if (ret) {
		dev_err(dev, "failed to register codec: %d\n", ret);
		return ret;
	}

	return 0;
}

static int aud96p22_i2c_remove(struct i2c_client *i2c)
{
	snd_soc_unregister_codec(&i2c->dev);
	return 0;
}

const struct of_device_id aud96p22_dt_ids[] = {
	{ .compatible = "zte,zx-aud96p22", },
	{ }
};
MODULE_DEVICE_TABLE(of, aud96p22_dt_ids);

static struct i2c_driver aud96p22_i2c_driver = {
	.driver = {
		.name = "zx_aud96p22",
		.of_match_table = aud96p22_dt_ids,
	},
	.probe = aud96p22_i2c_probe,
	.remove = aud96p22_i2c_remove,
};
module_i2c_driver(aud96p22_i2c_driver);

MODULE_DESCRIPTION("ZTE ASoC AUD96P22 CODEC driver");
MODULE_AUTHOR("Baoyou Xie <baoyou.xie@linaro.org>");
MODULE_LICENSE("GPL v2");
