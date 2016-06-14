/*
 * Video Capture driver for Freescale i.MX5/6 SOC
 *
 * Copyright (c) 2012-2016 Mentor Graphics Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef _IMX_CAMIF_H
#define _IMX_CAMIF_H

#define dprintk(dev, fmt, arg...)					\
	v4l2_dbg(1, 1, &dev->v4l2_dev, "%s: " fmt, __func__, ## arg)

/*
 * These numbers are somewhat arbitrary, but we need at least:
 * - 1 mipi-csi2 receiver subdev
 * - 2 video-mux subdevs
 * - 3 sensor subdevs (2 parallel, 1 mipi-csi2)
 * - 4 CSI subdevs
 */
#define IMXCAM_MAX_SUBDEVS       16
#define IMXCAM_MAX_SENSORS        8
#define IMXCAM_MAX_VIDEOMUX       4
#define IMXCAM_MAX_CSI            4

/*
 * How long before no EOF interrupts cause a stream restart, or a buffer
 * dequeue timeout, in msec. The dequeue timeout should be longer than
 * the EOF timeout.
 */
#define IMXCAM_EOF_TIMEOUT       1000
#define IMXCAM_DQ_TIMEOUT        5000

/*
 * How long to delay a restart on ADV718x status changes or NFB4EOF,
 * in msec.
 */
#define IMXCAM_RESTART_DELAY      200

/*
 * Internal subdev notifications
 */
#define IMXCAM_NFB4EOF_NOTIFY         _IO('6', 0)
#define IMXCAM_EOF_TIMEOUT_NOTIFY     _IO('6', 1)
#define IMXCAM_FRAME_INTERVAL_NOTIFY  _IO('6', 2)

/*
 * Frame Interval Monitor Control Indexes and default values
 */
enum {
	FIM_CL_ENABLE = 0,
	FIM_CL_NUM,
	FIM_CL_TOLERANCE_MIN,
	FIM_CL_TOLERANCE_MAX,
	FIM_CL_NUM_SKIP,
	FIM_NUM_CONTROLS,
};

#define FIM_CL_ENABLE_DEF      0 /* FIM disabled by default */
#define FIM_CL_NUM_DEF         8 /* average 8 frames */
#define FIM_CL_NUM_SKIP_DEF    8 /* skip 8 frames after restart */
#define FIM_CL_TOLERANCE_MIN_DEF  50 /* usec */
#define FIM_CL_TOLERANCE_MAX_DEF   0 /* no max tolerance (unbounded) */

struct imxcam_buffer {
	struct vb2_buffer vb; /* v4l buffer must be first */
	struct list_head  list;
};

static inline struct imxcam_buffer *to_imxcam_vb(struct vb2_buffer *vb)
{
	return container_of(vb, struct imxcam_buffer, vb);
}

struct imxcam_pixfmt {
	char	*name;
	u32	fourcc;
	u32     codes[4];
	int     bpp;     /* total bpp */
	int     y_depth; /* depth of first Y plane for planar formats */
};

struct imxcam_dma_buf {
	void          *virt;
	dma_addr_t     phys;
	unsigned long  len;
};

/*
 * A sensor's inputs parsed from v4l2_of_endpoint nodes in devicetree
 */
#define IMXCAM_MAX_INPUTS 16

struct imxcam_sensor_input {
	/* input values passed to s_routing */
	u32 value[IMXCAM_MAX_INPUTS];
	/* input capabilities (V4L2_IN_CAP_*) */
	u32 caps[IMXCAM_MAX_INPUTS];
	/* input names */
	char name[IMXCAM_MAX_INPUTS][32];

	/* number of inputs */
	int num;
	/* first and last input indexes from imxcam perspective */
	int first;
	int last;
};

struct imxcam_sensor {
	struct v4l2_subdev       *sd;
	struct v4l2_async_subdev *asd;
	struct v4l2_of_endpoint  ep;     /* sensor's endpoint info */

	/* csi node and subdev this sensor is connected to */
	struct device_node       *csi_np;
	struct v4l2_subdev       *csi_sd; 
	struct v4l2_of_endpoint  csi_ep; /* parsed endpoint info of csi port */

	struct imxcam_sensor_input input;

	/* input indeces of all video-muxes required to access this sensor */
	int vidmux_input[IMXCAM_MAX_VIDEOMUX];

	int power_count;                 /* power use counter */
	int stream_count;                /* stream use counter */
};

struct imxcam_ctx;
struct imxcam_dev;

/* frame interval monitor */
struct imxcam_fim {
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

	/*
	 * otherwise, the EOF method of measuring FI, called by
	 * streaming subdevs from eof irq
	 */
	int (*eof)(struct imxcam_dev *dev, struct timeval *now);
};

struct imxcam_dev {
	struct v4l2_device	v4l2_dev;
	struct video_device	*vfd;
	struct device           *dev;

	struct mutex		mutex;
	spinlock_t		irqlock;
	spinlock_t		notify_lock;

	/* buffer queue used in videobuf2 */
	struct vb2_queue        buffer_queue;

	/* v4l2 controls */
	struct v4l2_ctrl_handler ctrl_hdlr;
	int                      rotation; /* degrees */
	bool                     hflip;
	bool                     vflip;
	enum ipu_motion_sel      motion;

	/* derived from rotation, hflip, vflip controls */
	enum ipu_rotate_mode     rot_mode;

	struct imxcam_fim        fim;

	/* the format from sensor and from userland */
	struct v4l2_format        user_fmt;
	struct imxcam_pixfmt      *user_pixfmt;
	struct v4l2_mbus_framefmt sensor_fmt;
	struct v4l2_fract         sensor_tpf;
	struct imxcam_pixfmt      *sensor_pixfmt;
	struct v4l2_mbus_config   mbus_cfg;

	/*
	 * the crop rectangle (from s_crop) specifies the crop dimensions
	 * and position over the raw capture frame boundaries.
	 */
	struct v4l2_rect        crop_bounds;
	struct v4l2_rect        crop_defrect;
	struct v4l2_rect        crop;

	/* misc status */
	int                     current_input; /* the current input */
	v4l2_std_id             current_std;   /* current video standard */
	atomic_t                status_change; /* sensor status change */
	atomic_t                pending_restart; /* a restart is pending */
	bool                    signal_locked; /* sensor signal lock */
	bool                    encoder_on;    /* encode is on */
	bool                    using_ic;      /* IC is being used for encode */
	bool                    using_vdic;    /* VDIC is used for encode */
	bool                    vdic_direct;   /* VDIC is using the direct
						  CSI->VDIC pipeline */

	/* master descriptor list for async subdev registration */
	struct v4l2_async_subdev async_desc[IMXCAM_MAX_SUBDEVS];
	struct v4l2_async_subdev *async_ptrs[IMXCAM_MAX_SUBDEVS];

	/* for async subdev registration */
	struct v4l2_async_notifier subdev_notifier;

	/* camera sensor subdev list */
	struct imxcam_sensor    sensor_list[IMXCAM_MAX_SENSORS];
	struct imxcam_sensor    *sensor; /* the current active sensor */
	int                     num_sensor_inputs;
	int                     num_sensors;

	/* mipi-csi2 receiver subdev */
	struct v4l2_subdev      *csi2_sd;
	struct v4l2_async_subdev *csi2_asd;

	/* CSI subdev list */
	struct v4l2_subdev      *csi_list[IMXCAM_MAX_CSI];
	struct v4l2_async_subdev *csi_asd[IMXCAM_MAX_CSI];
	int                     num_csi;

	/* video-mux subdev list */
	struct v4l2_subdev      *vidmux_list[IMXCAM_MAX_VIDEOMUX];
	struct v4l2_async_subdev *vidmux_asd[IMXCAM_MAX_VIDEOMUX];
	int                     num_vidmux;

	/* synchronous prpenc, smfc, and vdic subdevs */
	struct v4l2_subdev      *smfc_sd;
	struct v4l2_subdev      *prpenc_sd;
	struct v4l2_subdev      *vdic_sd;

	int (*sensor_set_stream)(struct imxcam_dev *dev, int on);

	/*
	 * the current open context that is doing IO (there can only
	 * be one allowed IO context at a time).
	 */
	struct imxcam_ctx       *io_ctx;
};

struct imxcam_ctx {
	struct v4l2_fh          fh;
	struct imxcam_dev       *dev;

	struct vb2_alloc_ctx    *alloc_ctx;

	/* streaming buffer queue */
	struct list_head        ready_q;

	/* stream stop and restart handling */
	struct work_struct      restart_work;
	struct work_struct      stop_work;
	struct timer_list       restart_timer;
	bool                    stop; /* streaming is stopping */
};

struct v4l2_subdev *imxcam_smfc_init(struct imxcam_dev *dev);
struct v4l2_subdev *imxcam_ic_prpenc_init(struct imxcam_dev *dev);
struct v4l2_subdev *imxcam_vdic_init(struct imxcam_dev *dev);

#endif /* _IMX_CAMIF_H */
