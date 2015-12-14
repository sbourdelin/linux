/*
 * cs53l30.c  --  CS53l30 ALSA Soc Audio driver
 *
 * Copyright 2015 Cirrus Logic, Inc.
 *
 * Authors: Paul Handrigan <Paul.Handrigan@cirrus.com>,
 *          Tim Howe <Tim.Howe@cirrus.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>
#include <sound/tlv.h>
#include "cs53l30.h"

struct  cs53l30_private {
	struct regmap			*regmap;
	struct gpio_desc		*reset_gpio;
	u8				asp_config_ctl;
	u32				mclk;
};

static const struct reg_default cs53l30_reg_defaults[] = {
	{ CS53L30_PWRCTL,		THMS_PDN },
	{ CS53L30_MCLKCTL,		MCLK_DIV_DFLT },
	{ CS53L30_INT_SR_CTL,		INTERNAL_FS_DFLT },
	{ CS53L30_MICBIAS_CTL,		MIC_BIAS_DFLT },
	{ CS53L30_ASPCFG_CTL,		ASP_RATE_48K },
	{ CS53L30_ASP1_CTL,		ASP1_TDM_PDN },
	{ CS53L30_ASP1_TDMTX_CTL1,	ASP1_CHX_TX_DFLT_SLT47 },
	{ CS53L30_ASP1_TDMTX_CTL2,	ASP1_CHX_TX_DFLT_SLT47 },
	{ CS53L30_ASP1_TDMTX_CTL3,	ASP1_CHX_TX_DFLT_SLT47 },
	{ CS53L30_ASP1_TDMTX_CTL4,	ASP1_CHX_TX_DFLT_SLT47 },
	{ CS53L30_ASP1_TDMTX_EN1,	ASP_TX_DISABLED },
	{ CS53L30_ASP1_TDMTX_EN2,	ASP_TX_DISABLED },
	{ CS53L30_ASP1_TDMTX_EN3,	ASP_TX_DISABLED },
	{ CS53L30_ASP1_TDMTX_EN4,	ASP_TX_DISABLED },
	{ CS53L30_ASP1_TDMTX_EN5,	ASP_TX_DISABLED },
	{ CS53L30_ASP1_TDMTX_EN6,	ASP_TX_DISABLED },
	{ CS53L30_ASP2_CTL,		ASP2_CTRL_DFLT },
	{ CS53L30_SFT_RAMP,		SFT_RMP_DFLT },
	{ CS53L30_LRCLK_CTL1,		LRCK_CTLX_DFLT },
	{ CS53L30_LRCLK_CTL2,		LRCK_CTLX_DFLT },
	{ CS53L30_MUTEP_CTL1,		MUTEP_CTRL1_DFLT },
	{ CS53L30_MUTEP_CTL2,		MUTE_PDN_ULP },
	{ CS53L30_INBIAS_CTL1,		INBIAS_CTLX_DFLT },
	{ CS53L30_INBIAS_CTL2,		INBIAS_CTLX_DFLT },
	{ CS53L30_DMIC1_STR_CTL,	DMIC1_STEREO_DFLT },
	{ CS53L30_DMIC2_STR_CTL,	DMIC2_STEREO_DFLT },
	{ CS53L30_ADCDMIC1_CTL1,	ADC1_ON_CH_AB_IN },
	{ CS53L30_ADCDMIC1_CTL2,	ADC1_DMIC1_CTL2_DFLT },
	{ CS53L30_ADC1_CTL3,		ADC1_HPF_EN },
	{ CS53L30_ADC1_NG_CTL,		ADCX_ZERO_DFLT },
	{ CS53L30_ADC1A_AFE_CTL,	ADCX_ZERO_DFLT },
	{ CS53L30_ADC1B_AFE_CTL,	ADCX_ZERO_DFLT },
	{ CS53L30_ADC1A_DIG_VOL,	ADCX_ZERO_DFLT },
	{ CS53L30_ADC1B_DIG_VOL,	ADCX_ZERO_DFLT },
	{ CS53L30_ADCDMIC2_CTL1,	ADC2_ON_CH_AB_IN },
	{ CS53L30_ADCDMIC2_CTL2,	ADCX_ZERO_DFLT },
	{ CS53L30_ADC2_CTL3,		ADC2_HPF_EN },
	{ CS53L30_ADC2_NG_CTL,		ADCX_ZERO_DFLT },
	{ CS53L30_ADC2A_AFE_CTL,	ADCX_ZERO_DFLT },
	{ CS53L30_ADC2B_AFE_CTL,	ADCX_ZERO_DFLT },
	{ CS53L30_ADC2A_DIG_VOL,	ADCX_ZERO_DFLT },
	{ CS53L30_ADC2B_DIG_VOL,	ADCX_ZERO_DFLT },
	{ CS53L30_INT_MASK,		DEVICE_INT_MASK },
};

static bool cs53l30_volatile_register(struct device *dev, unsigned int reg)
{
	if (reg == CS53L30_IS)
		return true;
	else
		return false;
}

static bool cs53l30_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CS53L30_DEVID_AB:
	case CS53L30_DEVID_CD:
	case CS53L30_DEVID_E:
	case CS53L30_REVID:
	case CS53L30_PWRCTL:
	case CS53L30_MCLKCTL:
	case CS53L30_INT_SR_CTL:
	case CS53L30_MICBIAS_CTL:
	case CS53L30_ASPCFG_CTL:
	case CS53L30_ASP1_CTL:
	case CS53L30_ASP1_TDMTX_CTL1:
	case CS53L30_ASP1_TDMTX_CTL2:
	case CS53L30_ASP1_TDMTX_CTL3:
	case CS53L30_ASP1_TDMTX_CTL4:
	case CS53L30_ASP1_TDMTX_EN1:
	case CS53L30_ASP1_TDMTX_EN2:
	case CS53L30_ASP1_TDMTX_EN3:
	case CS53L30_ASP1_TDMTX_EN4:
	case CS53L30_ASP1_TDMTX_EN5:
	case CS53L30_ASP1_TDMTX_EN6:
	case CS53L30_ASP2_CTL:
	case CS53L30_SFT_RAMP:
	case CS53L30_LRCLK_CTL1:
	case CS53L30_LRCLK_CTL2:
	case CS53L30_MUTEP_CTL1:
	case CS53L30_MUTEP_CTL2:
	case CS53L30_INBIAS_CTL1:
	case CS53L30_INBIAS_CTL2:
	case CS53L30_DMIC1_STR_CTL:
	case CS53L30_DMIC2_STR_CTL:
	case CS53L30_ADCDMIC1_CTL1:
	case CS53L30_ADCDMIC1_CTL2:
	case CS53L30_ADC1_CTL3:
	case CS53L30_ADC1_NG_CTL:
	case CS53L30_ADC1A_AFE_CTL:
	case CS53L30_ADC1B_AFE_CTL:
	case CS53L30_ADC1A_DIG_VOL:
	case CS53L30_ADC1B_DIG_VOL:
	case CS53L30_ADCDMIC2_CTL1:
	case CS53L30_ADCDMIC2_CTL2:
	case CS53L30_ADC2_CTL3:
	case CS53L30_ADC2_NG_CTL:
	case CS53L30_ADC2A_AFE_CTL:
	case CS53L30_ADC2B_AFE_CTL:
	case CS53L30_ADC2A_DIG_VOL:
	case CS53L30_ADC2B_DIG_VOL:
	case CS53L30_INT_MASK:
		return true;
	default:
		return false;
	}
}

static DECLARE_TLV_DB_SCALE(adc_boost_tlv, 0, 2000, 0);
static DECLARE_TLV_DB_SCALE(adc_ng_boost_tlv, 0, 3000, 0);
static DECLARE_TLV_DB_SCALE(pga_tlv, -600, 50, 0);

static DECLARE_TLV_DB_SCALE(dig_tlv, -9600, 100, 1);

static const char * const input1_sel_text[] = { "DMIC1 On AB In",
	"DMIC1 On A In", "DMIC1 On B In", "ADC1 On AB In", "ADC1 On A In",
	"ADC1 On B In", "DMIC1 Off ADC1 Off", };

unsigned int const input1_sel_values[] = { DMIC1_ON_CH_AB_IN, DMIC1_ON_CH_A_IN,
	DMIC1_ON_CH_B_IN, ADC1_ON_CH_AB_IN, ADC1_ON_CH_A_IN, ADC1_ON_CH_B_IN,
	DMIC1_OFF_ADC1_OFF, };

static const char * const input2_sel_text[] = { "DMIC2 On AB In",
	"DMIC2 On A In", "DMIC2 On B In", "ADC2 On AB In", "ADC2 On A In",
	"ADC2 On B In", "DMIC2 Off ADC2 Off", };

unsigned int const input2_sel_values[] = { DMIC2_ON_CH_AB_IN, DMIC2_ON_CH_A_IN,
	DMIC2_ON_CH_B_IN, ADC2_ON_CH_AB_IN, ADC2_ON_CH_A_IN, ADC2_ON_CH_B_IN,
	DMIC2_OFF_ADC2_OFF, };

static const char * const input1_route_sel_text[] = { "ADC1_SEL",
						      "DMIC1_SEL" };

static const struct soc_enum input1_route_sel_enum =
	SOC_ENUM_SINGLE(CS53L30_ADCDMIC1_CTL1, 0,
		ARRAY_SIZE(input1_route_sel_text), input1_route_sel_text);

static SOC_VALUE_ENUM_SINGLE_DECL(input1_sel_enum, CS53L30_ADCDMIC1_CTL1, 0,
		ADC1_DMIC1_PDN_MASK, input1_sel_text, input1_sel_values);

static const struct snd_kcontrol_new input1_route_sel_mux =
	SOC_DAPM_ENUM("Input 1 Route", input1_route_sel_enum);

static const char * const input2_route_sel_text[] = { "ADC2_SEL",
						      "DMIC2_SEL" };

/* Note: CS53L30_ADCDMIC1_CTL1 CH_TYPE controls inputs 1 and 2 */
static const struct soc_enum input2_route_sel_enum =
	SOC_ENUM_SINGLE(CS53L30_ADCDMIC1_CTL1, 0,
		ARRAY_SIZE(input2_route_sel_text), input2_route_sel_text);

static SOC_VALUE_ENUM_SINGLE_DECL(input2_sel_enum, CS53L30_ADCDMIC2_CTL1, 0,
		ADC1_DMIC1_PDN_MASK, input2_sel_text, input2_sel_values);

static const struct snd_kcontrol_new input2_route_sel_mux =
	SOC_DAPM_ENUM("Input 2 Route", input2_route_sel_enum);

/*
 * TB = 6144*(MCLK(int) scaling factor)/MCLK(internal)
 * TB - Time base
 * NOTE: If MCLK_INT_SCALE = 0, then TB=1
 */
static const char * const cs53l30_ng_delay_text[] = {
	"TB*50ms", "TB*100ms", "TB*150ms", "TB*200ms" };

static const struct soc_enum adc1_ng_delay_enum =
	SOC_ENUM_SINGLE(CS53L30_ADC1_NG_CTL, 0,
		ARRAY_SIZE(cs53l30_ng_delay_text), cs53l30_ng_delay_text);

static const struct soc_enum adc2_ng_delay_enum =
	SOC_ENUM_SINGLE(CS53L30_ADC2_NG_CTL, 0,
		ARRAY_SIZE(cs53l30_ng_delay_text), cs53l30_ng_delay_text);

/* The noise gate threshold selected will depend on NG Boost */
static const char * const cs53l30_ng_thres_text[] = {
	"-64dB/-34dB", "-66dB/-36dB", "-70dB/-40dB", "-73dB/-43dB",
	"-76dB/-46dB", "-82dB/-52dB", "-58dB", "-64dB"};

static const struct soc_enum adc1_ng_thres_enum =
	SOC_ENUM_SINGLE(CS53L30_ADC1_NG_CTL, 2,
		ARRAY_SIZE(cs53l30_ng_thres_text), cs53l30_ng_thres_text);

static const struct soc_enum adc2_ng_thres_enum =
	SOC_ENUM_SINGLE(CS53L30_ADC2_NG_CTL, 2,
		ARRAY_SIZE(cs53l30_ng_thres_text), cs53l30_ng_thres_text);

/* ADC Preamp gain select */
static const char * const cs53l30_preamp_gain_sel_text[] = {
	"0dB", "10dB", "20dB"};

static const struct soc_enum adc1a_preamp_gain_enum =
	SOC_ENUM_SINGLE(CS53L30_ADC1A_AFE_CTL, 6,
		ARRAY_SIZE(cs53l30_preamp_gain_sel_text),
		cs53l30_preamp_gain_sel_text);

static const struct soc_enum adc1b_preamp_gain_enum =
	SOC_ENUM_SINGLE(CS53L30_ADC1B_AFE_CTL, 6,
		ARRAY_SIZE(cs53l30_preamp_gain_sel_text),
		cs53l30_preamp_gain_sel_text);

static const struct soc_enum adc2a_preamp_gain_enum =
	SOC_ENUM_SINGLE(CS53L30_ADC2A_AFE_CTL, 6,
		ARRAY_SIZE(cs53l30_preamp_gain_sel_text),
		cs53l30_preamp_gain_sel_text);

static const struct soc_enum adc2b_preamp_gain_enum =
	SOC_ENUM_SINGLE(CS53L30_ADC2B_AFE_CTL, 6,
		ARRAY_SIZE(cs53l30_preamp_gain_sel_text),
		cs53l30_preamp_gain_sel_text);

/* Set MIC Bias Voltage Control */
static const char * const cs53l30_micbias_text[] = {
	"HiZ", "1.8V", "2.75V"};

static const struct soc_enum micbias_enum =
	SOC_ENUM_SINGLE(CS53L30_MICBIAS_CTL, 0,
		ARRAY_SIZE(cs53l30_micbias_text),
		cs53l30_micbias_text);

/* Corner frequencies are with an Fs of 48kHz. */
static const char * const hpf_corner_freq_text[] = {
	"1.86Hz", "120Hz", "235Hz", "466Hz"};

static const struct soc_enum adc1_hpf_enum =
	SOC_ENUM_SINGLE(CS53L30_ADC1_CTL3, 1,
		ARRAY_SIZE(hpf_corner_freq_text), hpf_corner_freq_text);

static const struct soc_enum adc2_hpf_enum =
	SOC_ENUM_SINGLE(CS53L30_ADC2_CTL3, 1,
		ARRAY_SIZE(hpf_corner_freq_text), hpf_corner_freq_text);

static const struct snd_kcontrol_new cs53l30_snd_controls[] = {
	SOC_SINGLE("Digital Soft-Ramp Switch", CS53L30_SFT_RAMP, 5, 1, 0),
	SOC_SINGLE("ADC1 Noise Gate Ganging Switch",
		CS53L30_ADC1_CTL3, 0, 1, 0),
	SOC_SINGLE("ADC2 Noise Gate Ganging Switch",
		CS53L30_ADC2_CTL3, 0, 1, 0),
	SOC_SINGLE("ADC1A Noise Gate Enable Switch",
		CS53L30_ADC1_NG_CTL, 6, 1, 0),
	SOC_SINGLE("ADC1B Noise Gate Enable Switch",
		CS53L30_ADC1_NG_CTL, 7, 1, 0),
	SOC_SINGLE("ADC2A Noise Gate Enable Switch",
		CS53L30_ADC2_NG_CTL, 6, 1, 0),
	SOC_SINGLE("ADC2B Noise Gate Enable Switch",
		CS53L30_ADC2_NG_CTL, 7, 1, 0),
	SOC_SINGLE("ADC1 Notch Filter Switch",
		CS53L30_ADCDMIC1_CTL2, 7, 1, 1),
	SOC_SINGLE("ADC2 Notch Filter Switch",
		CS53L30_ADCDMIC2_CTL2, 7, 1, 1),
	SOC_SINGLE("ADC1A Invert Switch",
		CS53L30_ADCDMIC1_CTL2, 4, 1, 0),
	SOC_SINGLE("ADC1B Invert Switch",
		CS53L30_ADCDMIC1_CTL2, 5, 1, 0),
	SOC_SINGLE("ADC2A Invert Switch",
		CS53L30_ADCDMIC2_CTL2, 4, 1, 0),
	SOC_SINGLE("ADC2B Invert Switch",
		CS53L30_ADCDMIC2_CTL2, 5, 1, 0),

	SOC_SINGLE_TLV("ADC1A Digital Boost Volume",
			CS53L30_ADCDMIC1_CTL2, 0, 1, 0, adc_boost_tlv),
	SOC_SINGLE_TLV("ADC1B Digital Boost Volume",
		       CS53L30_ADCDMIC1_CTL2, 1, 1, 0, adc_boost_tlv),
	SOC_SINGLE_TLV("ADC2A Digital Boost Volume",
			CS53L30_ADCDMIC2_CTL2, 0, 1, 0, adc_boost_tlv),
	SOC_SINGLE_TLV("ADC2B Digital Boost Volume",
		       CS53L30_ADCDMIC2_CTL2, 1, 1, 0, adc_boost_tlv),
	SOC_SINGLE_TLV("ADC1 NG Boost Volume",
		       CS53L30_ADC1_NG_CTL, 5, 1, 0, adc_ng_boost_tlv),
	SOC_SINGLE_TLV("ADC2 NG Boost Volume",
		       CS53L30_ADC2_NG_CTL, 5, 1, 0, adc_ng_boost_tlv),

	SOC_ENUM("Input 1 Channel Select", input1_sel_enum),
	SOC_ENUM("Input 2 Channel Select", input2_sel_enum),

	SOC_ENUM("ADC1 HPF Select", adc1_hpf_enum),
	SOC_ENUM("ADC2 HPF Select", adc2_hpf_enum),
	SOC_ENUM("ADC1 NG Threshold", adc1_ng_thres_enum),
	SOC_ENUM("ADC2 NG Threshold", adc2_ng_thres_enum),
	SOC_ENUM("ADC1 NG Delay", adc1_ng_delay_enum),
	SOC_ENUM("ADC2 NG Delay", adc2_ng_delay_enum),
	SOC_ENUM("ADC1A Pre Amp Gain", adc1a_preamp_gain_enum),
	SOC_ENUM("ADC1B Pre Amp Gain", adc1b_preamp_gain_enum),
	SOC_ENUM("ADC2A Pre Amp Gain", adc2a_preamp_gain_enum),
	SOC_ENUM("ADC2B Pre Amp Gain", adc2b_preamp_gain_enum),
	SOC_ENUM("Mic Bias Voltage Select", micbias_enum),

	SOC_SINGLE_SX_TLV("ADC1A PGA Volume",
		    CS53L30_ADC1A_AFE_CTL, 0, 0x34, 0x18, pga_tlv),
	SOC_SINGLE_SX_TLV("ADC1B PGA Volume",
		    CS53L30_ADC1B_AFE_CTL, 0, 0x34, 0x18, pga_tlv),
	SOC_SINGLE_SX_TLV("ADC2A PGA Volume",
		    CS53L30_ADC2A_AFE_CTL, 0, 0x34, 0x18, pga_tlv),
	SOC_SINGLE_SX_TLV("ADC2B PGA Volume",
		    CS53L30_ADC2B_AFE_CTL, 0, 0x34, 0x18, pga_tlv),

	SOC_SINGLE_SX_TLV("ADC1A Digital Volume",
		    CS53L30_ADC1A_DIG_VOL, 0, 0xA0, 0x0C, dig_tlv),
	SOC_SINGLE_SX_TLV("ADC1B Digital Volume",
		    CS53L30_ADC1B_DIG_VOL, 0, 0xA0, 0x0C, dig_tlv),
	SOC_SINGLE_SX_TLV("ADC2A Digital Volume",
		    CS53L30_ADC2A_DIG_VOL, 0, 0xA0, 0x0C, dig_tlv),
	SOC_SINGLE_SX_TLV("ADC2B Digital Volume",
		    CS53L30_ADC2B_DIG_VOL, 0, 0xA0, 0x0C, dig_tlv),
};

static int cs53l30_asp_sdout_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);
	struct cs53l30_private *priv = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		regmap_update_bits(priv->regmap, CS53L30_ASP1_CTL,
				   ASP1_3ST, 0);
		break;
	case SND_SOC_DAPM_POST_PMD:
		regmap_update_bits(priv->regmap, CS53L30_ASP1_CTL,
				   ASP1_3ST, 1);
		break;
	default:
		pr_err("Invalid event = 0x%x\n", event);
		return -EINVAL;
	}

	return 0;
}

static const struct snd_soc_dapm_widget cs53l30_dapm_widgets[] = {
	SND_SOC_DAPM_INPUT("IN1_DMIC1"),
	SND_SOC_DAPM_INPUT("IN2"),
	SND_SOC_DAPM_INPUT("IN3_DMIC2"),
	SND_SOC_DAPM_INPUT("IN4"),
	SND_SOC_DAPM_SUPPLY("MIC1 Bias", CS53L30_MICBIAS_CTL, 4, 1, NULL, 0),
	SND_SOC_DAPM_SUPPLY("MIC2 Bias", CS53L30_MICBIAS_CTL, 5, 1, NULL, 0),
	SND_SOC_DAPM_SUPPLY("MIC3 Bias", CS53L30_MICBIAS_CTL, 6, 1, NULL, 0),
	SND_SOC_DAPM_SUPPLY("MIC4 Bias", CS53L30_MICBIAS_CTL, 7, 1, NULL, 0),

	SND_SOC_DAPM_AIF_OUT_E("ASP_SDOUT1", NULL,  0,
			       CS53L30_ASP1_CTL, ASP1_SDOUT_PDN,
			1, cs53l30_asp_sdout_event,
			(SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD)),
	SND_SOC_DAPM_AIF_OUT_E("ASP_SDOUT2", NULL,  0,
			       CS53L30_ASP2_CTL, ASP2_SDOUT_PDN,
			1, cs53l30_asp_sdout_event,
			(SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD)),

	SND_SOC_DAPM_MUX("Input Mux 1", SND_SOC_NOPM, 0, 0,
			 &input1_route_sel_mux),
	SND_SOC_DAPM_MUX("Input Mux 2", SND_SOC_NOPM, 0, 0,
			 &input2_route_sel_mux),

	SND_SOC_DAPM_ADC("ADC1A", NULL, CS53L30_ADCDMIC1_CTL1, 6, 1),
	SND_SOC_DAPM_ADC("ADC1B", NULL, CS53L30_ADCDMIC1_CTL1, 7, 1),
	SND_SOC_DAPM_ADC("ADC2A", NULL, CS53L30_ADCDMIC2_CTL1, 6, 1),
	SND_SOC_DAPM_ADC("ADC2B", NULL, CS53L30_ADCDMIC2_CTL1, 7, 1),
	SND_SOC_DAPM_ADC("DMIC1", NULL, CS53L30_ADCDMIC1_CTL1, 2, 1),
	SND_SOC_DAPM_ADC("DMIC2", NULL, CS53L30_ADCDMIC2_CTL1, 2, 1),
};

static const struct snd_soc_dapm_route cs53l30_audio_map[] = {

	/* ADC Input Paths */
	{"ADC1A", NULL, "IN1_DMIC1"},
	{"Input Mux 1", "ADC1_SEL", "ADC1A"},
	{"ADC1B", NULL, "IN2"},

	{"ADC2A", NULL, "IN3_DMIC2"},
	{"Input Mux 2", "ADC2_SEL", "ADC2A"},
	{"ADC2B", NULL, "IN4"},

	/* MIC Bias Paths */
	{"ADC1A", NULL, "MIC1 Bias"},
	{"ADC1B", NULL, "MIC2 Bias"},
	{"ADC2A", NULL, "MIC3 Bias"},
	{"ADC2B", NULL, "MIC4 Bias"},

	/* DMIC Paths */
	{"DMIC1", NULL, "IN1_DMIC1"},
	{"Input Mux 1", "DMIC1_SEL", "DMIC1"},

	{"DMIC2", NULL, "IN3_DMIC2"},
	{"Input Mux 2", "DMIC2_SEL", "DMIC2"},

	/* Output Paths */
	{"ASP_SDOUT1", NULL, "ADC1A" },
	{"ASP_SDOUT1", NULL, "Input Mux 1"},
	{"ASP_SDOUT1", NULL, "ADC1B"},

	{"ASP_SDOUT2", NULL, "ADC2A"},
	{"ASP_SDOUT2", NULL, "Input Mux 2"},
	{"ASP_SDOUT2", NULL, "ADC2B"},

	{"ASP1 Capture", NULL, "ASP_SDOUT1"},
	{"ASP2 Capture", NULL, "ASP_SDOUT2"},
};

struct cs53l30_mclk_div {
	u32 mclk;
	u32 srate;
	u8 asp_rate;
	u8 internal_fs_ratio;
	u8 mclk_int_scale;
};

static struct cs53l30_mclk_div cs53l30_mclk_coeffs[] = {
	/* NOTE: Enable MCLK_INT_SCALE to save power. */

	/* MCLK, Sample Rate, asp_rate, internal_fs_ratio, mclk_int_scale */
	{5644800, 11025, 0x4, 1, 1},
	{5644800, 22050, 0x8, 1, 1},
	{5644800, 44100, 0xC, 1, 1},

	{6000000,  8000, 0x1, 0, 1},
	{6000000, 11025, 0x2, 0, 1},
	{6000000, 12000, 0x4, 0, 1},
	{6000000, 16000, 0x5, 0, 1},
	{6000000, 22050, 0x6, 0, 1},
	{6000000, 24000, 0x8, 0, 1},
	{6000000, 32000, 0x9, 0, 1},
	{6000000, 44100, 0xA, 0, 1},
	{6000000, 48000, 0xC, 0, 1},

	{6144000,  8000, 0x1, 1, 1},
	{6144000, 11025, 0x2, 1, 1},
	{6144000, 12000, 0x4, 1, 1},
	{6144000, 16000, 0x5, 1, 1},
	{6144000, 22050, 0x6, 1, 1},
	{6144000, 24000, 0x8, 1, 1},
	{6144000, 32000, 0x9, 1, 1},
	{6144000, 44100, 0xA, 1, 1},
	{6144000, 48000, 0xC, 1, 1},

	{6400000,  8000, 0x1, 1, 1},
	{6400000, 11025, 0x2, 1, 1},
	{6400000, 12000, 0x4, 1, 1},
	{6400000, 16000, 0x5, 1, 1},
	{6400000, 22050, 0x6, 1, 1},
	{6400000, 24000, 0x8, 1, 1},
	{6400000, 32000, 0x9, 1, 1},
	{6400000, 44100, 0xA, 1, 1},
	{6400000, 48000, 0xC, 1, 1},
};

struct cs53l30_mclkx_div {
	u32 mclkx;
	u8 ratio;
	u8 mclkdiv;
};

static struct cs53l30_mclkx_div cs53l30_mclkx_coeffs[] = {
	{5644800,  1, 0},
	{6000000,  1, 0},
	{6144000,  1, 0},
	{11289600, 2, 1},
	{12288000, 2, 1},
	{12000000, 2, 1},
	{19200000, 3, 2},
};

static int cs53l30_get_mclkx_coeff(int mclkx)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cs53l30_mclkx_coeffs); i++) {
		if (cs53l30_mclkx_coeffs[i].mclkx == mclkx)
			return i;
	}
	return -EINVAL;
}

static int cs53l30_get_mclk_coeff(int mclk, int srate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cs53l30_mclk_coeffs); i++) {
		if (cs53l30_mclk_coeffs[i].mclk == mclk &&
		    cs53l30_mclk_coeffs[i].srate == srate)
			return i;
	}
	return -EINVAL;

}

static int cs53l30_set_sysclk(struct snd_soc_dai *dai,
			      int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_codec *codec = dai->codec;
	struct cs53l30_private *priv = snd_soc_codec_get_drvdata(codec);

	int mclkx_coeff;
	u32 mclk;
	unsigned int mclk_ctl;

	/* MCLKX -> MCLK */
	mclkx_coeff = cs53l30_get_mclkx_coeff(freq);
	if (mclkx_coeff < 0)
		return mclkx_coeff;

	mclk = cs53l30_mclkx_coeffs[mclkx_coeff].mclkx /
		cs53l30_mclkx_coeffs[mclkx_coeff].ratio;

	regmap_read(priv->regmap, CS53L30_MCLKCTL, &mclk_ctl);
	mclk_ctl &= ~MCLK_DIV;
	mclk_ctl |= cs53l30_mclkx_coeffs[mclkx_coeff].mclkdiv;

	regmap_write(priv->regmap, CS53L30_MCLKCTL, mclk_ctl);
	priv->mclk = mclk;
	return 0;
}

static int cs53l30_set_dai_fmt(struct snd_soc_dai *codec_dai, unsigned int fmt)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	struct cs53l30_private *priv = snd_soc_codec_get_drvdata(codec);
	unsigned int asp_config_ctl;

	regmap_read(priv->regmap, CS53L30_ASPCFG_CTL, &asp_config_ctl);

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		asp_config_ctl |= ASP_MS;
		break;

	case SND_SOC_DAIFMT_CBS_CFS:
		asp_config_ctl &= ~ASP_MS;
		break;

	default:
		return -EINVAL;
	}

	/* Check to see if the SCLK is inverted */
	if (fmt & (SND_SOC_DAIFMT_IB_NF | SND_SOC_DAIFMT_IB_IF))
		asp_config_ctl |= ASP_SCLK_INV;
	else
		asp_config_ctl &= ~ASP_SCLK_INV;

	priv->asp_config_ctl = asp_config_ctl;
	return 0;
}

static int cs53l30_pcm_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	struct cs53l30_private *priv = snd_soc_codec_get_drvdata(codec);
	int mclk_coeff;
	int srate = params_rate(params);
	unsigned int int_sr_ctl, mclk_ctl;

	/* MCLK -> srate */
	mclk_coeff =
	    cs53l30_get_mclk_coeff(priv->mclk, srate);
	if (mclk_coeff < 0)
		return -EINVAL;

	regmap_write(priv->regmap, CS53L30_ASPCFG_CTL, priv->asp_config_ctl);
	regmap_read(priv->regmap, CS53L30_INT_SR_CTL, &int_sr_ctl);
	if (cs53l30_mclk_coeffs[mclk_coeff].internal_fs_ratio)
		int_sr_ctl |= INTERNAL_FS_RATIO;
	else
		int_sr_ctl &= ~INTERNAL_FS_RATIO;
	regmap_write(priv->regmap, CS53L30_INT_SR_CTL, int_sr_ctl);

	regmap_read(priv->regmap, CS53L30_MCLKCTL, &mclk_ctl);
	if (cs53l30_mclk_coeffs[mclk_coeff].mclk_int_scale)
		mclk_ctl |= MCLK_INT_SCALE;
	else
		mclk_ctl &= ~MCLK_INT_SCALE;

	regmap_write(priv->regmap, CS53L30_MCLKCTL, mclk_ctl);
	priv->asp_config_ctl &= ASP_CNFG_MASK;
	priv->asp_config_ctl |= (cs53l30_mclk_coeffs[mclk_coeff].asp_rate
				& ASP_RATE);
	regmap_write(priv->regmap, CS53L30_MCLKCTL, priv->asp_config_ctl);

	return 0;
}

static int cs53l30_set_bias_level(struct snd_soc_codec *codec,
				enum snd_soc_bias_level level)
{
	struct snd_soc_dapm_context *dapm = snd_soc_codec_get_dapm(codec);
	struct cs53l30_private *priv = snd_soc_codec_get_drvdata(codec);
	unsigned int reg;
	int i, inter_max_check;

	switch (level) {
	case SND_SOC_BIAS_ON:
		break;
	case SND_SOC_BIAS_PREPARE:
		if (dapm->bias_level == SND_SOC_BIAS_STANDBY)
			regmap_update_bits(priv->regmap, CS53L30_PWRCTL,
					   PDN_LP, 0);
		break;
	case SND_SOC_BIAS_STANDBY:
		if (dapm->bias_level == SND_SOC_BIAS_OFF) {
			regmap_update_bits(priv->regmap, CS53L30_MCLKCTL,
					   MCLK_DIS, 0);
			regmap_update_bits(priv->regmap, CS53L30_PWRCTL,
					   PDN_ULP, 0);
			msleep(50);
		} else {
			regmap_update_bits(priv->regmap, CS53L30_PWRCTL,
					   PDN_LP, PDN_LP);
		}
		break;

	case SND_SOC_BIAS_OFF:
		regmap_update_bits(priv->regmap, CS53L30_INT_MASK,
				   PDN_DONE, 0);
		/* If digital softramp is set, the amount of time required
		 * for power down increases and depends on the digital
		 * volume setting.
		 */

		/* Set the max possible time if digsft is set */
		regmap_read(priv->regmap, CS53L30_SFT_RAMP, &reg);
		if (reg & DIGSFT)
			inter_max_check = PDN_POLL_MAX;
		else
			inter_max_check = 10;

		regmap_update_bits(priv->regmap, CS53L30_PWRCTL,
				   PDN_ULP, PDN_ULP);
		msleep(20); /* PDN_DONE will take a min of 20ms to be set.*/
		regmap_read(priv->regmap, CS53L30_IS, &reg); /* Clr status */
		for (i = 0; i < inter_max_check; i++) {
			usleep_range(1000, 1000);
			msleep(1);
			regmap_read(priv->regmap, CS53L30_IS, &reg);
			if (reg & PDN_DONE)
				break;
		}
		/* PDN_DONE is set.  We now can disable the MCLK */
		regmap_update_bits(priv->regmap, CS53L30_INT_MASK,
				   PDN_DONE, PDN_DONE);
		regmap_update_bits(priv->regmap, CS53L30_MCLKCTL,
				   MCLK_DIS, MCLK_DIS);
		break;
	}
	dapm->bias_level = level;

	return 0;
}

static int cs53l30_set_tristate(struct snd_soc_dai *dai, int tristate)
{
	struct snd_soc_codec *codec = dai->codec;
	struct cs53l30_private *priv = snd_soc_codec_get_drvdata(codec);

	return regmap_update_bits(priv->regmap, CS53L30_ASP1_CTL, ASP1_3ST,
				  (ASP1_3ST_VAL(tristate) & ASP1_3ST));
}

unsigned int const cs53l30_src_rates[] = {
	8000, 11025, 12000, 16000, 22050,
	24000, 32000, 44100, 48000
};

static struct snd_pcm_hw_constraint_list src_constraints = {
	.count  = ARRAY_SIZE(cs53l30_src_rates),
	.list   = cs53l30_src_rates,
};

static int cs53l30_pcm_startup(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	snd_pcm_hw_constraint_list(substream->runtime, 0,
					SNDRV_PCM_HW_PARAM_RATE,
					&src_constraints);

	return 0;
}

/* SNDRV_PCM_RATE_KNOT -> 12000, 24000 Hz, limit with constraint list */
#define CS53L30_RATES (SNDRV_PCM_RATE_8000_48000 | SNDRV_PCM_RATE_KNOT)

#define CS53L30_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE |\
			SNDRV_PCM_FMTBIT_S24_LE)

static const struct snd_soc_dai_ops cs53l30_ops = {
	.startup = cs53l30_pcm_startup,
	.hw_params = cs53l30_pcm_hw_params,
	.set_fmt = cs53l30_set_dai_fmt,
	.set_sysclk = cs53l30_set_sysclk,
	.set_tristate = cs53l30_set_tristate,
};

static struct snd_soc_dai_driver cs53l30_dai[] = {
	{
		.name = "cs53l30-asp1",
		.id = CS53L30_ASP1,
		.capture = {
			.stream_name = "ASP1 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = CS53L30_RATES,
			.formats = CS53L30_FORMATS,
		},
		.ops = &cs53l30_ops,
		.symmetric_rates = 1,
	 },
	{
		.name = "cs53l30-asp2",
		.id = CS53L30_ASP2,
		.capture = {
			.stream_name = "ASP2 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = CS53L30_RATES,
			.formats = CS53L30_FORMATS,
		},
		.ops = &cs53l30_ops,
		.symmetric_rates = 1,
	 }
};

static struct snd_soc_codec_driver soc_codec_dev_cs53l30 = {
	.set_bias_level = cs53l30_set_bias_level,

	.dapm_widgets = cs53l30_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(cs53l30_dapm_widgets),
	.dapm_routes = cs53l30_audio_map,
	.num_dapm_routes = ARRAY_SIZE(cs53l30_audio_map),

	.controls = cs53l30_snd_controls,
	.num_controls = ARRAY_SIZE(cs53l30_snd_controls),
};

static struct regmap_config cs53l30_regmap = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = CS53L30_MAX_REGISTER,
	.reg_defaults = cs53l30_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(cs53l30_reg_defaults),
	.volatile_reg = cs53l30_volatile_register,
	.readable_reg = cs53l30_readable_register,
	.cache_type = REGCACHE_RBTREE,
};

static int cs53l30_i2c_probe(struct i2c_client *client,
				       const struct i2c_device_id *id)
{
	struct cs53l30_private *cs53l30;
	int ret = 0;
	unsigned int devid = 0;
	unsigned int reg;

	cs53l30 = devm_kzalloc(&client->dev,
			       sizeof(struct cs53l30_private), GFP_KERNEL);
	if (!cs53l30) {
		dev_err(&client->dev, "could not allocate codec\n");
		return -ENOMEM;
	}

	/* Reset the Device */
	cs53l30->reset_gpio = devm_gpiod_get_optional(&client->dev,
		"reset", GPIOD_OUT_LOW);
	if (IS_ERR(cs53l30->reset_gpio))
		return PTR_ERR(cs53l30->reset_gpio);

	if (cs53l30->reset_gpio)
		gpiod_set_value_cansleep(cs53l30->reset_gpio, 1);

	i2c_set_clientdata(client, cs53l30);

	cs53l30->mclk = 0;

	cs53l30->regmap = devm_regmap_init_i2c(client, &cs53l30_regmap);
	if (IS_ERR(cs53l30->regmap)) {
		ret = PTR_ERR(cs53l30->regmap);
		dev_err(&client->dev, "regmap_init() failed: %d\n", ret);
		return ret;
	}
	/* initialize codec */
	ret = regmap_read(cs53l30->regmap, CS53L30_DEVID_AB, &reg);
	devid = reg << 12;

	ret = regmap_read(cs53l30->regmap, CS53L30_DEVID_CD, &reg);
	devid |= reg << 4;

	ret = regmap_read(cs53l30->regmap, CS53L30_DEVID_E, &reg);
	devid |= (reg & 0xF0) >> 4;

	if (devid != CS53L30_DEVID) {
		ret = -ENODEV;
		dev_err(&client->dev,
			"CS53L30 Device ID (%X). Expected %X\n",
			devid, CS53L30_DEVID);
		return ret;
	}

	ret = regmap_read(cs53l30->regmap, CS53L30_REVID, &reg);
	if (ret < 0) {
		dev_err(&client->dev, "Get Revision ID failed\n");
		return ret;
	}

	dev_info(&client->dev,
		 "Cirrus Logic CS53L30, Revision: %02X\n", reg & 0xFF);

	ret =  snd_soc_register_codec(&client->dev,
			&soc_codec_dev_cs53l30, cs53l30_dai,
			ARRAY_SIZE(cs53l30_dai));
	return ret;
}

static int cs53l30_i2c_remove(struct i2c_client *client)
{
	struct cs53l30_private *cs53l30 = i2c_get_clientdata(client);

	snd_soc_unregister_codec(&client->dev);

	/* Hold down reset */
	if (cs53l30->reset_gpio)
		gpiod_set_value_cansleep(cs53l30->reset_gpio, 0);

	return 0;
}

#ifdef CONFIG_PM
static int cs53l30_runtime_suspend(struct device *dev)
{
	struct cs53l30_private *cs53l30 = dev_get_drvdata(dev);

	regcache_cache_only(cs53l30->regmap, true);

	/* Hold down reset */
	if (cs53l30->reset_gpio)
		gpiod_set_value_cansleep(cs53l30->reset_gpio, 0);

	return 0;
}

static int cs53l30_runtime_resume(struct device *dev)
{
	struct cs53l30_private *cs53l30 = dev_get_drvdata(dev);

	if (cs53l30->reset_gpio)
		gpiod_set_value_cansleep(cs53l30->reset_gpio, 1);

	regcache_cache_only(cs53l30->regmap, false);
	regcache_sync(cs53l30->regmap);

	return 0;
}
#endif

static const struct dev_pm_ops cs53l30_runtime_pm = {
	SET_RUNTIME_PM_OPS(cs53l30_runtime_suspend, cs53l30_runtime_resume,
			   NULL)
};

static const struct of_device_id cs53l30_of_match[] = {
	{ .compatible = "cirrus,cs53l30", },
	{},
};

MODULE_DEVICE_TABLE(of, cs53l30_of_match);

static const struct i2c_device_id cs53l30_id[] = {
	{"cs53l30", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, cs53l30_id);

static struct i2c_driver cs53l30_i2c_driver = {
	.driver = {
		   .name = "cs53l30",
		   .owner = THIS_MODULE,
		   },
	.id_table = cs53l30_id,
	.probe = cs53l30_i2c_probe,
	.remove = cs53l30_i2c_remove,

};

module_i2c_driver(cs53l30_i2c_driver);

MODULE_DESCRIPTION("ASoC CS53L30 driver");
MODULE_AUTHOR("Paul Handrigan, Cirrus Logic Inc, <Paul.Handrigan@cirrus.com>");
MODULE_LICENSE("GPL");
