/*
 * Analog Device ADV7180 video decoder driver
 *
 * Copyright (c) 2012-2014 Mentor Graphics Inc.
 * Copyright 2005-2012 Freescale Semiconductor, Inc. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/semaphore.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/wait.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-async.h>
#include <media/v4l2-of.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/adv718x.h>

struct adv7180_dev {
	struct i2c_client *i2c_client;
	struct device *dev;
	struct v4l2_subdev sd;
	struct v4l2_of_endpoint ep; /* the parsed DT endpoint info */
	struct v4l2_ctrl_handler ctrl_hdl;
	struct v4l2_mbus_framefmt fmt;
	struct v4l2_captureparm streamcap;
	int rev_id;
	bool on;

	bool locked;             /* locked to signal */

	struct regulator *dvddio;
	struct regulator *dvdd;
	struct regulator *avdd;
	struct regulator *pvdd;
	int pwdn_gpio;

	v4l2_std_id std_id;

	/* Standard index of ADV7180. */
	int video_idx;

	/* Current analog input mux */
	int current_input;

	struct mutex mutex;
};

static inline struct adv7180_dev *to_adv7180_dev(struct v4l2_subdev *sd)
{
	return container_of(sd, struct adv7180_dev, sd);
}

static inline struct adv7180_dev *ctrl_to_adv7180_dev(struct v4l2_ctrl *ctrl)
{
	return container_of(ctrl->handler, struct adv7180_dev, ctrl_hdl);
}

/*! List of input video formats supported. The video formats is corresponding
 * with v4l2 id in video_fmt_t
 */
enum {
	ADV7180_NTSC = 0,	/*!< Locked on (M) NTSC video signal. */
	ADV7180_PAL,		/*!< (B, G, H, I, N)PAL video signal. */
};

/*! Number of video standards supported (including 'not locked' signal). */
#define ADV7180_STD_MAX		(ADV7180_PAL + 1)

/* video standard info */
struct adv7180_std_info {
	unsigned long width;
	unsigned long height;
	struct v4l2_standard std;
};

static struct adv7180_std_info adv7180_std[] = {
	[ADV7180_NTSC] = {
		.width = 720,
		.height = 480,
		.std = {
			.id = V4L2_STD_NTSC,
			.name = "NTSC",
			.framelines = 525,
			.frameperiod = {
				.numerator = 1001,
				.denominator = 30000,
			},
		},
	},
	[ADV7180_PAL] = {
		.width = 720,
		.height = 576,
		.std = {
			.id = V4L2_STD_PAL,
			.name = "PAL",
			.framelines = 625,
			.frameperiod = {
				.numerator = 1,
				.denominator = 25,
			},
		},
	},
};

#define IF_NAME                    "adv7180"

/* Main Register Map */
#define ADV7180_INPUT_CTL              0x00	/* Input Control */
#define ADV7180_EXT_OUT_CTL            0x04
#define ADV7180_STATUS_1               0x10	/* Status #1 */
#define   ADV7180_IN_LOCK              (1 << 0)
#define   ADV7180_LOST_LOCK            (1 << 1)
#define   ADV7180_FSC_LOCK             (1 << 2)
#define   ADV7180_AD_RESULT_BIT        4
#define   ADV7180_AD_RESULT_MASK       (0x7 << ADV7180_AD_RESULT_BIT)
#define   ADV7180_AD_NTSC              0
#define   ADV7180_AD_NTSC_4_43         1
#define   ADV7180_AD_PAL_M             2
#define   ADV7180_AD_PAL_60            3
#define   ADV7180_AD_PAL               4
#define   ADV7180_AD_SECAM             5
#define   ADV7180_AD_PAL_N             6
#define   ADV7180_AD_SECAM_525         7
#define ADV7180_CONTRAST               0x08	/* Contrast */
#define ADV7180_BRIGHTNESS             0x0a	/* Brightness */
#define ADV7180_HUE_REG                0x0b	/* Signed, inverted */
#define ADV7180_DEF_Y                  0x0c
#define   ADV7180_DEF_VAL_EN           (1 << 0)
#define   ADV7180_DEF_VAL_AUTO_EN      (1 << 1)
#define   ADV7180_DEF_Y_BIT            2
#define   ADV7180_DEF_Y_MASK           (0x3f << ADV7180_DEF_Y_BIT)
#define ADV7180_DEF_C                  0x0d
#define   ADV7180_DEF_CB_BIT           0
#define   ADV7180_DEF_CB_MASK          (0xf << ADV7180_DEF_CB_BIT)
#define   ADV7180_DEF_CR_BIT           4
#define   ADV7180_DEF_CR_MASK          (0xf << ADV7180_DEF_CR_BIT)
#define ADV7180_ADI_CTL_1              0x0e
#define ADV7180_PWR_MNG                0x0f     /* Power Management */
#define ADV7180_IDENT                  0x11	/* IDENT */
#define ADV7180_ANALOG_CLAMP_CTL       0x14
#define ADV7180_SHAP_FILTER_CTL_1      0x17
#define ADV7180_VSYNC_FIELD_CTL_1      0x31	/* VSYNC Field Control #1 */
#define ADV7180_MANUAL_WIN_CTL_1       0x3d	/* Manual Window Control 1 */
#define ADV7180_MANUAL_WIN_CTL_2       0x3e     /* Manual Window Control 2 */
#define ADV7180_MANUAL_WIN_CTL_3       0x3f     /* Manual Window Control 3 */
#define ADV7180_CTI_DNR                0x4d
#define   ADV7180_CTI_EN               (1 << 0) /* CTI enable */
#define   ADV7180_CTI_AB_EN            (1 << 1) /* CTI Alpha Blend enable */
#define   ADV7180_CTI_AB_BIT           2 /* CTI Alpha Blend sharpness */
#define   ADV7180_CTI_AB_MASK          (0x3 << ADV7180_CTI_AB_BIT)
#define   ADV7180_DNR_EN               (1 << 5) /* DNR enable */
#define ADV7180_CTI_THRESH             0x4e /* CTI Chroma Amplitude Threshold */
#define ADV7180_DNR_THRESH1            0x50
#define ADV7180_LOCK_COUNT             0x51
#define ADV7180_CVBS_TRIM              0x52
#define ADV7180_SD_OFFSET_CB           0xe1	/* SD Offset Cb */
#define ADV7180_SD_OFFSET_CR           0xe2	/* SD Offset Cr */
#define ADV7180_SD_SATURATION_CB       0xe3	/* SD Saturation Cb */
#define ADV7180_SD_SATURATION_CR       0xe4	/* SD Saturation Cr */
#define ADV7180_DRIVE_STRENGTH         0xf4
#define ADV7180_LUMA_PEAK_GAIN         0xfb
#define ADV7180_DNR_THRESH2            0xfc
/* Interrupt Register Map */
#define ADV7180_INT_CONFIG_1           0x40     /* Interrupt Config 1 */
#define ADV7180_INT_STATUS_1           0x42     /* Interrupt Status 1 (r/o) */
#define   ADV7180_INT_SD_LOCK          (1 << 0)
#define   ADV7180_INT_SD_UNLOCK        (1 << 1)
#define ADV7180_INT_CLEAR_1            0x43     /* Interrupt Clear 1 (w/o) */
#define ADV7180_INT_MASK_1             0x44     /* Interrupt Mask 1 */
#define ADV7180_INT_RAW_STATUS_2       0x45   /* Interrupt Raw Status 2 (r/o) */
#define  ADV7180_INT_SD_FIELD          (1 << 4)
#define ADV7180_INT_STATUS_2           0x46     /* Interrupt Status 2 (r/o) */
#define  ADV7180_INT_SD_FIELD_CHNG     (1 << 4)
#define ADV7180_INT_CLEAR_2            0x47     /* Interrupt Clear 2 (w/o) */
#define ADV7180_INT_MASK_2             0x48     /* Interrupt Mask 2 */
#define ADV7180_INT_RAW_STATUS_3       0x49   /* Interrupt Raw Status 3 (r/o) */
#define   ADV7180_INT_SD_V_LOCK        (1 << 1)
#define ADV7180_INT_STATUS_3           0x4a   /* Interrupt Status 3 (r/o) */
#define   ADV7180_INT_SD_V_LOCK_CHNG   (1 << 1)
#define   ADV7180_INT_SD_H_LOCK_CHNG   (1 << 2)
#define   ADV7180_INT_SD_AD_CHNG       (1 << 3)
#define ADV7180_INT_CLEAR_3            0x4b     /* Interrupt Clear 3 (w/o) */
#define ADV7180_INT_MASK_3             0x4c     /* Interrupt Mask 3 */

struct adv7180_inputs_t {
	const char *desc;   /* Analog input description */
	u8 insel;           /* insel bits to select this input */
};

/* Analog Inputs on 64-Lead and 48-Lead LQFP */
static const struct adv7180_inputs_t adv7180_inputs_64_48[] = {
	{ .insel = 0x00, .desc = "ADV7180 Composite on Ain1" },
	{ .insel = 0x01, .desc = "ADV7180 Composite on Ain2" },
	{ .insel = 0x02, .desc = "ADV7180 Composite on Ain3" },
	{ .insel = 0x03, .desc = "ADV7180 Composite on Ain4" },
	{ .insel = 0x04, .desc = "ADV7180 Composite on Ain5" },
	{ .insel = 0x05, .desc = "ADV7180 Composite on Ain6" },
	{ .insel = 0x06, .desc = "ADV7180 Y/C on Ain1/4" },
	{ .insel = 0x07, .desc = "ADV7180 Y/C on Ain2/5" },
	{ .insel = 0x08, .desc = "ADV7180 Y/C on Ain3/6" },
	{ .insel = 0x09, .desc = "ADV7180 YPbPr on Ain1/4/5" },
	{ .insel = 0x0a, .desc = "ADV7180 YPbPr on Ain2/3/6" },
};

#define NUM_INPUTS_64_48 ARRAY_SIZE(adv7180_inputs_64_48)

#if 0
/*
 * FIXME: there is no way to distinguish LQFP vs LFCSP chips, so
 * we will just have to assume LQFP.
 */
/* Analog Inputs on 40-Lead and 32-Lead LFCSP */
static const struct adv7180_inputs_t adv7180_inputs_40_32[] = {
	{ .insel = 0x00, .desc = "ADV7180 Composite on Ain1" },
	{ .insel = 0x03, .desc = "ADV7180 Composite on Ain2" },
	{ .insel = 0x04, .desc = "ADV7180 Composite on Ain3" },
	{ .insel = 0x06, .desc = "ADV7180 Y/C on Ain1/2" },
	{ .insel = 0x09, .desc = "ADV7180 YPbPr on Ain1/2/3" },
};

#define NUM_INPUTS_40_32 ARRAY_SIZE(adv7180_inputs_40_32)
#endif

struct adv7180_reg_tbl_t {
	u8 reg;
	u8 val;
};

/*
 * this is a special reg value inserted into a register table
 * indicating a delay is required in msec given in val
 */
#define ADV_DELAY  255

/*
 * This is the original register set from Freescale's adv7180
 * driver. Most of these registers and their values are undocumented,
 * and do not conform with Analog Device's Register Settings
 * Recommendations document for CVBS autodetect mode.
 * Later edit: This table has been stripped of the entries specifying
 * the same value as the register default value after reset (according
 * to ADV7180 datasheet rev J), in order to decrease the device
 * initialization time.
 */
static const struct adv7180_reg_tbl_t adv7180_fsl_init_reg[] = {
	{ 0x02, 0x04 }, { 0x03, 0x00 }, { 0x04, 0x45 }, { 0x05, 0x00 },
	{ 0x06, 0x02 }, { 0x13, 0x00 }, { 0x15, 0x00 }, { 0x16, 0x00 },
	{ 0xF1, 0x19 }, { 0x1A, 0x00 }, { 0x1B, 0x00 }, { 0x1C, 0x00 },
	{ 0x1D, 0x40 }, { 0x1E, 0x00 }, { 0x1F, 0x00 }, { 0x20, 0x00 },
	{ 0x21, 0x00 }, { 0x22, 0x00 }, { 0x23, 0xC0 }, { 0x24, 0x00 },
	{ 0x25, 0x00 }, { 0x26, 0x00 }, { 0x28, 0x00 }, { 0x29, 0x00 },
	{ 0x2A, 0x00 }, { 0x2D, 0xF4 }, { 0x2E, 0x00 }, { 0x2F, 0xF0 },
	{ 0x30, 0x00 }, { 0x31, 0x02 }, { 0x3B, 0x05 }, { 0x3C, 0x58 },
	{ 0x3E, 0x64 }, { 0x3F, 0xE4 }, { 0x40, 0x90 }, { 0x42, 0x7E },
	{ 0x43, 0xA4 }, { 0x44, 0xFF }, { 0x45, 0xB6 }, { 0x46, 0x12 },
	{ 0x4F, 0x08 }, { 0x51, 0xA4 }, { 0x53, 0x4E }, { 0x54, 0x80 },
	{ 0x55, 0x00 }, { 0x56, 0x10 }, { 0x57, 0x00 }, { 0x5A, 0x00 },
	{ 0x5B, 0x00 }, { 0x5C, 0x00 }, { 0x5D, 0x00 }, { 0x5E, 0x00 },
	{ 0x5F, 0x00 }, { 0x60, 0x00 }, { 0x61, 0x00 }, { 0x62, 0x20 },
	{ 0x63, 0x00 }, { 0x64, 0x00 }, { 0x65, 0x00 }, { 0x66, 0x00 },
	{ 0x67, 0x03 }, { 0x68, 0x01 }, { 0x69, 0x00 }, { 0x6A, 0x00 },
	{ 0x6B, 0xC0 }, { 0x6C, 0x00 }, { 0x6D, 0x00 }, { 0x6E, 0x00 },
	{ 0x6F, 0x00 }, { 0x70, 0x00 }, { 0x71, 0x00 }, { 0x72, 0x00 },
	{ 0x73, 0x10 }, { 0x74, 0x04 }, { 0x75, 0x01 }, { 0x76, 0x00 },
	{ 0x77, 0x3F }, { 0x78, 0xFF }, { 0x79, 0xFF }, { 0x7A, 0xFF },
	{ 0x7B, 0x1E }, { 0x7C, 0xC0 }, { 0x7D, 0x00 }, { 0x7E, 0x00 },
	{ 0x7F, 0x00 }, { 0x80, 0x00 }, { 0x81, 0xC0 }, { 0x82, 0x04 },
	{ 0x83, 0x00 }, { 0x84, 0x0C }, { 0x85, 0x02 }, { 0x86, 0x03 },
	{ 0x87, 0x63 }, { 0x88, 0x5A }, { 0x89, 0x08 }, { 0x8A, 0x10 },
	{ 0x8B, 0x00 }, { 0x8C, 0x40 }, { 0x8D, 0x00 }, { 0x8E, 0x40 },
	{ 0x90, 0x00 }, { 0x91, 0x50 }, { 0x92, 0x00 }, { 0x93, 0x00 },
	{ 0x94, 0x00 }, { 0x95, 0x00 }, { 0x96, 0x00 }, { 0x97, 0xF0 },
	{ 0x98, 0x00 }, { 0x99, 0x00 }, { 0x9A, 0x00 }, { 0x9B, 0x00 },
	{ 0x9C, 0x00 }, { 0x9D, 0x00 }, { 0x9E, 0x00 }, { 0x9F, 0x00 },
	{ 0xA0, 0x00 }, { 0xA1, 0x00 }, { 0xA2, 0x00 }, { 0xA3, 0x00 },
	{ 0xA4, 0x00 }, { 0xA5, 0x00 }, { 0xA6, 0x00 }, { 0xA7, 0x00 },
	{ 0xA8, 0x00 }, { 0xA9, 0x00 }, { 0xAA, 0x00 }, { 0xAB, 0x00 },
	{ 0xAC, 0x00 }, { 0xAD, 0x00 }, { 0xAE, 0x60 }, { 0xAF, 0x00 },
	{ 0xB0, 0x00 }, { 0xB1, 0x60 }, { 0xB3, 0x54 }, { 0xB4, 0x00 },
	{ 0xB5, 0x00 }, { 0xB6, 0x00 }, { 0xB7, 0x13 }, { 0xB8, 0x03 },
	{ 0xB9, 0x33 }, { 0xBF, 0x02 }, { 0xC0, 0x00 }, { 0xC1, 0x00 },
	{ 0xC2, 0x00 }, { 0xC4, 0x00 }, { 0xC5, 0x81 }, { 0xC6, 0x00 },
	{ 0xC7, 0x00 }, { 0xC8, 0x00 }, { 0xC9, 0x04 }, { 0xCC, 0x69 },
	{ 0xCD, 0x00 }, { 0xCE, 0x01 }, { 0xCF, 0xB4 }, { 0xD0, 0x00 },
	{ 0xD1, 0x10 }, { 0xD2, 0xFF }, { 0xD3, 0xFF }, { 0xD4, 0x7F },
	{ 0xD5, 0x7F }, { 0xD6, 0x3E }, { 0xD7, 0x08 }, { 0xD8, 0x3C },
	{ 0xD9, 0x08 }, { 0xDA, 0x3C }, { 0xDB, 0x9B }, { 0xDE, 0x00 },
	{ 0xDF, 0x00 }, { 0xE0, 0x14 }, { 0xEE, 0x00 }, { 0xEF, 0x4A },
	{ 0xF0, 0x44 }, { 0xF1, 0x0C }, { 0xF2, 0x32 }, { 0xF4, 0x3F },
	{ 0xF5, 0xE0 }, { 0xF6, 0x69 }, { 0xF7, 0x10 }, { 0xFA, 0xFA }
};

/*
 * This is Analog Device's Register Settings Recommendation for CVBS
 * autodetect mode. It is here for future reference only, we don't use
 * it yet because autodetect doesn't work very well when the chip is
 * initialized with this set.
 */
static const struct adv7180_reg_tbl_t __maybe_unused
adv7180_cvbs_autodetect[] = {
	/* Set analog mux for Composite Ain1 */
	{ ADV7180_INPUT_CTL, 0x00 },
	/* ADI Required Write: Reset Clamp Circuitry */
	{ ADV7180_ANALOG_CLAMP_CTL, 0x30 },
	/* Enable SFL Output */
	{ ADV7180_EXT_OUT_CTL, 0x57 },
	/* Select SH1 Chroma Shaping Filter */
	{ ADV7180_SHAP_FILTER_CTL_1, 0x41 },
	/* Enable NEWAVMODE */
	{ ADV7180_VSYNC_FIELD_CTL_1, 0x02 },
	/* ADI Required Write: optimize windowing function Step 1,2,3 */
	{ ADV7180_MANUAL_WIN_CTL_1, 0xA2 },
	{ ADV7180_MANUAL_WIN_CTL_2, 0x6A },
	{ ADV7180_MANUAL_WIN_CTL_3, 0xA0 },
	/* ADI Required Write: Enable ADC step 1,2,3 */
	{ ADV7180_ADI_CTL_1, 0x80 }, /* undocumented bit 7 */
	{ 0x55, 0x81 },              /* undocumented register 0x55 */
	{ ADV7180_ADI_CTL_1, 0x00 },
	/* Recommended AFE I BIAS Setting for CVBS mode */
	{ ADV7180_CVBS_TRIM, 0x0D },
};

#define ADV7180_VOLTAGE_ANALOG               1800000
#define ADV7180_VOLTAGE_DIGITAL_CORE         1800000
#define ADV7180_VOLTAGE_DIGITAL_IO           3300000
#define ADV7180_VOLTAGE_PLL                  1800000

static int adv7180_regulator_enable(struct adv7180_dev *sensor)
{
	struct device *dev = sensor->dev;
	int ret = 0;

	sensor->dvddio = devm_regulator_get(dev, "DOVDD");
	if (!IS_ERR(sensor->dvddio)) {
		regulator_set_voltage(sensor->dvddio,
				      ADV7180_VOLTAGE_DIGITAL_IO,
				      ADV7180_VOLTAGE_DIGITAL_IO);
		ret = regulator_enable(sensor->dvddio);
		if (ret) {
			v4l2_err(&sensor->sd, "set io voltage failed\n");
			return ret;
		}
	} else {
		v4l2_warn(&sensor->sd, "cannot get io voltage\n");
	}

	sensor->dvdd = devm_regulator_get(dev, "DVDD");
	if (!IS_ERR(sensor->dvdd)) {
		regulator_set_voltage(sensor->dvdd,
				      ADV7180_VOLTAGE_DIGITAL_CORE,
				      ADV7180_VOLTAGE_DIGITAL_CORE);
		ret = regulator_enable(sensor->dvdd);
		if (ret) {
			v4l2_err(&sensor->sd, "set core voltage failed\n");
			return ret;
		}
	} else {
		v4l2_warn(&sensor->sd, "cannot get core voltage\n");
	}

	sensor->avdd = devm_regulator_get(dev, "AVDD");
	if (!IS_ERR(sensor->avdd)) {
		regulator_set_voltage(sensor->avdd,
				      ADV7180_VOLTAGE_ANALOG,
				      ADV7180_VOLTAGE_ANALOG);
		ret = regulator_enable(sensor->avdd);
		if (ret) {
			v4l2_err(&sensor->sd, "set analog voltage failed\n");
			return ret;
		}
	} else {
		v4l2_warn(&sensor->sd, "cannot get analog voltage\n");
	}

	sensor->pvdd = devm_regulator_get(dev, "PVDD");
	if (!IS_ERR(sensor->pvdd)) {
		regulator_set_voltage(sensor->pvdd,
				      ADV7180_VOLTAGE_PLL,
				      ADV7180_VOLTAGE_PLL);
		ret = regulator_enable(sensor->pvdd);
		if (ret) {
			v4l2_err(&sensor->sd, "set pll voltage failed\n");
			return ret;
		}
	} else {
		v4l2_warn(&sensor->sd, "cannot get pll voltage\n");
	}

	return ret;
}

static void adv7180_regulator_disable(struct adv7180_dev *sensor)
{
	if (sensor->dvddio)
		regulator_disable(sensor->dvddio);

	if (sensor->dvdd)
		regulator_disable(sensor->dvdd);

	if (sensor->avdd)
		regulator_disable(sensor->avdd);

	if (sensor->pvdd)
		regulator_disable(sensor->pvdd);
}

/***********************************************************************
 * I2C transfer.
 ***********************************************************************/

/*! Read one register from a ADV7180 i2c slave device.
 *
 *  @param *reg		register in the device we wish to access.
 *
 *  @return		       0 if success, an error code otherwise.
 */
static int adv7180_read_reg(struct adv7180_dev *sensor, u8 reg, u8 *val)
{
	int ret;

	ret = i2c_smbus_read_byte_data(sensor->i2c_client, reg);
	if (ret < 0) {
		v4l2_err(&sensor->sd, "%s: read reg error: reg=%2x\n",
			 __func__, reg);
		return ret;
	}

	*val = ret;
	return 0;
}

/*! Write one register of a ADV7180 i2c slave device.
 *
 *  @param *reg		register in the device we wish to access.
 *
 *  @return		       0 if success, an error code otherwise.
 */
static int adv7180_write_reg(struct adv7180_dev *sensor, u8 reg, u8 val)
{
	int ret = i2c_smbus_write_byte_data(sensor->i2c_client, reg, val);

	if (ret < 0)
		v4l2_err(&sensor->sd, "%s: write reg error:reg=%2x,val=%2x\n",
			 __func__, reg, val);
	return ret;
}

#define ADV7180_READ_REG(s, r, v) {				\
		ret = adv7180_read_reg((s), (r), (v));		\
		if (ret)					\
			return ret;				\
	}
#define ADV7180_WRITE_REG(s, r, v) {				\
		ret = adv7180_write_reg((s), (r), (v));		\
		if (ret)					\
			return ret;				\
	}

static int adv7180_load_reg_tbl(struct adv7180_dev *sensor,
				const struct adv7180_reg_tbl_t *tbl, int n)
{
	int ret, i;

	for (i = 0; i < n; i++) {
		if (tbl[i].reg == ADV_DELAY) {
			unsigned long usec = (unsigned long)tbl[i].val * 1000;

			usleep_range(usec, usec + 1);
			continue;
		}

		ADV7180_WRITE_REG(sensor, tbl[i].reg, tbl[i].val);
	}

	return 0;
}

/* Read AD_RESULT to get the autodetected video standard */
static int adv7180_get_autodetect_std(struct adv7180_dev *sensor,
				      u8 stat1, bool *status_change)
{
	int ad_result, idx = ADV7180_PAL;
	v4l2_std_id std = V4L2_STD_PAL;

	*status_change = false;

	/*
	 * When the chip loses lock, it continues to send data at whatever
	 * standard was detected before, so leave the standard at the last
	 * detected standard.
	 */
	if (!sensor->locked)
		return 0; /* no status change */

	ad_result = (stat1 & ADV7180_AD_RESULT_MASK) >> ADV7180_AD_RESULT_BIT;

	switch (ad_result) {
	case ADV7180_AD_PAL:
		std = V4L2_STD_PAL;
		idx = ADV7180_PAL;
		break;
	case ADV7180_AD_PAL_M:
		std = V4L2_STD_PAL_M;
		/* PAL M is very similar to NTSC (same lines/field) */
		idx = ADV7180_NTSC;
		break;
	case ADV7180_AD_PAL_N:
		std = V4L2_STD_PAL_N;
		idx = ADV7180_PAL;
		break;
	case ADV7180_AD_PAL_60:
		std = V4L2_STD_PAL_60;
		/* PAL 60 has same lines as NTSC */
		idx = ADV7180_NTSC;
		break;
	case ADV7180_AD_NTSC:
		std = V4L2_STD_NTSC;
		idx = ADV7180_NTSC;
		break;
	case ADV7180_AD_NTSC_4_43:
		std = V4L2_STD_NTSC_443;
		idx = ADV7180_NTSC;
		break;
	case ADV7180_AD_SECAM:
		std = V4L2_STD_SECAM;
		idx = ADV7180_PAL;
		break;
	case ADV7180_AD_SECAM_525:
		/*
		 * FIXME: could not find any info on "SECAM 525", assume
		 * it is SECAM but with NTSC line standard.
		 */
		std = V4L2_STD_SECAM;
		idx = ADV7180_NTSC;
		break;
	}

	if (std != sensor->std_id) {
		sensor->video_idx = idx;
		sensor->std_id = std;
		sensor->streamcap.timeperframe =
			adv7180_std[sensor->video_idx].std.frameperiod;
		sensor->fmt.width = adv7180_std[sensor->video_idx].width;
		sensor->fmt.height = adv7180_std[sensor->video_idx].height;
		*status_change = true;
	}

	return 0;
}

/* Update lock status */
static int adv7180_update_lock_status(struct adv7180_dev *sensor,
				      u8 stat1, bool *status_change)
{
	u8 int_stat1, int_stat3, int_raw_stat3;
	u8 int_mask1, int_mask3;
	int ret;

	/* Switch to interrupt register map */
	ADV7180_WRITE_REG(sensor, ADV7180_ADI_CTL_1, 0x20);

	ADV7180_READ_REG(sensor, ADV7180_INT_MASK_1, &int_mask1);
	ADV7180_READ_REG(sensor, ADV7180_INT_MASK_3, &int_mask3);

	ADV7180_READ_REG(sensor, ADV7180_INT_STATUS_1, &int_stat1);
	ADV7180_READ_REG(sensor, ADV7180_INT_STATUS_3, &int_stat3);

	int_stat1 &= int_mask1;
	int_stat3 &= int_mask3;

	/* clear the interrupts */
	ADV7180_WRITE_REG(sensor, ADV7180_INT_CLEAR_1, int_stat1);
	ADV7180_WRITE_REG(sensor, ADV7180_INT_CLEAR_1, 0);
	ADV7180_WRITE_REG(sensor, ADV7180_INT_CLEAR_3, int_stat3);
	ADV7180_WRITE_REG(sensor, ADV7180_INT_CLEAR_3, 0);

	ADV7180_READ_REG(sensor, ADV7180_INT_RAW_STATUS_3, &int_raw_stat3);

	/* Switch back to normal register map */
	ADV7180_WRITE_REG(sensor, ADV7180_ADI_CTL_1, 0x00);

	*status_change = ((stat1 & ADV7180_LOST_LOCK) || int_stat1 ||
			  int_stat3);

	sensor->locked = ((stat1 & ADV7180_IN_LOCK) &&
			  (stat1 & ADV7180_FSC_LOCK) &&
			  (int_raw_stat3 & ADV7180_INT_SD_V_LOCK));

	return 0;
}

static void adv7180_power(struct adv7180_dev *sensor, bool enable)
{
	if (enable && !sensor->on) {
		if (gpio_is_valid(sensor->pwdn_gpio))
			gpio_set_value_cansleep(sensor->pwdn_gpio, 1);

		usleep_range(5000, 5001);
		adv7180_write_reg(sensor, ADV7180_PWR_MNG, 0);
	} else if (!enable && sensor->on) {
		adv7180_write_reg(sensor, ADV7180_PWR_MNG, 0x24);

		if (gpio_is_valid(sensor->pwdn_gpio))
			gpio_set_value_cansleep(sensor->pwdn_gpio, 0);
	}

	sensor->on = enable;
}

/* threaded irq handler */
static irqreturn_t adv7180_interrupt(int irq, void *dev_id)
{
	struct adv7180_dev *sensor = dev_id;
	bool std_change, lock_status_change;
	struct v4l2_event ev = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = 0,
	};
	u8 stat1;

	mutex_lock(&sensor->mutex);

	adv7180_read_reg(sensor, ADV7180_STATUS_1, &stat1);

	adv7180_update_lock_status(sensor, stat1, &lock_status_change);
	adv7180_get_autodetect_std(sensor, stat1, &std_change);

	mutex_unlock(&sensor->mutex);

	if (lock_status_change)
		ev.u.src_change.changes |= V4L2_EVENT_SRC_CH_LOCK_STATUS;
	if (std_change)
		ev.u.src_change.changes |= V4L2_EVENT_SRC_CH_RESOLUTION;

	if (ev.u.src_change.changes)
		v4l2_subdev_notify_event(&sensor->sd, &ev);

	return IRQ_HANDLED;
}

static const struct adv7180_inputs_t *
adv7180_find_input(struct adv7180_dev *sensor, u32 insel)
{
	int i;

	for (i = 0; i < NUM_INPUTS_64_48; i++) {
		if (insel == adv7180_inputs_64_48[i].insel)
			return &adv7180_inputs_64_48[i];
	}

	return NULL;
}

/* --------------- Subdev Operations --------------- */

static int adv7180_querystd(struct v4l2_subdev *sd, v4l2_std_id *std)
{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);
	bool std_change, lsc;
	int ret = 0;
	u8 stat1;

	mutex_lock(&sensor->mutex);

	/*
	 * If we have the ADV7180 irq, we can just return the currently
	 * detected standard. Otherwise we have to poll the AD_RESULT
	 * bits every time querystd() is called.
	 */
	if (!sensor->i2c_client->irq) {
		adv7180_read_reg(sensor, ADV7180_STATUS_1, &stat1);

		ret = adv7180_update_lock_status(sensor, stat1, &lsc);
		if (ret)
			goto unlock;
		ret = adv7180_get_autodetect_std(sensor, stat1, &std_change);
		if (ret)
			goto unlock;
	}

	*std = sensor->std_id;

unlock:
	mutex_unlock(&sensor->mutex);
	return ret;
}

static int adv7180_s_power(struct v4l2_subdev *sd, int on)
{
	return 0;
}

static int adv7180_g_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *a)
{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);
	struct v4l2_captureparm *cparm = &a->parm.capture;

	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	memset(a, 0, sizeof(*a));
	a->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	cparm->capability = sensor->streamcap.capability;
	cparm->timeperframe = sensor->streamcap.timeperframe;
	cparm->capturemode = sensor->streamcap.capturemode;

	return 0;
}

static int adv7180_s_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *a)
{
	return 0;
}

static int adv7180_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *format)
{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);
	struct v4l2_mbus_framefmt *mf = &format->format;

	if (format->pad)
		return -EINVAL;

	*mf = sensor->fmt;

	return 0;
}

/*
 * This driver autodetects a standard video mode, so we don't allow
 * setting a mode, just return the current autodetected mode.
 */
static int adv7180_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *format)
{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);
	struct v4l2_mbus_framefmt *mf = &format->format;

	if (format->pad)
		return -EINVAL;

	if (format->which == V4L2_SUBDEV_FORMAT_TRY)
		mf = &cfg->try_fmt;

	*mf = sensor->fmt;

	return 0;
}


/* Controls */

static int adv7180_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct adv7180_dev *sensor = ctrl_to_adv7180_dev(ctrl);
	int ret = 0;
	u8 tmp;

	switch (ctrl->id) {
	case V4L2_CID_BRIGHTNESS:
		tmp = ctrl->val;
		ADV7180_WRITE_REG(sensor, ADV7180_BRIGHTNESS, tmp);
		break;
	case V4L2_CID_CONTRAST:
		tmp = ctrl->val;
		ADV7180_WRITE_REG(sensor, ADV7180_CONTRAST, tmp);
		break;
	case V4L2_CID_SATURATION:
		tmp = ctrl->val;
		ADV7180_WRITE_REG(sensor, ADV7180_SD_SATURATION_CB, tmp);
		ADV7180_WRITE_REG(sensor, ADV7180_SD_SATURATION_CR, tmp);
		break;
	case V4L2_CID_HUE:
		tmp = ctrl->val;
		/* Hue is inverted according to HSL chart */
		ADV7180_WRITE_REG(sensor, ADV7180_HUE_REG, -tmp);
		break;
	case V4L2_CID_ADV718x_OFF_CR:
		tmp = ctrl->val + 128;
		ADV7180_WRITE_REG(sensor, ADV7180_SD_OFFSET_CR, tmp);
		break;
	case V4L2_CID_ADV718x_OFF_CB:
		tmp = ctrl->val + 128;
		ADV7180_WRITE_REG(sensor, ADV7180_SD_OFFSET_CB, tmp);
		break;
	case V4L2_CID_ADV718x_FREERUN_ENABLE:
		ADV7180_READ_REG(sensor, ADV7180_DEF_Y, &tmp);
		tmp &= ~ADV7180_DEF_VAL_AUTO_EN;
		if (ctrl->val)
			tmp |= ADV7180_DEF_VAL_AUTO_EN;
		ADV7180_WRITE_REG(sensor, ADV7180_DEF_Y, tmp);
		break;
	case V4L2_CID_ADV718x_FORCE_FREERUN:
		ADV7180_READ_REG(sensor, ADV7180_DEF_Y, &tmp);
		tmp &= ~ADV7180_DEF_VAL_EN;
		if (ctrl->val)
			tmp |= ADV7180_DEF_VAL_EN;
		ADV7180_WRITE_REG(sensor, ADV7180_DEF_Y, tmp);
		break;
	case V4L2_CID_ADV718x_FREERUN_Y:
		ADV7180_READ_REG(sensor, ADV7180_DEF_Y, &tmp);
		tmp &= ~ADV7180_DEF_Y_MASK;
		tmp |= (ctrl->val << ADV7180_DEF_Y_BIT);
		ADV7180_WRITE_REG(sensor, ADV7180_DEF_Y, tmp);
		break;
	case V4L2_CID_ADV718x_FREERUN_CB:
		ADV7180_READ_REG(sensor, ADV7180_DEF_C, &tmp);
		tmp &= ~ADV7180_DEF_CB_MASK;
		tmp |= (ctrl->val << ADV7180_DEF_CB_BIT);
		ADV7180_WRITE_REG(sensor, ADV7180_DEF_C, tmp);
		break;
	case V4L2_CID_ADV718x_FREERUN_CR:
		ADV7180_READ_REG(sensor, ADV7180_DEF_C, &tmp);
		tmp &= ~ADV7180_DEF_CR_MASK;
		tmp |= (ctrl->val << ADV7180_DEF_CR_BIT);
		ADV7180_WRITE_REG(sensor, ADV7180_DEF_C, tmp);
		break;
	case V4L2_CID_ADV718x_CTI_ENABLE:
		ADV7180_READ_REG(sensor, ADV7180_CTI_DNR, &tmp);
		tmp &= ~ADV7180_CTI_EN;
		if (ctrl->val)
			tmp |= ADV7180_CTI_EN;
		ADV7180_WRITE_REG(sensor, ADV7180_CTI_DNR, tmp);
		break;
	case V4L2_CID_ADV718x_CTI_AB_ENABLE:
		ADV7180_READ_REG(sensor, ADV7180_CTI_DNR, &tmp);
		tmp &= ~ADV7180_CTI_AB_EN;
		if (ctrl->val)
			tmp |= ADV7180_CTI_AB_EN;
		ADV7180_WRITE_REG(sensor, ADV7180_CTI_DNR, tmp);
		break;
	case V4L2_CID_ADV718x_CTI_AB:
		ADV7180_READ_REG(sensor, ADV7180_CTI_DNR, &tmp);
		tmp &= ~ADV7180_CTI_AB_MASK;
		tmp |= (ctrl->val << ADV7180_CTI_AB_BIT);
		ADV7180_WRITE_REG(sensor, ADV7180_CTI_DNR, tmp);
		break;
	case V4L2_CID_ADV718x_CTI_THRESH:
		tmp = ctrl->val;
		ADV7180_WRITE_REG(sensor, ADV7180_CTI_THRESH, tmp);
		break;
	case V4L2_CID_ADV718x_DNR_ENABLE:
		ADV7180_READ_REG(sensor, ADV7180_CTI_DNR, &tmp);
		tmp &= ~ADV7180_DNR_EN;
		if (ctrl->val)
			tmp |= ADV7180_DNR_EN;
		ADV7180_WRITE_REG(sensor, ADV7180_CTI_DNR, tmp);
		break;
	case V4L2_CID_ADV718x_DNR_THRESH1:
		tmp = ctrl->val;
		ADV7180_WRITE_REG(sensor, ADV7180_DNR_THRESH1, tmp);
		break;
	case V4L2_CID_ADV718x_LUMA_PEAK_GAIN:
		tmp = ctrl->val;
		ADV7180_WRITE_REG(sensor, ADV7180_LUMA_PEAK_GAIN, tmp);
		break;
	case V4L2_CID_ADV718x_DNR_THRESH2:
		tmp = ctrl->val;
		ADV7180_WRITE_REG(sensor, ADV7180_DNR_THRESH2, tmp);
		break;
	default:
		ret = -EPERM;
		break;
	}

	return ret;
}

static const struct v4l2_ctrl_ops adv7180_ctrl_ops = {
	.s_ctrl = adv7180_s_ctrl,
};

/* supported standard controls */
static const struct v4l2_ctrl_config adv7180_std_ctrl[] = {
	{
		.id = V4L2_CID_BRIGHTNESS,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Brightness",
		.def =   0,
		.min =   0,
		.max = 255,
		.step =  1,
	}, {
		.id = V4L2_CID_SATURATION,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Saturation",
		.def = 128,
		.min =   0,
		.max = 255,
		.step =  1,
	}, {
		.id = V4L2_CID_CONTRAST,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Contrast",
		.def = 128,
		.min =   0,
		.max = 255,
		.step =  1,
	}, {
		.id = V4L2_CID_HUE,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Hue",
		.def =    0,
		.min = -127,
		.max =  128,
		.step =   1,
	},
};

#define ADV7180_NUM_STD_CONTROLS ARRAY_SIZE(adv7180_std_ctrl)

/* supported custom controls */
static const struct v4l2_ctrl_config adv7180_custom_ctrl[] = {
	{
		.ops = &adv7180_ctrl_ops,
		.id = V4L2_CID_ADV718x_OFF_CR,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Offset Cr",
		.def =    0, /*    0 mV offset applied to the Cr channel */
		.min = -128, /* −312 mV offset applied to the Cr channel */
		.max =  127, /* +312 mV offset applied to the Cr channel */
		.step =   1,
	}, {
		.ops = &adv7180_ctrl_ops,
		.id = V4L2_CID_ADV718x_OFF_CB,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Offset Cb",
		.def =    0, /*    0 mV offset applied to the Cb channel */
		.min = -128, /* −312 mV offset applied to the Cb channel */
		.max =  127, /* +312 mV offset applied to the Cb channel */
		.step =   1,
	}, {
		.ops = &adv7180_ctrl_ops,
		.id = V4L2_CID_ADV718x_FREERUN_ENABLE,
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.name = "Freerun Enable",
		.def =  1,
		.min =  0,
		.max =  1,
		.step = 1,
	}, {
		.ops = &adv7180_ctrl_ops,
		.id = V4L2_CID_ADV718x_FORCE_FREERUN,
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.name = "Force Freerun",
		.def =  0,
		.min =  0,
		.max =  1,
		.step = 1,
	}, {
		.ops = &adv7180_ctrl_ops,
		.id = V4L2_CID_ADV718x_FREERUN_Y,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Freerun Y",
		.def =  13,
		.min =   0,
		.max =  63,
		.step =  1,
	}, {
		.ops = &adv7180_ctrl_ops,
		.id = V4L2_CID_ADV718x_FREERUN_CB,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Freerun Cb",
		.def =  12,
		.min =   0,
		.max =  15,
		.step =  1,
	}, {
		.ops = &adv7180_ctrl_ops,
		.id = V4L2_CID_ADV718x_FREERUN_CR,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Freerun Cr",
		.def =   7,
		.min =   0,
		.max =  15,
		.step =  1,
	}, {
		.ops = &adv7180_ctrl_ops,
		.id = V4L2_CID_ADV718x_CTI_ENABLE,
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.name = "CTI Enable",
		.def =  0,
		.min =  0,
		.max =  1,
		.step = 1,
	}, {
		.ops = &adv7180_ctrl_ops,
		.id = V4L2_CID_ADV718x_CTI_AB_ENABLE,
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.name = "CTI AB Enable",
		.def =  0,
		.min =  0,
		.max =  1,
		.step = 1,
	}, {
		.ops = &adv7180_ctrl_ops,
		.id = V4L2_CID_ADV718x_CTI_AB,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "CTI AB Sharpness",
		.def =  3,
		.min =  0,
		.max =  3,
		.step = 1,
	}, {
		.ops = &adv7180_ctrl_ops,
		.id = V4L2_CID_ADV718x_CTI_THRESH,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "CTI Threshold",
		.def =   8,
		.min =   0,
		.max = 255,
		.step =  1,
	}, {
		.ops = &adv7180_ctrl_ops,
		.id = V4L2_CID_ADV718x_DNR_ENABLE,
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.name = "DNR Enable",
		.def =  1,
		.min =  0,
		.max =  1,
		.step = 1,
	}, {
		.ops = &adv7180_ctrl_ops,
		.id = V4L2_CID_ADV718x_DNR_THRESH1,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "DNR1 Threshold",
		.def =   8,
		.min =   0,
		.max = 255,
		.step =  1,
	}, {
		.ops = &adv7180_ctrl_ops,
		.id = V4L2_CID_ADV718x_LUMA_PEAK_GAIN,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Luma Peak Gain",
		.def =  64,
		.min =   0,
		.max = 255,
		.step =  1,
	}, {
		.ops = &adv7180_ctrl_ops,
		.id = V4L2_CID_ADV718x_DNR_THRESH2,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "DNR2 Threshold",
		.def =   4,
		.min =   0,
		.max = 255,
		.step =  1,
	},
};

#define ADV7180_NUM_CUSTOM_CONTROLS ARRAY_SIZE(adv7180_custom_ctrl)

#define ADV7180_NUM_CONTROLS \
	(ADV7180_NUM_STD_CONTROLS + ADV7180_NUM_CUSTOM_CONTROLS)

static int adv7180_init_controls(struct adv7180_dev *sensor)
{
	const struct v4l2_ctrl_config *c;
	int i, err;

	v4l2_ctrl_handler_init(&sensor->ctrl_hdl, ADV7180_NUM_CONTROLS);

	for (i = 0; i < ADV7180_NUM_STD_CONTROLS; i++) {
		c = &adv7180_std_ctrl[i];

		v4l2_ctrl_new_std(&sensor->ctrl_hdl, &adv7180_ctrl_ops,
				  c->id, c->min, c->max, c->step, c->def);
	}

	for (i = 0; i < ADV7180_NUM_CUSTOM_CONTROLS; i++) {
		c = &adv7180_custom_ctrl[i];

		v4l2_ctrl_new_custom(&sensor->ctrl_hdl, c, NULL);
	}

	sensor->sd.ctrl_handler = &sensor->ctrl_hdl;
	if (sensor->ctrl_hdl.error) {
		err = sensor->ctrl_hdl.error;
		goto error;
	}

	err = v4l2_ctrl_handler_setup(&sensor->ctrl_hdl);
	if (err)
		goto error;

	return 0;

 error:
	v4l2_ctrl_handler_free(&sensor->ctrl_hdl);
	v4l2_err(&sensor->sd, "%s: error %d\n", __func__, err);

	return err;
}

static int adv7180_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);

	if (fse->pad)
		return -EINVAL;
	if (fse->index || fse->code != sensor->fmt.code)
		return -EINVAL;

	fse->min_width = adv7180_std[sensor->video_idx].width;
	fse->max_width = fse->min_width;
	fse->min_height = adv7180_std[sensor->video_idx].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int adv7180_g_input_status(struct v4l2_subdev *sd, u32 *status)
{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);

	mutex_lock(&sensor->mutex);

	*status = 0;

	if (sensor->on) {
		if (!sensor->locked)
			*status = V4L2_IN_ST_NO_SIGNAL | V4L2_IN_ST_NO_SYNC;
	} else {
		*status = V4L2_IN_ST_NO_POWER;
	}

	mutex_unlock(&sensor->mutex);

	return 0;
}

static int adv7180_s_routing(struct v4l2_subdev *sd, u32 input,
			     u32 output, u32 config)
{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);
	const struct adv7180_inputs_t *advinput;
	struct v4l2_event ev = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
	};

	advinput = adv7180_find_input(sensor, input);
	if (!advinput)
		return -EINVAL;

	mutex_lock(&sensor->mutex);

	if (input == sensor->current_input)
		goto out;

	adv7180_write_reg(sensor, ADV7180_INPUT_CTL, advinput->insel);

	/* ADI Required Write: Reset Clamp Circuitry */
	adv7180_write_reg(sensor, ADV7180_ANALOG_CLAMP_CTL, 0x30);

	sensor->current_input = input;

	v4l2_subdev_notify_event(&sensor->sd, &ev);
out:
	mutex_unlock(&sensor->mutex);
	return 0;
}

static int adv7180_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);

	if (code->pad)
		return -EINVAL;
	if (code->index != 0)
		return -EINVAL;

	code->code = sensor->fmt.code;

	return 0;
}

static int adv7180_g_mbus_config(struct v4l2_subdev *sd,
				 struct v4l2_mbus_config *cfg)
{
	struct adv7180_dev *sensor = to_adv7180_dev(sd);

	cfg->type = V4L2_MBUS_BT656;
	cfg->flags = sensor->ep.bus.parallel.flags;

	return 0;
}

static int adv7180_s_stream(struct v4l2_subdev *sd, int enable)
{
	return 0;
}

static int adv7180_subscribe_event(struct v4l2_subdev *sd,
				   struct v4l2_fh *fh,
				   struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_src_change_event_subdev_subscribe(sd, fh, sub);
	case V4L2_EVENT_CTRL:
		return v4l2_ctrl_subdev_subscribe_event(sd, fh, sub);
	default:
		return -EINVAL;
	}
}

static struct v4l2_subdev_core_ops adv7180_core_ops = {
	.subscribe_event = adv7180_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
	.s_power = adv7180_s_power,
	.g_ext_ctrls = v4l2_subdev_g_ext_ctrls,
	.try_ext_ctrls = v4l2_subdev_try_ext_ctrls,
	.s_ext_ctrls = v4l2_subdev_s_ext_ctrls,
	.g_ctrl = v4l2_subdev_g_ctrl,
	.s_ctrl = v4l2_subdev_s_ctrl,
	.queryctrl = v4l2_subdev_queryctrl,
	.querymenu = v4l2_subdev_querymenu,
};

static struct v4l2_subdev_video_ops adv7180_video_ops = {
	.s_parm = adv7180_s_parm,
	.g_parm = adv7180_g_parm,
	.g_input_status = adv7180_g_input_status,
	.s_routing = adv7180_s_routing,
	.querystd = adv7180_querystd,
	.g_mbus_config  = adv7180_g_mbus_config,
	.s_stream = adv7180_s_stream,
};

static struct v4l2_subdev_pad_ops adv7180_pad_ops = {
	.enum_mbus_code = adv7180_enum_mbus_code,
	.get_fmt = adv7180_get_fmt,
	.set_fmt = adv7180_set_fmt,
	.enum_frame_size = adv7180_enum_frame_size,
};

static struct v4l2_subdev_ops adv7180_subdev_ops = {
	.core = &adv7180_core_ops,
	.video = &adv7180_video_ops,
	.pad = &adv7180_pad_ops,
};

/***********************************************************************
 * I2C client and driver.
 ***********************************************************************/

/*! ADV7180 Reset function.
 *
 *  @return		None.
 */
static int adv7180_hard_reset(struct adv7180_dev *sensor)
{
	/* assert reset bit */
	adv7180_write_reg(sensor, ADV7180_PWR_MNG, 0x80);
	usleep_range(5000, 5001);

	return adv7180_load_reg_tbl(sensor, adv7180_fsl_init_reg,
				    ARRAY_SIZE(adv7180_fsl_init_reg));
}

/*
 * Enable the SD_UNLOCK and SD_AD_CHNG interrupts.
 */
static int adv7180_enable_interrupts(struct adv7180_dev *sensor)
{
	int ret;

	/* Switch to interrupt register map */
	ADV7180_WRITE_REG(sensor, ADV7180_ADI_CTL_1, 0x20);
	/* INTRQ active low, active until cleared */
	ADV7180_WRITE_REG(sensor, ADV7180_INT_CONFIG_1, 0xd5);
	/* unmask SD_UNLOCK and SD_LOCK */
	ADV7180_WRITE_REG(sensor, ADV7180_INT_MASK_1,
			  ADV7180_INT_SD_UNLOCK | ADV7180_INT_SD_LOCK);
	/* unmask SD_AD_CHNG and SD_V_LOCK_CHNG */
	ADV7180_WRITE_REG(sensor, ADV7180_INT_MASK_3,
			  ADV7180_INT_SD_AD_CHNG | ADV7180_INT_SD_V_LOCK_CHNG);
	/* Switch back to normal register map */
	ADV7180_WRITE_REG(sensor, ADV7180_ADI_CTL_1, 0x00);

	return 0;
}

/*!
 * ADV7180 I2C probe function.
 * Function set in i2c_driver struct.
 * Called by insmod.
 *
 *  @param *adapter	I2C adapter descriptor.
 *
 *  @return		Error code indicating success or failure.
 */
static int adv7180_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct device_node *endpoint;
	struct adv7180_dev *sensor;
	struct device_node *np;
	const char *norm = "pal";
	bool std_change, lsc;
	u8 stat1, rev_id;
	int ret = 0;

	sensor = devm_kzalloc(&client->dev, sizeof(struct adv7180_dev),
			      GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->dev = &client->dev;
	np = sensor->dev->of_node;

	ret = of_property_read_string(np, "default-std", &norm);
	if (ret < 0 && ret != -EINVAL) {
		dev_err(sensor->dev, "error reading default-std property!\n");
		return ret;
	}
	if (!strcasecmp(norm, "pal")) {
		sensor->std_id = V4L2_STD_PAL;
		sensor->video_idx = ADV7180_PAL;
		dev_info(sensor->dev, "defaulting to PAL!\n");
	} else if (!strcasecmp(norm, "ntsc")) {
		sensor->std_id = V4L2_STD_NTSC;
		sensor->video_idx = ADV7180_NTSC;
		dev_info(sensor->dev, "defaulting to NTSC!\n");
	} else {
		dev_err(sensor->dev, "invalid default-std value: '%s'!\n",
			norm);
		return -EINVAL;
	}

	/* Set initial values for the sensor struct. */
	sensor->i2c_client = client;
	sensor->streamcap.timeperframe =
		adv7180_std[sensor->video_idx].std.frameperiod;
	sensor->fmt.width = adv7180_std[sensor->video_idx].width;
	sensor->fmt.height = adv7180_std[sensor->video_idx].height;
	sensor->fmt.code = MEDIA_BUS_FMT_UYVY8_2X8;
	sensor->fmt.field = V4L2_FIELD_SEQ_BT;

	mutex_init(&sensor->mutex);

	endpoint = of_graph_get_next_endpoint(np, NULL);
	if (!endpoint) {
		dev_err(sensor->dev, "endpoint node not found\n");
		return -EINVAL;
	}

	v4l2_of_parse_endpoint(endpoint, &sensor->ep);
	if (sensor->ep.bus_type != V4L2_MBUS_BT656) {
		dev_err(sensor->dev, "invalid bus type, must be bt.656\n");
		return -EINVAL;
	}
	of_node_put(endpoint);

	ret = of_get_named_gpio(np, "pwdn-gpio", 0);
	if (gpio_is_valid(ret)) {
		sensor->pwdn_gpio = ret;
		ret = devm_gpio_request_one(sensor->dev,
					    sensor->pwdn_gpio,
					    GPIOF_OUT_INIT_HIGH,
					    "adv7180_pwdn");
		if (ret < 0) {
			dev_err(sensor->dev,
				"request for power down gpio failed\n");
			return ret;
		}
	} else {
		if (ret == -EPROBE_DEFER)
			return ret;
		/* assume a power-down gpio is not required */
		sensor->pwdn_gpio = -1;
	}

	adv7180_regulator_enable(sensor);

	/* Power on the chip */
	adv7180_power(sensor, true);

	/*! ADV7180 initialization. */
	ret = adv7180_hard_reset(sensor);
	if (ret) {
		dev_err(sensor->dev, "hard reset failed!\n");
		goto cleanup;
	}

	/*! Read the revision ID of the chip */
	ret = adv7180_read_reg(sensor, ADV7180_IDENT, &rev_id);
	if (ret < 0) {
		dev_err(sensor->dev,
			"failed to read ADV7180 IDENT register!\n");
		ret = -ENODEV;
		goto cleanup;
	}
	sensor->rev_id = rev_id;

	dev_info(sensor->dev, "Analog Devices ADV7180 Rev 0x%02X detected!\n",
		 sensor->rev_id);

	v4l2_i2c_subdev_init(&sensor->sd, client, &adv7180_subdev_ops);

	/* see if there is a signal lock already */
	adv7180_read_reg(sensor, ADV7180_STATUS_1, &stat1);

	ret = adv7180_update_lock_status(sensor, stat1, &lsc);
	if (ret < 0)
		goto cleanup;
	ret = adv7180_get_autodetect_std(sensor, stat1, &std_change);
	if (ret < 0)
		goto cleanup;

	if (sensor->i2c_client->irq) {
		ret = request_threaded_irq(sensor->i2c_client->irq,
					   NULL, adv7180_interrupt,
					   IRQF_TRIGGER_LOW | IRQF_ONESHOT,
					   IF_NAME, sensor);
		if (ret < 0) {
			dev_err(sensor->dev, "Failed to register irq %d\n",
				sensor->i2c_client->irq);
			goto cleanup;
		}

		adv7180_enable_interrupts(sensor);

		dev_info(sensor->dev, "Registered irq %d\n",
			 sensor->i2c_client->irq);
	}

	ret = adv7180_init_controls(sensor);
	if (ret)
		goto irqfree;

	ret = v4l2_async_register_subdev(&sensor->sd);
	if (ret)
		goto free_ctrls;

	return 0;

free_ctrls:
	v4l2_ctrl_handler_free(&sensor->ctrl_hdl);
irqfree:
	if (sensor->i2c_client->irq)
		free_irq(sensor->i2c_client->irq, sensor);
cleanup:
	adv7180_power(sensor, false);
	adv7180_regulator_disable(sensor);
	return ret;
}

/*!
 * ADV7180 I2C detach function.
 * Called on rmmod.
 *
 *  @param *client	struct i2c_client*.
 *
 *  @return		Error code indicating success or failure.
 */
static int adv7180_detach(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct adv7180_dev *sensor = to_adv7180_dev(sd);

	if (sensor->i2c_client->irq)
		free_irq(sensor->i2c_client->irq, sensor);

	v4l2_async_unregister_subdev(&sensor->sd);
	v4l2_device_unregister_subdev(sd);
	v4l2_ctrl_handler_free(&sensor->ctrl_hdl);

	/* Power off the chip */
	adv7180_power(sensor, false);

	adv7180_regulator_disable(sensor);

	return 0;
}

static const struct i2c_device_id adv7180_id[] = {
	{ "adv7180", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, adv7180_id);

static const struct of_device_id adv7180_dt_ids[] = {
	{ .compatible = "adi,adv7180" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, adv7180_dt_ids);

static struct i2c_driver adv7180_driver = {
	.driver = {
		.name	= "adv7180",
		.owner	= THIS_MODULE,
		.of_match_table	= adv7180_dt_ids,
	},
	.id_table	= adv7180_id,
	.probe		= adv7180_probe,
	.remove		= adv7180_detach,
};

module_i2c_driver(adv7180_driver);

MODULE_AUTHOR("Freescale Semiconductor, Inc.");
MODULE_DESCRIPTION("Analog Devices ADV7180 Subdev driver");
MODULE_LICENSE("GPL");
