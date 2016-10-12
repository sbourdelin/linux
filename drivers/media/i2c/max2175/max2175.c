/*
 * Maxim Integrated MAX2175 RF to Bits tuner driver
 *
 * This driver & most of the hard coded values are based on the reference
 * application delivered by Maxim for this chip.
 *
 * Copyright (C) 2016 Maxim Integrated Products
 * Copyright (C) 2016 Renesas Electronics Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-of.h>

#include "max2175.h"

#define DRIVER_NAME "max2175"

static unsigned int max2175_debug;
module_param(max2175_debug, uint, 0644);
MODULE_PARM_DESC(max2175_debug, "activate debug info");

#define mxm_dbg(ctx, fmt, arg...) \
	v4l2_dbg(1, max2175_debug, &ctx->sd, fmt, ## arg)

/* NOTE: Any addition/deletion in the below list should be reflected in
 * max2175_modetag enum
 */
static const struct max2175_rxmode eu_rx_modes[] = { /* Indexed by EU modetag */
	/* EU modes */
	{ MAX2175_BAND_VHF, 182640000, 0, { 0, 0, 0, 0 } },
};

static const struct max2175_rxmode na_rx_modes[] = { /* Indexed by NA modetag */
	/* NA modes */
	{ MAX2175_BAND_FM, 98255520, 1, { 0, 0, 0, 0 } },
};

/* Preset values:
 * Based on Maxim MAX2175 Register Table revision: 130p10
 */
static const u8 full_fm_eu_1p0[] = {
	0x15, 0x04, 0xb8, 0xe3, 0x35, 0x18, 0x7c, 0x00,
	0x00, 0x7d, 0x40, 0x08, 0x70, 0x7a, 0x88, 0x91,
	0x61, 0x61, 0x61, 0x61, 0x5a, 0x0f, 0x34, 0x1c,
	0x14, 0x88, 0x33, 0x02, 0x00, 0x09, 0x00, 0x65,
	0x9f, 0x2b, 0x80, 0x00, 0x95, 0x05, 0x2c, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,
	0x4a, 0x08, 0xa8, 0x0e, 0x0e, 0x2f, 0x7e, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0xab, 0x5e, 0xa9,
	0xae, 0xbb, 0x57, 0x18, 0x3b, 0x03, 0x3b, 0x64,
	0x40, 0x60, 0x00, 0x2a, 0xbf, 0x3f, 0xff, 0x9f,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00,
	0xff, 0xfc, 0xef, 0x1c, 0x40, 0x00, 0x00, 0x02,
	0x00, 0x00, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0xac, 0x40, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x75, 0x00, 0x00,
	0x00, 0x47, 0x00, 0x00, 0x11, 0x3f, 0x22, 0x00,
	0xf1, 0x00, 0x41, 0x03, 0xb0, 0x00, 0x00, 0x00,
	0x1b,
};

static const u8 full_fm_na_1p0[] = {
	0x13, 0x08, 0x8d, 0xc0, 0x35, 0x18, 0x7d, 0x3f,
	0x7d, 0x75, 0x40, 0x08, 0x70, 0x7a, 0x88, 0x91,
	0x61, 0x61, 0x61, 0x61, 0x5c, 0x0f, 0x34, 0x1c,
	0x14, 0x88, 0x33, 0x02, 0x00, 0x01, 0x00, 0x65,
	0x9f, 0x2b, 0x80, 0x00, 0x95, 0x05, 0x2c, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,
	0x4a, 0x08, 0xa8, 0x0e, 0x0e, 0xaf, 0x7e, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0xab, 0x5e, 0xa9,
	0xae, 0xbb, 0x57, 0x18, 0x3b, 0x03, 0x3b, 0x64,
	0x40, 0x60, 0x00, 0x2a, 0xbf, 0x3f, 0xff, 0x9f,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00,
	0xff, 0xfc, 0xef, 0x1c, 0x40, 0x00, 0x00, 0x02,
	0x00, 0x00, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0xa6, 0x40, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x75, 0x00, 0x00,
	0x00, 0x35, 0x00, 0x00, 0x11, 0x3f, 0x22, 0x00,
	0xf1, 0x00, 0x41, 0x03, 0xb0, 0x00, 0x00, 0x00,
	0x1b,
};

static const struct max2175_regmap dab12_map[] = {
	{ 0x01, 0x13 }, { 0x02, 0x0d }, { 0x03, 0x15 }, { 0x04, 0x55 },
	{ 0x05, 0x0a }, { 0x06, 0xa0 }, { 0x07, 0x40 }, { 0x08, 0x00 },
	{ 0x09, 0x00 }, { 0x0a, 0x7d }, { 0x0b, 0x4a }, { 0x0c, 0x28 },
	{ 0x0e, 0x43 }, { 0x0f, 0xb5 }, { 0x10, 0x31 }, { 0x11, 0x9e },
	{ 0x12, 0x68 }, { 0x13, 0x9e }, { 0x14, 0x68 }, { 0x15, 0x58 },
	{ 0x16, 0x2f }, { 0x17, 0x3f }, { 0x18, 0x40 }, { 0x1a, 0x88 },
	{ 0x1b, 0xaa }, { 0x1c, 0x9a }, { 0x1d, 0x00 }, { 0x1e, 0x00 },
	{ 0x23, 0x80 }, { 0x24, 0x00 }, { 0x25, 0x00 }, { 0x26, 0x00 },
	{ 0x27, 0x00 }, { 0x32, 0x08 }, { 0x33, 0xf8 }, { 0x36, 0x2d },
	{ 0x37, 0x7e }, { 0x55, 0xaf }, { 0x56, 0x3f }, { 0x57, 0xf8 },
	{ 0x58, 0x99 }, { 0x76, 0x00 }, { 0x77, 0x00 }, { 0x78, 0x02 },
	{ 0x79, 0x40 }, { 0x82, 0x00 }, { 0x83, 0x00 }, { 0x85, 0x00 },
	{ 0x86, 0x20 },
};

static const u16 ch_coeff_dab1[] = {
	0x001c, 0x0007, 0xffcd, 0x0056, 0xffa4, 0x0033, 0x0027, 0xff61,
	0x010e, 0xfec0, 0x0106, 0xffb8, 0xff1c, 0x023c, 0xfcb2, 0x039b,
	0xfd4e, 0x0055, 0x036a, 0xf7de, 0x0d21, 0xee72, 0x1499, 0x6a51,
};

static const u16 ch_coeff_fmeu[] = {
	0x0000, 0xffff, 0x0001, 0x0002, 0xfffa, 0xffff, 0x0015, 0xffec,
	0xffde, 0x0054, 0xfff9, 0xff52, 0x00b8, 0x00a2, 0xfe0a, 0x00af,
	0x02e3, 0xfc14, 0xfe89, 0x089d, 0xfa2e, 0xf30f, 0x25be, 0x4eb6,
};

static const u16 eq_coeff_fmeu1_ra02_m6db[] = {
	0x0040, 0xffc6, 0xfffa, 0x002c, 0x000d, 0xff90, 0x0037, 0x006e,
	0xffc0, 0xff5b, 0x006a, 0x00f0, 0xff57, 0xfe94, 0x0112, 0x0252,
	0xfe0c, 0xfc6a, 0x0385, 0x0553, 0xfa49, 0xf789, 0x0b91, 0x1a10,
};

static const u16 ch_coeff_fmna[] = {
	0x0001, 0x0003, 0xfffe, 0xfff4, 0x0000, 0x001f, 0x000c, 0xffbc,
	0xffd3, 0x007d, 0x0075, 0xff33, 0xff01, 0x0131, 0x01ef, 0xfe60,
	0xfc7a, 0x020e, 0x0656, 0xfd94, 0xf395, 0x02ab, 0x2857, 0x3d3f,
};

static const u16 eq_coeff_fmna1_ra02_m6db[] = {
	0xfff1, 0xffe1, 0xffef, 0x000e, 0x0030, 0x002f, 0xfff6, 0xffa7,
	0xff9d, 0x000a, 0x00a2, 0x00b5, 0xffea, 0xfed9, 0xfec5, 0x003d,
	0x0217, 0x021b, 0xff5a, 0xfc2b, 0xfcbd, 0x02c4, 0x0ac3, 0x0e85,
};

static const u8 adc_presets[2][23] = {
	{
		0x83, 0x00, 0xCF, 0xB4, 0x0F, 0x2C, 0x0C, 0x49,
		0x00, 0x00, 0x00, 0x8C,	0x02, 0x02, 0x00, 0x04,
		0xEC, 0x82, 0x4B, 0xCC, 0x01, 0x88, 0x0C,
	},
	{
		0x83, 0x00, 0xCF, 0xB4,	0x0F, 0x2C, 0x0C, 0x49,
		0x00, 0x00, 0x00, 0x8C,	0x02, 0x20, 0x33, 0x8C,
		0x57, 0xD7, 0x59, 0xB7,	0x65, 0x0E, 0x0C,
	},
};

/* Custom controls */
#define V4L2_CID_MAX2175_I2S_EN		(V4L2_CID_USER_MAX217X_BASE + 0x01)
#define V4L2_CID_MAX2175_I2S_MODE	(V4L2_CID_USER_MAX217X_BASE + 0x02)
#define V4L2_CID_MAX2175_AM_HIZ		(V4L2_CID_USER_MAX217X_BASE + 0x03)
#define V4L2_CID_MAX2175_HSLS		(V4L2_CID_USER_MAX217X_BASE + 0x04)
#define V4L2_CID_MAX2175_RX_MODE	(V4L2_CID_USER_MAX217X_BASE + 0x05)

/* Tuner bands */
static const struct v4l2_frequency_band bands_rf = {
	.tuner = 0,
	.type = V4L2_TUNER_RF,
	.index = 0,
	.capability = V4L2_TUNER_CAP_1HZ | V4L2_TUNER_CAP_FREQ_BANDS,
	.rangelow   = 160000000,
	.rangehigh  = 240000000,
};

struct max2175_ctx {
	struct v4l2_subdev sd;
	struct i2c_client *client;
	struct device *dev;

	/* Cached configuration */
	u8 regs[256];
	enum max2175_modetag mode;	/* Receive mode tag */
	u32 freq;			/* In Hz */
	struct max2175_rxmode *rx_modes;

	/* Device settings */
	bool master;
	u32 decim_ratio;
	u64 xtal_freq;

	/* ROM values */
	u8 rom_bbf_bw_am;
	u8 rom_bbf_bw_fm;
	u8 rom_bbf_bw_dab;

	/* Local copy of old settings */
	u8 i2s_test;

	u8 nbd_gain;
	u8 nbd_threshold;
	u8 wbd_gain;
	u8 wbd_threshold;
	u8 bbd_threshold;
	u8 bbdclip_threshold;
	u8 lt_wbd_threshold;
	u8 lt_wbd_gain;

	/* Controls */
	struct v4l2_ctrl_handler ctrl_hdl;
	struct v4l2_ctrl *lna_gain;	/* LNA gain value */
	struct v4l2_ctrl *if_gain;	/* I/F gain value */
	struct v4l2_ctrl *pll_lock;	/* PLL lock */
	struct v4l2_ctrl *i2s_en;	/* I2S output enable */
	struct v4l2_ctrl *i2s_mode;	/* I2S mode value */
	struct v4l2_ctrl *am_hiz;	/* AM High impledance input */
	struct v4l2_ctrl *hsls;		/* High-side/Low-side polarity */
	struct v4l2_ctrl *rx_mode;	/* Receive mode */

	/* Driver private variables */
	bool mode_resolved;		/* Flag to sanity check settings */
};

/* Helpers */

static int max2175_reg_write(struct max2175_ctx *ctx, u8 idx, u8 val)
{
	int ret;

	ret = i2c_smbus_write_byte_data(ctx->client, idx, val);
	if (ret) {
		v4l2_err(ctx->client,
			 "write: idx 0x%02x, val 0x%02x\n", idx, val);
		return ret;
	}

	/* Update local store */
	ctx->regs[idx] = val;
	if (max2175_debug >= 2)
		mxm_dbg(ctx, "write: reg[0x%02x] = 0x%02x\n", idx, val);

	return ret;
}

static int max2175_reg_read(struct max2175_ctx *ctx, u8 idx)
{
	int ret;

	ret = i2c_smbus_read_byte_data(ctx->client, idx);
	if (ret < 0) {
		v4l2_err(ctx->client, "read: idx 0x%02x\n", idx);
		return ret;
	}

	if (max2175_debug >= 2)
		mxm_dbg(ctx, "read: reg[0x%02x] = 0x%02x\n", idx, ret);

	return ret;
}

/* Flush local copy to device from idx to idx+len (inclusive) */
static void max2175_flush_regstore(struct max2175_ctx *ctx, u8 idx, u8 len)
{
	u8 i;

	for (i = idx; i <= len; i++)
		max2175_reg_write(ctx, i, ctx->regs[i]);
}

/* General bitops helpers */
static inline u8  __max2175_get_bits(u8 val, u8 msb, u8 lsb)
{
	return ((val & GENMASK(msb, lsb)) >> lsb);
}

static inline u8  __max2175_set_bits(u8 val, u8 msb, u8 lsb, u8 newval)
{
	u8 mask = GENMASK(msb, lsb);

	return ((val & ~mask) | ((newval << lsb) & mask));
}

/* Local store bitops helpers */
static u8 max2175_get_bits(struct max2175_ctx *ctx, u8 idx, u8 msb, u8 lsb)
{
	if (max2175_debug >= 2)
		mxm_dbg(ctx, "get_bits: idx:%u msb:%u lsb:%u\n",
			idx, msb, lsb);
	return __max2175_get_bits(ctx->regs[idx], msb, lsb);
}

static bool max2175_get_bit(struct max2175_ctx *ctx, u8 idx, u8 bit)
{
	return !!max2175_get_bits(ctx, idx, bit, bit);
}

static void max2175_set_bits(struct max2175_ctx *ctx, u8 idx,
		      u8 msb, u8 lsb, u8 newval)
{
	if (max2175_debug >= 2)
		mxm_dbg(ctx, "set_bits: idx:%u msb:%u lsb:%u newval:%u\n",
			idx, msb, lsb, newval);
	ctx->regs[idx] = __max2175_set_bits(ctx->regs[idx], msb, lsb,
					      newval);
}

static void max2175_set_bit(struct max2175_ctx *ctx, u8 idx, u8 bit, u8 newval)
{
	max2175_set_bits(ctx, idx, bit, bit, newval);
}

/* Device register bitops helpers */
static u8 max2175_read_bits(struct max2175_ctx *ctx, u8 idx, u8 msb, u8 lsb)
{
	return __max2175_get_bits(max2175_reg_read(ctx, idx), msb, lsb);
}

static void max2175_write_bits(struct max2175_ctx *ctx, u8 idx, u8 msb,
			u8 lsb, u8 newval)
{
	/* Update local copy & write to device */
	max2175_set_bits(ctx, idx, msb, lsb, newval);
	max2175_reg_write(ctx, idx, ctx->regs[idx]);
}

static void max2175_write_bit(struct max2175_ctx *ctx, u8 idx, u8 bit,
			      u8 newval)
{
	if (max2175_debug >= 2)
		mxm_dbg(ctx, "idx %u, bit %u, newval %u\n", idx, bit, newval);
	max2175_write_bits(ctx, idx, bit, bit, newval);
}

static int max2175_poll_timeout(struct max2175_ctx *ctx, u8 idx, u8 msb, u8 lsb,
				u8 exp_val, u32 timeout)
{
	unsigned long end = jiffies + msecs_to_jiffies(timeout);
	int ret;

	do {
		ret = max2175_read_bits(ctx, idx, msb, lsb);
		if (ret < 0)
			return ret;
		if (ret == exp_val)
			return 0;

		usleep_range(1000, 1500);
	} while (time_is_after_jiffies(end));

	return -EBUSY;
}

static int max2175_poll_csm_ready(struct max2175_ctx *ctx)
{
	return max2175_poll_timeout(ctx, 69, 1, 1, 0, 50);
}

#define MAX2175_IS_BAND_AM(ctx)		\
	(max2175_get_bits(ctx, 5, 1, 0) == MAX2175_BAND_AM)

#define MAX2175_IS_FM_MODE(ctx)		\
	(max2175_get_bits(ctx, 12, 5, 4) == 0)

#define MAX2175_IS_FMHD_MODE(ctx)	\
	(max2175_get_bits(ctx, 12, 5, 4) == 1)

#define MAX2175_IS_DAB_MODE(ctx)	\
	(max2175_get_bits(ctx, 12, 5, 4) == 2)

static int max2175_band_from_freq(u64 freq)
{
	if (freq >= 144000 && freq <= 26100000)
		return MAX2175_BAND_AM;
	else if (freq >= 65000000 && freq <= 108000000)
		return MAX2175_BAND_FM;
	else if (freq >= 160000000 && freq <= 240000000)
		return MAX2175_BAND_VHF;

	/* Add other bands on need basis */
	return -ENOTSUPP;
}

static int max2175_update_i2s_mode(struct max2175_ctx *ctx, u32 i2s_mode)
{
	/* Only change if it's new */
	if (max2175_read_bits(ctx, 29, 2, 0) == i2s_mode)
		return 0;

	max2175_write_bits(ctx, 29, 2, 0, i2s_mode);

	/* Based on I2S mode value I2S_WORD_CNT values change */
	if (i2s_mode == MAX2175_I2S_MODE3) {
		max2175_write_bits(ctx, 30, 6, 0, 1);
	} else if (i2s_mode == MAX2175_I2S_MODE2 ||
		   i2s_mode == MAX2175_I2S_MODE4) {
		max2175_write_bits(ctx, 30, 6, 0, 0);
	} else if (i2s_mode == MAX2175_I2S_MODE0) {
		max2175_write_bits(ctx, 30, 6, 0,
				   ctx->rx_modes[ctx->mode].i2s_word_size);
	} else {
		v4l2_err(ctx->client,
			 "failed: i2s_mode %u unsupported\n", i2s_mode);
		return 1;
	}
	mxm_dbg(ctx, "updated i2s_mode %u\n", i2s_mode);
	return 0;
}

static void max2175_i2s_enable(struct max2175_ctx *ctx, bool enable)
{
	if (enable) {
		/* Use old setting */
		max2175_write_bits(ctx, 104, 3, 0, ctx->i2s_test);
	} else {
		/* Cache old setting */
		ctx->i2s_test = max2175_read_bits(ctx, 104, 3, 0);
		max2175_write_bits(ctx, 104, 3, 0, 1);
	}
	mxm_dbg(ctx, "i2s %s: old val %u\n", enable ? "enabled" : "disabled",
		ctx->i2s_test);
}

static void max2175_set_filter_coeffs(struct max2175_ctx *ctx, u8 m_sel,
				      u8 bank, const u16 *coeffs)
{
	u8 i, coeff_addr, upper_address;

	mxm_dbg(ctx, "start: m_sel %d bank %d\n", m_sel, bank);
	max2175_write_bits(ctx, 114, 5, 4, m_sel);

	if (m_sel == 2)
		upper_address = 12;
	else
		upper_address = 24;

	max2175_set_bit(ctx, 117, 7, 1);
	for (i = 0; i < upper_address; i++) {
		coeff_addr = i + (bank * 24);
		max2175_set_bits(ctx, 115, 7, 0,
				 (u8)((coeffs[i] >> 8) & 0xff));
		max2175_set_bits(ctx, 116, 7, 0, (u8)(coeffs[i] & 0xff));
		max2175_set_bits(ctx, 117, 6, 0, coeff_addr);
		max2175_flush_regstore(ctx, 115, 3);
	}
	max2175_write_bit(ctx, 117, 7, 0);
}

static void max2175_load_dab_1p2(struct max2175_ctx *ctx)
{
	u32 i;

	/* Master is already set on init */
	for (i = 0; i < ARRAY_SIZE(dab12_map); i++)
		max2175_reg_write(ctx, dab12_map[i].idx, dab12_map[i].val);

	/* The default settings assume master */
	if (!ctx->master)
		max2175_write_bit(ctx, 30, 7, 1);

	/* Cache i2s_test value at this point */
	ctx->i2s_test = max2175_get_bits(ctx, 104, 3, 0);
	ctx->decim_ratio = 1;

	/* Load the Channel Filter Coefficients into channel filter bank #2 */
	max2175_set_filter_coeffs(ctx, MAX2175_CH_MSEL, 2, ch_coeff_dab1);
}

static void max2175_set_bbfilter(struct max2175_ctx *ctx)
{
	if (MAX2175_IS_BAND_AM(ctx)) {
		max2175_write_bits(ctx, 12, 3, 0, ctx->rom_bbf_bw_am);
		mxm_dbg(ctx, "max2175_set_bbfilter: AM: rom value = %d\n",
			ctx->rom_bbf_bw_am);
	} else {
		/* FM or DAB mode */
		if (MAX2175_IS_DAB_MODE(ctx)) {
			/* DAB mode. Load ROM values */
			max2175_write_bits(ctx, 12, 3, 0, ctx->rom_bbf_bw_dab);
			mxm_dbg(ctx, "max2175_set_bbfilter: DAB: rom value = %d\n",
				ctx->rom_bbf_bw_dab);
		} else {
			/* FM or FM HD mode. Load ROM values */
			max2175_write_bits(ctx, 12, 3, 0, ctx->rom_bbf_bw_fm);
			mxm_dbg(ctx, "max2175_set_bbfilter: FM: rom value = %d\n",
				ctx->rom_bbf_bw_fm);
		}
	}
}

static bool max2175_set_csm_mode(struct max2175_ctx *ctx,
			  enum max2175_csm_mode new_mode)
{
	int ret = max2175_poll_csm_ready(ctx);

	if (ret) {
		v4l2_err(ctx->client, "csm not ready: new mode %d\n", new_mode);
		return ret;
	}

	mxm_dbg(ctx, "set csm mode: new mode %d\n", new_mode);

	max2175_write_bits(ctx, 0, 2, 0, new_mode);

	/* Wait for a fixed settle down time depending on new mode and band */
	switch (new_mode) {
	case MAX2175_CSM_MODE_JUMP_FAST_TUNE:
		if (MAX2175_IS_BAND_AM(ctx)) {
			usleep_range(1250, 1500);	/* 1.25ms */
		} else {
			if (MAX2175_IS_DAB_MODE(ctx))
				usleep_range(3000, 3500);	/* 3ms */
			else
				usleep_range(1250, 1500);	/* 1.25ms */
		}
		break;

	/* Other mode switches can be added in the future if needed */
	default:
		break;
	}

	ret = max2175_poll_csm_ready(ctx);
	if (ret) {
		v4l2_err(ctx->client, "csm did not settle: new mode %d\n",
			 new_mode);
		return ret;
	}
	return ret;
}

static int max2175_csm_action(struct max2175_ctx *ctx,
			      enum max2175_csm_mode action)
{
	int ret;
	int load_buffer = MAX2175_CSM_MODE_LOAD_TO_BUFFER;

	mxm_dbg(ctx, "csm action: %d\n", action);

	/* Perform one or two CSM mode commands. */
	switch (action)	{
	case MAX2175_CSM_MODE_NO_ACTION:
		/* Take no FSM Action. */
		return 0;
	case MAX2175_CSM_MODE_LOAD_TO_BUFFER:
	case MAX2175_CSM_MODE_PRESET_TUNE:
	case MAX2175_CSM_MODE_SEARCH:
	case MAX2175_CSM_MODE_AF_UPDATE:
	case MAX2175_CSM_MODE_JUMP_FAST_TUNE:
	case MAX2175_CSM_MODE_CHECK:
	case MAX2175_CSM_MODE_LOAD_AND_SWAP:
	case MAX2175_CSM_MODE_END:
		ret = max2175_set_csm_mode(ctx, action);
		break;
	case MAX2175_CSM_MODE_BUFFER_PLUS_PRESET_TUNE:
		ret = max2175_set_csm_mode(ctx, load_buffer);
		if (ret) {
			v4l2_err(ctx->client, "csm action %d load buf failed\n",
				 action);
			break;
		}
		ret = max2175_set_csm_mode(ctx, MAX2175_CSM_MODE_PRESET_TUNE);
		break;
	case MAX2175_CSM_MODE_BUFFER_PLUS_SEARCH:
		ret = max2175_set_csm_mode(ctx, load_buffer);
		if (ret) {
			v4l2_err(ctx->client, "csm action %d load buf failed\n",
				 action);
			break;
		}
		ret = max2175_set_csm_mode(ctx, MAX2175_CSM_MODE_SEARCH);
		break;
	case MAX2175_CSM_MODE_BUFFER_PLUS_AF_UPDATE:
		ret = max2175_set_csm_mode(ctx, load_buffer);
		if (ret) {
			v4l2_err(ctx->client, "csm action %d load buf failed\n",
				 action);
			break;
		}
		ret = max2175_set_csm_mode(ctx, MAX2175_CSM_MODE_AF_UPDATE);
		break;
	case MAX2175_CSM_MODE_BUFFER_PLUS_JUMP_FAST_TUNE:
		ret = max2175_set_csm_mode(ctx, load_buffer);
		if (ret) {
			v4l2_err(ctx->client, "csm action %d load buf failed\n",
				 action);
			break;
		}
		ret = max2175_set_csm_mode(ctx,
					   MAX2175_CSM_MODE_JUMP_FAST_TUNE);
		break;
	case MAX2175_CSM_MODE_BUFFER_PLUS_CHECK:
		ret = max2175_set_csm_mode(ctx, load_buffer);
		if (ret) {
			v4l2_err(ctx->client, "csm action %d load buf failed\n",
				 action);
			break;
		}
		ret = max2175_set_csm_mode(ctx, MAX2175_CSM_MODE_CHECK);
		break;
	case MAX2175_CSM_MODE_BUFFER_PLUS_LOAD_AND_SWAP:
		ret = max2175_set_csm_mode(ctx, load_buffer);
		if (ret) {
			v4l2_err(ctx->client, "csm action %d load buf failed\n",
				 action);
			break;
		}
		ret = max2175_set_csm_mode(ctx, MAX2175_CSM_MODE_LOAD_AND_SWAP);
		break;
	default:
		ret = max2175_set_csm_mode(ctx, MAX2175_CSM_MODE_NO_ACTION);
		break;
	}
	return ret;
}

static int max2175_set_lo_freq(struct max2175_ctx *ctx, u64 lo_freq)
{
	int ret;
	u32 lo_mult;
	u64 scaled_lo_freq;
	const u64 scale_factor = 1000000ULL;
	u64 scaled_npf, scaled_integer, scaled_fraction;
	u32 frac_desired, int_desired;
	u8 loband_bits, vcodiv_bits;

	scaled_lo_freq = lo_freq;
	/* Scale to larger number for precision */
	scaled_lo_freq = scaled_lo_freq * scale_factor * 100;

	mxm_dbg(ctx, "scaled lo_freq %llu lo_freq %llu\n",
		scaled_lo_freq, lo_freq);

	if (MAX2175_IS_BAND_AM(ctx)) {
		if (max2175_get_bit(ctx, 5, 7) == 0)
			loband_bits = 0;
			vcodiv_bits = 0;
			lo_mult = 16;
	} else if (max2175_get_bits(ctx, 5, 1, 0) == MAX2175_BAND_FM) {
		if (lo_freq <= 74700000) {
			loband_bits = 0;
			vcodiv_bits = 0;
			lo_mult = 16;
		} else if ((lo_freq > 74700000) && (lo_freq <= 110000000)) {
			loband_bits = 1;
			vcodiv_bits = 0;
		} else {
			loband_bits = 1;
			vcodiv_bits = 3;
		}
		lo_mult = 8;
	} else if (max2175_get_bits(ctx, 5, 1, 0) == MAX2175_BAND_VHF) {
		if (lo_freq <= 210000000) {
			loband_bits = 2;
			vcodiv_bits = 2;
		} else {
			loband_bits = 2;
			vcodiv_bits = 1;
		}
		lo_mult = 4;
	} else {
		loband_bits = 3;
		vcodiv_bits = 2;
		lo_mult = 2;
	}

	if (max2175_get_bits(ctx, 5, 1, 0) == MAX2175_BAND_L)
		scaled_npf = (scaled_lo_freq / ctx->xtal_freq / lo_mult) / 100;
	else
		scaled_npf = (scaled_lo_freq / ctx->xtal_freq * lo_mult) / 100;

	scaled_integer = scaled_npf / scale_factor * scale_factor;
	int_desired = (u32)(scaled_npf / scale_factor);
	scaled_fraction = scaled_npf - scaled_integer;
	frac_desired = (u32)(scaled_fraction * 1048576 / scale_factor);

	/* Check CSM is not busy */
	ret = max2175_poll_csm_ready(ctx);
	if (ret) {
		v4l2_err(ctx->client, "lo_freq: csm busy. freq %llu\n",
			 lo_freq);
		return ret;
	}

	mxm_dbg(ctx, "loband %u vcodiv %u lo_mult %u scaled_npf %llu\n",
		loband_bits, vcodiv_bits, lo_mult, scaled_npf);
	mxm_dbg(ctx, "scaled int %llu frac %llu desired int %u frac %u\n",
		scaled_integer, scaled_fraction, int_desired, frac_desired);

	/* Write the calculated values to the appropriate registers */
	max2175_set_bits(ctx, 5, 3, 2, loband_bits);
	max2175_set_bits(ctx, 6, 7, 6, vcodiv_bits);
	max2175_set_bits(ctx, 1, 7, 0, (u8)(int_desired & 0xff));
	max2175_set_bits(ctx, 2, 3, 0, (u8)((frac_desired >> 16) & 0x1f));
	max2175_set_bits(ctx, 3, 7, 0, (u8)((frac_desired >> 8) & 0xff));
	max2175_set_bits(ctx, 4, 7, 0, (u8)(frac_desired & 0xff));
	/* Flush the above registers to device */
	max2175_flush_regstore(ctx, 1, 6);
	return ret;
}

static int max2175_set_nco_freq(struct max2175_ctx *ctx, s64 nco_freq_desired)
{
	int ret;
	u64 clock_rate, abs_nco_freq;
	s64  nco_freq, nco_val_desired;
	u32 nco_reg;
	const u64 scale_factor = 1000000ULL;

	mxm_dbg(ctx, "nco_freq: freq = %lld\n", nco_freq_desired);
	clock_rate = ctx->xtal_freq / ctx->decim_ratio;
	nco_freq = -nco_freq_desired;

	if (nco_freq < 0)
		abs_nco_freq = -nco_freq;
	else
		abs_nco_freq = nco_freq;

	/* Scale up the values for precision */
	if (abs_nco_freq < (clock_rate / 2)) {
		nco_val_desired = (2 * nco_freq * scale_factor) / clock_rate;
	} else {
		if (nco_freq < 0)
			nco_val_desired = (-2 * (clock_rate - abs_nco_freq) *
				scale_factor) / clock_rate;
		else
			nco_val_desired = (2 * (clock_rate - abs_nco_freq) *
				scale_factor) / clock_rate;
	}

	/* Scale down to get the fraction */
	if (nco_freq < 0)
		nco_reg = 0x200000 + ((nco_val_desired * 1048576) /
				      scale_factor);
	else
		nco_reg = (nco_val_desired * 1048576) / scale_factor;

	/* Check CSM is not busy */
	ret = max2175_poll_csm_ready(ctx);
	if (ret) {
		v4l2_err(ctx->client, "nco_freq: csm busy. freq %llu\n",
			 nco_freq);
		return ret;
	}

	mxm_dbg(ctx, "clk %llu decim %u abs %llu desired %lld reg %u\n",
		clock_rate, ctx->decim_ratio, abs_nco_freq,
		nco_val_desired, nco_reg);

	/* Write the calculated values to the appropriate registers */
	max2175_set_bits(ctx, 7, 4, 0, (u8)((nco_reg >> 16) & 0x1f));
	max2175_set_bits(ctx, 8, 7, 0, (u8)((nco_reg >> 8) & 0xff));
	max2175_set_bits(ctx, 9, 7, 0, (u8)(nco_reg & 0xff));
	max2175_flush_regstore(ctx, 7, 3);  /* Send all 3 NCO registers. */
	return ret;
}

static int max2175_set_rf_freq_non_am_bands(struct max2175_ctx *ctx, u64 freq,
					    u32 lo_pos)
{
	int ret;
	s64 adj_freq;
	u64 low_if_freq;

	mxm_dbg(ctx, "rf_freq: non AM bands\n");

	if (MAX2175_IS_FM_MODE(ctx))
		low_if_freq = 128000;
	else if (MAX2175_IS_FMHD_MODE(ctx))
		low_if_freq = 228000;
	else
		return max2175_set_lo_freq(ctx, freq);

	if (max2175_get_bits(ctx, 5, 1, 0) == MAX2175_BAND_VHF) {
		if (lo_pos == MAX2175_LO_ABOVE_DESIRED)
			adj_freq = freq + low_if_freq;
		else
			adj_freq = freq - low_if_freq;
	} else {
		if (lo_pos == MAX2175_LO_ABOVE_DESIRED)
			adj_freq = freq - low_if_freq;
		else
			adj_freq = freq + low_if_freq;
	}

	ret = max2175_set_lo_freq(ctx, adj_freq);
	if (ret)
		return ret;

	return max2175_set_nco_freq(ctx, low_if_freq);
}

static int max2175_set_rf_freq(struct max2175_ctx *ctx, u64 freq, u32 lo_pos)
{
	int ret;

	if (MAX2175_IS_BAND_AM(ctx))
		ret = max2175_set_nco_freq(ctx, freq);
	else
		ret = max2175_set_rf_freq_non_am_bands(ctx, freq, lo_pos);

	if (!ret)
		mxm_dbg(ctx, "set rf_freq: freq = %llu. done\n", freq);

	return ret;
}

static int max2175_tune_rf_freq(struct max2175_ctx *ctx, u64 freq, u32 hsls)
{
	int ret;

	ret = max2175_set_rf_freq(ctx, freq, hsls);
	if (ret)
		return ret;

	ret = max2175_csm_action(ctx,
				 MAX2175_CSM_MODE_BUFFER_PLUS_JUMP_FAST_TUNE);
	if (ret)
		return ret;

	mxm_dbg(ctx, "tuned rf_freq: old %u new %llu\n", ctx->freq, freq);
	ctx->freq = freq;
	return ret;
}

static void max2175_set_hsls(struct max2175_ctx *ctx, u32 lo_pos)
{
	mxm_dbg(ctx, "set_hsls: lo_pos = %u\n", lo_pos);

	if (lo_pos == MAX2175_LO_BELOW_DESIRED)	{
		if (max2175_get_bits(ctx, 5, 1, 0) == MAX2175_BAND_VHF)
			max2175_write_bit(ctx, 5, 4, 1);
		else
			max2175_write_bit(ctx, 5, 4, 0);
	} else {
		if (max2175_get_bits(ctx, 5, 1, 0) == MAX2175_BAND_VHF)
			max2175_write_bit(ctx, 5, 4, 0);
		else
			max2175_write_bit(ctx, 5, 4, 1);
	}
}

static int max2175_set_rx_mode(struct max2175_ctx *ctx, u32 rx_mode,
			       u32 am_hiz, u32 hsls)
{
	mxm_dbg(ctx, "receive mode: %u am_hiz %u\n", rx_mode, am_hiz);

	switch (rx_mode) {
	case MAX2175_DAB_1_2:
		max2175_load_dab_1p2(ctx);
		break;

	/* Other modes can be added in future if needed */
	default:
		v4l2_err(ctx->client, "invalid rx_mode %u\n", rx_mode);
		return -EINVAL;
	}

	if (am_hiz) {
		mxm_dbg(ctx, "setting AM HiZ related config\n");
		max2175_write_bit(ctx, 50, 5, 1);
		max2175_write_bit(ctx, 90, 7, 1);
		max2175_write_bits(ctx, 73, 1, 0, 2);
		max2175_write_bits(ctx, 80, 5, 0, 33);
	}

	/* Store some of the initial values loaded, which will be needed at
	 * a later stage
	 */
	ctx->nbd_threshold = max2175_get_bits(ctx, 14, 7, 4);
	ctx->nbd_gain = max2175_get_bits(ctx, 16, 7, 5);
	ctx->wbd_threshold = max2175_get_bits(ctx, 14, 3, 0);
	ctx->wbd_gain = max2175_get_bits(ctx, 15, 5, 4);
	ctx->bbd_threshold = max2175_get_bits(ctx, 15, 3, 0);
	ctx->bbdclip_threshold = max2175_get_bits(ctx, 35, 7, 5);
	ctx->lt_wbd_threshold = max2175_get_bits(ctx, 32, 3, 0);
	ctx->lt_wbd_gain = max2175_get_bits(ctx, 32, 5, 4);

	/* Load BB filter trim values saved in ROM */
	max2175_set_bbfilter(ctx);

	/* Set HSLS */
	max2175_set_hsls(ctx, hsls);

	ctx->mode = rx_mode;
	ctx->mode_resolved = true;
	return 0;
}

static bool max2175_is_i2s_rx_mode_valid(struct max2175_ctx *ctx,
					 u32 mode, u32 i2s_mode)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(ctx->rx_modes[mode].i2s_modes); i++)
		if (ctx->rx_modes[mode].i2s_modes[i] == i2s_mode)
			return true;

	v4l2_err(ctx->client, "i2s_mode %u not suitable for rx mode %u\n",
		 i2s_mode, mode);
	return false;
}

static int max2175_mode_from_freq(struct max2175_ctx *ctx, u32 freq, u32 *mode)
{
	u32 i;
	int band;

	band = max2175_band_from_freq(freq);
	if (band < 0) {
		v4l2_err(ctx->client, "mode from freq failed: %u ret %d\n",
			 freq, band);
		return 1;
	}

	/* Pick the first match always */
	for (i = 0; i <= ctx->rx_mode->maximum; i++) {
		if (ctx->rx_modes[i].band == band) {
			*mode = i;
			mxm_dbg(ctx, "mode from freq: freq %u mode %d\n",
				freq, *mode);
			return 0;
		}
	}
	v4l2_err(ctx->client, "mode from freq failed: freq %u\n", freq);
	return 1;
}

static bool max2175_is_freq_rx_mode_valid(struct max2175_ctx *ctx,
					 u32 mode, u32 freq)
{
	int band;

	band = max2175_band_from_freq(freq);
	if (band < 0) {
		v4l2_err(ctx->client, "freq rx mode failed: freq %u ret %d\n",
			 freq, band);
		return band;
	}

	mxm_dbg(ctx, "freq rx mode valid: freq %u mode %u\n", freq, mode);
	return (ctx->rx_modes[mode].band == band);
}

static void max2175_load_adc_presets(struct max2175_ctx *ctx)
{
	u32 i;

	for (i = 0; i < 2; i++) {
		memcpy(&ctx->regs[146 + (i * 55)], &adc_presets[i][0], 23);
		max2175_flush_regstore(ctx, 146 + (i * 55), 23);
	}
}

static int max2175_init_power_manager(struct max2175_ctx *ctx)
{
	int ret;

	/* Execute on-chip power-up/calibration */
	max2175_write_bit(ctx, 99, 2, 0);
	usleep_range(1000, 1500);
	max2175_write_bit(ctx, 99, 2, 1);

	/* Wait for the power manager to finish. */
	ret = max2175_poll_timeout(ctx, 69, 7, 7, 1, 50);
	if (ret)
		v4l2_err(ctx->client, "init pm failed\n");
	return ret;
}

static int max2175_recalibrate_adc(struct max2175_ctx *ctx)
{
	int ret;

	/* ADC Re-calibration */
	max2175_reg_write(ctx, 150, 0xff);
	max2175_reg_write(ctx, 205, 0xff);
	max2175_reg_write(ctx, 147, 0x20);
	max2175_reg_write(ctx, 147, 0x00);
	max2175_reg_write(ctx, 202, 0x20);
	max2175_reg_write(ctx, 202, 0x00);

	ret = max2175_poll_timeout(ctx, 69, 4, 3, 3, 50);
	if (ret)
		v4l2_err(ctx->client, "adc recalibration failed\n");
	return ret;
}

static u8 max2175_read_rom(struct max2175_ctx *ctx, u8 row)
{
	u8 data;

	max2175_write_bit(ctx, 56, 4, 0);
	max2175_write_bits(ctx, 56, 3, 0, row);
	usleep_range(2000, 2500);

	data = max2175_reg_read(ctx, 58);
	max2175_write_bits(ctx, 56, 3, 0, 0);

	mxm_dbg(ctx, "read rom: row 0x%02x, data 0x%02x\n", row, data);
	return data;
}

static void max2175_load_from_rom(struct max2175_ctx *ctx)
{
	u8 data = 0;

	data = max2175_read_rom(ctx, 0);
	ctx->rom_bbf_bw_am = (data & 0x0f);
	max2175_write_bits(ctx, 81, 3, 0, (data >> 4) & 0xf);

	data = max2175_read_rom(ctx, 1);
	ctx->rom_bbf_bw_fm = (data & 0x0f);
	ctx->rom_bbf_bw_dab = (data & 0xf0) >> 4;

	data = max2175_read_rom(ctx, 2);
	max2175_write_bits(ctx, 82, 4, 0, data & 0x1f);
	max2175_write_bits(ctx, 82, 7, 5, data >> 5);

	data = max2175_read_rom(ctx, 3);
	if (ctx->am_hiz->val) {
		data &= 0x0f;
		data |= (max2175_read_rom(ctx, 7) & 0x40) >> 2;
		if (!data)
			data |= 2;
	} else {
		data = (data & 0xf0) >> 4;
		data |= (max2175_read_rom(ctx, 7) & 0x80) >> 3;
		if (!data)
			data |= 30;
	}
	max2175_write_bits(ctx, 80, 5, 0, data + 31);

	data = max2175_read_rom(ctx, 6);
	max2175_write_bits(ctx, 81, 7, 6, data >> 6);
}

static void max2175_load_fm_eu_1p0_full(struct max2175_ctx *ctx)
{
	memcpy(&ctx->regs[1], full_fm_eu_1p0, sizeof(full_fm_eu_1p0));
	ctx->decim_ratio = 36;
}

static void max2175_load_fm_na_1p0_full(struct max2175_ctx *ctx)
{
	memcpy(&ctx->regs[1], full_fm_na_1p0, sizeof(full_fm_na_1p0));
	ctx->decim_ratio = 27;
}

static int max2175_core_init(struct max2175_ctx *ctx, u32 refout_bits)
{
	int ret;

	/* MAX2175 uses 36.864MHz clock for EU & 40.154MHz for NA region */
	if (ctx->xtal_freq == MAX2175_EU_XTAL_FREQ)
		max2175_load_fm_eu_1p0_full(ctx);
	else
		max2175_load_fm_na_1p0_full(ctx);

	/* The default settings assume master */
	if (!ctx->master)
		max2175_set_bit(ctx, 30, 7, 1);

	mxm_dbg(ctx, "refout_bits %u\n", refout_bits);
	/* Set REFOUT */
	max2175_set_bits(ctx, 56, 7, 5, refout_bits);

	/* Send out all the registers to device except register 0 */
	max2175_flush_regstore(ctx, 1, 145);
	usleep_range(5000, 5500);

	/* ADC Reset */
	max2175_write_bit(ctx, 99, 1, 0);
	usleep_range(1000, 1500);
	max2175_write_bit(ctx, 99, 1, 1);

	/* Load ADC preset values */
	max2175_load_adc_presets(ctx);

	/* Initialize the power management state machine */
	ret = max2175_init_power_manager(ctx);
	if (ret)
		return ret;
	mxm_dbg(ctx, "init pm done\n");

	/* Recalibrate ADC */
	ret = max2175_recalibrate_adc(ctx);
	if (ret)
		return ret;
	mxm_dbg(ctx, "adc recalibration done\n");

	/* Load ROM values to appropriate registers */
	max2175_load_from_rom(ctx);

	if (ctx->xtal_freq == MAX2175_EU_XTAL_FREQ) {
		/* Load FIR coefficients into bank 0 */
		max2175_set_filter_coeffs(ctx, MAX2175_CH_MSEL, 0,
					  ch_coeff_fmeu);
		max2175_set_filter_coeffs(ctx, MAX2175_EQ_MSEL, 0,
					  eq_coeff_fmeu1_ra02_m6db);
	} else {
		/* Load FIR coefficients into bank 0 */
		max2175_set_filter_coeffs(ctx, MAX2175_CH_MSEL, 0,
					  ch_coeff_fmna);
		max2175_set_filter_coeffs(ctx, MAX2175_EQ_MSEL, 0,
					  eq_coeff_fmna1_ra02_m6db);
	}
	mxm_dbg(ctx, "core initialized\n");
	return 0;
}

#define max2175_ctx_from_sd(x)	\
	container_of(x, struct max2175_ctx, sd)
#define max2175_ctx_from_ctrl(x)	\
	container_of(x, struct max2175_ctx, ctrl_hdl)

static void max2175_s_ctrl_i2s_mode(struct max2175_ctx *ctx, u32 i2s_mode)
{
	mxm_dbg(ctx, "s_ctrl_i2s_mode: %u resolved %d\n", i2s_mode,
		ctx->mode_resolved);

	/* Update i2s mode on device only when mode is resolved & it is valid
	 * for the configured mode
	 */
	if (ctx->mode_resolved &&
	    max2175_is_i2s_rx_mode_valid(ctx, ctx->mode, i2s_mode))
		max2175_update_i2s_mode(ctx, i2s_mode);
}

static void max2175_s_ctrl_rx_mode(struct max2175_ctx *ctx, u32 rx_mode)
{
	int ret;

	/* Load mode. Range check already done */
	ret = max2175_set_rx_mode(ctx, rx_mode,
				  ctx->am_hiz->val, ctx->hsls->val);
	if (ret)
		return;

	/* Get current i2s_mode and update if needed for given rx_mode */
	if (max2175_is_i2s_rx_mode_valid(ctx, rx_mode, ctx->i2s_mode->val))
		max2175_update_i2s_mode(ctx, ctx->i2s_mode->val);
	else
		ctx->i2s_mode->val = max2175_read_bits(ctx, 29, 2, 0);

	mxm_dbg(ctx, "s_ctrl_rx_mode: %u curr freq %u\n", rx_mode, ctx->freq);
	/* Check if current freq valid for mode & update */
	if (max2175_is_freq_rx_mode_valid(ctx, rx_mode, ctx->freq))
		max2175_tune_rf_freq(ctx, ctx->freq, ctx->hsls->val);
	else
		/* Use default freq of mode if current freq is not valid */
		max2175_tune_rf_freq(ctx, ctx->rx_modes[rx_mode].freq,
				     ctx->hsls->val);
}

static int max2175_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct max2175_ctx *ctx = max2175_ctx_from_ctrl(ctrl->handler);
	int ret = 0;

	mxm_dbg(ctx, "s_ctrl: id 0x%x, val %u\n", ctrl->id, ctrl->val);
	switch (ctrl->id) {
	case V4L2_CID_MAX2175_I2S_EN:
		max2175_i2s_enable(ctx, ctrl->val == 1);
		break;
	case V4L2_CID_MAX2175_I2S_MODE:
		max2175_s_ctrl_i2s_mode(ctx, ctrl->val);
		break;
	case V4L2_CID_MAX2175_AM_HIZ:
		v4l2_ctrl_activate(ctx->am_hiz, false);
		break;
	case V4L2_CID_MAX2175_HSLS:
		v4l2_ctrl_activate(ctx->hsls, false);
		break;
	case V4L2_CID_MAX2175_RX_MODE:
		mxm_dbg(ctx, "rx mode %u\n", ctrl->val);
		max2175_s_ctrl_rx_mode(ctx, ctrl->val);
		break;
	default:
		v4l2_err(ctx->client, "s:invalid ctrl id 0x%x\n", ctrl->id);
		ret = -EINVAL;
	}

	return ret;
}

static int max2175_get_lna_gain(struct max2175_ctx *ctx)
{
	int gain = 0;
	enum max2175_band band = max2175_get_bits(ctx, 5, 1, 0);

	switch (band) {
	case MAX2175_BAND_AM:
		gain = max2175_read_bits(ctx, 51, 3, 1);
		break;
	case MAX2175_BAND_FM:
		gain = max2175_read_bits(ctx, 50, 3, 1);
		break;
	case MAX2175_BAND_VHF:
		gain = max2175_read_bits(ctx, 52, 3, 0);
		break;
	default:
		v4l2_err(ctx->client, "invalid band %d to get rf gain\n", band);
		break;
	}
	return gain;
}

static int max2175_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct max2175_ctx *ctx = max2175_ctx_from_ctrl(ctrl->handler);

	/* Only the dynamically changing values need to be in g_volatile_ctrl */
	mxm_dbg(ctx, "g_volatile_ctrl: id 0x%x\n", ctrl->id);
	switch (ctrl->id) {
	case V4L2_CID_RF_TUNER_LNA_GAIN:
		ctrl->val = max2175_get_lna_gain(ctx);
		break;
	case V4L2_CID_RF_TUNER_IF_GAIN:
		ctrl->val = max2175_read_bits(ctx, 49, 4, 0);
		break;
	case V4L2_CID_RF_TUNER_PLL_LOCK:
		ctrl->val = (max2175_read_bits(ctx, 60, 7, 6) == 3);
		break;
	default:
		v4l2_err(ctx->client, "g:invalid ctrl id 0x%x\n", ctrl->id);
		return -EINVAL;
	}
	mxm_dbg(ctx, "g_volatile_ctrl: id 0x%x val %d\n", ctrl->id, ctrl->val);
	return 0;
};

static int max2175_set_freq_and_mode(struct max2175_ctx *ctx, u32 freq)
{
	u32 rx_mode;
	int ret;

	/* Get band from frequency */
	ret = max2175_mode_from_freq(ctx, freq, &rx_mode);
	if (ret) {
		v4l2_err(ctx->client, "set freq mode failed: freq %u ret %d\n",
			 freq, ret);
		return ret;
	}
	mxm_dbg(ctx, "set_freq_and_mode: freq %u rx_mode %d\n", freq, rx_mode);

	/* Load mode */
	ret = max2175_set_rx_mode(ctx, rx_mode, ctx->am_hiz->val,
				  ctx->hsls->val);
	if (ret)
		return ret;

	/* Get current i2s_mode and update if needed for given rx_mode */
	if (max2175_is_i2s_rx_mode_valid(ctx, rx_mode, ctx->i2s_mode->val))
		max2175_update_i2s_mode(ctx, ctx->i2s_mode->val);
	else
		v4l2_ctrl_s_ctrl(ctx->i2s_mode,
				 max2175_read_bits(ctx, 29, 2, 0));

	/* Tune to the new freq given */
	return max2175_tune_rf_freq(ctx, freq, ctx->hsls->val);
}

static int max2175_s_frequency(struct v4l2_subdev *sd,
			       const struct v4l2_frequency *vf)
{
	struct max2175_ctx *ctx = max2175_ctx_from_sd(sd);
	u32 freq;
	int ret = 0;

	mxm_dbg(ctx, "s_freq: tuner %u type %u freq: new %u, curr %u\n",
		vf->tuner, vf->type, vf->frequency, ctx->freq);

	if (vf->tuner != 0) {
		v4l2_err(ctx->client, "s_freq: invalid tuner %u\n", vf->tuner);
		return -EINVAL;
	}
	/* RF freq */
	freq = clamp(vf->frequency, bands_rf.rangelow, bands_rf.rangehigh);
	if (ctx->mode_resolved)
		/* Check new freq valid for mode */
		if (max2175_is_freq_rx_mode_valid(ctx, ctx->mode, freq))
			ret = max2175_tune_rf_freq(ctx, freq, ctx->hsls->val);
		else
			/* Find default mode for freq and tune to it */
			ret = max2175_set_freq_and_mode(ctx, freq);
	else
		ret = max2175_set_freq_and_mode(ctx, freq);

	mxm_dbg(ctx, "s_freq: ret %d mode %d freq: curr %u\n",
		ret, ctx->mode, ctx->freq);
	return ret;
}

static int max2175_g_frequency(struct v4l2_subdev *sd,
			       struct v4l2_frequency *vf)
{
	struct max2175_ctx *ctx = max2175_ctx_from_sd(sd);
	int ret = 0;

	if (vf->tuner != 0) {
		v4l2_err(ctx->client, "g_freq: invalid tuner %u\n", vf->tuner);
		return -EINVAL;
	}

	/* RF freq */
	vf->type = V4L2_TUNER_RF;
	vf->frequency = ctx->freq;
	mxm_dbg(ctx, "g_freq: ret %d tuner %u, type %u, vf %u\n",
		ret, vf->tuner, vf->type, vf->frequency);

	return ret;
}

static int max2175_enum_freq_bands(struct v4l2_subdev *sd,
			    struct v4l2_frequency_band *band)
{
	struct max2175_ctx *ctx = max2175_ctx_from_sd(sd);
	int ret = 0;

	if (band->tuner == 0 && band->index == 0)
		*band = bands_rf;
	else
		ret = -EINVAL;

	mxm_dbg(ctx,
		"enum_freq: ret %d tuner %u type %u index %u low %u high %u\n",
		ret, band->tuner, band->type, band->index, band->rangelow,
		band->rangehigh);
	return ret;
}

static int max2175_g_tuner(struct v4l2_subdev *sd, struct v4l2_tuner *vt)
{
	if (vt->index > 0)
		return -EINVAL;

	strlcpy(vt->name, "RF", sizeof(vt->name));
	vt->type = V4L2_TUNER_RF;
	vt->capability =
		V4L2_TUNER_CAP_1HZ | V4L2_TUNER_CAP_FREQ_BANDS;
	vt->rangelow = bands_rf.rangelow;
	vt->rangehigh = bands_rf.rangehigh;
	return 0;
}

static int max2175_s_tuner(struct v4l2_subdev *sd, const struct v4l2_tuner *vt)
{
	/* Check tuner index is valid */
	if (vt->index > 0)
		return -EINVAL;

	return 0;
}

static const struct v4l2_subdev_tuner_ops max2175_tuner_ops = {
	.s_frequency = max2175_s_frequency,
	.g_frequency = max2175_g_frequency,
	.enum_freq_bands = max2175_enum_freq_bands,
	.g_tuner = max2175_g_tuner,
	.s_tuner = max2175_s_tuner,
};

static const struct v4l2_subdev_ops max2175_ops = {
	.tuner = &max2175_tuner_ops,
};

static const struct v4l2_ctrl_ops max2175_ctrl_ops = {
	.s_ctrl = max2175_s_ctrl,
	.g_volatile_ctrl = max2175_g_volatile_ctrl,
};

static const struct v4l2_ctrl_config max2175_i2s_en = {
	.ops = &max2175_ctrl_ops,
	.id = V4L2_CID_MAX2175_I2S_EN,
	.name = "I2S Enable",
	.type = V4L2_CTRL_TYPE_BOOLEAN,
	.min = 0,
	.max = 1,
	.step = 1,
	.def = 1,
};

static const struct v4l2_ctrl_config max2175_i2s_mode = {
	.ops = &max2175_ctrl_ops,
	.id = V4L2_CID_MAX2175_I2S_MODE,
	.name = "I2S_MODE value",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = 0,
	.max = 4,
	.step = 1,
	.def = 0,
};

static const struct v4l2_ctrl_config max2175_am_hiz = {
	.ops = &max2175_ctrl_ops,
	.id = V4L2_CID_MAX2175_AM_HIZ,
	.name = "AM High impedance input",
	.type = V4L2_CTRL_TYPE_BOOLEAN,
	.min = 0,
	.max = 1,
	.step = 1,
	.def = 0,
};

static const struct v4l2_ctrl_config max2175_hsls = {
	.ops = &max2175_ctrl_ops,
	.id = V4L2_CID_MAX2175_HSLS,
	.name = "HSLS above/below desired",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = 0,
	.max = 1,
	.step = 1,
	.def = 1,
};


/* NOTE: Any addition/deletion in the below list should be reflected in
 * max2175_modetag enum
 */
static const char * const max2175_ctrl_eu_rx_mode_strings[] = {
	"DAB 1.2",
	"NULL",
};

static const char * const max2175_ctrl_na_rx_mode_strings[] = {
	"NA FM 1.0",
	"NULL",
};

static const struct v4l2_ctrl_config max2175_eu_rx_mode = {
	.ops = &max2175_ctrl_ops,
	.id = V4L2_CID_MAX2175_RX_MODE,
	.name = "RX MODE",
	.type = V4L2_CTRL_TYPE_MENU,
	.max = ARRAY_SIZE(max2175_ctrl_eu_rx_mode_strings) - 2,
	.def = 0,
	.qmenu = max2175_ctrl_eu_rx_mode_strings,
};

static const struct v4l2_ctrl_config max2175_na_rx_mode = {
	.ops = &max2175_ctrl_ops,
	.id = V4L2_CID_MAX2175_RX_MODE,
	.name = "RX MODE",
	.type = V4L2_CTRL_TYPE_MENU,
	.max = ARRAY_SIZE(max2175_ctrl_na_rx_mode_strings) - 2,
	.def = 0,
	.qmenu = max2175_ctrl_na_rx_mode_strings,
};

static u32 max2175_refout_load_to_bits(struct i2c_client *client, u32 load)
{
	u32 bits = 0;	/* REFOUT disabled */

	if (load >= 0 && load <= 40)
		bits = load / 10;
	else if (load >= 60 && load <= 70)
		bits = load / 10 - 1;
	else
		dev_warn(&client->dev, "invalid refout_load %u\n", load);

	return bits;
}

static int max2175_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct max2175_ctx *ctx;
	struct device *dev = &client->dev;
	struct v4l2_subdev *sd;
	struct v4l2_ctrl_handler *hdl;
	struct clk *clk;
	bool master = true;
	u32 refout_load, refout_bits = 0;	/* REFOUT disabled */
	int ret;

	/* Check if the adapter supports the needed features */
	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(&client->dev, "i2c check failed\n");
		return -EIO;
	}

	if (of_find_property(client->dev.of_node, "maxim,slave", NULL))
		master = false;

	if (!of_property_read_u32(client->dev.of_node, "maxim,refout-load",
				 &refout_load))
		refout_bits = max2175_refout_load_to_bits(client, refout_load);

	clk = devm_clk_get(&client->dev, "xtal");
	if (IS_ERR(clk)) {
		ret = PTR_ERR(clk);
		dev_err(&client->dev, "cannot get xtal clock %d\n", ret);
		return -ENODEV;
	}

	ctx = kzalloc(sizeof(struct max2175_ctx),
			     GFP_KERNEL);
	if (ctx == NULL)
		return -ENOMEM;

	sd = &ctx->sd;
	ctx->master = master;
	ctx->mode_resolved = false;

	/* Set the defaults */
	ctx->freq = bands_rf.rangelow;

	ctx->xtal_freq = clk_get_rate(clk);
	dev_info(&client->dev, "xtal freq %lluHz\n", ctx->xtal_freq);

	v4l2_i2c_subdev_init(sd, client, &max2175_ops);
	ctx->client = client;

	sd->flags = V4L2_SUBDEV_FL_HAS_DEVNODE;
	ctx->dev = dev;

	/* Controls */
	hdl = &ctx->ctrl_hdl;
	ret = v4l2_ctrl_handler_init(hdl, 8);
	if (ret) {
		dev_err(&client->dev, "ctrl handler init failed\n");
		goto err;
	}

	ctx->lna_gain = v4l2_ctrl_new_std(hdl, &max2175_ctrl_ops,
					  V4L2_CID_RF_TUNER_LNA_GAIN,
					  0, 15, 1, 2);
	ctx->lna_gain->flags |= (V4L2_CTRL_FLAG_VOLATILE |
				 V4L2_CTRL_FLAG_READ_ONLY);
	ctx->if_gain = v4l2_ctrl_new_std(hdl, &max2175_ctrl_ops,
					 V4L2_CID_RF_TUNER_IF_GAIN,
					 0, 31, 1, 0);
	ctx->if_gain->flags |= (V4L2_CTRL_FLAG_VOLATILE |
				V4L2_CTRL_FLAG_READ_ONLY);
	ctx->pll_lock = v4l2_ctrl_new_std(hdl, &max2175_ctrl_ops,
					  V4L2_CID_RF_TUNER_PLL_LOCK,
					  0, 1, 1, 0);
	ctx->pll_lock->flags |= (V4L2_CTRL_FLAG_VOLATILE |
				 V4L2_CTRL_FLAG_READ_ONLY);
	ctx->i2s_en = v4l2_ctrl_new_custom(hdl, &max2175_i2s_en, NULL);
	ctx->i2s_mode = v4l2_ctrl_new_custom(hdl, &max2175_i2s_mode, NULL);
	ctx->am_hiz = v4l2_ctrl_new_custom(hdl, &max2175_am_hiz, NULL);
	ctx->hsls = v4l2_ctrl_new_custom(hdl, &max2175_hsls, NULL);

	if (ctx->xtal_freq == MAX2175_EU_XTAL_FREQ) {
		ctx->rx_mode = v4l2_ctrl_new_custom(hdl,
						    &max2175_eu_rx_mode, NULL);
		ctx->rx_modes = (struct max2175_rxmode *)eu_rx_modes;
	} else {
		ctx->rx_mode = v4l2_ctrl_new_custom(hdl,
						    &max2175_na_rx_mode, NULL);
		ctx->rx_modes = (struct max2175_rxmode *)na_rx_modes;
	}
	ctx->sd.ctrl_handler = &ctx->ctrl_hdl;

	ret = v4l2_async_register_subdev(sd);
	if (ret) {
		dev_err(&client->dev, "register subdev failed\n");
		goto err_reg;
	}
	dev_info(&client->dev, "subdev registered\n");

	/* Initialize device */
	ret = max2175_core_init(ctx, refout_bits);
	if (ret)
		goto err_init;

	mxm_dbg(ctx, "probed\n");
	return 0;

err_init:
	v4l2_async_unregister_subdev(sd);
err_reg:
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
err:
	kfree(ctx);
	return ret;
}

static int max2175_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct max2175_ctx *ctx = max2175_ctx_from_sd(sd);

	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	v4l2_async_unregister_subdev(sd);
	mxm_dbg(ctx, "removed\n");
	kfree(ctx);
	return 0;
}

static const struct i2c_device_id max2175_id[] = {
	{ DRIVER_NAME, 0},
	{},
};

MODULE_DEVICE_TABLE(i2c, max2175_id);

static const struct of_device_id max2175_of_ids[] = {
	{ .compatible = "maxim, max2175", },
	{ }
};
MODULE_DEVICE_TABLE(of, max2175_of_ids);

static struct i2c_driver max2175_driver = {
	.driver = {
		.name	= DRIVER_NAME,
		.of_match_table = max2175_of_ids,
	},
	.probe		= max2175_probe,
	.remove		= max2175_remove,
	.id_table	= max2175_id,
};

module_i2c_driver(max2175_driver);

MODULE_DESCRIPTION("Maxim MAX2175 RF to Bits tuner driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Ramesh Shanmugasundaram <ramesh.shanmugasundaram@bp.renesas.com>");
