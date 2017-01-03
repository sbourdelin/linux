/*
 * Frame Interval Monitor.
 *
 * Copyright (c) 2016 Mentor Graphics Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#ifdef CONFIG_IMX_GPT_ICAP
#include <linux/mxc_icap.h>
#endif
#include <media/v4l2-subdev.h>
#include <media/v4l2-of.h>
#include <media/v4l2-ctrls.h>
#include <media/imx.h>
#include "imx-media.h"

enum {
	FIM_CL_ENABLE = 0,
	FIM_CL_NUM,
	FIM_CL_TOLERANCE_MIN,
	FIM_CL_TOLERANCE_MAX,
	FIM_CL_NUM_SKIP,
	FIM_NUM_CONTROLS,
};

#define FIM_CL_ENABLE_DEF          0 /* FIM disabled by default */
#define FIM_CL_NUM_DEF             8 /* average 8 frames */
#define FIM_CL_NUM_SKIP_DEF        2 /* skip 2 frames after restart */
#define FIM_CL_TOLERANCE_MIN_DEF  50 /* usec */
#define FIM_CL_TOLERANCE_MAX_DEF   0 /* no max tolerance (unbounded) */

struct imx_media_fim {
	struct imx_media_dev *md;

	/* the owning subdev of this fim instance */
	struct v4l2_subdev *sd;

	/* FIM's control handler */
	struct v4l2_ctrl_handler ctrl_handler;

	/* control cluster */
	struct v4l2_ctrl  *ctrl[FIM_NUM_CONTROLS];

	/* default ctrl values parsed from device tree */
	u32               of_defaults[FIM_NUM_CONTROLS];

	/* current control values */
	bool              enabled;
	int               num_avg;
	int               num_skip;
	unsigned long     tolerance_min; /* usec */
	unsigned long     tolerance_max; /* usec */

	int               counter;
	struct timespec   last_ts;
	unsigned long     sum;       /* usec */
	unsigned long     nominal;   /* usec */

	/*
	 * input capture method of measuring FI (channel and flags
	 * from device tree)
	 */
	int               icap_channel;
	int               icap_flags;
	struct completion icap_first_event;
};

static void update_fim_nominal(struct imx_media_fim *fim,
			       struct imx_media_subdev *sensor)
{
	struct v4l2_streamparm parm;
	struct v4l2_fract tpf;
	int ret;

	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = v4l2_subdev_call(sensor->sd, video, g_parm, &parm);
	tpf = parm.parm.capture.timeperframe;

	if (ret || tpf.denominator == 0) {
		dev_dbg(fim->sd->dev, "no tpf from sensor, FIM disabled\n");
		fim->enabled = false;
		return;
	}

	fim->nominal = DIV_ROUND_CLOSEST(1000 * 1000 * tpf.numerator,
					 tpf.denominator);

	dev_dbg(fim->sd->dev, "sensor FI=%lu usec\n", fim->nominal);
}

static void reset_fim(struct imx_media_fim *fim, bool curval)
{
	struct v4l2_ctrl *en = fim->ctrl[FIM_CL_ENABLE];
	struct v4l2_ctrl *num = fim->ctrl[FIM_CL_NUM];
	struct v4l2_ctrl *skip = fim->ctrl[FIM_CL_NUM_SKIP];
	struct v4l2_ctrl *tol_min = fim->ctrl[FIM_CL_TOLERANCE_MIN];
	struct v4l2_ctrl *tol_max = fim->ctrl[FIM_CL_TOLERANCE_MAX];

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
}

static void send_fim_event(struct imx_media_fim *fim, unsigned long error)
{
	static const struct v4l2_event ev = {
		.type = V4L2_EVENT_IMX_FRAME_INTERVAL,
	};

	v4l2_subdev_notify_event(fim->sd, &ev);
}

/*
 * Monitor an averaged frame interval. If the average deviates too much
 * from the sensor's nominal frame rate, send the frame interval error
 * event. The frame intervals are averaged in order to quiet noise from
 * (presumably random) interrupt latency.
 */
static void frame_interval_monitor(struct imx_media_fim *fim,
				   struct timespec *ts)
{
	unsigned long interval, error, error_avg;
	struct timespec diff;
	bool send_event = false;

	if (!fim->enabled || ++fim->counter <= 0)
		goto out_update_ts;

	diff = timespec_sub(*ts, fim->last_ts);
	interval = diff.tv_sec * 1000 * 1000 + diff.tv_nsec / 1000;
	error = abs(interval - fim->nominal);

	if (fim->tolerance_max && error >= fim->tolerance_max) {
		dev_dbg(fim->sd->dev,
			"FIM: %lu ignored, out of tolerance bounds\n",
			error);
		fim->counter--;
		goto out_update_ts;
	}

	fim->sum += error;

	if (fim->counter == fim->num_avg) {
		error_avg = DIV_ROUND_CLOSEST(fim->sum, fim->num_avg);

		if (error_avg > fim->tolerance_min)
			send_event = true;

		dev_dbg(fim->sd->dev, "FIM: error: %lu usec%s\n",
			error_avg, send_event ? " (!!!)" : "");

		fim->counter = 0;
		fim->sum = 0;
	}

out_update_ts:
	fim->last_ts = *ts;
	if (send_event)
		send_fim_event(fim, error_avg);
}

#ifdef CONFIG_IMX_GPT_ICAP
/*
 * Input Capture method of measuring frame intervals. Not subject
 * to interrupt latency.
 */
static void fim_input_capture_handler(int channel, void *dev_id,
				      struct timespec *ts)
{
	struct imx_media_fim *fim = dev_id;

	frame_interval_monitor(fim, ts);

	if (!completion_done(&fim->icap_first_event))
		complete(&fim->icap_first_event);
}

static int fim_request_input_capture(struct imx_media_fim *fim)
{
	init_completion(&fim->icap_first_event);

	return mxc_request_input_capture(fim->icap_channel,
					 fim_input_capture_handler,
					 fim->icap_flags, fim);
}

static void fim_free_input_capture(struct imx_media_fim *fim)
{
	mxc_free_input_capture(fim->icap_channel, fim);
}

#else /* CONFIG_IMX_GPT_ICAP */

static int fim_request_input_capture(struct imx_media_fim *fim)
{
	return 0;
}

static void fim_free_input_capture(struct imx_media_fim *fim)
{
}

#endif /* CONFIG_IMX_GPT_ICAP */

/*
 * In case we are monitoring the first frame interval after streamon
 * (when fim->num_skip = 0), we need a valid fim->last_ts before we
 * can begin. This only applies to the input capture method. It is not
 * possible to accurately measure the first FI after streamon using the
 * EOF method, so fim->num_skip minimum is set to 1 in that case, so this
 * function is a noop when the EOF method is used.
 */
static void fim_acquire_first_ts(struct imx_media_fim *fim)
{
	unsigned long ret;

	if (!fim->enabled || fim->num_skip > 0)
		return;

	ret = wait_for_completion_timeout(
		&fim->icap_first_event,
		msecs_to_jiffies(IMX_MEDIA_EOF_TIMEOUT));
	if (ret == 0)
		v4l2_warn(fim->sd, "wait first icap event timeout\n");
}

/* FIM Controls */
static int fim_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx_media_fim *fim = container_of(ctrl->handler,
						 struct imx_media_fim,
						 ctrl_handler);

	switch (ctrl->id) {
	case V4L2_CID_IMX_FIM_ENABLE:
		reset_fim(fim, false);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops fim_ctrl_ops = {
	.s_ctrl = fim_s_ctrl,
};

static const struct v4l2_ctrl_config imx_media_fim_ctrl[] = {
	[FIM_CL_ENABLE] = {
		.ops = &fim_ctrl_ops,
		.id = V4L2_CID_IMX_FIM_ENABLE,
		.name = "FIM Enable",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.def = FIM_CL_ENABLE_DEF,
		.min = 0,
		.max = 1,
		.step = 1,
	},
	[FIM_CL_NUM] = {
		.ops = &fim_ctrl_ops,
		.id = V4L2_CID_IMX_FIM_NUM,
		.name = "FIM Num Average",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.def = FIM_CL_NUM_DEF,
		.min =  1, /* no averaging */
		.max = 64, /* average 64 frames */
		.step = 1,
	},
	[FIM_CL_TOLERANCE_MIN] = {
		.ops = &fim_ctrl_ops,
		.id = V4L2_CID_IMX_FIM_TOLERANCE_MIN,
		.name = "FIM Tolerance Min",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.def = FIM_CL_TOLERANCE_MIN_DEF,
		.min =    2,
		.max =  200,
		.step =   1,
	},
	[FIM_CL_TOLERANCE_MAX] = {
		.ops = &fim_ctrl_ops,
		.id = V4L2_CID_IMX_FIM_TOLERANCE_MAX,
		.name = "FIM Tolerance Max",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.def = FIM_CL_TOLERANCE_MAX_DEF,
		.min =    0,
		.max =  500,
		.step =   1,
	},
	[FIM_CL_NUM_SKIP] = {
		.ops = &fim_ctrl_ops,
		.id = V4L2_CID_IMX_FIM_NUM_SKIP,
		.name = "FIM Num Skip",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.def = FIM_CL_NUM_SKIP_DEF,
		.min =   0, /* skip no frames */
		.max = 256, /* skip 256 frames */
		.step =  1,
	},
};

static int init_fim_controls(struct imx_media_fim *fim)
{
	struct v4l2_ctrl_handler *hdlr = &fim->ctrl_handler;
	struct v4l2_ctrl_config fim_c;
	int i, ret;

	v4l2_ctrl_handler_init(hdlr, FIM_NUM_CONTROLS);

	for (i = 0; i < FIM_NUM_CONTROLS; i++) {
		fim_c = imx_media_fim_ctrl[i];
		fim_c.def = fim->of_defaults[i];

		/*
		 * it's not possible to accurately measure the first
		 * FI after streamon using the EOF method, so force
		 * num_skip minimum to 1 in that case.
		 */
		if (i == FIM_CL_NUM_SKIP && fim->icap_channel < 0)
			fim_c.min = 1;

		fim->ctrl[i] = v4l2_ctrl_new_custom(hdlr, &fim_c, NULL);
	}

	if (hdlr->error) {
		ret = hdlr->error;
		goto err_free;
	}

	v4l2_ctrl_cluster(FIM_NUM_CONTROLS, fim->ctrl);

	/* add the FIM controls to the calling subdev ctrl handler */
	ret = v4l2_ctrl_add_handler(fim->sd->ctrl_handler,
				    &fim->ctrl_handler, NULL);
	if (ret)
		goto err_free;

	return 0;
err_free:
	v4l2_ctrl_handler_free(hdlr);
	return ret;
}

static int of_parse_fim(struct imx_media_fim *fim, struct device_node *np)
{
	struct device_node *fim_np;
	u32 val, tol[2], icap[2];
	int ret;

	fim_np = of_get_child_by_name(np, "fim");
	if (!fim_np) {
		/* set to the default defaults */
		fim->of_defaults[FIM_CL_ENABLE] = FIM_CL_ENABLE_DEF;
		fim->of_defaults[FIM_CL_NUM] = FIM_CL_NUM_DEF;
		fim->of_defaults[FIM_CL_NUM_SKIP] = FIM_CL_NUM_SKIP_DEF;
		fim->of_defaults[FIM_CL_TOLERANCE_MIN] =
			FIM_CL_TOLERANCE_MIN_DEF;
		fim->of_defaults[FIM_CL_TOLERANCE_MAX] =
			FIM_CL_TOLERANCE_MAX_DEF;
		fim->icap_channel = -1;
		return 0;
	}

	ret = of_property_read_u32(fim_np, "enable", &val);
	if (ret)
		val = FIM_CL_ENABLE_DEF;
	fim->of_defaults[FIM_CL_ENABLE] = val;

	ret = of_property_read_u32(fim_np, "num-avg", &val);
	if (ret)
		val = FIM_CL_NUM_DEF;
	fim->of_defaults[FIM_CL_NUM] = val;

	ret = of_property_read_u32(fim_np, "num-skip", &val);
	if (ret)
		val = FIM_CL_NUM_SKIP_DEF;
	fim->of_defaults[FIM_CL_NUM_SKIP] = val;

	ret = of_property_read_u32_array(fim_np, "tolerance-range", tol, 2);
	if (ret) {
		tol[0] = FIM_CL_TOLERANCE_MIN_DEF;
		tol[1] = FIM_CL_TOLERANCE_MAX_DEF;
	}
	fim->of_defaults[FIM_CL_TOLERANCE_MIN] = tol[0];
	fim->of_defaults[FIM_CL_TOLERANCE_MAX] = tol[1];

	fim->icap_channel = -1;
	if (IS_ENABLED(CONFIG_IMX_GPT_ICAP)) {
		ret = of_property_read_u32_array(fim_np,
						 "input-capture-channel",
						 icap, 2);
		if (!ret) {
			fim->icap_channel = icap[0];
			fim->icap_flags = icap[1];
		}
	}

	of_node_put(fim_np);
	return 0;
}

/*
 * Called by the subdevs that interface directly with the CSI,
 * in their EOF interrupt handlers with their irqlock held. This
 * way of measuring frame intervals is subject to uncertainty errors
 * introduced by interrupt latency.
 *
 * This is a noop if the Input Capture method is being used, since
 * the frame_interval_monitor() is called by the input capture event
 * callback handler in that case.
 */
void imx_media_fim_eof_monitor(struct imx_media_fim *fim, struct timespec *ts)
{
	if (fim->icap_channel >= 0)
		return;

	frame_interval_monitor(fim, ts);
}
EXPORT_SYMBOL_GPL(imx_media_fim_eof_monitor);

/* Called by the subdev in its s_power callback */
int imx_media_fim_set_power(struct imx_media_fim *fim, bool on)
{
	int ret = 0;

	if (fim->icap_channel >= 0) {
		if (on)
			ret = fim_request_input_capture(fim);
		else
			fim_free_input_capture(fim);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(imx_media_fim_set_power);

/* Called by the subdev in its s_stream callback */
int imx_media_fim_set_stream(struct imx_media_fim *fim,
			     struct imx_media_subdev *sensor,
			     bool on)
{
	if (on) {
		reset_fim(fim, true);
		update_fim_nominal(fim, sensor);

		if (fim->icap_channel >= 0)
			fim_acquire_first_ts(fim);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(imx_media_fim_set_stream);

/* Called by the subdev in its subdev registered callback */
struct imx_media_fim *imx_media_fim_init(struct v4l2_subdev *sd)
{
	struct device_node *node = sd->of_node;
	struct imx_media_fim *fim;
	int ret;

	fim = devm_kzalloc(sd->dev, sizeof(*fim), GFP_KERNEL);
	if (!fim)
		return ERR_PTR(-ENOMEM);

	/* get media device */
	fim->md = dev_get_drvdata(sd->v4l2_dev->dev);
	fim->sd = sd;

	ret = of_parse_fim(fim, node);
	if (ret)
		return ERR_PTR(ret);

	ret = init_fim_controls(fim);
	if (ret)
		return ERR_PTR(ret);

	return fim;
}
EXPORT_SYMBOL_GPL(imx_media_fim_init);

void imx_media_fim_free(struct imx_media_fim *fim)
{
	v4l2_ctrl_handler_free(&fim->ctrl_handler);
}
EXPORT_SYMBOL_GPL(imx_media_fim_free);
