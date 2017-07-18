/*
 * ALSA SoC Texas Instruments TAS6424 Quad-Channel Audio Amplifier
 *
 * Copyright (C)2016 Texas Instruments Incorporated -  http://www.ti.com
 * Andreas Dannenberg <dannenberg@ti.com>
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

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>

#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>

#include "tas6424.h"

/* Define how often to check (and clear) the fault status register (in ms) */
#define TAS6424_FAULT_CHECK_INTERVAL		200

static const char * const tas6424_supply_names[] = {
	"dvdd",		/* Digital power supply. Connect to 3.3-V supply. */
	"vbat",		/* Supply used for higher voltage analog circuits. */
	"pvdd",		/* Class-D amp output FETs supply. */
};

#define TAS6424_NUM_SUPPLIES	ARRAY_SIZE(tas6424_supply_names)

struct tas6424_data {
	struct snd_soc_codec *codec;
	struct regmap *regmap;
	struct i2c_client *tas6424_client;
	struct regulator_bulk_data supplies[TAS6424_NUM_SUPPLIES];
	struct delayed_work fault_check_work;
	unsigned int last_fault1;
	unsigned int last_fault2;
	unsigned int last_warn;
};

static int tas6424_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	unsigned int rate = params_rate(params);
	unsigned int width = params_width(params);
	u8 sap_ctrl;
	int ret;

	dev_dbg(codec->dev, "%s() rate=%u width=%u\n", __func__, rate, width);

	switch (rate) {
	case 44100:
		sap_ctrl = TAS6424_SAP_RATE_44100;
		break;
	case 48000:
		sap_ctrl = TAS6424_SAP_RATE_48000;
		break;
	case 96000:
		sap_ctrl = TAS6424_SAP_RATE_96000;
		break;
	default:
		dev_err(codec->dev, "unsupported sample rate: %u\n", rate);
		return -EINVAL;
	}

	switch (width) {
	case 16:
		sap_ctrl |= TAS6424_SAP_TDM_SLOT_SZ_16;
		break;
	case 24:
	case 32:
		break;
	default:
		dev_err(codec->dev, "unsupported sample width: %u\n", width);
		return -EINVAL;
	}

	ret = snd_soc_update_bits(codec, TAS6424_SAP_CTRL_REG,
				  TAS6424_SAP_RATE_MASK, sap_ctrl);
	if (ret < 0) {
		dev_err(codec->dev, "error setting sample rate: %d\n", ret);
		return ret;
	}

	return 0;
}

static int tas6424_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_codec *codec = dai->codec;
	u8 serial_format;
	int ret;

	dev_dbg(codec->dev, "%s() fmt=0x%0x\n", __func__, fmt);

	if ((fmt & SND_SOC_DAIFMT_MASTER_MASK) != SND_SOC_DAIFMT_CBS_CFS) {
		dev_vdbg(codec->dev, "DAI Format master is not found\n");
		return -EINVAL;
	}

	switch (fmt & (SND_SOC_DAIFMT_FORMAT_MASK |
		       SND_SOC_DAIFMT_INV_MASK)) {
	case (SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF):
		/* 1st data bit occur one BCLK cycle after the frame sync */
		serial_format = TAS6424_SAP_I2S;
		break;
	case (SND_SOC_DAIFMT_DSP_A | SND_SOC_DAIFMT_NB_NF):
		/*
		 * DSP_A format means that the first data bit is delayed. For
		 * this, invoke the dedicated DSP mode of the TAS6424. See
		 * device datasheet for additional details on the signal
		 * formatting.
		 */
		serial_format = TAS6424_SAP_DSP;
		break;
	case (SND_SOC_DAIFMT_DSP_B | SND_SOC_DAIFMT_NB_NF):
		/*
		 * We can use the fact that the TAS6424 does not care about the
		 * LRCLK duty cycle during TDM to receive DSP_B formatted data
		 * in LEFTJ mode (no delaying of the 1st data bit).
		 */
		serial_format = TAS6424_SAP_LEFTJ;
		break;
	case (SND_SOC_DAIFMT_LEFT_J | SND_SOC_DAIFMT_NB_NF):
		/* No delay after the frame sync */
		serial_format = TAS6424_SAP_LEFTJ;
		break;
	default:
		dev_vdbg(codec->dev, "DAI Format is not found\n");
		return -EINVAL;
	}

	ret = snd_soc_update_bits(codec, TAS6424_SAP_CTRL_REG,
				  TAS6424_SAP_FMT_MASK,
				  serial_format);
	if (ret < 0) {
		dev_err(codec->dev, "error setting SAIF format: %d\n", ret);
		return ret;
	}

	return 0;
}

static int tas6424_set_dai_tdm_slot(struct snd_soc_dai *dai,
				    unsigned int tx_mask, unsigned int rx_mask,
				    int slots, int slot_width)
{
	struct snd_soc_codec *codec = dai->codec;
	unsigned int first_slot, last_slot;
	bool sap_tdm_slot_last;
	int ret;

	dev_dbg(codec->dev, "%s() tx_mask=%d rx_mask=%d\n", __func__,
		tx_mask, rx_mask);

	if (!tx_mask) {
		dev_err(codec->dev, "tdm mask must not be 0\n");
		return -EINVAL;
	}

	/*
	 * Determine the first slot and last slot that is being requested so
	 * we'll be able to more easily enforce certain constraints as the
	 * TAS6424's TDM interface is not fully configurable.
	 */
	first_slot = __ffs(tx_mask);
	last_slot = __fls(rx_mask);

	if (last_slot - first_slot != 4) {
		dev_err(codec->dev, "tdm mask must cover 4 contiguous slots\n");
		return -EINVAL;
	}

	switch (first_slot) {
	case 0:
		sap_tdm_slot_last = false;
		break;
	case 4:
		sap_tdm_slot_last = true;
		break;
	default:
		dev_err(codec->dev, "tdm mask must start at slot 0 or 4\n");
		return -EINVAL;
	}

	/* Configure the TDM slots to process audio from */
	ret = snd_soc_update_bits(codec, TAS6424_SAP_CTRL_REG,
				  TAS6424_SAP_TDM_SLOT_LAST,
				  sap_tdm_slot_last ? TAS6424_SAP_TDM_SLOT_LAST : 0);
	if (ret < 0) {
		dev_err(codec->dev, "error configuring TDM mode: %d\n", ret);
		return ret;
	}

	return 0;
}

static int tas6424_mute(struct snd_soc_dai *dai, int mute)
{
	struct snd_soc_codec *codec = dai->codec;
	unsigned int val;
	int ret;

	dev_dbg(codec->dev, "%s() mute=%d\n", __func__, mute);

	if (mute)
		val = TAS6424_CH1_STATE_MUTE | TAS6424_CH2_STATE_MUTE |
		      TAS6424_CH3_STATE_MUTE | TAS6424_CH4_STATE_MUTE;
	else
		val = TAS6424_CH1_STATE_PLAY | TAS6424_CH2_STATE_PLAY |
		      TAS6424_CH3_STATE_PLAY | TAS6424_CH4_STATE_PLAY;

	ret = snd_soc_update_bits(codec, TAS6424_CH_STATE_CTRL_REG, 0xff, val);

	if (ret < 0) {
		dev_err(codec->dev, "error (un-)muting device: %d\n", ret);
		return ret;
	}

	return 0;
}

static void tas6424_fault_check_work(struct work_struct *work)
{
	struct tas6424_data *tas6424 = container_of(work, struct tas6424_data,
			fault_check_work.work);
	struct device *dev = tas6424->codec->dev;
	unsigned int reg;
	int ret;

	ret = regmap_read(tas6424->regmap, TAS6424_GLOB_FAULT1_REG, &reg);
	if (ret < 0) {
		dev_err(dev, "failed to read FAULT1 register: %d\n", ret);
		goto out;
	}

	/*
	 * Ignore any clock faults as there is no clean way to check for them.
	 * We would need to start checking for those faults *after* the SAIF
	 * stream has been setup, and stop checking *before* the stream is
	 * stopped to avoid any false-positives. However there are no
	 * appropriate hooks to monitor these events.
	 */
	reg &= TAS6424_FAULT_PVDD_OV | TAS6424_FAULT_VBAT_OV |
	       TAS6424_FAULT_PVDD_UV | TAS6424_FAULT_VBAT_UV;

	if (reg)
		goto check_global_fault2_reg;

	/*
	 * Only flag errors once for a given occurrence. This is needed as
	 * the TAS6424 will take time clearing the fault condition internally
	 * during which we don't want to bombard the system with the same
	 * error message over and over.
	 */
	if ((reg & TAS6424_FAULT_PVDD_OV) && !(tas6424->last_fault1 & TAS6424_FAULT_PVDD_OV))
		dev_crit(dev, "experienced a PVDD overvoltage fault\n");

	if ((reg & TAS6424_FAULT_VBAT_OV) && !(tas6424->last_fault1 & TAS6424_FAULT_VBAT_OV))
		dev_crit(dev, "experienced a VBAT overvoltage fault\n");

	if ((reg & TAS6424_FAULT_PVDD_UV) && !(tas6424->last_fault1 & TAS6424_FAULT_PVDD_UV))
		dev_crit(dev, "experienced a PVDD undervoltage fault\n");

	if ((reg & TAS6424_FAULT_VBAT_UV) && !(tas6424->last_fault1 & TAS6424_FAULT_VBAT_UV))
		dev_crit(dev, "experienced a VBAT undervoltage fault\n");

	/* Store current fault1 value so we can detect any changes next time */
	tas6424->last_fault1 = reg;

check_global_fault2_reg:
	ret = regmap_read(tas6424->regmap, TAS6424_GLOB_FAULT2_REG, &reg);
	if (ret < 0) {
		dev_err(dev, "failed to read FAULT2 register: %d\n", ret);
		goto out;
	}

	reg &= TAS6424_FAULT_OTSD | TAS6424_FAULT_OTSD_CH1 |
	       TAS6424_FAULT_OTSD_CH2 | TAS6424_FAULT_OTSD_CH3 |
	       TAS6424_FAULT_OTSD_CH4;

	if (!reg)
		goto check_warn_reg;

	if ((reg & TAS6424_FAULT_OTSD) && !(tas6424->last_fault2 & TAS6424_FAULT_OTSD))
		dev_crit(dev, "experienced a global overtemp shutdown\n");

	if ((reg & TAS6424_FAULT_OTSD_CH1) && !(tas6424->last_fault2 & TAS6424_FAULT_OTSD_CH1))
		dev_crit(dev, "experienced an overtemp shutdown on CH1\n");

	if ((reg & TAS6424_FAULT_OTSD_CH2) && !(tas6424->last_fault2 & TAS6424_FAULT_OTSD_CH2))
		dev_crit(dev, "experienced an overtemp shutdown on CH2\n");

	if ((reg & TAS6424_FAULT_OTSD_CH3) && !(tas6424->last_fault2 & TAS6424_FAULT_OTSD_CH3))
		dev_crit(dev, "experienced an overtemp shutdown on CH3\n");

	if ((reg & TAS6424_FAULT_OTSD_CH4) && !(tas6424->last_fault2 & TAS6424_FAULT_OTSD_CH4))
		dev_crit(dev, "experienced an overtemp shutdown on CH4\n");

	/* Store current fault2 value so we can detect any changes next time */
	tas6424->last_fault2 = reg;

check_warn_reg:
	ret = regmap_read(tas6424->regmap, TAS6424_WARN_REG, &reg);
	if (ret < 0) {
		dev_err(dev, "failed to read WARN register: %d\n", ret);
		goto out;
	}

	reg &= TAS6424_WARN_VDD_UV | TAS6424_WARN_VDD_POR |
	       TAS6424_WARN_VDD_OTW | TAS6424_WARN_VDD_OTW_CH1 |
	       TAS6424_WARN_VDD_OTW_CH2 | TAS6424_WARN_VDD_OTW_CH3 |
	       TAS6424_WARN_VDD_OTW_CH4;

	if (!reg)
		goto out;

	if ((reg & TAS6424_WARN_VDD_UV) && !(tas6424->last_warn & TAS6424_WARN_VDD_UV))
		dev_warn(dev, "experienced a VDD under voltage condition\n");

	if ((reg & TAS6424_WARN_VDD_POR) && !(tas6424->last_warn & TAS6424_WARN_VDD_POR))
		dev_warn(dev, "experienced a VDD POR condition\n");

	if ((reg & TAS6424_WARN_VDD_OTW) && !(tas6424->last_warn & TAS6424_WARN_VDD_OTW))
		dev_warn(dev, "experienced a global overtemp warning\n");

	if ((reg & TAS6424_WARN_VDD_OTW_CH1) && !(tas6424->last_warn & TAS6424_WARN_VDD_OTW_CH1))
		dev_warn(dev, "experienced an overtemp warning on CH1\n");

	if ((reg & TAS6424_WARN_VDD_OTW_CH2) && !(tas6424->last_warn & TAS6424_WARN_VDD_OTW_CH2))
		dev_warn(dev, "experienced an overtemp warning on CH2\n");

	if ((reg & TAS6424_WARN_VDD_OTW_CH3) && !(tas6424->last_warn & TAS6424_WARN_VDD_OTW_CH3))
		dev_warn(dev, "experienced an overtemp warning on CH3\n");

	if ((reg & TAS6424_WARN_VDD_OTW_CH4) && !(tas6424->last_warn & TAS6424_WARN_VDD_OTW_CH4))
		dev_warn(dev, "experienced an overtemp warning on CH4\n");

	/* Store current warn value so we can detect any changes next time */
	tas6424->last_warn = reg;

	/* Clear any faults by toggling the CLEAR_FAULT control bit */
	ret = regmap_write_bits(tas6424->regmap, TAS6424_MISC_CTRL3_REG,
				TAS6424_CLEAR_FAULT, TAS6424_CLEAR_FAULT);
	if (ret < 0)
		dev_err(dev, "failed to write MISC_CTRL3 register: %d\n", ret);

	ret = regmap_write_bits(tas6424->regmap, TAS6424_MISC_CTRL3_REG,
				TAS6424_CLEAR_FAULT, 0);
	if (ret < 0)
		dev_err(dev, "failed to write MISC_CTRL3 register: %d\n", ret);

out:
	/* Schedule the next fault check at the specified interval */
	schedule_delayed_work(&tas6424->fault_check_work,
			      msecs_to_jiffies(TAS6424_FAULT_CHECK_INTERVAL));
}

static int tas6424_codec_probe(struct snd_soc_codec *codec)
{
	struct tas6424_data *tas6424 = snd_soc_codec_get_drvdata(codec);
	int ret;

	tas6424->codec = codec;

	ret = regulator_bulk_enable(ARRAY_SIZE(tas6424->supplies),
				    tas6424->supplies);
	if (ret != 0) {
		dev_err(codec->dev, "failed to enable supplies: %d\n", ret);
		return ret;
	}

	/* Reset device to establish well-defined startup state */
	ret = snd_soc_update_bits(codec, TAS6424_MODE_CTRL_REG,
				  TAS6424_RESET, TAS6424_RESET);
	if (ret < 0)
		goto error_snd_soc_update_bits;

	/* Set device to Hi-Z mode to minimize current consumption. */
	ret = snd_soc_update_bits(codec, TAS6424_CH_STATE_CTRL_REG, 0xff,
				  TAS6424_CH1_STATE_HIZ | TAS6424_CH2_STATE_HIZ |
				  TAS6424_CH3_STATE_HIZ | TAS6424_CH4_STATE_HIZ);
	if (ret < 0)
		goto error_snd_soc_update_bits;

	INIT_DELAYED_WORK(&tas6424->fault_check_work, tas6424_fault_check_work);

	return 0;

error_snd_soc_update_bits:
	dev_err(codec->dev, "error configuring device registers: %d\n", ret);

	regulator_bulk_disable(ARRAY_SIZE(tas6424->supplies),
			       tas6424->supplies);
	return ret;
}

static int tas6424_codec_remove(struct snd_soc_codec *codec)
{
	struct tas6424_data *tas6424 = snd_soc_codec_get_drvdata(codec);
	int ret;

	cancel_delayed_work_sync(&tas6424->fault_check_work);

	ret = regulator_bulk_disable(ARRAY_SIZE(tas6424->supplies),
				     tas6424->supplies);
	if (ret < 0)
		dev_err(codec->dev, "failed to disable supplies: %d\n", ret);

	return ret;
};

static int tas6424_dac_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);
	struct tas6424_data *tas6424 = snd_soc_codec_get_drvdata(codec);

	dev_dbg(codec->dev, "%s() event=0x%0x\n", __func__, event);

	if (event & SND_SOC_DAPM_POST_PMU) {
		/* Observe codec shutdown-to-active time */
		msleep(12);

		/* Turn on TAS6424 periodic fault checking/handling */
		tas6424->last_fault1 = 0;
		tas6424->last_fault2 = 0;
		tas6424->last_warn = 0;
		schedule_delayed_work(&tas6424->fault_check_work,
				      msecs_to_jiffies(TAS6424_FAULT_CHECK_INTERVAL));
	} else if (event & SND_SOC_DAPM_PRE_PMD) {
		/* Disable TAS6424 periodic fault checking/handling */
		cancel_delayed_work_sync(&tas6424->fault_check_work);
	}

	return 0;
}

#ifdef CONFIG_PM
static int tas6424_suspend(struct snd_soc_codec *codec)
{
	struct tas6424_data *tas6424 = snd_soc_codec_get_drvdata(codec);
	int ret;

	regcache_cache_only(tas6424->regmap, true);
	regcache_mark_dirty(tas6424->regmap);

	ret = regulator_bulk_disable(ARRAY_SIZE(tas6424->supplies),
				     tas6424->supplies);
	if (ret < 0)
		dev_err(codec->dev, "failed to disable supplies: %d\n", ret);

	return ret;
}

static int tas6424_resume(struct snd_soc_codec *codec)
{
	struct tas6424_data *tas6424 = snd_soc_codec_get_drvdata(codec);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(tas6424->supplies),
				    tas6424->supplies);
	if (ret < 0) {
		dev_err(codec->dev, "failed to enable supplies: %d\n", ret);
		return ret;
	}

	regcache_cache_only(tas6424->regmap, false);

	ret = regcache_sync(tas6424->regmap);
	if (ret < 0) {
		dev_err(codec->dev, "failed to sync regcache: %d\n", ret);
		return ret;
	}

	return 0;
}
#else
#define tas6424_suspend NULL
#define tas6424_resume NULL
#endif

static int tas6424_set_bias_level(struct snd_soc_codec *codec,
				  enum snd_soc_bias_level level)
{
	int ret;

	dev_dbg(codec->dev, "%s() level=%d\n", __func__, level);

	switch (level) {
	case SND_SOC_BIAS_ON:
	case SND_SOC_BIAS_PREPARE:
		msleep(500);
		break;
	case SND_SOC_BIAS_STANDBY:
		ret = snd_soc_update_bits(codec, TAS6424_CH_STATE_CTRL_REG,
					  0xff, TAS6424_CH1_STATE_MUTE | TAS6424_CH2_STATE_MUTE |
					  TAS6424_CH3_STATE_MUTE | TAS6424_CH4_STATE_MUTE);

		if (ret < 0) {
			dev_err(codec->dev, "error resuming device: %d\n", ret);
			return ret;
		}
		break;
	case SND_SOC_BIAS_OFF:
		ret = snd_soc_update_bits(codec, TAS6424_CH_STATE_CTRL_REG,
					  0xff, TAS6424_CH1_STATE_HIZ | TAS6424_CH2_STATE_HIZ |
					  TAS6424_CH3_STATE_HIZ | TAS6424_CH4_STATE_HIZ);

		if (ret < 0) {
			dev_err(codec->dev, "error suspending device: %d\n", ret);
			return ret;
		}
		break;
	}

	return 0;
}

static bool tas6424_is_writable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TAS6424_MODE_CTRL_REG:
	case TAS6424_MISC_CTRL1_REG:
	case TAS6424_MISC_CTRL2_REG:
	case TAS6424_SAP_CTRL_REG:
	case TAS6424_CH_STATE_CTRL_REG:
	case TAS6424_CH1_VOL_CTRL_REG:
	case TAS6424_CH2_VOL_CTRL_REG:
	case TAS6424_CH3_VOL_CTRL_REG:
	case TAS6424_CH4_VOL_CTRL_REG:
	case TAS6424_DC_DIAG_CTRL1_REG:
	case TAS6424_DC_DIAG_CTRL2_REG:
	case TAS6424_DC_DIAG_CTRL3_REG:
	case TAS6424_PIN_CTRL_REG:
	case TAS6424_AC_DIAG_CTRL_REG:
	case TAS6424_MISC_CTRL3_REG:
	case TAS6424_CLIP_CTRL_REG:
	case TAS6424_CLIP_WINDOW_REG:
	case TAS6424_CLIP_WARN_REG:
	case TAS6424_CBC_STAT_REG:
	case TAS6424_MISC_CTRL4_REG:
		return true;
	default:
		return false;
	}
}

static bool tas6424_is_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TAS6424_DC_LOAD_DIAG_REP12_REG:
	case TAS6424_DC_LOAD_DIAG_REP34_REG:
	case TAS6424_DC_LOAD_DIAG_REPLO_REG:
	case TAS6424_CHANNEL_STATE_REG:
	case TAS6424_CHANNEL_FAULT_REG:
	case TAS6424_GLOB_FAULT1_REG:
	case TAS6424_GLOB_FAULT2_REG:
	case TAS6424_WARN_REG:
	case TAS6424_AC_LOAD_DIAG_REP1_REG:
	case TAS6424_AC_LOAD_DIAG_REP2_REG:
	case TAS6424_AC_LOAD_DIAG_REP3_REG:
	case TAS6424_AC_LOAD_DIAG_REP4_REG:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config tas6424_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = TAS6424_MAX_REG,
	.cache_type = REGCACHE_RBTREE,
	.writeable_reg = tas6424_is_writable_reg,
	.volatile_reg = tas6424_is_volatile_reg,
};

/*
 * DAC digital volumes. From -103.5 to 24 dB in 0.5 dB steps. Note that
 * setting the gain below -100 dB (register value <0x7) is effectively a MUTE
 * as per device datasheet.
 */
static DECLARE_TLV_DB_SCALE(dac_tlv, -10350, 50, 0);

static const struct snd_kcontrol_new tas6424_snd_controls[] = {
	SOC_SINGLE_TLV("Speaker Driver CH1 Playback Volume",
		       TAS6424_CH1_VOL_CTRL_REG, 0, 0xff, 0, dac_tlv),
	SOC_SINGLE_TLV("Speaker Driver CH2 Playback Volume",
		       TAS6424_CH2_VOL_CTRL_REG, 0, 0xff, 0, dac_tlv),
	SOC_SINGLE_TLV("Speaker Driver CH3 Playback Volume",
		       TAS6424_CH3_VOL_CTRL_REG, 0, 0xff, 0, dac_tlv),
	SOC_SINGLE_TLV("Speaker Driver CH4 Playback Volume",
		       TAS6424_CH4_VOL_CTRL_REG, 0, 0xff, 0, dac_tlv),
};

static const struct snd_soc_dapm_widget tas6424_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("DAC IN", "Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_DAC_E("DAC", NULL, SND_SOC_NOPM, 0, 0, tas6424_dac_event,
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUTPUT("OUT")
};

static const struct snd_soc_dapm_route tas6424_audio_map[] = {
	{ "DAC", NULL, "DAC IN" },
	{ "OUT", NULL, "DAC" },
};

static struct snd_soc_codec_driver soc_codec_dev_tas6424 = {
	.probe = tas6424_codec_probe,
	.remove = tas6424_codec_remove,
	.suspend = tas6424_suspend,
	.resume = tas6424_resume,
	.set_bias_level = tas6424_set_bias_level,
	.idle_bias_off = true,

	.component_driver = {
		.controls = tas6424_snd_controls,
		.num_controls = ARRAY_SIZE(tas6424_snd_controls),
		.dapm_widgets = tas6424_dapm_widgets,
		.num_dapm_widgets = ARRAY_SIZE(tas6424_dapm_widgets),
		.dapm_routes = tas6424_audio_map,
		.num_dapm_routes = ARRAY_SIZE(tas6424_audio_map),
	},
};

/* PCM rates supported by the TAS6424 driver */
#define TAS6424_RATES	(SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |\
			 SNDRV_PCM_RATE_96000)

/* Formats supported by TAS6424 driver */
#define TAS6424_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |\
			 SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_ops tas6424_speaker_dai_ops = {
	.hw_params	= tas6424_hw_params,
	.set_fmt	= tas6424_set_dai_fmt,
	.set_tdm_slot	= tas6424_set_dai_tdm_slot,
	.digital_mute	= tas6424_mute,
};

/* TAS6424 DAI structure */
static struct snd_soc_dai_driver tas6424_dai[] = {
	{
		.name = "tas6424-amplifier",
		.playback = {
			.stream_name = "Playback",
			.channels_min = 1,
			.channels_max = 4,
			.rates = TAS6424_RATES,
			.formats = TAS6424_FORMATS,
		},
		.ops = &tas6424_speaker_dai_ops,
	},
};

static int tas6424_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct tas6424_data *data;
	int ret;
	int i;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->tas6424_client = client;
	data->regmap = devm_regmap_init_i2c(client, &tas6424_regmap_config);
	if (IS_ERR(data->regmap)) {
		ret = PTR_ERR(data->regmap);
		dev_err(dev, "failed to allocate register map: %d\n", ret);
		return ret;
	}

	for (i = 0; i < ARRAY_SIZE(data->supplies); i++)
		data->supplies[i].supply = tas6424_supply_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(data->supplies),
				      data->supplies);
	if (ret != 0) {
		dev_err(dev, "failed to request supplies: %d\n", ret);
		return ret;
	}

	dev_set_drvdata(dev, data);

	ret = snd_soc_register_codec(&client->dev,
				     &soc_codec_dev_tas6424,
				     tas6424_dai, ARRAY_SIZE(tas6424_dai));
	if (ret < 0) {
		dev_err(dev, "failed to register codec: %d\n", ret);
		return ret;
	}

	return 0;
}

static int tas6424_remove(struct i2c_client *client)
{
	struct device *dev = &client->dev;

	snd_soc_unregister_codec(dev);

	return 0;
}

static const struct i2c_device_id tas6424_id[] = {
	{ "tas6424", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tas6424_id);

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id tas6424_of_match[] = {
	{ .compatible = "ti,tas6424", },
	{ },
};
MODULE_DEVICE_TABLE(of, tas6424_of_match);
#endif

static struct i2c_driver tas6424_i2c_driver = {
	.driver = {
		.name = "tas6424",
		.of_match_table = of_match_ptr(tas6424_of_match),
	},
	.probe = tas6424_probe,
	.remove = tas6424_remove,
	.id_table = tas6424_id,
};
module_i2c_driver(tas6424_i2c_driver);

MODULE_AUTHOR("Andreas Dannenberg <dannenberg@ti.com>");
MODULE_DESCRIPTION("TAS6424 Audio amplifier driver");
MODULE_LICENSE("GPL");
