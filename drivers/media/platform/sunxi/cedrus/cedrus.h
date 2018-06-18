/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Sunxi-Cedrus VPU driver
 *
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 *
 * Based on the vim2m driver, that is:
 *
 * Copyright (c) 2009-2010 Samsung Electronics Co., Ltd.
 * Pawel Osciak, <pawel@osciak.com>
 * Marek Szyprowski, <m.szyprowski@samsung.com>
 */

#ifndef _CEDRUS_H_
#define _CEDRUS_H_

#include <linux/platform_device.h>

#include <media/videobuf2-v4l2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>

#define CEDRUS_NAME	"cedrus"

enum cedrus_control_id {
	CEDRUS_CTRL_DEC_MPEG2_SLICE_HEADER = 0,
	CEDRUS_CTRL_MAX,
};

struct cedrus_control {
	u32	id;
	u32	elem_size;
};

struct cedrus_fmt {
	u32		fourcc;
	int		depth;
	u32		types;
	unsigned int	num_planes;
};

struct cedrus_mpeg2_run {
	const struct v4l2_ctrl_mpeg2_slice_header	*hdr;
};

struct cedrus_run {
	struct vb2_v4l2_buffer	*src;
	struct vb2_v4l2_buffer	*dst;

	union {
		struct cedrus_mpeg2_run	mpeg2;
	};
};

struct cedrus_ctx {
	struct v4l2_fh			fh;
	struct cedrus_dev		*dev;

	struct cedrus_fmt		*vpu_src_fmt;
	struct v4l2_pix_format_mplane	src_fmt;
	struct cedrus_fmt		*vpu_dst_fmt;
	struct v4l2_pix_format_mplane	dst_fmt;

	struct v4l2_ctrl_handler	hdl;
	struct v4l2_ctrl		*ctrls[CEDRUS_CTRL_MAX];

	struct vb2_buffer		*dst_bufs[VIDEO_MAX_FRAME];

	int				job_abort;

	struct work_struct		try_schedule_work;
	struct work_struct		run_work;
	struct list_head		src_list;
	struct list_head		dst_list;
};

struct cedrus_buffer {
	struct vb2_v4l2_buffer		vb;
	enum vb2_buffer_state		state;
	struct list_head		list;
};

struct cedrus_dev {
	struct v4l2_device	v4l2_dev;
	struct video_device	vfd;
	struct media_device	mdev;
	struct media_pad	pad[2];
	struct platform_device	*pdev;
	struct device		*dev;
	struct v4l2_m2m_dev	*m2m_dev;

	/* Device file mutex */
	struct mutex		dev_mutex;
	/* Interrupt spinlock */
	spinlock_t		irq_lock;

	void __iomem		*base;

	struct clk		*mod_clk;
	struct clk		*ahb_clk;
	struct clk		*ram_clk;

	struct reset_control	*rstc;
};

static inline void cedrus_write(struct cedrus_dev *dev, u32 reg, u32 val)
{
	writel(val, dev->base + reg);
}

static inline u32 cedrus_read(struct cedrus_dev *dev, u32 reg)
{
	return readl(dev->base + reg);
}

#endif
