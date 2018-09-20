/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * dw-csi-plat.h
 *
 * Copyright(c) 2018-present, Synopsys, Inc. and/or its affiliates.
 * Luis Oliveira <Luis.Oliveira@synopsys.com>
 *
 */

#ifndef _DW_CSI_PLAT_H__
#define _DW_CSI_PLAT_H__

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/ratelimit.h>
#include <linux/reset.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/videodev2.h>
#include <linux/wait.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dv-timings.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "dw-mipi-csi.h"

#define CSI_HOST_NAME	"dw-mipi-csi"

/* Video formats supported by the MIPI CSI-2 */
const struct mipi_fmt dw_mipi_csi_formats[] = {
	{
		/* RAW 8 */
		.code = MEDIA_BUS_FMT_SBGGR8_1X8,
		.depth = 8,
	},
	{
		/* RAW 10 */
		.code = MEDIA_BUS_FMT_SBGGR10_2X8_PADHI_BE,
		.depth = 10,
	},
	{
		/* RGB 565 */
		.code = MEDIA_BUS_FMT_RGB565_2X8_BE,
		.depth = 16,
	},
	{
		/* BGR 565 */
		.code = MEDIA_BUS_FMT_RGB565_2X8_LE,
		.depth = 16,
	},
	{
		/* RGB 888 */
		.code = MEDIA_BUS_FMT_RGB888_2X12_LE,
		.depth = 24,
	},
	{
		/* BGR 888 */
		.code = MEDIA_BUS_FMT_RGB888_2X12_BE,
		.depth = 24,
	},
};

static inline struct mipi_csi_dev *sd_to_mipi_csi_dev(struct v4l2_subdev *sdev)
{
	return container_of(sdev, struct mipi_csi_dev, sd);
}

#endif	/* _DW_CSI_PLAT_H__ */
