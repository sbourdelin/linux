/*
 * Video Camera Capture driver for Freescale i.MX5/6 SOC
 *
 * Copyright (c) 2012-2016 Mentor Graphics Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/of_platform.h>
#include <linux/mxc_icap.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-of.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <video/imx-ipu-v3.h>
#include <media/imx.h>
#include "imx-camif.h"
#include "imx-of.h"

/*
 * Min/Max supported width and heights.
 */
#define MIN_W       176
#define MIN_H       144
#define MAX_W      8192
#define MAX_H      4096
#define MAX_W_IC   1024
#define MAX_H_IC   1024
#define MAX_W_VDIC  968
#define MAX_H_VDIC 2048

#define H_ALIGN    3 /* multiple of 8 */
#define S_ALIGN    1 /* multiple of 2 */

#define DEVICE_NAME "imx-camera"

/* In bytes, per queue */
#define VID_MEM_LIMIT	SZ_64M

static struct vb2_ops imxcam_qops;

static inline struct imxcam_dev *sd2dev(struct v4l2_subdev *sd)
{
	return container_of(sd->v4l2_dev, struct imxcam_dev, v4l2_dev);
}

static inline struct imxcam_dev *notifier2dev(struct v4l2_async_notifier *n)
{
	return container_of(n, struct imxcam_dev, subdev_notifier);
}

static inline struct imxcam_dev *fim2dev(struct imxcam_fim *fim)
{
	return container_of(fim, struct imxcam_dev, fim);
}

static inline struct imxcam_ctx *file2ctx(struct file *file)
{
	return container_of(file->private_data, struct imxcam_ctx, fh);
}

static inline bool is_io_ctx(struct imxcam_ctx *ctx)
{
	return ctx == ctx->dev->io_ctx;
}

/* forward references */
static void imxcam_bump_restart_timer(struct imxcam_ctx *ctx);

/* Supported user and sensor pixel formats */
static struct imxcam_pixfmt imxcam_pixformats[] = {
	{
		.name	= "RGB565",
		.fourcc	= V4L2_PIX_FMT_RGB565,
		.codes  = {MEDIA_BUS_FMT_RGB565_2X8_LE},
		.bpp    = 16,
	}, {
		.name	= "RGB24",
		.fourcc	= V4L2_PIX_FMT_RGB24,
		.codes  = {MEDIA_BUS_FMT_RGB888_1X24,
			   MEDIA_BUS_FMT_RGB888_2X12_LE},
		.bpp    = 24,
	}, {
		.name	= "BGR24",
		.fourcc	= V4L2_PIX_FMT_BGR24,
		.bpp    = 24,
	}, {
		.name	= "RGB32",
		.fourcc	= V4L2_PIX_FMT_RGB32,
		.codes = {MEDIA_BUS_FMT_ARGB8888_1X32},
		.bpp   = 32,
	}, {
		.name	= "BGR32",
		.fourcc	= V4L2_PIX_FMT_BGR32,
		.bpp    = 32,
	}, {
		.name	= "4:2:2 packed, YUYV",
		.fourcc	= V4L2_PIX_FMT_YUYV,
		.codes = {MEDIA_BUS_FMT_YUYV8_2X8, MEDIA_BUS_FMT_YUYV8_1X16},
		.bpp   = 16,
	}, {
		.name	= "4:2:2 packed, UYVY",
		.fourcc	= V4L2_PIX_FMT_UYVY,
		.codes = {MEDIA_BUS_FMT_UYVY8_2X8, MEDIA_BUS_FMT_UYVY8_1X16},
		.bpp   = 16,
	}, {
		.name	= "4:2:0 planar, YUV",
		.fourcc	= V4L2_PIX_FMT_YUV420,
		.bpp    = 12,
		.y_depth = 8,
	}, {
		.name   = "4:2:0 planar, YVU",
		.fourcc = V4L2_PIX_FMT_YVU420,
		.bpp    = 12,
		.y_depth = 8,
	}, {
		.name   = "4:2:2 planar, YUV",
		.fourcc = V4L2_PIX_FMT_YUV422P,
		.bpp    = 16,
		.y_depth = 8,
	}, {
		.name   = "4:2:0 planar, Y/CbCr",
		.fourcc = V4L2_PIX_FMT_NV12,
		.bpp    = 12,
		.y_depth = 8,
	}, {
		.name   = "4:2:2 planar, Y/CbCr",
		.fourcc = V4L2_PIX_FMT_NV16,
		.bpp    = 16,
		.y_depth = 8,
	},
};

#define NUM_FORMATS ARRAY_SIZE(imxcam_pixformats)

static struct imxcam_pixfmt *imxcam_get_format(u32 fourcc, u32 code)
{
	struct imxcam_pixfmt *fmt, *ret = NULL;
	int i, j;

	for (i = 0; i < NUM_FORMATS; i++) {
		fmt = &imxcam_pixformats[i];

		if (fourcc && fmt->fourcc == fourcc) {
			ret = fmt;
			goto out;
		}

		for (j = 0; fmt->codes[j]; j++) {
			if (fmt->codes[j] == code) {
				ret = fmt;
				goto out;
			}
		}
	}
out:
	return ret;
}

/* Support functions */

/* find the sensor that is handling this input index */
static struct imxcam_sensor *
find_sensor_by_input_index(struct imxcam_dev *dev, int input_idx)
{
	struct imxcam_sensor *sensor;
	int i;

	for (i = 0; i < dev->num_sensors; i++) {
		sensor = &dev->sensor_list[i];
		if (!sensor->sd)
			continue;

		if (input_idx >= sensor->input.first &&
		    input_idx <= sensor->input.last)
			break;
	}

	return (i < dev->num_sensors) ? sensor : NULL;
}

/*
 * Set all the video muxes required to receive data from the
 * current sensor.
 */
static int imxcam_set_video_muxes(struct imxcam_dev *dev)
{
	struct imxcam_sensor *sensor = dev->sensor;
	int i, ret;

	for (i = 0; i < IMXCAM_MAX_VIDEOMUX; i++) {
		if (sensor->vidmux_input[i] < 0)
			continue;
		dev_dbg(dev->dev, "%s: vidmux %d, input %d\n",
			sensor->sd->name, i, sensor->vidmux_input[i]);
		ret = v4l2_subdev_call(dev->vidmux_list[i], video, s_routing,
				       sensor->vidmux_input[i], 0, 0);
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * Query sensor and update signal lock status. Returns true if lock
 * status has changed.
 */
static bool update_signal_lock_status(struct imxcam_dev *dev)
{
	bool locked, changed;
	u32 status;
	int ret;

	ret = v4l2_subdev_call(dev->sensor->sd, video, g_input_status, &status);
	if (ret)
		return false;

	locked = ((status & (V4L2_IN_ST_NO_SIGNAL | V4L2_IN_ST_NO_SYNC)) == 0);
	changed = (dev->signal_locked != locked);
	dev->signal_locked = locked;

	return changed;
}

/*
 * Return true if the VDIC deinterlacer is needed. We need the VDIC
 * if the sensor is transmitting fields, and userland is requesting
 * motion compensation (rather than simple weaving).
 */
static bool need_vdic(struct imxcam_dev *dev,
		      struct v4l2_mbus_framefmt *sf)
{
	return dev->motion != MOTION_NONE && V4L2_FIELD_HAS_BOTH(sf->field);
}

/*
 * Return true if sensor format currently meets the VDIC
 * restrictions:
 *     o the full-frame resolution to the VDIC must be at or below 968x2048.
 *     o the pixel format to the VDIC must be YUV422
 */
static bool can_use_vdic(struct imxcam_dev *dev,
			 struct v4l2_mbus_framefmt *sf)
{
	return sf->width <= MAX_W_VDIC &&
		sf->height <= MAX_H_VDIC &&
		(sf->code == MEDIA_BUS_FMT_UYVY8_2X8 ||
		 sf->code == MEDIA_BUS_FMT_UYVY8_1X16 ||
		 sf->code == MEDIA_BUS_FMT_YUYV8_2X8 ||
		 sf->code == MEDIA_BUS_FMT_YUYV8_1X16);
}

/*
 * Return true if the current capture parameters require the use of
 * the Image Converter. We need the IC for scaling, colorspace conversion,
 * and rotation.
 */
static bool need_ic(struct imxcam_dev *dev,
		    struct v4l2_mbus_framefmt *sf,
		    struct v4l2_format *uf,
		    struct v4l2_rect *crop)
{
	struct v4l2_pix_format *user_fmt = &uf->fmt.pix;
	enum ipu_color_space sensor_cs, user_cs;
	bool ret;

	sensor_cs = ipu_mbus_code_to_colorspace(sf->code);
	user_cs = ipu_pixelformat_to_colorspace(user_fmt->pixelformat);

	ret = (user_fmt->width != crop->width ||
	       user_fmt->height != crop->height ||
	       user_cs != sensor_cs ||
	       dev->rot_mode != IPU_ROTATE_NONE);

	return ret;
}

/*
 * Return true if user and sensor formats currently meet the IC
 * restrictions:
 *     o the parallel CSI bus cannot be 16-bit wide.
 *     o the endpoint id of the CSI this sensor connects to must be 0
 *       (for MIPI CSI2, the endpoint id is the virtual channel number,
 *        and only VC0 can pass through the IC).
 *     o the resizer output size must be at or below 1024x1024.
 */
static bool can_use_ic(struct imxcam_dev *dev,
		       struct v4l2_mbus_framefmt *sf,
		       struct v4l2_format *uf)
{
	struct imxcam_sensor *sensor = dev->sensor;

	return (sensor->ep.bus_type == V4L2_MBUS_CSI2 ||
		sensor->ep.bus.parallel.bus_width < 16) &&
		sensor->csi_ep.base.id == 0 &&
		uf->fmt.pix.width <= MAX_W_IC &&
		uf->fmt.pix.height <= MAX_H_IC;
}

/*
 * Adjusts passed width and height to meet IC resizer limits.
 */
static void adjust_to_resizer_limits(struct imxcam_dev *dev,
				     struct v4l2_format *uf,
				     struct v4l2_rect *crop)
{
	u32 *width, *height;

	if (uf->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		width = &uf->fmt.pix.width;
		height = &uf->fmt.pix.height;
	} else {
		width = &uf->fmt.win.w.width;
		height = &uf->fmt.win.w.height;
	}

	/* output of resizer can't be above 1024x1024 */
	*width = min_t(__u32, *width, MAX_W_IC);
	*height = min_t(__u32, *height, MAX_H_IC);

	/* resizer cannot downsize more than 4:1 */
	if (ipu_rot_mode_is_irt(dev->rot_mode)) {
		*height = max_t(__u32, *height, crop->width / 4);
		*width = max_t(__u32, *width, crop->height / 4);
	} else {
		*width = max_t(__u32, *width, crop->width / 4);
		*height = max_t(__u32, *height, crop->height / 4);
	}
}

static void adjust_user_fmt(struct imxcam_dev *dev,
			    struct v4l2_mbus_framefmt *sf,
			    struct v4l2_format *uf,
			    struct v4l2_rect *crop)
{
	struct imxcam_pixfmt *fmt;

	/*
	 * Make sure resolution is within IC resizer limits
	 * if we need the Image Converter.
	 */
	if (need_ic(dev, sf, uf, crop))
		adjust_to_resizer_limits(dev, uf, crop);

	/*
	 * Force the resolution to match crop window if
	 * we can't use the Image Converter.
	 */
	if (!can_use_ic(dev, sf, uf)) {
		uf->fmt.pix.width = crop->width;
		uf->fmt.pix.height = crop->height;
	}

	fmt = imxcam_get_format(uf->fmt.pix.pixelformat, 0);

	uf->fmt.pix.bytesperline = (uf->fmt.pix.width * fmt->bpp) >> 3;
	uf->fmt.pix.sizeimage = uf->fmt.pix.height * uf->fmt.pix.bytesperline;
}

/*
 * calculte the default active crop window, given a sensor frame and
 * video standard. This crop window will be stored to dev->crop_defrect.
 */
static void calc_default_crop(struct imxcam_dev *dev,
			      struct v4l2_rect *rect,
			      struct v4l2_mbus_framefmt *sf,
			      v4l2_std_id std)
{
	rect->width = sf->width;
	rect->height = sf->height;
	rect->top = 0;
	rect->left = 0;

	/*
	 * FIXME: For NTSC standards, top must be set to an
	 * offset of 13 lines to match fixed CCIR programming
	 * in the IPU.
	 */
	if (std != V4L2_STD_UNKNOWN && (std & V4L2_STD_525_60))
		rect->top = 13;

	/* adjust crop window to h/w alignment restrictions */
	rect->width &= ~0x7;
}

static int update_sensor_std(struct imxcam_dev *dev)
{
	return v4l2_subdev_call(dev->sensor->sd, video, querystd,
				&dev->current_std);
}

static void update_fim(struct imxcam_dev *dev)
{
	struct imxcam_fim *fim = &dev->fim;

	if (dev->sensor_tpf.denominator == 0) {
		fim->enabled = false;
		return;
	}

	fim->nominal = DIV_ROUND_CLOSEST(
		1000 * 1000 * dev->sensor_tpf.numerator,
		dev->sensor_tpf.denominator);
}

static int update_sensor_fmt(struct imxcam_dev *dev)
{
	struct v4l2_subdev_format fmt;
	struct v4l2_streamparm parm;
	struct v4l2_rect crop;
	int ret;

	update_sensor_std(dev);

	fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	fmt.pad = 0;

	ret = v4l2_subdev_call(dev->sensor->sd, pad, get_fmt, NULL, &fmt);
	if (ret)
		return ret;

	dev->sensor_fmt = fmt.format;

	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = v4l2_subdev_call(dev->sensor->sd, video, g_parm, &parm);
	if (ret)
		memset(&dev->sensor_tpf, 0, sizeof(dev->sensor_tpf));
	else
		dev->sensor_tpf = parm.parm.capture.timeperframe;
	update_fim(dev);

	ret = v4l2_subdev_call(dev->sensor->sd, video, g_mbus_config,
			       &dev->mbus_cfg);
	if (ret)
		return ret;

	dev->sensor_pixfmt = imxcam_get_format(0, dev->sensor_fmt.code);

	/* get new sensor default crop window */
	calc_default_crop(dev, &crop, &dev->sensor_fmt, dev->current_std);

	/* and update crop bounds */
	dev->crop_bounds.top = dev->crop_bounds.left = 0;
	dev->crop_bounds.width = crop.width + (u32)crop.left;
	dev->crop_bounds.height = crop.height + (u32)crop.top;

	/*
	 * reset the user crop window to defrect if defrect has changed,
	 * or if user crop is not initialized yet.
	 */
	if (dev->crop_defrect.width != crop.width ||
	    dev->crop_defrect.left != crop.left ||
	    dev->crop_defrect.height != crop.height ||
	    dev->crop_defrect.top != crop.top ||
	    !dev->crop.width || !dev->crop.height) {
		dev->crop_defrect = crop;
		dev->crop = dev->crop_defrect;
	}

	return 0;
}

/*
 * Turn current sensor power on/off according to power_count.
 */
static int sensor_set_power(struct imxcam_dev *dev, int on)
{
	struct imxcam_sensor *sensor = dev->sensor;
	struct v4l2_subdev *sd = sensor->sd;
	int ret;

	if (on && sensor->power_count++ > 0)
		return 0;
	else if (!on && (sensor->power_count == 0 ||
			 --sensor->power_count > 0))
		return 0;

	if (on) {
		/* power-on the csi2 receiver */
		if (sensor->ep.bus_type == V4L2_MBUS_CSI2 && dev->csi2_sd) {
			ret = v4l2_subdev_call(dev->csi2_sd, core, s_power,
					       true);
			if (ret)
				goto out;
		}

		ret = v4l2_subdev_call(sd, core, s_power, true);
		if (ret && ret != -ENOIOCTLCMD)
			goto csi2_off;
	} else {
		v4l2_subdev_call(sd, core, s_power, false);
		if (sensor->ep.bus_type == V4L2_MBUS_CSI2 && dev->csi2_sd)
			v4l2_subdev_call(dev->csi2_sd, core, s_power, false);
	}

	return 0;

csi2_off:
	if (sensor->ep.bus_type == V4L2_MBUS_CSI2 && dev->csi2_sd)
		v4l2_subdev_call(dev->csi2_sd, core, s_power, false);
out:
	sensor->power_count--;
	return ret;
}

static void reset_fim(struct imxcam_dev *dev, bool curval)
{
	struct imxcam_fim *fim = &dev->fim;
	struct v4l2_ctrl *en = fim->ctrl[FIM_CL_ENABLE];
	struct v4l2_ctrl *num = fim->ctrl[FIM_CL_NUM];
	struct v4l2_ctrl *skip = fim->ctrl[FIM_CL_NUM_SKIP];
	struct v4l2_ctrl *tol_min = fim->ctrl[FIM_CL_TOLERANCE_MIN];
	struct v4l2_ctrl *tol_max = fim->ctrl[FIM_CL_TOLERANCE_MAX];
	unsigned long flags;

	spin_lock_irqsave(&dev->irqlock, flags);

	if (curval) {
		fim->enabled = en->cur.val;
		fim->num_avg = num->cur.val;
		fim->num_skip = skip->cur.val;
		fim->tolerance_min = tol_min->cur.val;
		fim->tolerance_max = tol_max->cur.val;
	} else {
		fim->enabled = en->val;
		fim->num_avg = num->val;
		fim->num_skip = skip->val;
		fim->tolerance_min = tol_min->val;
		fim->tolerance_max = tol_max->val;
	}

	/* disable tolerance range if max <= min */
	if (fim->tolerance_max <= fim->tolerance_min)
		fim->tolerance_max = 0;

	fim->counter = -fim->num_skip;
	fim->sum = 0;

	spin_unlock_irqrestore(&dev->irqlock, flags);
}

/*
 * Monitor an averaged frame interval. If the average deviates too much
 * from the sensor's nominal frame rate, return -EIO. The frame intervals
 * are averaged in order to quiet noise from (presumably random) interrupt
 * latency.
 */
static int frame_interval_monitor(struct imxcam_fim *fim, struct timespec *ts)
{
	unsigned long interval, error, error_avg;
	struct imxcam_dev *dev = fim2dev(fim);
	struct timespec diff;
	int ret = 0;

	if (++fim->counter <= 0)
		goto out_update_ts;

	diff = timespec_sub(*ts, fim->last_ts);
	interval = diff.tv_sec * 1000 * 1000 + diff.tv_nsec / 1000;
	error = abs(interval - fim->nominal);

	if (fim->tolerance_max && error >= fim->tolerance_max) {
		dev_dbg(dev->dev,
			"FIM: %lu ignored, out of tolerance bounds\n",
			error);
		fim->counter--;
		goto out_update_ts;
	}

	fim->sum += error;

	if (fim->counter == fim->num_avg) {
		error_avg = DIV_ROUND_CLOSEST(fim->sum, fim->num_avg);

		if (error_avg > fim->tolerance_min)
			ret = -EIO;

		dev_dbg(dev->dev, "FIM: error: %lu usec%s\n",
			error_avg, ret ? " (!!!)" : "");

		fim->counter = 0;
		fim->sum = 0;
	}

out_update_ts:
	fim->last_ts = *ts;
	return ret;
}

/*
 * Called by the encode and vdic subdevs in their EOF interrupt
 * handlers with the irqlock held. This way of measuring frame
 * intervals is subject to errors introduced by interrupt latency.
 */
static int fim_eof_handler(struct imxcam_dev *dev, struct timeval *now)
{
	struct imxcam_fim *fim = &dev->fim;
	struct timespec ts;

	if (!fim->enabled)
		return 0;

	ts.tv_sec = now->tv_sec;
	ts.tv_nsec = now->tv_usec * 1000;

	return frame_interval_monitor(fim, &ts);
}

/*
 * Input Capture method of measuring frame intervals. Not subject
 * to interrupt latency.
 */
static void fim_input_capture_handler(int channel, void *dev_id,
				      struct timespec *now)
{
	struct imxcam_fim *fim = dev_id;
	struct imxcam_dev *dev = fim2dev(fim);
	struct imxcam_ctx *ctx;
	unsigned long flags;

	if (!fim->enabled)
		return;

	if (!frame_interval_monitor(fim, now))
		return;

	spin_lock_irqsave(&dev->notify_lock, flags);
	ctx = dev->io_ctx;
	if (ctx && !ctx->stop && !atomic_read(&dev->pending_restart))
		imxcam_bump_restart_timer(ctx);
	spin_unlock_irqrestore(&dev->notify_lock, flags);
}

static int fim_request_input_capture(struct imxcam_dev *dev)
{
	struct imxcam_fim *fim = &dev->fim;

	if (fim->icap_channel < 0)
		return 0;

	return mxc_request_input_capture(fim->icap_channel,
					 fim_input_capture_handler,
					 fim->icap_flags, fim);
}

static void fim_free_input_capture(struct imxcam_dev *dev)
{
	struct imxcam_fim *fim = &dev->fim;

	if (fim->icap_channel < 0)
		return;

	mxc_free_input_capture(fim->icap_channel, fim);
}

/*
 * Turn current sensor and CSI streaming on/off according to stream_count.
 */
static int sensor_set_stream(struct imxcam_dev *dev, int on)
{
	struct imxcam_sensor *sensor = dev->sensor;
	int ret;

	if (on && sensor->stream_count++ > 0)
		return 0;
	else if (!on && (sensor->stream_count == 0 ||
			 --sensor->stream_count > 0))
		return 0;

	if (on) {
		ret = v4l2_subdev_call(sensor->sd, video, s_stream, true);
		if (ret && ret != -ENOIOCTLCMD)
			goto out;

		if (dev->sensor->ep.bus_type == V4L2_MBUS_CSI2 && dev->csi2_sd) {
			ret = v4l2_subdev_call(dev->csi2_sd, video, s_stream,
					       true);
			if (ret)
				goto sensor_off;
		}

		ret = v4l2_subdev_call(sensor->csi_sd, video, s_stream, true);
		if (ret)
			goto csi2_off;

		ret = fim_request_input_capture(dev);
		if (ret)
			goto csi_off;
	} else {
		fim_free_input_capture(dev);
		v4l2_subdev_call(sensor->csi_sd, video, s_stream, false);
		if (dev->sensor->ep.bus_type == V4L2_MBUS_CSI2 && dev->csi2_sd)
			v4l2_subdev_call(dev->csi2_sd, video, s_stream, false);
		v4l2_subdev_call(sensor->sd, video, s_stream, false);
	}

	return 0;

csi_off:
	v4l2_subdev_call(sensor->csi_sd, video, s_stream, false);
csi2_off:
	if (dev->sensor->ep.bus_type == V4L2_MBUS_CSI2 && dev->csi2_sd)
		v4l2_subdev_call(dev->csi2_sd, video, s_stream, false);
sensor_off:
	v4l2_subdev_call(sensor->sd, video, s_stream, false);
out:
	sensor->stream_count--;
	return ret;
}

/*
 * Start the encoder for buffer streaming. There must be at least two
 * frames in the vb2 queue.
 */
static int start_encoder(struct imxcam_dev *dev)
{
	struct v4l2_subdev *streaming_sd;
	int ret;

	if (dev->encoder_on)
		return 0;

	if (dev->using_vdic)
		streaming_sd = dev->vdic_sd;
	else if (dev->using_ic)
		streaming_sd = dev->prpenc_sd;
	else
		streaming_sd = dev->smfc_sd;

	ret = v4l2_subdev_call(streaming_sd, video, s_stream, 1);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "encoder stream on failed\n");
		return ret;
	}

	dev->encoder_on = true;
	return 0;
}

/*
 * Stop the encoder.
 */
static int stop_encoder(struct imxcam_dev *dev)
{
	struct v4l2_subdev *streaming_sd;
	int ret;

	if (!dev->encoder_on)
		return 0;

	if (dev->using_vdic)
		streaming_sd = dev->vdic_sd;
	else if (dev->using_ic)
		streaming_sd = dev->prpenc_sd;
	else
		streaming_sd = dev->smfc_sd;

	/* encoder/vdic off */
	ret = v4l2_subdev_call(streaming_sd, video, s_stream, 0);
	if (ret)
		v4l2_err(&dev->v4l2_dev, "encoder stream off failed\n");

	dev->encoder_on = false;
	return ret;
}

/*
 * Start/Stop streaming.
 */
static int set_stream(struct imxcam_ctx *ctx, bool on)
{
	struct imxcam_dev *dev = ctx->dev;
	int ret = 0;

	if (on) {
		if (atomic_read(&dev->status_change)) {
			update_signal_lock_status(dev);
			update_sensor_fmt(dev);
			atomic_set(&dev->status_change, 0);
			v4l2_info(&dev->v4l2_dev, "at stream on: %s, %s\n",
				  v4l2_norm_to_name(dev->current_std),
				  dev->signal_locked ?
				  "signal locked" : "no signal");
		}

		atomic_set(&dev->pending_restart, 0);

		dev->using_ic =
			(need_ic(dev, &dev->sensor_fmt, &dev->user_fmt,
				 &dev->crop) &&
			 can_use_ic(dev, &dev->sensor_fmt, &dev->user_fmt));

		dev->using_vdic = need_vdic(dev, &dev->sensor_fmt) &&
			can_use_vdic(dev, &dev->sensor_fmt);

		reset_fim(dev, true);

		/*
		 * If there are two or more frames in the queue, we can start
		 * the encoder now. Otherwise the encoding will start once
		 * two frames have been queued.
		 */
		if (!list_empty(&ctx->ready_q) &&
		    !list_is_singular(&ctx->ready_q))
			ret = start_encoder(dev);
	} else {
		ret = stop_encoder(dev);
	}

	return ret;
}

/*
 * Restart work handler. This is called in three cases during active
 * streaming.
 *
 * o NFB4EOF errors
 * o A decoder's signal lock status or autodetected video standard changes
 * o End-of-Frame timeouts
 */
static void restart_work_handler(struct work_struct *w)
{
	struct imxcam_ctx *ctx = container_of(w, struct imxcam_ctx,
					      restart_work);
	struct imxcam_dev *dev = ctx->dev;

	mutex_lock(&dev->mutex);

	/* this can happen if we are releasing the io context */
	if (!is_io_ctx(ctx))
		goto out_unlock;

	if (!vb2_is_streaming(&dev->buffer_queue))
		goto out_unlock;

	if (!ctx->stop) {
		v4l2_warn(&dev->v4l2_dev, "restarting\n");
		set_stream(ctx, false);
		set_stream(ctx, true);
	}

out_unlock:
	mutex_unlock(&dev->mutex);
}

/*
 * Stop work handler. Not currently needed but keep around.
 */
static void stop_work_handler(struct work_struct *w)
{
	struct imxcam_ctx *ctx = container_of(w, struct imxcam_ctx,
					      stop_work);
	struct imxcam_dev *dev = ctx->dev;

	mutex_lock(&dev->mutex);

	if (vb2_is_streaming(&dev->buffer_queue)) {
		v4l2_err(&dev->v4l2_dev, "stopping\n");
		vb2_streamoff(&dev->buffer_queue, V4L2_BUF_TYPE_VIDEO_CAPTURE);
	}

	mutex_unlock(&dev->mutex);
}

/*
 * Restart timer function. Schedules a restart.
 */
static void imxcam_restart_timeout(unsigned long data)
{
	struct imxcam_ctx *ctx = (struct imxcam_ctx *)data;

	schedule_work(&ctx->restart_work);
}

/*
 * bump the restart timer and set the pending restart flag.
 * notify_lock must be held when calling.
 */
static void imxcam_bump_restart_timer(struct imxcam_ctx *ctx)
{
	struct imxcam_dev *dev = ctx->dev;

	mod_timer(&ctx->restart_timer, jiffies +
		  msecs_to_jiffies(IMXCAM_RESTART_DELAY));
	atomic_set(&dev->pending_restart, 1);
}

/* Controls */
static int imxcam_set_rotation(struct imxcam_dev *dev,
			       int rotation, bool hflip, bool vflip)
{
	enum ipu_rotate_mode rot_mode;
	int ret;

	ret = ipu_degrees_to_rot_mode(&rot_mode, rotation,
				      hflip, vflip);
	if (ret)
		return ret;

	if (rot_mode != dev->rot_mode) {
		/* can't change rotation mid-streaming */
		if (vb2_is_streaming(&dev->buffer_queue)) {
			v4l2_err(&dev->v4l2_dev,
				 "%s: not allowed while streaming\n",
				 __func__);
			return -EBUSY;
		}

		if (rot_mode != IPU_ROTATE_NONE &&
		    !can_use_ic(dev, &dev->sensor_fmt, &dev->user_fmt)) {
			v4l2_err(&dev->v4l2_dev,
				 "%s: current format does not allow rotation\n",
				 __func__);
			return -EINVAL;
		}
	}

	dev->rot_mode = rot_mode;
	dev->rotation = rotation;
	dev->hflip = hflip;
	dev->vflip = vflip;

	return 0;
}

static int imxcam_set_motion(struct imxcam_dev *dev,
			     enum ipu_motion_sel motion)
{
	if (motion != dev->motion) {
		/* can't change motion setting mid-streaming */
		if (vb2_is_streaming(&dev->buffer_queue)) {
			v4l2_err(&dev->v4l2_dev,
				 "%s: not allowed while streaming\n",
				 __func__);
			return -EBUSY;
		}

		if (motion != MOTION_NONE &&
		    !can_use_vdic(dev, &dev->sensor_fmt)) {
			v4l2_err(&dev->v4l2_dev,
				 "sensor format does not allow deinterlace\n");
			return -EINVAL;
		}
	}

	dev->motion = motion;
	return 0;
}

static int imxcam_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imxcam_dev *dev = container_of(ctrl->handler,
					      struct imxcam_dev, ctrl_hdlr);
	enum ipu_motion_sel motion;
	bool hflip, vflip;
	int rotation;

	rotation = dev->rotation;
	hflip = dev->hflip;
	vflip = dev->vflip;

	switch (ctrl->id) {
	case V4L2_CID_HFLIP:
		hflip = (ctrl->val == 1);
		break;
	case V4L2_CID_VFLIP:
		vflip = (ctrl->val == 1);
		break;
	case V4L2_CID_ROTATE:
		rotation = ctrl->val;
		break;
	case V4L2_CID_IMX_MOTION:
		motion = ctrl->val;
		return imxcam_set_motion(dev, motion);
	case V4L2_CID_IMX_FIM_ENABLE:
		reset_fim(dev, false);
		return 0;
	default:
		v4l2_err(&dev->v4l2_dev, "Invalid control\n");
		return -EINVAL;
	}

	return imxcam_set_rotation(dev, rotation, hflip, vflip);
}

static const struct v4l2_ctrl_ops imxcam_ctrl_ops = {
	.s_ctrl = imxcam_s_ctrl,
};

static const struct v4l2_ctrl_config imxcam_std_ctrl[] = {
	{
		.id = V4L2_CID_HFLIP,
		.name = "Horizontal Flip",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.def =  0,
		.min =  0,
		.max =  1,
		.step = 1,
	}, {
		.id = V4L2_CID_VFLIP,
		.name = "Vertical Flip",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.def =  0,
		.min =  0,
		.max =  1,
		.step = 1,
	}, {
		.id = V4L2_CID_ROTATE,
		.name = "Rotation",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.def =   0,
		.min =   0,
		.max = 270,
		.step = 90,
	},
};

#define IMXCAM_NUM_STD_CONTROLS ARRAY_SIZE(imxcam_std_ctrl)

static const struct v4l2_ctrl_config imxcam_custom_ctrl[] = {
	{
		.ops = &imxcam_ctrl_ops,
		.id = V4L2_CID_IMX_MOTION,
		.name = "Motion Compensation",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.def = MOTION_NONE,
		.min = MOTION_NONE,
		.max = HIGH_MOTION,
		.step = 1,
	},
};

#define IMXCAM_NUM_CUSTOM_CONTROLS ARRAY_SIZE(imxcam_custom_ctrl)

static const struct v4l2_ctrl_config imxcam_fim_ctrl[] = {
	[FIM_CL_ENABLE] = {
		.ops = &imxcam_ctrl_ops,
		.id = V4L2_CID_IMX_FIM_ENABLE,
		.name = "FIM Enable",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.def = FIM_CL_ENABLE_DEF,
		.min = 0,
		.max = 1,
		.step = 1,
	},
	[FIM_CL_NUM] = {
		.ops = &imxcam_ctrl_ops,
		.id = V4L2_CID_IMX_FIM_NUM,
		.name = "FIM Num Average",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.def = FIM_CL_NUM_DEF,
		.min =  1, /* no averaging */
		.max = 64, /* average 64 frames */
		.step = 1,
	},
	[FIM_CL_TOLERANCE_MIN] = {
		.ops = &imxcam_ctrl_ops,
		.id = V4L2_CID_IMX_FIM_TOLERANCE_MIN,
		.name = "FIM Tolerance Min",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.def = FIM_CL_TOLERANCE_MIN_DEF,
		.min =    2,
		.max =  200,
		.step =   1,
	},
	[FIM_CL_TOLERANCE_MAX] = {
		.ops = &imxcam_ctrl_ops,
		.id = V4L2_CID_IMX_FIM_TOLERANCE_MAX,
		.name = "FIM Tolerance Max",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.def = FIM_CL_TOLERANCE_MAX_DEF,
		.min =    0,
		.max =  500,
		.step =   1,
	},
	[FIM_CL_NUM_SKIP] = {
		.ops = &imxcam_ctrl_ops,
		.id = V4L2_CID_IMX_FIM_NUM_SKIP,
		.name = "FIM Num Skip",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.def = FIM_CL_NUM_SKIP_DEF,
		.min =   1, /* skip 1 frame */
		.max = 256, /* skip 256 frames */
		.step =  1,
	},
};

/*
 * the adv7182 has the most controls with 27, so add 32
 * on top of our own
 */
#define IMXCAM_NUM_CONTROLS (IMXCAM_NUM_STD_CONTROLS    + \
			     IMXCAM_NUM_CUSTOM_CONTROLS + \
			     FIM_NUM_CONTROLS + 32)

static int imxcam_init_controls(struct imxcam_dev *dev)
{
	struct v4l2_ctrl_handler *hdlr = &dev->ctrl_hdlr;
	struct imxcam_fim *fim = &dev->fim;
	const struct v4l2_ctrl_config *c;
	struct v4l2_ctrl_config fim_c;
	int i, ret;

	v4l2_ctrl_handler_init(hdlr, IMXCAM_NUM_CONTROLS);

	for (i = 0; i < IMXCAM_NUM_STD_CONTROLS; i++) {
		c = &imxcam_std_ctrl[i];

		v4l2_ctrl_new_std(hdlr, &imxcam_ctrl_ops,
				  c->id, c->min, c->max, c->step, c->def);
	}

	for (i = 0; i < IMXCAM_NUM_CUSTOM_CONTROLS; i++) {
		c = &imxcam_custom_ctrl[i];

		v4l2_ctrl_new_custom(hdlr, c, NULL);
	}

	for (i = 0; i < FIM_NUM_CONTROLS; i++) {
		fim_c = imxcam_fim_ctrl[i];
		fim_c.def = fim->of_defaults[i];
		fim->ctrl[i] = v4l2_ctrl_new_custom(hdlr, &fim_c, NULL);
	}

	if (hdlr->error) {
		ret = hdlr->error;
		v4l2_ctrl_handler_free(hdlr);
		return ret;
	}

	v4l2_ctrl_cluster(FIM_NUM_CONTROLS, fim->ctrl);

	dev->v4l2_dev.ctrl_handler = hdlr;
	dev->vfd->ctrl_handler = hdlr;

	return 0;
}

/*
 * Video ioctls follow
 */

static int vidioc_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	strncpy(cap->driver, DEVICE_NAME, sizeof(cap->driver) - 1);
	strncpy(cap->card, DEVICE_NAME, sizeof(cap->card) - 1);
	cap->bus_info[0] = 0;
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static int vidioc_enum_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	struct imxcam_pixfmt *fmt;

	if (f->index >= NUM_FORMATS)
		return -EINVAL;

	fmt = &imxcam_pixformats[f->index];
	strncpy(f->description, fmt->name, sizeof(f->description) - 1);
	f->pixelformat = fmt->fourcc;
	return 0;
}

static int vidioc_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct imxcam_dev *dev = ctx->dev;

	f->fmt.pix = dev->user_fmt.fmt.pix;
	return 0;
}

static int vidioc_try_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct imxcam_dev *dev = ctx->dev;
	struct v4l2_subdev_pad_config pad_cfg;
	struct v4l2_subdev_format format;
	struct imxcam_pixfmt *fmt;
	unsigned int width_align;
	struct v4l2_rect crop;
	int ret;

	fmt = imxcam_get_format(f->fmt.pix.pixelformat, 0);
	if (!fmt) {
		v4l2_err(&dev->v4l2_dev,
			 "Fourcc format (0x%08x) invalid.\n",
			 f->fmt.pix.pixelformat);
		return -EINVAL;
	}

	/*
	 * simple IDMAC interleaving using ILO field doesn't work
	 * when combined with the 16-bit planar formats (YUV422P
	 * and NV16). This looks like a silicon bug, no satisfactory
	 * replies to queries about it from Freescale. So workaround
	 * the issue by forcing the formats to the 12-bit planar versions.
	 */
	if (V4L2_FIELD_HAS_BOTH(dev->sensor_fmt.field) &&
	    dev->motion == MOTION_NONE) {
		switch (fmt->fourcc) {
		case V4L2_PIX_FMT_YUV422P:
			v4l2_info(&dev->v4l2_dev,
				  "ILO workaround: YUV422P forced to YUV420\n");
			f->fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
			break;
		case V4L2_PIX_FMT_NV16:
			v4l2_info(&dev->v4l2_dev,
				  "ILO workaround: NV16 forced to NV12\n");
			f->fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
			break;
		default:
			break;
		}
		fmt = imxcam_get_format(f->fmt.pix.pixelformat, 0);
	}

	/*
	 * We have to adjust the width such that the physaddrs and U and
	 * U and V plane offsets are multiples of 8 bytes as required by
	 * the IPU DMA Controller. For the planar formats, this corresponds
	 * to a pixel alignment of 16. For all the packed formats, 8 is
	 * good enough.
	 *
	 * For height alignment, we have to ensure that the heights
	 * are multiples of 8 lines, to satisfy the requirement of the
	 * IRT (the IRT performs rotations on 8x8 blocks at a time).
	 */
	width_align = ipu_pixelformat_is_planar(fmt->fourcc) ? 4 : 3;

	v4l_bound_align_image(&f->fmt.pix.width, MIN_W, MAX_W,
			      width_align, &f->fmt.pix.height,
			      MIN_H, MAX_H, H_ALIGN, S_ALIGN);

	format.which = V4L2_SUBDEV_FORMAT_TRY;
	format.pad = 0;
	v4l2_fill_mbus_format(&format.format, &f->fmt.pix, 0);
	ret = v4l2_subdev_call(dev->sensor->sd, pad, set_fmt, &pad_cfg, &format);
	if (ret)
		return ret;

	fmt = imxcam_get_format(0, pad_cfg.try_fmt.code);
	if (!fmt) {
		v4l2_err(&dev->v4l2_dev,
			 "Sensor mbus format (0x%08x) invalid\n",
			 pad_cfg.try_fmt.code);
		return -EINVAL;
	}

	/*
	 * calculate what the optimal crop window will be for this
	 * sensor format and make any user format adjustments.
	 */
	calc_default_crop(dev, &crop, &pad_cfg.try_fmt, dev->current_std);
	adjust_user_fmt(dev, &pad_cfg.try_fmt, f, &crop);

	/* this driver only delivers progressive frames to userland */
	f->fmt.pix.field = V4L2_FIELD_NONE;

	return 0;
}

static int vidioc_s_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct imxcam_dev *dev = ctx->dev;
	struct v4l2_subdev_format format;
	int ret;

	if (vb2_is_busy(&dev->buffer_queue)) {
		v4l2_err(&dev->v4l2_dev, "%s queue busy\n", __func__);
		return -EBUSY;
	}

	ret = vidioc_try_fmt_vid_cap(file, priv, f);
	if (ret)
		return ret;

	format.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	format.pad = 0;
	v4l2_fill_mbus_format(&format.format, &f->fmt.pix, 0);
	ret = v4l2_subdev_call(dev->sensor->sd, pad, set_fmt, NULL, &format);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "%s set_fmt failed\n", __func__);
		return ret;
	}

	ret = update_sensor_fmt(dev);
	if (ret)
		return ret;

	dev->user_fmt = *f;
	dev->user_pixfmt = imxcam_get_format(f->fmt.pix.pixelformat, 0);

	return 0;
}

static int vidioc_enum_framesizes(struct file *file, void *priv,
				  struct v4l2_frmsizeenum *fsize)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct imxcam_dev *dev = ctx->dev;
	struct imxcam_pixfmt *fmt;
	struct v4l2_format uf;

	fmt = imxcam_get_format(fsize->pixel_format, 0);
	if (!fmt)
		return -EINVAL;

	if (fsize->index)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = MIN_W;
	fsize->stepwise.step_width =
		ipu_pixelformat_is_planar(fmt->fourcc) ? 16 : 8;
	fsize->stepwise.min_height = MIN_H;
	fsize->stepwise.step_height = 1 << H_ALIGN;

	uf = dev->user_fmt;
	uf.fmt.pix.pixelformat = fmt->fourcc;

	if (need_ic(dev, &dev->sensor_fmt, &uf, &dev->crop)) {
		fsize->stepwise.max_width = MAX_W_IC;
		fsize->stepwise.max_height = MAX_H_IC;
	} else {
		fsize->stepwise.max_width = MAX_W;
		fsize->stepwise.max_height = MAX_H;
	}

	return 0;
}

static int vidioc_enum_frameintervals(struct file *file, void *priv,
				      struct v4l2_frmivalenum *fival)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct imxcam_dev *dev = ctx->dev;
	struct imxcam_pixfmt *fmt;
	struct v4l2_subdev_frame_interval_enum fie = {
		.index = fival->index,
		.pad = 0,
		.width = fival->width,
		.height = fival->height,
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	int ret;

	fmt = imxcam_get_format(fival->pixel_format, 0);
	if (!fmt)
		return -EINVAL;

	fie.code = fmt->codes[0];

	ret = v4l2_subdev_call(dev->sensor->sd, pad, enum_frame_interval,
			       NULL, &fie);
	if (ret)
		return ret;

	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fival->discrete = fie.interval;
	return 0;
}

static int vidioc_querystd(struct file *file, void *priv, v4l2_std_id *std)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct imxcam_dev *dev = ctx->dev;
	int ret;

	ret = update_sensor_std(dev);
	if (!ret)
		*std = dev->current_std;
	return ret;
}

static int vidioc_g_std(struct file *file, void *priv, v4l2_std_id *std)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct imxcam_dev *dev = ctx->dev;

	*std = dev->current_std;
	return 0;
}

static int vidioc_s_std(struct file *file, void *priv, v4l2_std_id std)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct imxcam_dev *dev = ctx->dev;
	int ret;

	if (vb2_is_busy(&dev->buffer_queue))
		return -EBUSY;

	ret = v4l2_subdev_call(dev->sensor->sd, video, s_std, std);
	if (ret < 0)
		return ret;

	dev->current_std = std;
	return 0;
}

static int vidioc_enum_input(struct file *file, void *priv,
			     struct v4l2_input *input)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct imxcam_dev *dev = ctx->dev;
	struct imxcam_sensor_input *sinput;
	struct imxcam_sensor *sensor;
	int sensor_input;

	/* find the sensor that is handling this input */
	sensor = find_sensor_by_input_index(dev, input->index);
	if (!sensor)
		return -EINVAL;

	sinput = &sensor->input;
	sensor_input = input->index - sinput->first;

	input->type = V4L2_INPUT_TYPE_CAMERA;
	input->capabilities = sinput->caps[sensor_input];
	strncpy(input->name, sinput->name[sensor_input], sizeof(input->name));

	if (input->index == dev->current_input) {
		v4l2_subdev_call(sensor->sd, video, g_input_status, &input->status);
		update_sensor_std(dev);
		input->std = dev->current_std;
	} else {
		input->status = V4L2_IN_ST_NO_SIGNAL;
		input->std = V4L2_STD_UNKNOWN;
	}

	return 0;
}

static int vidioc_g_input(struct file *file, void *priv, unsigned int *index)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct imxcam_dev *dev = ctx->dev;

	*index = dev->current_input;
	return 0;
}

static int vidioc_s_input(struct file *file, void *priv, unsigned int index)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct imxcam_dev *dev = ctx->dev;
	struct imxcam_sensor_input *sinput;
	struct imxcam_sensor *sensor;
	int ret, sensor_input, fim_actv;

	if (index == dev->current_input)
		return 0;

	/* find the sensor that is handling this input */
	sensor = find_sensor_by_input_index(dev, index);
	if (!sensor)
		return -EINVAL;

	if (dev->sensor != sensor) {
		/*
		 * don't allow switching sensors if there are queued buffers
		 * or there are other users of the current sensor besides us.
		 */
		if (vb2_is_busy(&dev->buffer_queue) ||
		    dev->sensor->power_count > 1)
			return -EBUSY;

		v4l2_info(&dev->v4l2_dev, "switching to sensor %s\n",
			  sensor->sd->name);

		/* power down current sensor before enabling new one */
		ret = sensor_set_power(dev, 0);
		if (ret)
			v4l2_warn(&dev->v4l2_dev, "sensor power off failed\n");

		/* set new sensor and the video mux(es) in the pipeline to it */
		dev->sensor = sensor;
		ret = imxcam_set_video_muxes(dev);
		if (ret)
			v4l2_warn(&dev->v4l2_dev, "set video muxes failed\n");

		/*
		 * turn on FIM if ADV718x is selected else turn off FIM
		 * for other sensors.
		 */
		if (strncasecmp(sensor->sd->name, "adv718", 6) == 0)
			fim_actv = 1;
		else
			fim_actv = 0;
		v4l2_ctrl_s_ctrl(dev->fim.ctrl[FIM_CL_ENABLE], fim_actv);

		/* power-on the new sensor */
		ret = sensor_set_power(dev, 1);
		if (ret)
			v4l2_warn(&dev->v4l2_dev, "sensor power on failed\n");
	}

	/* finally select the sensor's input */
	sinput = &sensor->input;
	sensor_input = index - sinput->first;
	ret = v4l2_subdev_call(sensor->sd, video, s_routing,
			       sinput->value[sensor_input], 0, 0);

	dev->current_input = index;

	/*
	 * Status update required if there is a change
	 * of inputs
	 */
	atomic_set(&dev->status_change, 1);

	return 0;
}

static int vidioc_g_parm(struct file *file, void *fh,
			 struct v4l2_streamparm *a)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct imxcam_dev *dev = ctx->dev;

	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	return v4l2_subdev_call(dev->sensor->sd, video, g_parm, a);
}

static int vidioc_s_parm(struct file *file, void *fh,
			 struct v4l2_streamparm *a)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct imxcam_dev *dev = ctx->dev;

	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	return v4l2_subdev_call(dev->sensor->sd, video, s_parm, a);
}

static int vidioc_g_selection(struct file *file, void *priv,
			      struct v4l2_selection *sel)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct imxcam_dev *dev = ctx->dev;

	if (sel->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
	case V4L2_SEL_TGT_COMPOSE:
		/*
		 * compose windows are not supported in this driver,
		 * compose window is same as user buffers from s_fmt.
		 */
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = dev->user_fmt.fmt.pix.width;
		sel->r.height = dev->user_fmt.fmt.pix.height;
		break;
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r = dev->crop_bounds;
		break;
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r = dev->crop_defrect;
		break;
	case V4L2_SEL_TGT_CROP:
		sel->r = dev->crop;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int vidioc_s_selection(struct file *file, void *priv,
			      struct v4l2_selection *sel)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct imxcam_dev *dev = ctx->dev;
	struct v4l2_rect *bounds = &dev->crop_bounds;

	if (sel->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
	    sel->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	if (vb2_is_busy(&dev->buffer_queue))
		return -EBUSY;

	/* make sure crop window is within bounds */
	if (sel->r.top < 0 || sel->r.left < 0 ||
	    sel->r.left + sel->r.width > bounds->width ||
	    sel->r.top + sel->r.height > bounds->height)
		return -EINVAL;

	/*
	 * FIXME: the IPU currently does not setup the CCIR code
	 * registers properly to handle arbitrary vertical crop
	 * windows. So return error if the sensor bus is BT.656
	 * and user is asking to change vertical cropping.
	 */
	if (dev->sensor->ep.bus_type == V4L2_MBUS_BT656 &&
	    (sel->r.top != dev->crop.top ||
	     sel->r.height != dev->crop.height)) {
		v4l2_err(&dev->v4l2_dev,
			 "vertical crop is not supported for this sensor!\n");
		return -EINVAL;
	}

	/* adjust crop window to h/w alignment restrictions */
	sel->r.width &= ~0x7;
	sel->r.left &= ~0x3;

	dev->crop = sel->r;

	/*
	 * Crop window has changed, we need to adjust the user
	 * width/height to meet new IC resizer restrictions or to
	 * match the new crop window if the IC can't be used.
	 */
	adjust_user_fmt(dev, &dev->sensor_fmt, &dev->user_fmt,
			&dev->crop);

	return 0;
}

static int vidioc_reqbufs(struct file *file, void *priv,
			  struct v4l2_requestbuffers *reqbufs)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct imxcam_dev *dev = ctx->dev;
	struct vb2_queue *vq = &dev->buffer_queue;
	unsigned long flags;
	int ret;

	if (vb2_is_busy(vq) || (dev->io_ctx && !is_io_ctx(ctx)))
		return -EBUSY;

	ctx->alloc_ctx = vb2_dma_contig_init_ctx(dev->dev);
	if (IS_ERR(ctx->alloc_ctx)) {
		v4l2_err(&dev->v4l2_dev, "failed to alloc vb2 context\n");
		return PTR_ERR(ctx->alloc_ctx);
	}

	INIT_LIST_HEAD(&ctx->ready_q);
	INIT_WORK(&ctx->restart_work, restart_work_handler);
	INIT_WORK(&ctx->stop_work, stop_work_handler);
	__init_timer(&ctx->restart_timer, TIMER_IRQSAFE);
	ctx->restart_timer.data = (unsigned long)ctx;
	ctx->restart_timer.function = imxcam_restart_timeout;

	vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	vq->drv_priv = ctx;
	vq->buf_struct_size = sizeof(struct imxcam_buffer);
	vq->ops = &imxcam_qops;
	vq->mem_ops = &vb2_dma_contig_memops;
	vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	ret = vb2_queue_init(vq);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "vb2_queue_init failed\n");
		goto alloc_ctx_free;
	}

	ret = vb2_reqbufs(vq, reqbufs);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "vb2_reqbufs failed\n");
		goto alloc_ctx_free;
	}

	spin_lock_irqsave(&dev->notify_lock, flags);
	dev->io_ctx = ctx;
	spin_unlock_irqrestore(&dev->notify_lock, flags);

	return 0;

alloc_ctx_free:
	vb2_dma_contig_cleanup_ctx(ctx->alloc_ctx);
	return ret;
}

static int vidioc_querybuf(struct file *file, void *priv,
			   struct v4l2_buffer *buf)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct vb2_queue *vq = &ctx->dev->buffer_queue;

	return vb2_querybuf(vq, buf);
}

static int vidioc_qbuf(struct file *file, void *priv, struct v4l2_buffer *buf)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct vb2_queue *vq = &ctx->dev->buffer_queue;

	if (!is_io_ctx(ctx))
		return -EBUSY;

	return vb2_qbuf(vq, buf);
}

static int vidioc_dqbuf(struct file *file, void *priv, struct v4l2_buffer *buf)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct vb2_queue *vq = &ctx->dev->buffer_queue;

	if (!is_io_ctx(ctx))
		return -EBUSY;

	return vb2_dqbuf(vq, buf, file->f_flags & O_NONBLOCK);
}

static int vidioc_expbuf(struct file *file, void *priv,
			 struct v4l2_exportbuffer *eb)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct vb2_queue *vq = &ctx->dev->buffer_queue;

	if (!is_io_ctx(ctx))
		return -EBUSY;

	return vb2_expbuf(vq, eb);
}

static int vidioc_streamon(struct file *file, void *priv,
			   enum v4l2_buf_type type)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct vb2_queue *vq = &ctx->dev->buffer_queue;

	if (!is_io_ctx(ctx))
		return -EBUSY;

	return vb2_streamon(vq, type);
}

static int vidioc_streamoff(struct file *file, void *priv,
			    enum v4l2_buf_type type)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct vb2_queue *vq = &ctx->dev->buffer_queue;

	if (!is_io_ctx(ctx))
		return -EBUSY;

	return vb2_streamoff(vq, type);
}

static const struct v4l2_ioctl_ops imxcam_ioctl_ops = {
	.vidioc_querycap	= vidioc_querycap,

	.vidioc_enum_fmt_vid_cap        = vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap           = vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap         = vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap           = vidioc_s_fmt_vid_cap,

	.vidioc_enum_framesizes         = vidioc_enum_framesizes,
	.vidioc_enum_frameintervals     = vidioc_enum_frameintervals,

	.vidioc_querystd        = vidioc_querystd,
	.vidioc_g_std           = vidioc_g_std,
	.vidioc_s_std           = vidioc_s_std,

	.vidioc_enum_input      = vidioc_enum_input,
	.vidioc_g_input         = vidioc_g_input,
	.vidioc_s_input         = vidioc_s_input,

	.vidioc_g_parm          = vidioc_g_parm,
	.vidioc_s_parm          = vidioc_s_parm,

	.vidioc_g_selection     = vidioc_g_selection,
	.vidioc_s_selection     = vidioc_s_selection,

	.vidioc_reqbufs		= vidioc_reqbufs,
	.vidioc_querybuf	= vidioc_querybuf,
	.vidioc_qbuf		= vidioc_qbuf,
	.vidioc_dqbuf		= vidioc_dqbuf,
	.vidioc_expbuf		= vidioc_expbuf,

	.vidioc_streamon	= vidioc_streamon,
	.vidioc_streamoff	= vidioc_streamoff,
};

/*
 * Queue operations
 */

static int imxcam_queue_setup(struct vb2_queue *vq,
			      unsigned int *nbuffers, unsigned int *nplanes,
			      unsigned int sizes[], void *alloc_ctxs[])
{
	struct imxcam_ctx *ctx = vb2_get_drv_priv(vq);
	struct imxcam_dev *dev = ctx->dev;
	unsigned int count = *nbuffers;
	u32 sizeimage = dev->user_fmt.fmt.pix.sizeimage;

	if (vq->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	while (sizeimage * count > VID_MEM_LIMIT)
		count--;

	*nplanes = 1;
	*nbuffers = count;
	sizes[0] = sizeimage;

	alloc_ctxs[0] = ctx->alloc_ctx;

	dprintk(dev, "get %d buffer(s) of size %d each.\n", count, sizeimage);

	return 0;
}

static int imxcam_buf_init(struct vb2_buffer *vb)
{
	struct imxcam_buffer *buf = to_imxcam_vb(vb);

	INIT_LIST_HEAD(&buf->list);
	return 0;
}

static int imxcam_buf_prepare(struct vb2_buffer *vb)
{
	struct imxcam_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct imxcam_dev *dev = ctx->dev;

	if (vb2_plane_size(vb, 0) < dev->user_fmt.fmt.pix.sizeimage) {
		v4l2_err(&dev->v4l2_dev,
			 "data will not fit into plane (%lu < %lu)\n",
			 vb2_plane_size(vb, 0),
			 (long)dev->user_fmt.fmt.pix.sizeimage);
		return -EINVAL;
	}

	vb2_set_plane_payload(vb, 0, dev->user_fmt.fmt.pix.sizeimage);

	return 0;
}

static void imxcam_buf_queue(struct vb2_buffer *vb)
{
	struct imxcam_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct imxcam_dev *dev = ctx->dev;
	struct imxcam_buffer *buf = to_imxcam_vb(vb);
	unsigned long flags;
	bool kickstart;

	spin_lock_irqsave(&dev->irqlock, flags);

	list_add_tail(&buf->list, &ctx->ready_q);

	/* kickstart DMA chain if we have two frames in active q */
	kickstart = (vb2_is_streaming(vb->vb2_queue) &&
		     !(list_empty(&ctx->ready_q) ||
		       list_is_singular(&ctx->ready_q)));

	spin_unlock_irqrestore(&dev->irqlock, flags);

	if (kickstart)
		start_encoder(dev);
}

static void imxcam_lock(struct vb2_queue *vq)
{
	struct imxcam_ctx *ctx = vb2_get_drv_priv(vq);
	struct imxcam_dev *dev = ctx->dev;

	mutex_lock(&dev->mutex);
}

static void imxcam_unlock(struct vb2_queue *vq)
{
	struct imxcam_ctx *ctx = vb2_get_drv_priv(vq);
	struct imxcam_dev *dev = ctx->dev;

	mutex_unlock(&dev->mutex);
}

static int imxcam_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct imxcam_ctx *ctx = vb2_get_drv_priv(vq);
	struct imxcam_buffer *buf, *tmp;
	int ret;

	if (vb2_is_streaming(vq))
		return 0;

	ctx->stop = false;

	ret = set_stream(ctx, true);
	if (ret)
		goto return_bufs;

	return 0;

return_bufs:
	list_for_each_entry_safe(buf, tmp, &ctx->ready_q, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb, VB2_BUF_STATE_QUEUED);
	}
	return ret;
}

static void imxcam_stop_streaming(struct vb2_queue *vq)
{
	struct imxcam_ctx *ctx = vb2_get_drv_priv(vq);
	struct imxcam_dev *dev = ctx->dev;
	struct imxcam_buffer *frame;
	unsigned long flags;

	if (!vb2_is_streaming(vq))
		return;

	/*
	 * signal that streaming is being stopped, so that the
	 * restart_work_handler() will skip unnecessary stream
	 * restarts, and to stop kicking the restart timer.
	 */
	ctx->stop = true;

	set_stream(ctx, false);

	spin_lock_irqsave(&dev->irqlock, flags);

	/* release all active buffers */
	while (!list_empty(&ctx->ready_q)) {
		frame = list_entry(ctx->ready_q.next,
				   struct imxcam_buffer, list);
		list_del(&frame->list);
		vb2_buffer_done(&frame->vb, VB2_BUF_STATE_ERROR);
	}

	spin_unlock_irqrestore(&dev->irqlock, flags);
}

static struct vb2_ops imxcam_qops = {
	.queue_setup	 = imxcam_queue_setup,
	.buf_init        = imxcam_buf_init,
	.buf_prepare	 = imxcam_buf_prepare,
	.buf_queue	 = imxcam_buf_queue,
	.wait_prepare	 = imxcam_unlock,
	.wait_finish	 = imxcam_lock,
	.start_streaming = imxcam_start_streaming,
	.stop_streaming  = imxcam_stop_streaming,
};

/*
 * File operations
 */
static int imxcam_open(struct file *file)
{
	struct imxcam_dev *dev = video_drvdata(file);
	struct imxcam_ctx *ctx;
	int ret;

	if (mutex_lock_interruptible(&dev->mutex))
		return -ERESTARTSYS;

	if (!dev->sensor || !dev->sensor->sd) {
		v4l2_err(&dev->v4l2_dev, "no subdevice registered\n");
		ret = -ENODEV;
		goto unlock;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto unlock;
	}

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	ctx->dev = dev;
	v4l2_fh_add(&ctx->fh);

	ret = sensor_set_power(dev, 1);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "sensor power on failed\n");
		goto ctx_free;
	}

	/* update the sensor's current lock status and format */
	update_signal_lock_status(dev);
	update_sensor_fmt(dev);

	mutex_unlock(&dev->mutex);
	return 0;

ctx_free:
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
unlock:
	mutex_unlock(&dev->mutex);
	return ret;
}

static int imxcam_release(struct file *file)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct imxcam_dev *dev = ctx->dev;
	unsigned long flags;
	int ret = 0;

	mutex_lock(&dev->mutex);

	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);

	if (is_io_ctx(ctx)) {
		vb2_queue_release(&dev->buffer_queue);
		vb2_dma_contig_cleanup_ctx(ctx->alloc_ctx);

		spin_lock_irqsave(&dev->notify_lock, flags);
		/* cancel any pending or scheduled restart timer */
		del_timer_sync(&ctx->restart_timer);
		dev->io_ctx = NULL;
		spin_unlock_irqrestore(&dev->notify_lock, flags);

		/*
		 * cancel any scheduled restart work, we have to release
		 * the dev->mutex in case it has already been scheduled.
		 */
		mutex_unlock(&dev->mutex);
		cancel_work_sync(&ctx->restart_work);
		mutex_lock(&dev->mutex);
	}

	if (!dev->sensor || !dev->sensor->sd) {
		v4l2_warn(&dev->v4l2_dev, "lost the slave?\n");
		goto free_ctx;
	}

	ret = sensor_set_power(dev, 0);
	if (ret)
		v4l2_err(&dev->v4l2_dev, "sensor power off failed\n");

free_ctx:
	kfree(ctx);
	mutex_unlock(&dev->mutex);
	return ret;
}

static unsigned int imxcam_poll(struct file *file,
				struct poll_table_struct *wait)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct imxcam_dev *dev = ctx->dev;
	struct vb2_queue *vq = &dev->buffer_queue;
	int ret;

	if (mutex_lock_interruptible(&dev->mutex))
		return -ERESTARTSYS;

	ret = vb2_poll(vq, file, wait);

	mutex_unlock(&dev->mutex);
	return ret;
}

static int imxcam_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct imxcam_ctx *ctx = file2ctx(file);
	struct imxcam_dev *dev = ctx->dev;
	struct vb2_queue *vq = &dev->buffer_queue;
	int ret;

	if (mutex_lock_interruptible(&dev->mutex))
		return -ERESTARTSYS;

	ret = vb2_mmap(vq, vma);

	mutex_unlock(&dev->mutex);
	return ret;
}

static const struct v4l2_file_operations imxcam_fops = {
	.owner		= THIS_MODULE,
	.open		= imxcam_open,
	.release	= imxcam_release,
	.poll		= imxcam_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= imxcam_mmap,
};

static struct video_device imxcam_videodev = {
	.name		= DEVICE_NAME,
	.fops		= &imxcam_fops,
	.ioctl_ops	= &imxcam_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release,
	.vfl_dir	= VFL_DIR_RX,
	.tvnorms	= V4L2_STD_NTSC | V4L2_STD_PAL | V4L2_STD_SECAM,
};

/*
 * Handle notifications from the subdevs.
 */
static void imxcam_subdev_notification(struct v4l2_subdev *sd,
				       unsigned int notification,
				       void *arg)
{
	struct imxcam_dev *dev;
	struct imxcam_ctx *ctx;
	struct v4l2_event *ev;
	unsigned long flags;

	if (!sd)
		return;

	dev = sd2dev(sd);

	spin_lock_irqsave(&dev->notify_lock, flags);

	ctx = dev->io_ctx;

	switch (notification) {
	case IMXCAM_NFB4EOF_NOTIFY:
		if (ctx && !ctx->stop)
			imxcam_bump_restart_timer(ctx);
		break;
	case IMXCAM_FRAME_INTERVAL_NOTIFY:
		if (ctx && !ctx->stop && !atomic_read(&dev->pending_restart))
			imxcam_bump_restart_timer(ctx);
		break;
	case IMXCAM_EOF_TIMEOUT_NOTIFY:
		if (ctx && !ctx->stop) {
			/*
			 * cancel a running restart timer since we are
			 * restarting now anyway
			 */
			del_timer_sync(&ctx->restart_timer);
			/* and restart now */
			schedule_work(&ctx->restart_work);
		}
		break;
	case V4L2_DEVICE_NOTIFY_EVENT:
		ev = (struct v4l2_event *)arg;
		if (ev && ev->type == V4L2_EVENT_SOURCE_CHANGE) {
			atomic_set(&dev->status_change, 1);
			if (ctx && !ctx->stop) {
				v4l2_warn(&dev->v4l2_dev,
					  "decoder status change\n");
				imxcam_bump_restart_timer(ctx);
			}
			/* send decoder status events to userspace */
			v4l2_event_queue(dev->vfd, ev);
		}
		break;
	}

	spin_unlock_irqrestore(&dev->notify_lock, flags);
}


static void imxcam_unregister_sync_subdevs(struct imxcam_dev *dev)
{
	if (!IS_ERR_OR_NULL(dev->smfc_sd))
		v4l2_device_unregister_subdev(dev->smfc_sd);

	if (!IS_ERR_OR_NULL(dev->prpenc_sd))
		v4l2_device_unregister_subdev(dev->prpenc_sd);

	if (!IS_ERR_OR_NULL(dev->vdic_sd))
		v4l2_device_unregister_subdev(dev->vdic_sd);
}

static int imxcam_register_sync_subdevs(struct imxcam_dev *dev)
{
	int ret;

	dev->smfc_sd = imxcam_smfc_init(dev);
	if (IS_ERR(dev->smfc_sd))
		return PTR_ERR(dev->smfc_sd);

	dev->prpenc_sd = imxcam_ic_prpenc_init(dev);
	if (IS_ERR(dev->prpenc_sd))
		return PTR_ERR(dev->prpenc_sd);

	dev->vdic_sd = imxcam_vdic_init(dev);
	if (IS_ERR(dev->vdic_sd))
		return PTR_ERR(dev->vdic_sd);

	ret = v4l2_device_register_subdev(&dev->v4l2_dev, dev->smfc_sd);
	if (ret < 0) {
		v4l2_err(&dev->v4l2_dev, "failed to register subdev %s\n",
			 dev->smfc_sd->name);
		goto unreg;
	}
	v4l2_info(&dev->v4l2_dev, "Registered subdev %s\n", dev->smfc_sd->name);

	ret = v4l2_device_register_subdev(&dev->v4l2_dev, dev->prpenc_sd);
	if (ret < 0) {
		v4l2_err(&dev->v4l2_dev, "failed to register subdev %s\n",
			 dev->prpenc_sd->name);
		goto unreg;
	}
	v4l2_info(&dev->v4l2_dev, "Registered subdev %s\n", dev->prpenc_sd->name);

	ret = v4l2_device_register_subdev(&dev->v4l2_dev, dev->vdic_sd);
	if (ret < 0) {
		v4l2_err(&dev->v4l2_dev, "failed to register subdev %s\n",
			 dev->vdic_sd->name);
		goto unreg;
	}
	v4l2_info(&dev->v4l2_dev, "Registered subdev %s\n", dev->vdic_sd->name);

	return 0;

unreg:
	imxcam_unregister_sync_subdevs(dev);
	return ret;
}

/* async subdev bound notifier */
static int imxcam_subdev_bound(struct v4l2_async_notifier *notifier,
			       struct v4l2_subdev *sd,
			       struct v4l2_async_subdev *asd)
{
	struct imxcam_dev *dev = notifier2dev(notifier);
	struct imxcam_sensor_input *sinput;
	struct imxcam_sensor *sensor;
	int i, ret = -EINVAL;

	if (dev->csi2_asd &&
	    sd->dev->of_node == dev->csi2_asd->match.of.node) {
		dev->csi2_sd = sd;
		ret = 0;
		goto out;
	}

	for (i = 0; i < dev->num_csi; i++) {
		if (dev->csi_asd[i] &&
		    sd->dev->of_node == dev->csi_asd[i]->match.of.node) {
			dev->csi_list[i] = sd;
			ret = 0;
			goto out;
		}
	}

	for (i = 0; i < dev->num_vidmux; i++) {
		if (dev->vidmux_asd[i] &&
		    sd->dev->of_node == dev->vidmux_asd[i]->match.of.node) {
			dev->vidmux_list[i] = sd;
			ret = 0;
			goto out;
		}
	}

	for (i = 0; i < dev->num_sensors; i++) {
		sensor = &dev->sensor_list[i];
		if (sensor->asd &&
		    sd->dev->of_node == sensor->asd->match.of.node) {
			sensor->sd = sd;

			/* set sensor input names if needed */
			sinput = &sensor->input;
			for (i = 0; i < sinput->num; i++) {
				if (strlen(sinput->name[i]))
					continue;
				snprintf(sinput->name[i],
					 sizeof(sinput->name[i]),
					 "%s-%d", sd->name, i);
			}

			ret = 0;
			break;
		}
	}

out:
	if (ret)
		v4l2_warn(&dev->v4l2_dev, "Received unknown subdev %s\n",
			  sd->name);
	else
		v4l2_info(&dev->v4l2_dev, "Registered subdev %s\n", sd->name);

	return ret;
}

/* async subdev complete notifier */
static int imxcam_probe_complete(struct v4l2_async_notifier *notifier)
{
	struct imxcam_dev *dev = notifier2dev(notifier);
	struct imxcam_sensor *sensor;
	int i, j, ret;

	/* assign CSI subdevs to every sensor */
	for (i = 0; i < dev->num_sensors; i++) {
		sensor = &dev->sensor_list[i];
		for (j = 0; j < dev->num_csi; j++) {
			if (sensor->csi_np == dev->csi_asd[j]->match.of.node) {
				sensor->csi_sd = dev->csi_list[j];
				break;
			}
		}
		if (j >= dev->num_csi) {
			v4l2_err(&dev->v4l2_dev,
				 "Failed to find a CSI for sensor %s\n",
				 sensor->sd->name);
			return -ENODEV;
		}
	}

	/* make default sensor the first in list */
	dev->sensor = &dev->sensor_list[0];

	/* setup our controls */
	ret = v4l2_ctrl_handler_setup(&dev->ctrl_hdlr);
	if (ret)
		goto free_ctrls;

	ret = video_register_device(dev->vfd, VFL_TYPE_GRABBER, 0);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register video device\n");
		goto free_ctrls;
	}

	ret = v4l2_device_register_subdev_nodes(&dev->v4l2_dev);
	if (ret)
		goto unreg;

	/* set video mux(es) in the pipeline to this sensor */
	ret = imxcam_set_video_muxes(dev);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to set video muxes\n");
		goto unreg;
	}

	dev->v4l2_dev.notify = imxcam_subdev_notification;

	v4l2_info(&dev->v4l2_dev, "Device registered as /dev/video%d\n",
		  dev->vfd->num);

	return 0;

unreg:
	video_unregister_device(dev->vfd);
free_ctrls:
	v4l2_ctrl_handler_free(&dev->ctrl_hdlr);
	return ret;
}

static int imxcam_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct imxcam_dev *dev;
	struct video_device *vfd;
	struct pinctrl *pinctrl;
	int ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->dev = &pdev->dev;
	mutex_init(&dev->mutex);
	spin_lock_init(&dev->irqlock);
	spin_lock_init(&dev->notify_lock);

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret)
		return ret;

	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);

	vfd = video_device_alloc();
	if (!vfd) {
		v4l2_err(&dev->v4l2_dev, "Failed to allocate video device\n");
		ret = -ENOMEM;
		goto unreg_dev;
	}

	*vfd = imxcam_videodev;
	vfd->lock = &dev->mutex;
	vfd->v4l2_dev = &dev->v4l2_dev;

	video_set_drvdata(vfd, dev);
	snprintf(vfd->name, sizeof(vfd->name), "%s", imxcam_videodev.name);
	dev->vfd = vfd;

	platform_set_drvdata(pdev, dev);

	/* Get any pins needed */
	pinctrl = devm_pinctrl_get_select_default(&pdev->dev);

	/* setup some defaults */
	dev->user_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dev->user_fmt.fmt.pix.width = 640;
	dev->user_fmt.fmt.pix.height = 480;
	dev->user_fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
	dev->user_fmt.fmt.pix.bytesperline = (640 * 12) >> 3;
	dev->user_fmt.fmt.pix.sizeimage =
		(480 * dev->user_fmt.fmt.pix.bytesperline);
	dev->user_pixfmt =
		imxcam_get_format(dev->user_fmt.fmt.pix.pixelformat, 0);
	dev->current_std = V4L2_STD_UNKNOWN;

	dev->sensor_set_stream = sensor_set_stream;

	ret = imxcam_of_parse(dev, node);
	if (ret)
		goto unreg_dev;

	if (dev->fim.icap_channel < 0)
		dev->fim.eof = fim_eof_handler;

	/* init our controls */
	ret = imxcam_init_controls(dev);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "init controls failed\n");
		goto unreg_dev;
	}

	ret = imxcam_register_sync_subdevs(dev);
	if (ret)
		goto unreg_dev;

	/* prepare the async subdev notifier and register it */
	dev->subdev_notifier.subdevs = dev->async_ptrs;
	dev->subdev_notifier.bound = imxcam_subdev_bound;
	dev->subdev_notifier.complete = imxcam_probe_complete;
	ret = v4l2_async_notifier_register(&dev->v4l2_dev,
					   &dev->subdev_notifier);
	if (ret)
		goto unreg_dev;

	return 0;

unreg_dev:
	v4l2_device_unregister(&dev->v4l2_dev);
	return ret;
}

static int imxcam_remove(struct platform_device *pdev)
{
	struct imxcam_dev *dev =
		(struct imxcam_dev *)platform_get_drvdata(pdev);

	v4l2_info(&dev->v4l2_dev, "Removing " DEVICE_NAME "\n");
	v4l2_ctrl_handler_free(&dev->ctrl_hdlr);
	v4l2_async_notifier_unregister(&dev->subdev_notifier);
	video_unregister_device(dev->vfd);
	imxcam_unregister_sync_subdevs(dev);
	v4l2_device_unregister(&dev->v4l2_dev);

	return 0;
}

static const struct of_device_id imxcam_dt_ids[] = {
	{ .compatible = "fsl,imx-video-capture" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imxcam_dt_ids);

static struct platform_driver imxcam_pdrv = {
	.probe		= imxcam_probe,
	.remove		= imxcam_remove,
	.driver		= {
		.name	= DEVICE_NAME,
		.owner	= THIS_MODULE,
		.of_match_table	= imxcam_dt_ids,
	},
};

module_platform_driver(imxcam_pdrv);

MODULE_DESCRIPTION("i.MX5/6 v4l2 capture driver");
MODULE_AUTHOR("Mentor Graphics Inc.");
MODULE_LICENSE("GPL");
