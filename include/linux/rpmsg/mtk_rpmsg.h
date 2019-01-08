/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2018 Google LLC.
 */

#ifndef __LINUX_RPMSG_MTK_RPMSG_H
#define __LINUX_RPMSG_MTK_RPMSG_H

#include <linux/device.h>
#include <linux/remoteproc.h>

#if IS_ENABLED(CONFIG_RPMSG_MTK_SCP)

struct rproc_subdev *
mtk_rpmsg_create_rproc_subdev(struct platform_device *scp_pdev,
			      struct rproc *scp_rproc);

void mtk_rpmsg_destroy_rproc_subdev(struct rproc_subdev *subdev);

#else

static inline struct rproc_subdev *
mtk_rpmsg_create_rproc_subdev(struct platform_device *scp_pdev,
			      struct rproc *scp_rproc)
{
	return NULL;
}

static inline void mtk_rpmsg_destroy_rproc_subdev(struct rproc_subdev *subdev)
{
}

#endif

#endif
