/*
 * GSXXXX device registration. Tested with GS1662 device.
 *
 * Copyright (C) 2015-2016 Nexvision
 * Author: Charles-Antoine Couret <charles-antoine.couret@nexvision.fr>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/platform_device.h>
#include <linux/ctype.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/videodev2.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-dv-timings.h>
#include <linux/v4l2-dv-timings.h>


#define REG_STATUS			0x04
#define REG_FORCE_FMT			0x06
#define REG_LINES_PER_FRAME		0x12
#define REG_WORDS_PER_LINE		0x13
#define REG_WORDS_PER_ACT_LINE		0x14
#define REG_ACT_LINES_PER_FRAME	0x15

#define MASK_H_LOCK		0x001
#define MASK_V_LOCK		0x002
#define MASK_STD_LOCK		0x004
#define MASK_FORCE_STD		0x020
#define MASK_STD_STATUS	0x3E0
#define ADDRESS_MASK		0x0FFF

#define GS_WIDTH_MIN		0
#define GS_WIDTH_MAX		2048
#define GS_HEIGHT_MIN		0
#define GS_HEIGHT_MAX		1080
#define GS_PIXELCLOCK_MIN	10519200
#define GS_PIXELCLOCK_MAX	74250000

#define READ_FLAG	0x8000
#define WRITE_FLAG	0x0000
#define BURST_FLAG	0x1000

struct gsxxxx {
	struct spi_device *pdev;
	struct v4l2_subdev sd;
	struct v4l2_dv_timings current_timings;
};

struct gsxxxx_reg_fmt {
	u16 reg_value;
	struct v4l2_dv_timings format;
};

struct gsxxxx_reg_fmt_custom {
	u16 reg_value;
	__u32 width;
	__u32 height;
	__u64 pixelclock;
	__u32 interlaced;
};

static const struct spi_device_id gsxxxx_id[] = {
	{ "gs1662", 0 },
	{ }
};
MODULE_DEVICE_TABLE(pdev, gsxxxx_id);

static const struct gsxxxx_reg_fmt reg_fmt[] = {
	{ 0x00, V4L2_DV_BT_CEA_1280X720P60 },
	{ 0x01, V4L2_DV_BT_CEA_1280X720P60 },
	{ 0x02, V4L2_DV_BT_CEA_1280X720P30 },
	{ 0x03, V4L2_DV_BT_CEA_1280X720P30 },
	{ 0x04, V4L2_DV_BT_CEA_1280X720P50 },
	{ 0x05, V4L2_DV_BT_CEA_1280X720P50 },
	{ 0x06, V4L2_DV_BT_CEA_1280X720P25 },
	{ 0x07, V4L2_DV_BT_CEA_1280X720P25 },
	{ 0x08, V4L2_DV_BT_CEA_1280X720P24 },
	{ 0x09, V4L2_DV_BT_CEA_1280X720P24 },
	{ 0x0A, V4L2_DV_BT_CEA_1920X1080I60 },
	{ 0x0B, V4L2_DV_BT_CEA_1920X1080P30 },

	/* Default value: keep this field before 0xC */
	{ 0x14, V4L2_DV_BT_CEA_1920X1080I50 },
	{ 0x0C, V4L2_DV_BT_CEA_1920X1080I50 },
	{ 0x0D, V4L2_DV_BT_CEA_1920X1080P25 },
	{ 0x0E, V4L2_DV_BT_CEA_1920X1080P25},
	{ 0x10, V4L2_DV_BT_CEA_1920X1080P24 },
	{ 0x12, V4L2_DV_BT_CEA_1920X1080P24 },
	{ 0x18, V4L2_DV_BT_CEA_720X576P50 },
	{ 0x1A, V4L2_DV_BT_CEA_720X576P50 },
};

/* Non CEA standard values supported by this component family.
 * It does not care about *porch information.
 */
static const struct gsxxxx_reg_fmt_custom reg_fmt_custom[] = {
	/* NULL value which means auto-detection mode or error */
	{ 0xFF, 0, 0, 0 },
	{ 0x0F, 1920, 1080, 25920000, 1 },
	{ 0x11, 1920, 1080, 24883200, 1 },
	{ 0x13, 1920, 1080, 24883200, 1 },
	{ 0x15, 1920, 1035, 59616000, 1 },
	{ 0x16, 720, 487, 10519200, 1 },
	{ 0x17, 720, 507, 10951200, 1 },
	{ 0x19, 720, 487, 10519200, 1 },
	{ 0x1B, 720, 507, 10951200, 1 },
	{ 0x1C, 2048, 1080, 55296000, 0 },
};

static const struct v4l2_dv_timings_cap gsxxxx_timings_cap = {
	.type = V4L2_DV_BT_656_1120,
	/* keep this initialization for compatibility with GCC < 4.4.6 */
	.reserved = { 0 },
	V4L2_INIT_BT_TIMINGS(GS_WIDTH_MIN, GS_WIDTH_MAX, GS_HEIGHT_MIN,
			     GS_HEIGHT_MAX, GS_PIXELCLOCK_MIN,
			     GS_PIXELCLOCK_MAX, V4L2_DV_BT_STD_CEA861,
			     V4L2_DV_BT_CAP_PROGRESSIVE
			     | V4L2_DV_BT_CAP_INTERLACED
			     | V4L2_DV_BT_CAP_CUSTOM)
};

static int gsxxxx_read_register(struct spi_device *spi, u16 addr, u16 *value)
{
	int ret;
	u16 buf_addr = (READ_FLAG | (ADDRESS_MASK & addr));
	u16 buf_value = 0;
	struct spi_message msg;
	struct spi_transfer tx[] = {
		{
			.tx_buf = &buf_addr,
			.len = 2,
			.delay_usecs = 1,
		}, {
			.rx_buf = &buf_value,
			.len = 2,
			.delay_usecs = 1,
		},
	};

	spi_message_init(&msg);
	spi_message_add_tail(&tx[0], &msg);
	spi_message_add_tail(&tx[1], &msg);
	ret = spi_sync(spi, &msg);

	*value = buf_value;

	return ret;
}

static int gsxxxx_write_register(struct spi_device *spi, u16 addr, u16 value)
{
	int ret;
	u16 buf_addr = (WRITE_FLAG | (ADDRESS_MASK & addr));
	u16 buf_value = value;
	struct spi_message msg;
	struct spi_transfer tx[] = {
		{
			.tx_buf = &buf_addr,
			.len = 2,
			.delay_usecs = 1,
		}, {
			.tx_buf = &buf_value,
			.len = 2,
			.delay_usecs = 1,
		},
	};

	spi_message_init(&msg);
	spi_message_add_tail(&tx[0], &msg);
	spi_message_add_tail(&tx[1], &msg);
	ret = spi_sync(spi, &msg);

	return ret;
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int gsxxxx_g_register(struct v4l2_subdev *sd,
			     struct v4l2_dbg_register *reg)
{
	struct spi_device *spi = v4l2_get_subdevdata(sd);
	u16 val;
	int ret;

	ret = gsxxxx_read_register(spi, reg->reg & 0xFFFF, &val);
	reg->val = val;
	reg->size = 2;
	return ret;
}

static int gsxxxx_s_register(struct v4l2_subdev *sd,
			     struct v4l2_dbg_register *reg)
{
	struct spi_device *spi = v4l2_get_subdevdata(sd);

	return gsxxxx_write_register(spi, reg->reg & 0xFFFF,
				     reg->val & 0xFFFF);
}
#endif

static void custom_to_timings(const struct gsxxxx_reg_fmt_custom *custom,
			      struct v4l2_dv_timings *timings)
{
	timings->type = V4L2_DV_BT_656_1120;
	timings->bt.width = custom->width;
	timings->bt.height = custom->height;
	timings->bt.pixelclock = custom->pixelclock;
	timings->bt.interlaced = custom->interlaced;
	timings->bt.polarities = 0;
	timings->bt.hbackporch = 0;
	timings->bt.hsync = 0;
	timings->bt.hfrontporch = 0;
	timings->bt.vbackporch = 0;
	timings->bt.vsync = 0;
	timings->bt.vfrontporch = 0;
	timings->bt.il_vbackporch = 0;
	timings->bt.il_vsync = 0;
	timings->bt.il_vfrontporch = 0;
	timings->bt.standards = 0;
	timings->bt.flags = 0;
}

static int find_custom(struct v4l2_dv_timings *timings)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(reg_fmt_custom); i++) {
		if (timings->bt.width == reg_fmt_custom[i].width
		    && timings->bt.height == reg_fmt_custom[i].height
		    && timings->bt.interlaced == reg_fmt_custom[i].interlaced
		    && timings->bt.pixelclock == reg_fmt_custom[i].pixelclock)
			return i;
	}

	return -EINVAL;
}

static int gsxxxx_status_format(u16 status, struct v4l2_dv_timings *timings)
{
	int std = (status & MASK_STD_STATUS) >> 5;
	int i;

	/* We parse CEA formats first */
	for (i = 0; i < ARRAY_SIZE(reg_fmt); i++) {
		if (reg_fmt[i].reg_value == std) {
			*timings = reg_fmt[i].format;
			return 0;
		}
	}

	/* Then custom formats */
	for (i = 0; i < ARRAY_SIZE(reg_fmt_custom); i++) {
		if (reg_fmt_custom[i].reg_value == std) {
			custom_to_timings(&reg_fmt_custom[i], timings);
			return 0;
		}
	}

	return -ERANGE;
}

static u16 get_register_timings(struct v4l2_dv_timings *timings)
{
	int i, ret;

	/* We parse CEA formats first */
	for (i = 0; i < ARRAY_SIZE(reg_fmt); i++) {
		if (v4l2_match_dv_timings(timings, &reg_fmt[i].format,
					  0, false))
			return reg_fmt[i].reg_value | MASK_FORCE_STD;
	}

	/* Then custom formats */
	ret = find_custom(timings);
	if (ret >= 0)
		return reg_fmt_custom[ret].reg_value | MASK_FORCE_STD;

	return 0x0;
}

static inline struct gsxxxx *to_gsxxxx(struct v4l2_subdev *sd)
{
	return container_of(sd, struct gsxxxx, sd);
}

static int gsxxxx_reset(struct v4l2_subdev *sd, u32 val)
{
	struct gsxxxx *gs = to_gsxxxx(sd);

	/* To renable auto-detection mode */
	return gsxxxx_write_register(gs->pdev, REG_FORCE_FMT, 0x0);
}

static int gsxxxx_s_dv_timings(struct v4l2_subdev *sd,
			       struct v4l2_dv_timings *timings)
{
	struct gsxxxx *gs = to_gsxxxx(sd);
	int reg_value;

	reg_value = get_register_timings(timings);
	if (reg_value == 0x0)
		return -EINVAL;

	gs->current_timings = *timings;
	return gsxxxx_write_register(gs->pdev, REG_FORCE_FMT, reg_value);
}

static int gsxxxx_g_dv_timings(struct v4l2_subdev *sd,
			       struct v4l2_dv_timings *timings)
{
	struct gsxxxx *gs = to_gsxxxx(sd);

	*timings = gs->current_timings;
	return 0;
}

static int gsxxxx_query_dv_timings(struct v4l2_subdev *sd,
				   struct v4l2_dv_timings *timings)
{
	struct gsxxxx *gs = to_gsxxxx(sd);
	struct v4l2_dv_timings fmt;
	u16 reg_value, i;
	int ret;

	/* If the device detects video (pixels, lines or fields) */
	for (i = 0; i < 4; i++) {
		gsxxxx_read_register(gs->pdev, REG_LINES_PER_FRAME + i,
				     &reg_value);
		if (reg_value)
			break;
	}

	if (i >= 4)
		return -ENOLINK;

	/* Video locked */
	gsxxxx_read_register(gs->pdev, REG_STATUS, &reg_value);
	if (!(reg_value & MASK_H_LOCK) || !(reg_value & MASK_V_LOCK))
		return -ENOLCK;
	if (!(reg_value & MASK_STD_LOCK))
		return -ERANGE;

	/* Video format */
	ret = gsxxxx_status_format(reg_value, &fmt);

	if (ret < 0)
		return ret;

	*timings = fmt;
	return 0;
}

static int gsxxxx_enum_dv_timings(struct v4l2_subdev *sd,
				  struct v4l2_enum_dv_timings *timings)
{
	if (timings->index >= ARRAY_SIZE(reg_fmt))
		return -EINVAL;

	timings->timings = reg_fmt[timings->index].format;
	return 0;
}

static int gsxxxx_dv_timings_cap(struct v4l2_subdev *sd,
				 struct v4l2_dv_timings_cap *cap)
{
	if (cap->pad != 0)
		return -EINVAL;

	*cap = gsxxxx_timings_cap;
	return 0;
}

/* V4L2 core operation handlers */
static const struct v4l2_subdev_core_ops gsxxxx_core_ops = {
	.reset = gsxxxx_reset,
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.g_register = gsxxxx_g_register,
	.s_register = gsxxxx_s_register,
#endif
};

static const struct v4l2_subdev_video_ops gsxxxx_video_ops = {
	.s_dv_timings = gsxxxx_s_dv_timings,
	.g_dv_timings = gsxxxx_g_dv_timings,
	.query_dv_timings = gsxxxx_query_dv_timings,
};

static const struct v4l2_subdev_pad_ops gsxxxx_pad_ops = {
	.enum_dv_timings = gsxxxx_enum_dv_timings,
	.dv_timings_cap = gsxxxx_dv_timings_cap,
};

/* V4L2 top level operation handlers */
static const struct v4l2_subdev_ops gsxxxx_ops = {
	.core = &gsxxxx_core_ops,
	.video = &gsxxxx_video_ops,
	.pad = &gsxxxx_pad_ops,
};

static int gsxxxx_probe(struct spi_device *spi)
{
	int ret;
	struct gsxxxx *gs;
	struct v4l2_subdev *sd;
	struct v4l2_dv_timings timings;

	gs = devm_kzalloc(&spi->dev, sizeof(struct gsxxxx), GFP_KERNEL);
	if (!gs)
		return -ENOMEM;

	gs->pdev = spi;
	sd = &gs->sd;

	spi->mode = SPI_MODE_0;
	spi->irq = -1;
	spi->max_speed_hz = 10000000;
	spi->bits_per_word = 16;
	ret = spi_setup(spi);

	if (ret)
		return ret;

	v4l2_spi_subdev_init(sd, spi, &gsxxxx_ops);

	/* Default timing is NULL -> Auto detection */
	custom_to_timings(&reg_fmt_custom[0], &timings);
	gs->current_timings = timings;

	/* Set H_CONFIG to SMPTE timings */
	gsxxxx_write_register(spi, 0x0, 0x100);

	return 0;
}

static int gsxxxx_remove(struct spi_device *spi)
{
	struct v4l2_subdev *sd = spi_get_drvdata(spi);
	struct gsxxxx *gs = to_gsxxxx(sd);

	v4l2_device_unregister_subdev(sd);
	kfree(gs);
	return 0;
}

static struct spi_driver gsxxxx_driver = {
	.driver = {
		.name		= "gs1662",
		.owner		= THIS_MODULE,
	},

	.probe		= gsxxxx_probe,
	.remove		= gsxxxx_remove,
	.id_table	= gsxxxx_id,
};

static int __init gsxxxx_init(void)
{
	spi_register_driver(&gsxxxx_driver);
	return 0;
}

static void __exit gsxxxx_exit(void)
{
	spi_unregister_driver(&gsxxxx_driver);
}

module_init(gsxxxx_init);
module_exit(gsxxxx_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Charles-Antoine Couret <charles-antoine.couret@nexvision.fr>");
MODULE_DESCRIPTION("GSXXXX SPI driver to read and write its registers");
