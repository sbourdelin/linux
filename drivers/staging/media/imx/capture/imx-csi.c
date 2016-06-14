/*
 * V4L2 Capture CSI Subdev for Freescale i.MX5/6 SOC
 *
 * Copyright (c) 2014-2016 Mentor Graphics Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-of.h>
#include <media/v4l2-ctrls.h>
#include <video/imx-ipu-v3.h>
#include "imx-camif.h"

struct csi_priv {
	struct device *dev;
	struct imxcam_dev *camif;
	struct v4l2_subdev sd;
	struct ipu_soc *ipu;
	struct ipu_csi *csi;
};

static inline struct csi_priv *sd_to_dev(struct v4l2_subdev *sdev)
{
	return container_of(sdev, struct csi_priv, sd);
}

/*
 * Update the CSI whole sensor and active windows, and initialize
 * the CSI interface and muxes.
 */
static void csi_setup(struct csi_priv *priv)
{
	struct imxcam_dev *camif = priv->camif;
	int vc_num = camif->sensor->csi_ep.base.id;
	bool is_csi2 = camif->sensor->ep.bus_type == V4L2_MBUS_CSI2;
	enum ipu_csi_dest dest;

	ipu_csi_set_window(priv->csi, &camif->crop);
	ipu_csi_init_interface(priv->csi, &camif->mbus_cfg,
			       &camif->sensor_fmt);
	if (is_csi2)
		ipu_csi_set_mipi_datatype(priv->csi, vc_num,
					  &camif->sensor_fmt);

	/* select either parallel or MIPI-CSI2 as input to our CSI */
	ipu_csi_set_src(priv->csi, vc_num, is_csi2);

	/* set CSI destination */
	if (camif->using_vdic && camif->vdic_direct)
		dest = IPU_CSI_DEST_VDIC;
	else if (camif->using_ic && !camif->using_vdic)
		dest = IPU_CSI_DEST_IC;
	else
		dest = IPU_CSI_DEST_IDMAC;
	ipu_csi_set_dest(priv->csi, dest);

	ipu_csi_dump(priv->csi);
}

static void csi_put_ipu_resources(struct csi_priv *priv)
{
	if (!IS_ERR_OR_NULL(priv->csi))
		ipu_csi_put(priv->csi);
	priv->csi = NULL;
}

static int csi_get_ipu_resources(struct csi_priv *priv)
{
	struct imxcam_dev *camif = priv->camif;
	int csi_id = camif->sensor->csi_ep.base.port;

	priv->ipu = dev_get_drvdata(priv->dev->parent);

	priv->csi = ipu_csi_get(priv->ipu, csi_id);
	if (IS_ERR(priv->csi)) {
		v4l2_err(&priv->sd, "failed to get CSI %d\n", csi_id);
		return PTR_ERR(priv->csi);
	}

	return 0;
}

static int csi_start(struct csi_priv *priv)
{
	int err;

	err = csi_get_ipu_resources(priv);
	if (err)
		return err;

	csi_setup(priv);

	err = ipu_csi_enable(priv->csi);
	if (err) {
		v4l2_err(&priv->sd, "CSI enable error: %d\n", err);
		goto out_put_ipu;
	}

	return 0;

out_put_ipu:
	csi_put_ipu_resources(priv);
	return err;
}

static int csi_stop(struct csi_priv *priv)
{
	ipu_csi_disable(priv->csi);

	csi_put_ipu_resources(priv);

	return 0;
}

static int csi_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct csi_priv *priv = v4l2_get_subdevdata(sd);

	if (!sd->v4l2_dev || !sd->v4l2_dev->dev)
		return -ENODEV;

	/* get imxcam host device */
	priv->camif = dev_get_drvdata(sd->v4l2_dev->dev);

	return enable ? csi_start(priv) : csi_stop(priv);
}

static struct v4l2_subdev_video_ops csi_video_ops = {
	.s_stream = csi_s_stream,
};

static struct v4l2_subdev_ops csi_subdev_ops = {
	.video = &csi_video_ops,
};

static int imxcam_csi_probe(struct platform_device *pdev)
{
	struct csi_priv *priv;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, &priv->sd);

	priv->dev = &pdev->dev;

	v4l2_subdev_init(&priv->sd, &csi_subdev_ops);
	v4l2_set_subdevdata(&priv->sd, priv);
	priv->sd.dev = &pdev->dev;
	priv->sd.owner = THIS_MODULE;
	strlcpy(priv->sd.name, "imx-camera-csi", sizeof(priv->sd.name));

	return v4l2_async_register_subdev(&priv->sd);
}

static int imxcam_csi_remove(struct platform_device *pdev)
{
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct csi_priv *priv = sd_to_dev(sd);

	v4l2_async_unregister_subdev(&priv->sd);
	v4l2_device_unregister_subdev(sd);

	return 0;
}

static const struct platform_device_id imxcam_csi_ids[] = {
	{ .name = "imx-ipuv3-csi" },
	{ },
};
MODULE_DEVICE_TABLE(platform, imxcam_csi_ids);

static struct platform_driver imxcam_csi_driver = {
	.probe = imxcam_csi_probe,
	.remove = imxcam_csi_remove,
	.id_table = imxcam_csi_ids,
	.driver = {
		.name = "imx-ipuv3-csi",
		.owner = THIS_MODULE,
	},
};
module_platform_driver(imxcam_csi_driver);

MODULE_AUTHOR("Mentor Graphics Inc.");
MODULE_DESCRIPTION("i.MX CSI subdev driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:imx-ipuv3-csi");
