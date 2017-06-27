/*
 * Copyright (c) 2017 Yong Deng <yong.deng@magewell.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __SUNXI_VIDEO_H__
#define __SUNXI_VIDEO_H__

#include <media/v4l2-dev.h>
#include <media/videobuf2-core.h>

/*
 * struct sunxi_csi_format - CSI media bus format information
 * @fourcc: Fourcc code for this format
 * @mbus_code: V4L2 media bus format code.
 * @bpp: Bytes per pixel (when stored in memory)
 */
struct sunxi_csi_format {
	u32				fourcc;
	u32				mbus_code;
	u8				bpp;
};

struct sunxi_csi;

struct sunxi_video {
	struct video_device		vdev;
	struct media_pad		pad;
	struct sunxi_csi		*csi;

	struct mutex			lock;

	struct vb2_queue		vb2_vidq;
	spinlock_t			dma_queue_lock;
	struct list_head		dma_queue;

	struct sunxi_csi_buffer		*cur_frm;
	unsigned int			sequence;

	struct sunxi_csi_format		*formats;
	unsigned int			num_formats;
	struct sunxi_csi_format		*current_fmt;
	struct v4l2_format		fmt;
};

int sunxi_video_init(struct sunxi_video *video, struct sunxi_csi *csi,
		     const char *name);
void sunxi_video_cleanup(struct sunxi_video *video);

void sunxi_video_frame_done(struct sunxi_video *video);

#endif /* __SUNXI_VIDEO_H__ */
