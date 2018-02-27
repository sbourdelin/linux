/*
 * TDA7419 audio processor driver
 *
 * Copyright 2018 Konsulko Group
 *
 * Author: Matt Porter <mporter@konsulko.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <sound/core.h>
#include <sound/control.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#define TDA7419_MAIN_SRC_REG		0x00
#define TDA7419_LOUDNESS_REG		0x01
#define TDA7419_MUTE_CLK_REG		0x02
#define TDA7419_VOLUME_REG		0x03
#define TDA7419_TREBLE_REG		0x04
#define TDA7419_MIDDLE_REG		0x05
#define TDA7419_BASS_REG		0x06
#define TDA7419_SECOND_SRC_REG		0x07
#define TDA7419_SUB_MID_BASS_REG	0x08
#define TDA7419_MIXING_GAIN_REG		0x09
#define TDA7419_ATTENUATOR_LF_REG	0x0a
#define TDA7419_ATTENUATOR_RF_REG	0x0b
#define TDA7419_ATTENUATOR_LR_REG	0x0c
#define TDA7419_ATTENUATOR_RR_REG	0x0d
#define TDA7419_MIXING_LEVEL_REG	0x0e
#define TDA7419_ATTENUATOR_SUB_REG	0x0f
#define TDA7419_SA_CLK_AC_REG		0x10
#define TDA7419_TESTING_REG		0x11

#define TDA7419_MAIN_SRC_SEL		0
#define TDA7419_MAIN_SRC_GAIN		3
#define TDA7419_MAIN_SRC_AUTOZERO	7

#define TDA7419_LOUDNESS_ATTEN		0
#define TDA7419_LOUDNESS_CENTER_FREQ	4
#define TDA7419_LOUDNESS_BOOST		6
#define TDA7419_LOUDNESS_SOFT_STEP	7

#define TDA7419_VOLUME_SOFT_STEP	7

#define TDA7419_SOFT_MUTE		0
#define TDA7419_MUTE_INFLUENCE		1
#define TDA7419_SOFT_MUTE_TIME		2
#define TDA7419_SOFT_STEP_TIME		4
#define TDA7419_CLK_FAST_MODE		7

#define TDA7419_TREBLE_CENTER_FREQ	5
#define TDA7419_REF_OUT_SELECT		7

#define TDA7419_MIDDLE_Q_FACTOR		5
#define TDA7419_MIDDLE_SOFT_STEP	7

#define TDA7419_BASS_Q_FACTOR		5
#define TDA7419_BASS_SOFT_STEP		7

#define TDA7419_SECOND_SRC_SEL		0
#define TDA7419_SECOND_SRC_GAIN		3
#define TDA7419_REAR_SPKR_SRC		7

#define TDA7419_SUB_CUT_OFF_FREQ	0
#define TDA7419_MIDDLE_CENTER_FREQ	2
#define TDA7419_BASS_CENTER_FREQ	4
#define TDA7419_BASS_DC_MODE		6
#define TDA7419_SMOOTHING_FILTER	7

#define TDA7419_MIX_LF			0
#define TDA7419_MIX_RF			1
#define TDA7419_MIX_ENABLE		2
#define TDA7419_SUB_ENABLE		3
#define TDA7419_HPF_GAIN		4

#define TDA7419_SA_Q_FACTOR		0
#define TDA7419_RESET_MODE		1
#define TDA7419_SA_SOURCE		2
#define TDA7419_SA_RUN			3
#define TDA7419_RESET			4
#define TDA7419_CLK_SOURCE		5
#define TDA7419_COUPLING_MODE		6

struct tda7419_data {
	struct regmap *regmap;
};

static bool tda7419_writeable_reg(struct device *dev, unsigned int reg)
{
	return true;
}

static bool tda7419_readable_reg(struct device *dev, unsigned int reg)
{
	return false;
}

static const struct reg_default tda7419_regmap_defaults[] = {
	{ TDA7419_MAIN_SRC_REG,	0xfe },
	{ TDA7419_LOUDNESS_REG, 0xfe },
	{ TDA7419_MUTE_CLK_REG, 0xfe },
	{ TDA7419_VOLUME_REG, 0xfe },
	{ TDA7419_TREBLE_REG, 0xfe },
	{ TDA7419_MIDDLE_REG, 0xfe },
	{ TDA7419_BASS_REG, 0xfe },
	{ TDA7419_SECOND_SRC_REG, 0xfe },
	{ TDA7419_SUB_MID_BASS_REG, 0xfe },
	{ TDA7419_MIXING_GAIN_REG, 0xfe },
	{ TDA7419_ATTENUATOR_LF_REG, 0xfe },
	{ TDA7419_ATTENUATOR_RF_REG, 0xfe },
	{ TDA7419_ATTENUATOR_LR_REG, 0xfe },
	{ TDA7419_ATTENUATOR_RR_REG, 0xfe },
	{ TDA7419_MIXING_LEVEL_REG, 0xfe },
	{ TDA7419_ATTENUATOR_SUB_REG, 0xfe },
	{ TDA7419_SA_CLK_AC_REG, 0xfe },
	{ TDA7419_TESTING_REG, 0xfe },
};

static const struct regmap_config tda7419_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = TDA7419_TESTING_REG,
	.cache_type = REGCACHE_RBTREE,
	.writeable_reg = tda7419_writeable_reg,
	.readable_reg = tda7419_readable_reg,
	.reg_defaults = tda7419_regmap_defaults,
	.num_reg_defaults = ARRAY_SIZE(tda7419_regmap_defaults),
};

struct tda7419_vol_control {
	int min, max;
	unsigned int reg, rreg, mask, thresh;
	unsigned int invert:1;
};

static inline bool tda7419_vol_is_stereo(struct tda7419_vol_control *tvc)
{
	if (tvc->reg == tvc->rreg)
		return 0;

	return 1;
}

static int tda7419_vol_info(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_info *uinfo)
{
	struct tda7419_vol_control *tvc =
		(struct tda7419_vol_control *)kcontrol->private_value;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = tda7419_vol_is_stereo(tvc) ? 2 : 1;
	uinfo->value.integer.min = tvc->min;
	uinfo->value.integer.max = tvc->max;

	return 0;
}

static inline int tda7419_vol_get_value(int val, unsigned int mask,
					int thresh, unsigned int invert)
{
	val &= mask;
	if (val < thresh) {
		if (invert)
			val = 0 - val;
	} else if (val > thresh) {
		if (invert)
			val = val - thresh;
		else
			val = thresh - val;
	}

	return val;
}

static int tda7419_vol_get(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct tda7419_vol_control *tvc =
		(struct tda7419_vol_control *)kcontrol->private_value;
	unsigned int reg = tvc->reg;
	unsigned int rreg = tvc->rreg;
	unsigned int mask = tvc->mask;
	int thresh = tvc->thresh;
	unsigned int invert = tvc->invert;
	int val;
	int ret;

	ret = snd_soc_component_read(component, reg, &val);
	if (ret < 0)
		return ret;
	ucontrol->value.integer.value[0] =
		tda7419_vol_get_value(val, mask, thresh, invert);

	if (tda7419_vol_is_stereo(tvc)) {
		ret = snd_soc_component_read(component, rreg, &val);
		if (ret < 0)
			return ret;
		ucontrol->value.integer.value[1] =
			tda7419_vol_get_value(val, mask, thresh, invert);
	}

	return 0;
}

static inline int tda7419_vol_put_value(int val, int thresh,
					unsigned int invert)
{
	if (val < 0) {
		if (invert)
			val = abs(val);
		else
			val = thresh - val;
	} else if ((val > 0) && invert) {
		val += thresh;
	}

	return val;
}

static int tda7419_vol_put(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_kcontrol_chip(kcontrol);
	struct tda7419_vol_control *tvc =
		(struct tda7419_vol_control *)kcontrol->private_value;
	unsigned int reg = tvc->reg;
	unsigned int rreg = tvc->rreg;
	unsigned int mask = tvc->mask;
	int thresh = tvc->thresh;
	unsigned int invert = tvc->invert;
	int val;
	int ret;

	val = tda7419_vol_put_value(ucontrol->value.integer.value[0],
				    thresh, invert);
	ret = snd_soc_component_update_bits(component, reg,
					    mask, val);
	if (ret < 0)
		return ret;

	if (tda7419_vol_is_stereo(tvc)) {
		val = tda7419_vol_put_value(ucontrol->value.integer.value[1],
					    thresh, invert);
		ret = snd_soc_component_update_bits(component, rreg,
						    mask, val);
	}

	return ret;
}

#define TDA7419_SINGLE_VALUE(xreg, xmask, xmin, xmax, xthresh, xinvert) \
	((unsigned long)&(struct tda7419_vol_control) \
	{.reg = xreg, .rreg = xreg, .mask = xmask, .min = xmin, \
	 .max = xmax, .thresh = xthresh, .invert = xinvert})

#define TDA7419_DOUBLE_R_VALUE(xregl, xregr, xmask, xmin, xmax, xthresh, \
			       xinvert) \
	((unsigned long)&(struct tda7419_vol_control) \
	{.reg = xregl, .rreg = xregr, .mask = xmask, .min = xmin, \
	 .max = xmax, .thresh = xthresh, .invert = xinvert})

#define TDA7419_SINGLE_TLV(xname, xreg, xmask, xmin, xmax, xthresh, \
			   xinvert, xtlv_array) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	.name = xname, \
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ | \
		SNDRV_CTL_ELEM_ACCESS_READWRITE, \
	.tlv.p = (xtlv_array), \
	.info = tda7419_vol_info, \
	.get = tda7419_vol_get, \
	.put = tda7419_vol_put, \
	.private_value = TDA7419_SINGLE_VALUE(xreg, xmask, xmin, \
					      xmax, xthresh, xinvert), \
}

#define TDA7419_DOUBLE_R_TLV(xname, xregl, xregr, xmask, xmin, xmax, \
			     xthresh, xinvert, xtlv_array) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	.name = xname, \
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ | \
		SNDRV_CTL_ELEM_ACCESS_READWRITE, \
	.tlv.p = (xtlv_array), \
	.info = tda7419_vol_info, \
	.get = tda7419_vol_get, \
	.put = tda7419_vol_put, \
	.private_value = TDA7419_DOUBLE_R_VALUE(xregl, xregr, xmask, \
						xmin, xmax, xthresh, \
						xinvert), \
}

static const char * const enum_src_sel[] = {
	"QD", "SE1", "SE2", "SE3", "SE", "Mute", "Mute", "Mute"};
static SOC_ENUM_SINGLE_DECL(soc_enum_main_src_sel,
	TDA7419_MAIN_SRC_REG, TDA7419_MAIN_SRC_SEL, enum_src_sel);
static DECLARE_TLV_DB_SCALE(tlv_src_gain, 0, 100, 0);

static DECLARE_TLV_DB_SCALE(tlv_loudness_atten, -1500, 100, 0);
static const char * const enum_loudness_center_freq[] = {
	"Flat", "400 Hz", "800 Hz", "2400 Hz"};
static SOC_ENUM_SINGLE_DECL(soc_enum_loudness_center_freq,
	TDA7419_LOUDNESS_REG, TDA7419_LOUDNESS_CENTER_FREQ,
	enum_loudness_center_freq);
static const char * const enum_mute_influence[] = {
	"Pin and IIC", "IIC"};
static SOC_ENUM_SINGLE_DECL(soc_enum_mute_influence,
	TDA7419_MUTE_CLK_REG, TDA7419_MUTE_INFLUENCE, enum_mute_influence);
static const char * const enum_soft_mute_time[] = {
	"0.48 ms", "0.96 ms", "123 ms", "123 ms"};
static SOC_ENUM_SINGLE_DECL(soc_enum_soft_mute_time,
	TDA7419_MUTE_CLK_REG, TDA7419_SOFT_MUTE_TIME, enum_soft_mute_time);
static const char * const enum_soft_step_time[] = {
	"0.160 ms", "0.321 ms", "0.642 ms", "1.28 ms",
	"2.56 ms", "5.12 ms", "10.24 ms", "20.48 ms"};
static SOC_ENUM_SINGLE_DECL(soc_enum_soft_step_time,
	TDA7419_MUTE_CLK_REG, TDA7419_SOFT_STEP_TIME, enum_soft_step_time);
static DECLARE_TLV_DB_SCALE(tlv_volume, -8000, 100, 1);
static const char * const enum_treble_center_freq[] = {
	"10.0 kHz", "12.5 kHz", "15.0 kHz", "17.5 kHz"};
static DECLARE_TLV_DB_SCALE(tlv_filter, -1500, 100, 0);
static SOC_ENUM_SINGLE_DECL(soc_enum_treble_center_freq,
	TDA7419_TREBLE_REG, TDA7419_TREBLE_CENTER_FREQ,
	enum_treble_center_freq);
static const char * const enum_ref_out_select[] = {
	"External Vref (4 V)", "Internal Vref (3.3 V)"};
static SOC_ENUM_SINGLE_DECL(soc_enum_ref_out_select,
	TDA7419_TREBLE_REG, TDA7419_REF_OUT_SELECT, enum_ref_out_select);
static const char * const enum_middle_q_factor[] = {
	"0.5", "0.75", "1.0", "1.25"};
static SOC_ENUM_SINGLE_DECL(soc_enum_middle_q_factor,
	TDA7419_MIDDLE_REG, TDA7419_MIDDLE_Q_FACTOR, enum_middle_q_factor);
static const char * const enum_bass_q_factor[] = {
	"1.0", "1.25", "1.5", "2.0"};
static SOC_ENUM_SINGLE_DECL(soc_enum_bass_q_factor,
	TDA7419_BASS_REG, TDA7419_BASS_Q_FACTOR, enum_bass_q_factor);
static SOC_ENUM_SINGLE_DECL(soc_enum_second_src_sel,
	TDA7419_SECOND_SRC_REG, TDA7419_SECOND_SRC_SEL, enum_src_sel);
static const char * const enum_rear_spkr_src[] = {
	"Main", "Second"};
static SOC_ENUM_SINGLE_DECL(soc_enum_rear_spkr_src,
	TDA7419_SECOND_SRC_REG, TDA7419_REAR_SPKR_SRC, enum_rear_spkr_src);
static const char * const enum_sub_cut_off_freq[] = {
	"Flat", "80 Hz", "120 Hz", "160 Hz"};
static SOC_ENUM_SINGLE_DECL(soc_enum_sub_cut_off_freq,
	TDA7419_SUB_MID_BASS_REG, TDA7419_SUB_CUT_OFF_FREQ,
	enum_sub_cut_off_freq);
static const char * const enum_middle_center_freq[] = {
	"500 Hz", "1000 Hz", "1500 Hz", "2500 Hz"};
static SOC_ENUM_SINGLE_DECL(soc_enum_middle_center_freq,
	TDA7419_SUB_MID_BASS_REG, TDA7419_MIDDLE_CENTER_FREQ,
	enum_middle_center_freq);
static const char * const enum_bass_center_freq[] = {
	"60 Hz", "80 Hz", "100 Hz", "200 Hz"};
static SOC_ENUM_SINGLE_DECL(soc_enum_bass_center_freq,
	TDA7419_SUB_MID_BASS_REG, TDA7419_BASS_CENTER_FREQ,
	enum_bass_center_freq);
static DECLARE_TLV_DB_SCALE(tlv_hpf_gain, 400, 200, 0);
static const char * const enum_sa_q_factor[] = {
	"3.5", "1.75" };
static SOC_ENUM_SINGLE_DECL(soc_enum_sa_q_factor,
	TDA7419_SA_CLK_AC_REG, TDA7419_SA_Q_FACTOR, enum_sa_q_factor);
static const char * const enum_reset_mode[] = {
	"IIC", "Auto" };
static SOC_ENUM_SINGLE_DECL(soc_enum_reset_mode,
	TDA7419_SA_CLK_AC_REG, TDA7419_RESET_MODE, enum_reset_mode);
static const char * const enum_sa_src[] = {
	"Bass", "In Gain" };
static SOC_ENUM_SINGLE_DECL(soc_enum_sa_src,
	TDA7419_SA_CLK_AC_REG, TDA7419_SA_SOURCE, enum_sa_src);
static const char * const enum_clk_src[] = {
	"Internal", "External" };
static SOC_ENUM_SINGLE_DECL(soc_enum_clk_src,
	TDA7419_SA_CLK_AC_REG, TDA7419_CLK_SOURCE, enum_clk_src);
static const char * const enum_coupling_mode[] = {
	"DC Coupling (without HPF)", "AC Coupling after In Gain",
	"DC Coupling (with HPF)", "AC Coupling after Bass" };
static SOC_ENUM_SINGLE_DECL(soc_enum_coupling_mode,
	TDA7419_SA_CLK_AC_REG, TDA7419_COUPLING_MODE, enum_coupling_mode);

/* ASoC Controls */
static struct snd_kcontrol_new tda7419_controls[] = {
SOC_ENUM("Main Source Select", soc_enum_main_src_sel),
SOC_SINGLE_TLV("Main Source Capture Volume", TDA7419_MAIN_SRC_REG,
	       TDA7419_MAIN_SRC_GAIN, 15, 0, tlv_src_gain),
SOC_SINGLE("Main Source AutoZero", TDA7419_MAIN_SRC_REG,
	   TDA7419_MAIN_SRC_AUTOZERO, 1, 1),
SOC_SINGLE_TLV("Loudness Playback Volume", TDA7419_LOUDNESS_REG,
	       TDA7419_LOUDNESS_ATTEN, 15, 1, tlv_loudness_atten),
SOC_ENUM("Loudness Center Frequency", soc_enum_loudness_center_freq),
SOC_SINGLE("Loudness High Boost", TDA7419_LOUDNESS_REG,
	   TDA7419_LOUDNESS_BOOST, 1, 1),
SOC_SINGLE("Loudness Soft Step", TDA7419_LOUDNESS_REG,
	   TDA7419_LOUDNESS_SOFT_STEP, 1, 1),
SOC_SINGLE("Soft Mute", TDA7419_MUTE_CLK_REG, TDA7419_SOFT_MUTE, 1, 1),
SOC_ENUM("Mute Influence", soc_enum_mute_influence),
SOC_ENUM("Soft Mute Time", soc_enum_soft_mute_time),
SOC_ENUM("Soft Step Time", soc_enum_soft_step_time),
SOC_SINGLE("Clock Fast Mode", TDA7419_MUTE_CLK_REG,
	   TDA7419_CLK_FAST_MODE, 1, 1),
TDA7419_SINGLE_TLV("Master Playback Volume", TDA7419_VOLUME_REG,
		   0x7f, -80, 15, 0x10, 0, tlv_volume),
SOC_SINGLE("Volume Soft Step", TDA7419_VOLUME_REG,
	   TDA7419_VOLUME_SOFT_STEP, 1, 1),
TDA7419_SINGLE_TLV("Treble Playback Volume", TDA7419_TREBLE_REG,
		   0x1f, -15, 15, 0x10, 1, tlv_filter),
SOC_ENUM("Treble Center Frequency", soc_enum_treble_center_freq),
SOC_ENUM("Reference Output Select", soc_enum_ref_out_select),
TDA7419_SINGLE_TLV("Middle Playback Volume", TDA7419_MIDDLE_REG,
		   0x1f, -15, 15, 0x10, 1, tlv_filter),
SOC_ENUM("Middle Q Factor", soc_enum_middle_q_factor),
SOC_SINGLE("Middle Soft Step", TDA7419_MIDDLE_REG,
	   TDA7419_MIDDLE_SOFT_STEP, 1, 1),
TDA7419_SINGLE_TLV("Bass Playback Volume", TDA7419_BASS_REG,
		   0x1f, -15, 15, 0x10, 1, tlv_filter),
SOC_ENUM("Bass Q Factor", soc_enum_bass_q_factor),
SOC_SINGLE("Bass Soft Step", TDA7419_BASS_REG,
	   TDA7419_BASS_SOFT_STEP, 1, 1),
SOC_ENUM("Second Source Select", soc_enum_second_src_sel),
SOC_SINGLE_TLV("Second Source Capture Volume", TDA7419_SECOND_SRC_REG,
	       TDA7419_SECOND_SRC_GAIN, 15, 0, tlv_src_gain),
SOC_ENUM("Rear Speaker Source", soc_enum_rear_spkr_src),
SOC_ENUM("Subwoofer Cut-off Frequency", soc_enum_sub_cut_off_freq),
SOC_ENUM("Middle Center Frequency", soc_enum_middle_center_freq),
SOC_ENUM("Bass Center Frequency", soc_enum_bass_center_freq),
SOC_SINGLE("Bass DC Mode", TDA7419_SUB_MID_BASS_REG,
	   TDA7419_BASS_DC_MODE, 1, 1),
SOC_SINGLE("Smoothing Filter", TDA7419_SUB_MID_BASS_REG,
	   TDA7419_SMOOTHING_FILTER, 1, 1),
SOC_SINGLE("Mix to LF Speaker", TDA7419_MIXING_GAIN_REG,
	   TDA7419_MIX_LF, 1, 1),
SOC_SINGLE("Mix to RF Speaker", TDA7419_MIXING_GAIN_REG,
	   TDA7419_MIX_RF, 1, 1),
SOC_SINGLE("Mix Enable", TDA7419_MIXING_GAIN_REG, TDA7419_MIX_ENABLE, 1, 1),
SOC_SINGLE("Subwoofer Enable", TDA7419_MIXING_GAIN_REG,
	   TDA7419_SUB_ENABLE, 1, 1),
SOC_SINGLE_TLV("HPF Filter Playback Volume", TDA7419_MIXING_GAIN_REG,
	       TDA7419_HPF_GAIN, 9, 0, tlv_hpf_gain),
TDA7419_DOUBLE_R_TLV("Front Playback Volume", TDA7419_ATTENUATOR_LF_REG,
		     TDA7419_ATTENUATOR_RF_REG, 0x7f, -80, 15, 0x10, 0,
		     tlv_volume),
SOC_SINGLE("Left Front Soft Step", TDA7419_ATTENUATOR_LF_REG,
	   TDA7419_VOLUME_SOFT_STEP, 1, 1),
SOC_SINGLE("Right Front Soft Step", TDA7419_ATTENUATOR_RF_REG,
	   TDA7419_VOLUME_SOFT_STEP, 1, 1),
TDA7419_DOUBLE_R_TLV("Rear Playback Volume", TDA7419_ATTENUATOR_LR_REG,
		     TDA7419_ATTENUATOR_RR_REG, 0x7f, -80, 15, 0x10, 0,
		     tlv_volume),
SOC_SINGLE("Left Rear Soft Step", TDA7419_ATTENUATOR_LR_REG,
	   TDA7419_VOLUME_SOFT_STEP, 1, 1),
SOC_SINGLE("Right Rear Soft Step", TDA7419_ATTENUATOR_RR_REG,
	   TDA7419_VOLUME_SOFT_STEP, 1, 1),
TDA7419_SINGLE_TLV("Mixing Capture Volume", TDA7419_MIXING_LEVEL_REG,
		   0x7f, -80, 15, 0x10, 0, tlv_volume),
SOC_SINGLE("Mixing Level Soft Step", TDA7419_MIXING_LEVEL_REG,
	   TDA7419_VOLUME_SOFT_STEP, 1, 1),
TDA7419_SINGLE_TLV("Subwoofer Playback Volume", TDA7419_ATTENUATOR_SUB_REG,
		   0x7f, -80, 15, 0x10, 0, tlv_volume),
SOC_SINGLE("Subwoofer Soft Step", TDA7419_ATTENUATOR_SUB_REG,
	   TDA7419_VOLUME_SOFT_STEP, 1, 1),
SOC_ENUM("Spectrum Analyzer Q Factor", soc_enum_sa_q_factor),
SOC_ENUM("Spectrum Analyzer Reset Mode", soc_enum_reset_mode),
SOC_ENUM("Spectrum Analyzer Source", soc_enum_sa_src),
SOC_SINGLE("Spectrum Analyzer Run", TDA7419_SA_CLK_AC_REG,
	   TDA7419_SA_RUN, 1, 1),
SOC_SINGLE("Spectrum Analyzer Reset", TDA7419_SA_CLK_AC_REG,
	   TDA7419_RESET, 1, 1),
SOC_ENUM("Clock Source", soc_enum_clk_src),
SOC_ENUM("Coupling Mode", soc_enum_coupling_mode),
};

static const struct snd_soc_component_driver tda7419_component_driver = {
	.name			= "tda7419",
	.controls		= tda7419_controls,
	.num_controls		= ARRAY_SIZE(tda7419_controls),
};

static int tda7419_probe(struct i2c_client *i2c,
			 const struct i2c_device_id *id)
{
	struct tda7419_data *tda7419;
	int ret;

	tda7419 = devm_kzalloc(&i2c->dev,
			       sizeof(struct tda7419_data),
			       GFP_KERNEL);
	if (tda7419 == NULL)
		return -ENOMEM;

	i2c_set_clientdata(i2c, tda7419);

	tda7419->regmap = devm_regmap_init_i2c(i2c, &tda7419_regmap_config);
	if (IS_ERR(tda7419->regmap)) {
		ret = PTR_ERR(tda7419->regmap);
		dev_err(&i2c->dev, "error initializing regmap: %d\n",
				ret);
		return ret;
	}

	/* Configure registers */
	regmap_write(tda7419->regmap, TDA7419_VOLUME_REG, 0xe0);
	regmap_write(tda7419->regmap, TDA7419_MIXING_GAIN_REG, 0x0f);
	regmap_write(tda7419->regmap, TDA7419_ATTENUATOR_LF_REG, 0xe0);
	regmap_write(tda7419->regmap, TDA7419_ATTENUATOR_RF_REG, 0xe0);
	regmap_write(tda7419->regmap, TDA7419_ATTENUATOR_LR_REG, 0xe0);
	regmap_write(tda7419->regmap, TDA7419_ATTENUATOR_RR_REG, 0xe0);
	regmap_write(tda7419->regmap, TDA7419_MIXING_LEVEL_REG, 0xe0);
	regmap_write(tda7419->regmap, TDA7419_ATTENUATOR_SUB_REG, 0xe0);

	ret = devm_snd_soc_register_component(&i2c->dev,
		&tda7419_component_driver, NULL, 0);
	if (ret < 0) {
		dev_err(&i2c->dev, "error registering component: %d\n",
				ret);
	}

	return ret;
}

static int tda7419_remove(struct i2c_client *i2c)
{
	int i;
	struct tda7419_data *tda7419 = i2c_get_clientdata(i2c);

	/* Reset registers to defaults */
	for (i = 0; i < ARRAY_SIZE(tda7419_regmap_defaults); i++)
		regmap_write(tda7419->regmap,
			     tda7419_regmap_defaults[i].reg,
			     tda7419_regmap_defaults[i].def);

	return 0;
}

static const struct i2c_device_id tda7419_i2c_id[] = {
	{ "tda7419", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tda7419_i2c_id);

static const struct of_device_id tda7419_of_match[] = {
	{ .compatible = "st,tda7419" },
	{ },
};

static struct i2c_driver tda7419_driver = {
	.driver = {
		.name   = "tda7419",
		.of_match_table = tda7419_of_match,
	},
	.probe          = tda7419_probe,
	.remove		= tda7419_remove,
	.id_table       = tda7419_i2c_id,
};

module_i2c_driver(tda7419_driver);

MODULE_AUTHOR("Matt Porter <mporter@konsulko.com>");
MODULE_DESCRIPTION("TDA7419 audio processor driver");
MODULE_LICENSE("GPL");
