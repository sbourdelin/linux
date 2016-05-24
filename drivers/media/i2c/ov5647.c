/*
 * A V4L2 driver for OmniVision OV5647 cameras.
 *
 * Based on Samsung S5K6AAFX SXGA 1/6" 1.3M CMOS Image Sensor driver
 * Copyright (C) 2011 Sylwester Nawrocki <s.nawrocki@samsung.com>
 *
 * Based on Omnivision OV7670 Camera Driver
 * Copyright (C) 2006-7 Jonathan Corbet <corbet@lwn.net>
 *
 * Copyright (C) 2016, Synopsys, Inc.
 *
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-image-sizes.h>
#include <media/v4l2-of.h>
#include <linux/io.h>

static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Debug level (0-1)");

/*
 * The ov5647 sits on i2c with ID 0x6c
 */
#define OV5647_I2C_ADDR 0x6c
#define SENSOR_NAME "ov5647"

#define OV5647_REG_CHIPID_H	0x300A
#define OV5647_REG_CHIPID_L	0x300B

#define REG_TERM 0xfffe
#define VAL_TERM 0xfe
#define REG_DLY  0xffff

/*define the voltage level of control signal*/
#define CSI_STBY_ON		1
#define CSI_STBY_OFF		0
#define CSI_RST_ON		0
#define CSI_RST_OFF		1
#define CSI_PWR_ON		1
#define CSI_PWR_OFF		0
#define CSI_AF_PWR_ON		1
#define CSI_AF_PWR_OFF		0


#define OV5647_ROW_START		0x01
#define OV5647_ROW_START_MIN		0
#define OV5647_ROW_START_MAX		2004
#define OV5647_ROW_START_DEF		54

#define OV5647_COLUMN_START		0x02
#define OV5647_COLUMN_START_MIN		0
#define OV5647_COLUMN_START_MAX		2750
#define OV5647_COLUMN_START_DEF		16

#define OV5647_WINDOW_HEIGHT		0x03
#define OV5647_WINDOW_HEIGHT_MIN	2
#define OV5647_WINDOW_HEIGHT_MAX	2006
#define OV5647_WINDOW_HEIGHT_DEF	1944

#define OV5647_WINDOW_WIDTH		0x04
#define OV5647_WINDOW_WIDTH_MIN		2
#define OV5647_WINDOW_WIDTH_MAX		2752
#define OV5647_WINDOW_WIDTH_DEF		2592

enum power_seq_cmd {
	CSI_SUBDEV_PWR_OFF = 0x00,
	CSI_SUBDEV_PWR_ON = 0x01,
};

struct regval_list {
	uint16_t addr;
	uint8_t data;
};

struct cfg_array {
	struct regval_list *regs;
	int size;
};

struct sensor_win_size {
	int width;
	int height;
	unsigned int hoffset;
	unsigned int voffset;
	unsigned int hts;
	unsigned int vts;
	unsigned int pclk;
	unsigned int mipi_bps;
	unsigned int fps_fixed;
	unsigned int bin_factor;
	unsigned int intg_min;
	unsigned int intg_max;
	void *regs;
	int regs_size;
	int (*set_size)(struct v4l2_subdev *subdev);
};


struct ov5647 {
	struct device			*dev;
	struct v4l2_subdev		subdev;
	struct media_pad		pad;
	struct mutex			lock;
	struct v4l2_mbus_framefmt	format;
	struct sensor_format_struct	*fmt;
	unsigned int			width;
	unsigned int			height;
	unsigned int			capture_mode;
	int				hue;
	struct v4l2_fract		tpf;
	struct sensor_win_size		*current_wins;
};

static inline struct ov5647 *to_state(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct ov5647, subdev);
}

static struct regval_list sensor_oe_disable_regs[] = {
	{0x3000, 0x00},
	{0x3001, 0x00},
	{0x3002, 0x00},
};

static struct regval_list sensor_oe_enable_regs[] = {
	{0x3000, 0x0f},
	{0x3001, 0xff},
	{0x3002, 0xe4},
};

static struct regval_list ov5647_640x480[] = {
	{0x0100, 0x00},
	{0x0103, 0x01},
	{0x3034, 0x08},
	{0x3035, 0x21},
	{0x3036, 0x46},
	{0x303c, 0x11},
	{0x3106, 0xf5},
	{0x3821, 0x07},
	{0x3820, 0x41},
	{0x3827, 0xec},
	{0x370c, 0x0f},
	{0x3612, 0x59},
	{0x3618, 0x00},
	{0x5000, 0x06},
	{0x5001, 0x01},
	{0x5002, 0x41},
	{0x5003, 0x08},
	{0x5a00, 0x08},
	{0x3000, 0x00},
	{0x3001, 0x00},
	{0x3002, 0x00},
	{0x3016, 0x08},
	{0x3017, 0xe0},
	{0x3018, 0x44},
	{0x301c, 0xf8},
	{0x301d, 0xf0},
	{0x3a18, 0x00},
	{0x3a19, 0xf8},
	{0x3c01, 0x80},
	{0x3b07, 0x0c},
	{0x380c, 0x07},
	{0x380d, 0x68},
	{0x380e, 0x03},
	{0x380f, 0xd8},
	{0x3814, 0x31},
	{0x3815, 0x31},
	{0x3708, 0x64},
	{0x3709, 0x52},
	{0x3808, 0x02},
	{0x3809, 0x80},
	{0x380a, 0x01},
	{0x380b, 0xE0},
	{0x3801, 0x00},
	{0x3802, 0x00},
	{0x3803, 0x00},
	{0x3804, 0x0a},
	{0x3805, 0x3f},
	{0x3806, 0x07},
	{0x3807, 0xa1},
	{0x3811, 0x08},
	{0x3813, 0x02},
	{0x3630, 0x2e},
	{0x3632, 0xe2},
	{0x3633, 0x23},
	{0x3634, 0x44},
	{0x3636, 0x06},
	{0x3620, 0x64},
	{0x3621, 0xe0},
	{0x3600, 0x37},
	{0x3704, 0xa0},
	{0x3703, 0x5a},
	{0x3715, 0x78},
	{0x3717, 0x01},
	{0x3731, 0x02},
	{0x370b, 0x60},
	{0x3705, 0x1a},
	{0x3f05, 0x02},
	{0x3f06, 0x10},
	{0x3f01, 0x0a},
	{0x3a08, 0x01},
	{0x3a09, 0x27},
	{0x3a0a, 0x00},
	{0x3a0b, 0xf6},
	{0x3a0d, 0x04},
	{0x3a0e, 0x03},
	{0x3a0f, 0x58},
	{0x3a10, 0x50},
	{0x3a1b, 0x58},
	{0x3a1e, 0x50},
	{0x3a11, 0x60},
	{0x3a1f, 0x28},
	{0x4001, 0x02},
	{0x4004, 0x02},
	{0x4000, 0x09},
	{0x4837, 0x24},
	{0x4050, 0x6e},
	{0x4051, 0x8f},
	{0x0100, 0x01},
};

struct sensor_format_struct;  

/**
* @short I2C Write operation
* @param[in] i2c_client I2C client
* @param[in] reg register address
* @param[in] val value to write
* @return Error code
*/
static int ov5647_write(struct v4l2_subdev *sd, uint16_t reg, uint8_t val)
{
	int ret;
	unsigned char data[3] = { reg >> 8, reg & 0xff, val};
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	ret = i2c_master_send(client, data, 3);
	if (ret < 3) {
		v4l2_dbg(1, debug, sd, "%s: i2c write error, reg: %x\n",
				__func__, reg);
		return ret < 0 ? ret : -EIO;
	}
	return 0;
}

/**
* @short I2C Read operation
* @param[in] i2c_client I2C client
* @param[in] reg register address
* @param[out] val value read
* @return Error code
*/
static int ov5647_read(struct v4l2_subdev *sd, uint16_t reg, uint8_t *val)
{
	int ret;
	unsigned char data_w[2] = { reg >> 8, reg & 0xff };
	struct i2c_client *client = v4l2_get_subdevdata(sd);


	ret = i2c_master_send(client, data_w, 2);

	if (ret < 2) {
		v4l2_dbg(1, debug, sd, "%s: i2c read error, reg: %x\n",
			__func__, reg);
		return ret < 0 ? ret : -EIO;
	}


	ret = i2c_master_recv(client, val, 1);

	if (ret < 1) {
		v4l2_dbg(1, debug, sd, "%s: i2c read error, reg: %x\n",
				__func__, reg);
		return ret < 0 ? ret : -EIO;
	}
	return 0;
}

static int ov5647_write_array(struct v4l2_subdev *subdev,
				struct regval_list *regs, int array_size)
{
	int i = 0;
	int ret = 0;

	if (!regs)
		return -EINVAL;

	while (i < array_size) {
		if (regs->addr == REG_DLY)
			mdelay(regs->data);
		else
			ret = ov5647_write(subdev, regs->addr, regs->data);

		if (ret == -EIO)
			return ret;

		i++;
		regs++;
	}
	return 0;
}

static void ov5647_set_virtual_channel(struct v4l2_subdev *subdev, int channel)
{
	u8 channel_id;

	ov5647_read(subdev, 0x4814, &channel_id);
	channel_id &= ~(3 << 6);
	ov5647_write(subdev, 0x4814, channel_id | (channel << 6));
}

/**
* @short Stream ON
* @param[in] subdev v4l2 subdev
* @return Error code
*/
void ov5647_stream_on(struct v4l2_subdev *subdev)
{
	ov5647_write(subdev, 0x4202, 0x00);
	v4l2_dbg(1, debug, subdev, "Stream on");
	ov5647_write(subdev, 0x300D, 0x00);

}

/**
* @short Stream OFF
* @param[in] subdev v4l2 subdev
* @return Error code
*/
void ov5647_stream_off(struct v4l2_subdev *subdev)
{
	ov5647_write(subdev, 0x4202, 0x0f);
	v4l2_dbg(1, debug, subdev, "Stream off");
	ov5647_write(subdev, 0x300D, 0x01);
}

/****************************************************************************/

/**
 * @short Set SW standby
 * @param[in] subdev v4l2 subdev
 * @param[in] on_off standby on or off
 * @return Error code
 */
static int sensor_s_sw_stby(struct v4l2_subdev *subdev, int on_off)
{
	int ret;
	unsigned char rdval;

	ret = ov5647_read(subdev, 0x0100, &rdval);
	if (ret != 0)
		return ret;

	if (on_off == CSI_STBY_ON)
		ret = ov5647_write(subdev, 0x0100, rdval&0xfe);

	else
		ret = ov5647_write(subdev, 0x0100, rdval|0x01);

	return ret;
}

/**
 * @short Store information about the video data format.
 */
static struct sensor_format_struct {
	__u8 *desc;
	u32 mbus_code;
	struct regval_list *regs;
	int regs_size;
	int bpp;
} sensor_formats[] = {
	{
		.desc		= "Raw RGB Bayer",
		.mbus_code	= MEDIA_BUS_FMT_SBGGR8_1X8,
		.regs		= ov5647_640x480,
		.regs_size	= ARRAY_SIZE(ov5647_640x480),
		.bpp		= 1
	},
};
#define N_FMTS ARRAY_SIZE(sensor_formats)

/* ----------------------------------------------------------------------- */

/**
* @short Initialize sensor
* @param[in] subdev v4l2 subdev
* @param[in] val not used
* @return Error code
*/
static int __sensor_init(struct v4l2_subdev *subdev)
{
	int ret;
	uint8_t resetval;
	unsigned char rdval;

	v4l2_dbg(1, debug, subdev, "sensor init\n");

	ret = ov5647_read(subdev, 0x0100, &rdval);
	if (ret != 0)
		return ret;

	ov5647_write(subdev, 0x4800, 0x25);
	ov5647_stream_off(subdev);

	ret = ov5647_write_array(subdev, ov5647_640x480,
					ARRAY_SIZE(ov5647_640x480));
	if (ret < 0) {
		v4l2_err(subdev, "write sensor_default_regs error\n");
		return ret;
	}

	ov5647_set_virtual_channel(subdev, 0);

	ov5647_read(subdev, 0x0100, &resetval);
		if (!resetval&0x01) {
			v4l2_dbg(1, debug, subdev,
					"DEVICE WAS IN SOFTWARE STANDBY");
			ov5647_write(subdev, 0x0100, 0x01);
		}

	ov5647_write(subdev, 0x4800, 0x04);
	ov5647_stream_on(subdev);

	return 0;
}

/**
* @short Control sensor power state
* @param[in] subdev v4l2 subdev
* @param[in] on Sensor power
* @return Error code
*/
static int sensor_power(struct v4l2_subdev *subdev, int on)
{
	int ret;
	struct ov5647 *ov5647 = to_state(subdev);

	ret = 0;
	mutex_lock(&ov5647->lock);

	switch (on) {
	case CSI_SUBDEV_PWR_OFF:
		v4l2_dbg(1, debug, subdev, "CSI SUBDEV PWR OFF!\n");

		v4l2_dbg(1, debug, subdev, "disable oe!\n");
		ret = ov5647_write_array(subdev, sensor_oe_disable_regs,
				ARRAY_SIZE(sensor_oe_disable_regs));

		if (ret < 0)
			v4l2_dbg(1, debug, subdev, "disable oe failed!\n");

		ret = sensor_s_sw_stby(subdev, CSI_STBY_ON);

		if (ret < 0)
			v4l2_dbg(1, debug, subdev, "soft stby failed!\n");

		break;
	case CSI_SUBDEV_PWR_ON:
		v4l2_dbg(1, debug, subdev, "CSI SUBDEV PWR ON!\n");

		ret = ov5647_write_array(subdev, sensor_oe_enable_regs,
				ARRAY_SIZE(sensor_oe_enable_regs));

		ret = __sensor_init(subdev);

		if (ret < 0) {
			v4l2_err(subdev,
				"Camera not available! Check Power!\n");
			break;
		}
		break;
	default:
		return -EINVAL;
	}

	mutex_unlock(&ov5647->lock);

	return 0;
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
/**
* @short Get register value
* @param[in] subdev v4l2 subdev
* @param[in] reg register struct
* @return Error code
*/
static int sensor_get_register(struct v4l2_subdev *subdev,
				struct v4l2_dbg_register *reg)
{
	unsigned char val = 0;
	int ret;

	ret = ov5647_read(subdev, reg->reg & 0xff, &val);
	reg->val = val;
	reg->size = 1;
	return ret;
}

/**
* @short Set register value
* @param[in] subdev v4l2 subdev
* @param[in] reg register struct
* @return Error code
*/
static int sensor_set_register(struct v4l2_subdev *subdev,
				const struct v4l2_dbg_register *reg)
{
	ov5647_write(subdev, reg->reg & 0xff, reg->val & 0xff);
	return 0;
}
#endif

/* ----------------------------------------------------------------------- */

/**
 * @short Subdev core operations registration
 */
static const struct v4l2_subdev_core_ops sensor_core_ops = {
	.s_power		= sensor_power,
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.g_register		= sensor_get_register,
	.s_register		= sensor_set_register,
#endif
};

/* ----------------------------------------------------------------------- */



/**
* @short Enumerate available image formats
* @param[in] subdev v4l2 subdev
* @param[in] index index
* @param[in] code MBUS Pixel code
* @return Error code
*/
static int sensor_enum_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index >= N_FMTS)
		return -EINVAL;

	code->code = sensor_formats[code->index].mbus_code;
	return 0;
}

/**
* @short Try frame format internal function
* @param[in] subdev v4l2 subdev
* @param[in] fmt frame format
* @return Error code
*/
static int sensor_try_fmt_internal(struct v4l2_subdev *subdev,
	struct v4l2_mbus_framefmt *fmt, struct sensor_format_struct **ret_fmt,
	struct sensor_win_size **ret_wsize)
{
	int index;

	for (index = 0; index < N_FMTS; index++)
		if (sensor_formats[index].mbus_code == fmt->code)
			break;

	if (index >= N_FMTS)
		return -EINVAL;

	if (ret_fmt != NULL)
		*ret_fmt = sensor_formats + index;

	fmt->field = V4L2_FIELD_NONE;

	return 0;
}

/**
* @short Set frame format
* @param[in] subdev v4l2 subdev
* @param[in] fmt frame format
* @return Error code
*/
static int sensor_s_fmt(struct v4l2_subdev *subdev,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_format *fmt)
{
	int ret;
	struct sensor_format_struct *sensor_fmt;
	struct sensor_win_size *wsize;
	struct ov5647 *info = to_state(subdev);

	ov5647_write_array(subdev, sensor_oe_disable_regs,
					ARRAY_SIZE(sensor_oe_disable_regs));

	ret = sensor_try_fmt_internal(subdev, &fmt->format,
					&sensor_fmt, &wsize);
	if (ret)
		return ret;

	ov5647_write_array(subdev, sensor_fmt->regs, sensor_fmt->regs_size);

	ret = 0;

	if (wsize->regs)
		ov5647_write_array(subdev, wsize->regs, wsize->regs_size);

	if (wsize->set_size)
		wsize->set_size(subdev);

	info->fmt = sensor_fmt;
	info->width = wsize->width;
	info->height = wsize->height;

	ov5647_write_array(subdev, sensor_oe_enable_regs,
				ARRAY_SIZE(sensor_oe_enable_regs));

	return 0;
}

/**
* @short Set stream parameters
* @param[in] subdev v4l2 subdev
* @param[in] parms stream parameters
* @return Error code
*/
static int sensor_s_parm(struct v4l2_subdev *subdev,
				struct v4l2_streamparm *parms)
{
	struct v4l2_captureparm *cp = &parms->parm.capture;
	struct ov5647 *info = to_state(subdev);

	if (parms->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (info->tpf.numerator == 0)
		return -EINVAL;

	info->capture_mode = cp->capturemode;

	return 0;
}

/**
* @short Get stream parameters
* @param[in] subdev v4l2 subdev
* @param[in] parms stream parameters
* @return Error code
*/
static int sensor_g_parm(struct v4l2_subdev *subdev,
				struct v4l2_streamparm *parms)
{
	struct v4l2_captureparm *cp = &parms->parm.capture;
	struct ov5647 *info = to_state(subdev);

	if (parms->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	memset(cp, 0, sizeof(struct v4l2_captureparm));
	cp->capability = V4L2_CAP_TIMEPERFRAME;
	cp->capturemode = info->capture_mode;

	return 0;
}

static const struct v4l2_subdev_pad_ops ov7670_pad_ops = {
	.enum_mbus_code	= sensor_enum_fmt,
	.set_fmt	= sensor_s_fmt,
};

/**
 * @short Subdev video operations registration
 *
 */
static const struct v4l2_subdev_video_ops sensor_video_ops = {
	.s_parm		= sensor_s_parm,
	.g_parm		= sensor_g_parm,
};

/* ----------------------------------------------------------------------- */

/**
 * @short Subdev operations registration
 *
 */
static const struct v4l2_subdev_ops subdev_ops = {
	.core			= &sensor_core_ops,
	.video			= &sensor_video_ops,
};

/* -----------------------------------------------------------------------------
 * V4L2 subdev internal operations
 */

/**
* @short Detect camera version and model
* @param[in] subdev v4l2 subdev
* @return Error code
*/
int ov5647_detect(struct v4l2_subdev *sd)
{
	unsigned char v;
	int ret;

	ret = sensor_power(sd, 1);
	if (ret < 0)
		return ret;
	ret = ov5647_read(sd, OV5647_REG_CHIPID_H, &v);
	if (ret < 0)
		return ret;
	if (v != 0x56) /* OV manuf. id. */
		return -ENODEV;
	ret = ov5647_read(sd, OV5647_REG_CHIPID_L, &v);
	if (ret < 0)
		return ret;
	if (v != 0x47)
		return -ENODEV;

	ret = sensor_power(sd, 0);
	if (ret < 0)
		return ret;

	return 0;
}

/**
* @short Detect if camera is registered
* @param[in] subdev v4l2 subdev
* @return Error code
*/
static int ov5647_registered(struct v4l2_subdev *subdev)
{
	struct i2c_client *client = v4l2_get_subdevdata(subdev);

	v4l2_info(subdev, "OV5647 detected at address 0x%02x\n", client->addr);

	return 0;
}

/**
* @short Open device
* @param[in] subdev v4l2 subdev
* @param[in] fh v4l2 file handler
* @return Error code
*/
static int ov5647_open(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	struct v4l2_mbus_framefmt *format =
				v4l2_subdev_get_try_format(subdev, fh->pad, 0);
	struct v4l2_rect *crop =
				v4l2_subdev_get_try_crop(subdev, fh->pad, 0);

	crop->left = OV5647_COLUMN_START_DEF;
	crop->top = OV5647_ROW_START_DEF;
	crop->width = OV5647_WINDOW_WIDTH_DEF;
	crop->height = OV5647_WINDOW_HEIGHT_DEF;

	format->code = MEDIA_BUS_FMT_SBGGR8_1X8;

	format->width = OV5647_WINDOW_WIDTH_DEF;
	format->height = OV5647_WINDOW_HEIGHT_DEF;
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_SRGB;

	return sensor_power(subdev, 1);
}

/**
* @short Open device
* @param[in] subdev v4l2 subdev
* @param[in] fh v4l2 file handler
* @return Error code
*/
static int ov5647_close(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	return sensor_power(subdev, 0);
}

/**
 * @short Subdev internal operations registration
 *
 */
static const struct v4l2_subdev_internal_ops ov5647_subdev_internal_ops = {
	.registered = ov5647_registered,
	.open = ov5647_open,
	.close = ov5647_close,
};

/**
* @short Initialization routine - Entry point of the driver
* @param[in] client pointer to the i2c client structure
* @param[in] id pointer to the i2c device id structure
* @return 0 on success and a negative number on failure
* Refer to Linux errors.
*/
static int ov5647_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct ov5647 *sensor;
	int ret = 0;
	struct v4l2_subdev *sd;

	pr_info("Installing OmniVision OV5647 camera driver\n");

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (sensor == NULL)
		return -ENOMEM;

	mutex_init(&sensor->lock);
	sensor->dev = dev;

	sd = &sensor->subdev;
	v4l2_i2c_subdev_init(sd, client, &subdev_ops);
	sensor->subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sd->entity, 1, &sensor->pad);
	if (ret < 0)
		return ret;


	ret = v4l2_async_register_subdev(sd);
	if (ret < 0)
		media_entity_cleanup(&sd->entity);

	return ret;
}

/**
* @short Exit routine - Exit point of the driver
* @param[in] client pointer to the i2c clietn structure
* @return 0 on success and a negative number on failure
* Refer to Linux errors.
*/
static int ov5647_remove(struct i2c_client *client)
{
	struct v4l2_subdev *subdev = i2c_get_clientdata(client);
	struct ov5647 *ov5647 = to_state(subdev);

	v4l2_async_unregister_subdev(&ov5647->subdev);
	media_entity_cleanup(&ov5647->subdev.entity);
	v4l2_device_unregister_subdev(subdev);

	return 0;
}

/**
* @short of_device_id structure
*/
static const struct i2c_device_id ov5647_id[] = {
	{ "ov5647", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ov5647_id);

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id ov5647_of_match[] = {
	{ .compatible = "ov5647", 0 },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, ov5647_of_match);
#endif

/**
* @short i2c driver structure
*/
static struct i2c_driver ov5647_driver = {
	.driver = {
		.of_match_table = of_match_ptr(ov5647_of_match),
		.owner	= THIS_MODULE,
		.name	= "ov5647",
	},
	.probe		= ov5647_probe,
	.remove		= ov5647_remove,
	.id_table	= ov5647_id,
};

module_i2c_driver(ov5647_driver);

MODULE_AUTHOR("Ramiro Oliveira <roliveir@synopsys.com>");
MODULE_DESCRIPTION("A low-level driver for OmniVision ov5647 sensors");
MODULE_LICENSE("GPL");
