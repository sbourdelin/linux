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

#ifndef __SUNXI_CSI_H__
#define __SUNXI_CSI_H__

#include <media/v4l2-device.h>
#include <media/v4l2-of.h>

#include "sunxi_video.h"

struct sunxi_csi;

/**
 * struct sunxi_csi_config - configs for sunxi csi
 * @pixelformat: v4l2 pixel format (V4L2_PIX_FMT_*)
 * @code:	media bus format code (MEDIA_BUS_FMT_*)
 * @field:	used interlacing type (enum v4l2_field)
 * @width:	frame width
 * @height:	frame height
 */
struct sunxi_csi_config {
	u32		pixelformat;
	u32		code;
	u32		field;
	u32		width;
	u32		height;
};

struct sunxi_csi_ops {
	int (*get_supported_pixformats)(struct sunxi_csi *csi,
					const u32 **pixformats);
	bool (*is_format_support)(struct sunxi_csi *csi, u32 pixformat,
				  u32 mbus_code);
	int (*s_power)(struct sunxi_csi *csi, bool enable);
	int (*update_config)(struct sunxi_csi *csi,
			     struct sunxi_csi_config *config);
	int (*update_buf_addr)(struct sunxi_csi *csi, dma_addr_t addr);
	int (*s_stream)(struct sunxi_csi *csi, bool enable);
};

struct sunxi_csi {
	struct device			*dev;
	struct v4l2_device		v4l2_dev;
	struct media_device		media_dev;

	struct list_head		entities;
	unsigned int			num_subdevs;
	struct v4l2_async_notifier	notifier;

	/* video port settings */
	struct v4l2_of_endpoint		v4l2_ep;

	struct sunxi_csi_config		config;

	struct sunxi_video		video;

	struct sunxi_csi_ops		*ops;
};

int sunxi_csi_init(struct sunxi_csi *csi);
int sunxi_csi_cleanup(struct sunxi_csi *csi);

/**
 * sunxi_csi_get_supported_pixformats() - get csi supported pixformats
 * @csi: 	pointer to the csi
 * @pixformats: supported pixformats return from csi
 *
 * @return the count of pixformats or error(< 0)
 */
static inline int
sunxi_csi_get_supported_pixformats(struct sunxi_csi *csi,
				   const u32 **pixformats)
{
	if (csi->ops != NULL && csi->ops->get_supported_pixformats != NULL)
		return csi->ops->get_supported_pixformats(csi, pixformats);

	return -ENOIOCTLCMD;
}

/**
 * sunxi_csi_is_format_support() - check if the format supported by csi
 * @csi: 	pointer to the csi
 * @pixformat:	v4l2 pixel format (V4L2_PIX_FMT_*)
 * @mbus_code:	media bus format code (MEDIA_BUS_FMT_*)
 */
static inline bool
sunxi_csi_is_format_support(struct sunxi_csi *csi, u32 pixformat, u32 mbus_code)
{
	if (csi->ops != NULL && csi->ops->is_format_support != NULL)
		return csi->ops->is_format_support(csi, pixformat, mbus_code);

	return -ENOIOCTLCMD;
}

/**
 * sunxi_csi_set_power() - power on/off the csi
 * @csi: 	pointer to the csi
 * @enable:	on/off
 */
static inline int sunxi_csi_set_power(struct sunxi_csi *csi, bool enable)
{
	if (csi->ops != NULL && csi->ops->s_power != NULL)
		return csi->ops->s_power(csi, enable);

	return -ENOIOCTLCMD;
}

/**
 * sunxi_csi_update_config() - update the csi register setttings
 * @csi: 	pointer to the csi
 * @config:	see struct sunxi_csi_config
 */
static inline int
sunxi_csi_update_config(struct sunxi_csi *csi, struct sunxi_csi_config *config)
{
	if (csi->ops != NULL && csi->ops->update_config != NULL)
		return csi->ops->update_config(csi, config);

	return -ENOIOCTLCMD;
}

/**
 * sunxi_csi_update_buf_addr() - update the csi frame buffer address
 * @csi: 	pointer to the csi
 * @addr:	frame buffer's physical address
 */
static inline int sunxi_csi_update_buf_addr(struct sunxi_csi *csi,
					    dma_addr_t addr)
{
	if (csi->ops != NULL && csi->ops->update_buf_addr != NULL)
		return csi->ops->update_buf_addr(csi, addr);

	return -ENOIOCTLCMD;
}

/**
 * sunxi_csi_set_stream() - start/stop csi streaming
 * @csi: 	pointer to the csi
 * @enable:	start/stop
 */
static inline int sunxi_csi_set_stream(struct sunxi_csi *csi, bool enable)
{
	if (csi->ops != NULL && csi->ops->s_stream != NULL)
		return csi->ops->s_stream(csi, enable);

	return -ENOIOCTLCMD;
}

static inline int v4l2_pixformat_get_bpp(unsigned int pixformat)
{
	switch (pixformat) {
	case V4L2_PIX_FMT_SBGGR8:
	case V4L2_PIX_FMT_SGBRG8:
	case V4L2_PIX_FMT_SGRBG8:
	case V4L2_PIX_FMT_SRGGB8:
		return 8;
	case V4L2_PIX_FMT_SBGGR10:
	case V4L2_PIX_FMT_SGBRG10:
	case V4L2_PIX_FMT_SGRBG10:
	case V4L2_PIX_FMT_SRGGB10:
		return 10;
	case V4L2_PIX_FMT_SBGGR12:
	case V4L2_PIX_FMT_SGBRG12:
	case V4L2_PIX_FMT_SGRBG12:
	case V4L2_PIX_FMT_SRGGB12:
	case V4L2_PIX_FMT_HM12:
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
	case V4L2_PIX_FMT_YUV420:
	case V4L2_PIX_FMT_YVU420:
		return 12;
	case V4L2_PIX_FMT_YUYV:
	case V4L2_PIX_FMT_YVYU:
	case V4L2_PIX_FMT_UYVY:
	case V4L2_PIX_FMT_VYUY:
	case V4L2_PIX_FMT_NV16:
	case V4L2_PIX_FMT_NV61:
	case V4L2_PIX_FMT_YUV422P:
		return 16;
	case V4L2_PIX_FMT_RGB24:
	case V4L2_PIX_FMT_BGR24:
		return 24;
	case V4L2_PIX_FMT_RGB32:
	case V4L2_PIX_FMT_BGR32:
		return 32;
	}

	return 0;
}

#endif /* __SUNXI_CSI_H__ */
