/*
 * DWC MIPI CSI-2 Host device driver
 *
 * Copyright (C) 2016 Synopsys, Inc. All rights reserved.
 * Author: Ramiro Oliveira <ramiro.oliveira@synopsys.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation, either version 2 of the License,
 * or (at your option) any later version.
 */


#include "dw_mipi_csi.h"

/** @short Driver args: debug */
static unsigned int debug;
module_param(debug, uint, 0644);
MODULE_PARM_DESC(debug, "Activates debug info");

/*
 * Basic IO read and write operations
 */

/**
 * @short Video formats supported by the MIPI CSI-2
 */
static const struct mipi_fmt dw_mipi_csi_formats[] = {
	{
	 .name = "RAW BGGR 8",
	 .code = MEDIA_BUS_FMT_SBGGR8_1X8,
	 .depth = 8,
	 },
	{
	 .name = "RAW10",
	 .code = MEDIA_BUS_FMT_SBGGR10_2X8_PADHI_BE,
	 .depth = 10,
	 },
	{
	 .name = "RGB565",
	 .code = MEDIA_BUS_FMT_RGB565_2X8_BE,
	 .depth = 16,
	 },
	{
	 .name = "BGR565",
	 .code = MEDIA_BUS_FMT_RGB565_2X8_LE,
	 .depth = 16,
	 },
	{
	 .name = "RGB888",
	 .code = MEDIA_BUS_FMT_RGB888_2X12_LE,
	 .depth = 24,
	 },
	{
	 .name = "BGR888",
	 .code = MEDIA_BUS_FMT_RGB888_2X12_BE,
	 .depth = 24,
	 },
};

static struct mipi_csi_dev *
sd_to_mipi_csi_dev(struct v4l2_subdev *sdev)
{
	return container_of(sdev, struct mipi_csi_dev, sd);
}

void
dw_mipi_csi_write(struct mipi_csi_dev *dev,
		  unsigned int address, unsigned int data)
{
	iowrite32(data, dev->base_address + address);
}

u32
dw_mipi_csi_read(struct mipi_csi_dev *dev, unsigned long address)
{
	return ioread32(dev->base_address + address);
}

void
dw_mipi_csi_write_part(struct mipi_csi_dev *dev,
		       unsigned long address, unsigned long data,
		       unsigned char shift, unsigned char width)
{
	u32 mask = (1 << width) - 1;
	u32 temp = dw_mipi_csi_read(dev, address);

	temp &= ~(mask << shift);
	temp |= (data & mask) << shift;
	dw_mipi_csi_write(dev, address, temp);
}

static const struct mipi_fmt *
find_dw_mipi_csi_format(struct v4l2_mbus_framefmt *mf)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dw_mipi_csi_formats); i++)
		if (mf->code == dw_mipi_csi_formats[i].code)
			return &dw_mipi_csi_formats[i];
	return NULL;
}

int
enable_video_output(struct mipi_csi_dev *dev)
{
	return 0;
}

int
disable_video_output(struct mipi_csi_dev *dev)
{
	phy_power_off(dev->phy);
	return 0;
}

static void
dw_mipi_csi_reset(struct mipi_csi_dev *dev)
{
	dw_mipi_csi_write(dev, R_CSI2_CTRL_RESETN, 0);
	/* mdelay(1); */
	dw_mipi_csi_write(dev, R_CSI2_CTRL_RESETN, 1);
}

static int
dw_mipi_csi_mask_irq_power_off(struct mipi_csi_dev *dev)
{
	/* set only one lane (lane 0) as active (ON) */
	dw_mipi_csi_write(dev, R_CSI2_N_LANES, 0);

	dw_mipi_csi_write(dev, R_CSI2_MASK_INT_PHY_FATAL, 0);
	dw_mipi_csi_write(dev, R_CSI2_MASK_INT_PKT_FATAL, 0);
	dw_mipi_csi_write(dev, R_CSI2_MASK_INT_FRAME_FATAL, 0);
	dw_mipi_csi_write(dev, R_CSI2_MASK_INT_PHY, 0);
	dw_mipi_csi_write(dev, R_CSI2_MASK_INT_PKT, 0);
	dw_mipi_csi_write(dev, R_CSI2_MASK_INT_LINE, 0);
	dw_mipi_csi_write(dev, R_CSI2_MASK_INT_IPI, 0);

	dw_mipi_csi_write(dev, R_CSI2_CTRL_RESETN, 0);

	return 0;

}

static int
dw_mipi_csi_hw_stdby(struct mipi_csi_dev *dev)
{
	/* set only one lane (lane 0) as active (ON) */
	dw_mipi_csi_reset(dev);

	dw_mipi_csi_write(dev, R_CSI2_N_LANES, 0);

	phy_init(dev->phy);

	dw_mipi_csi_write(dev, R_CSI2_MASK_INT_PHY_FATAL, 0xFFFFFFFF);
	dw_mipi_csi_write(dev, R_CSI2_MASK_INT_PKT_FATAL, 0xFFFFFFFF);
	dw_mipi_csi_write(dev, R_CSI2_MASK_INT_FRAME_FATAL, 0xFFFFFFFF);
	dw_mipi_csi_write(dev, R_CSI2_MASK_INT_PHY, 0xFFFFFFFF);
	dw_mipi_csi_write(dev, R_CSI2_MASK_INT_PKT, 0xFFFFFFFF);
	dw_mipi_csi_write(dev, R_CSI2_MASK_INT_LINE, 0xFFFFFFFF);
	dw_mipi_csi_write(dev, R_CSI2_MASK_INT_IPI, 0xFFFFFFFF);

	return 0;

}

void
dw_mipi_csi_set_ipi_fmt(struct mipi_csi_dev *dev)
{
	switch (dev->fmt->code) {
	case MEDIA_BUS_FMT_RGB565_2X8_BE:
	case MEDIA_BUS_FMT_RGB565_2X8_LE:
		dw_mipi_csi_write(dev, R_CSI2_IPI_DATA_TYPE, CSI_2_RGB565);
		v4l2_dbg(1, debug, &dev->sd, "DT: RGB 565");
		break;

	case MEDIA_BUS_FMT_RGB888_2X12_LE:
	case MEDIA_BUS_FMT_RGB888_2X12_BE:
		dw_mipi_csi_write(dev, R_CSI2_IPI_DATA_TYPE, CSI_2_RGB888);
		v4l2_dbg(1, debug, &dev->sd, "DT: RGB 888");
		break;
	case MEDIA_BUS_FMT_SBGGR10_2X8_PADHI_BE:
		dw_mipi_csi_write(dev, R_CSI2_IPI_DATA_TYPE, CSI_2_RAW10);
		v4l2_dbg(1, debug, &dev->sd, "DT: RAW 10");
		break;
	case MEDIA_BUS_FMT_SBGGR8_1X8:
		dw_mipi_csi_write(dev, R_CSI2_IPI_DATA_TYPE, CSI_2_RAW8);
		v4l2_dbg(1, debug, &dev->sd, "DT: RAW 8");
		break;
	default:
		dw_mipi_csi_write(dev, R_CSI2_IPI_DATA_TYPE, CSI_2_RGB565);
		v4l2_dbg(1, debug, &dev->sd, "Error");
		break;
	}
}

void
__dw_mipi_csi_fill_timings(struct mipi_csi_dev *dev,
			   const struct v4l2_bt_timings *bt)
{

	if (bt == NULL)
		return;

	dev->hw.hsa = bt->hsync;
	dev->hw.hbp = bt->hbackporch;
	dev->hw.hsd = bt->hsync;
	dev->hw.htotal = bt->height + bt->vfrontporch +
	    bt->vsync + bt->vbackporch;
	dev->hw.vsa = bt->vsync;
	dev->hw.vbp = bt->vbackporch;
	dev->hw.vfp = bt->vfrontporch;
	dev->hw.vactive = bt->height;
}

void
dw_mipi_csi_start(struct mipi_csi_dev *dev)
{
	const struct v4l2_bt_timings *bt = &v4l2_dv_timings_presets[0].bt;

	__dw_mipi_csi_fill_timings(dev, bt);

	dw_mipi_csi_write(dev, R_CSI2_N_LANES, (dev->hw.num_lanes - 1));
	v4l2_dbg(1, debug, &dev->sd, "N Lanes: %d\n", dev->hw.num_lanes);

	/*IPI Related Configuration */
	if ((dev->hw.output_type == IPI_OUT)
	    || (dev->hw.output_type == BOTH_OUT)) {

		dw_mipi_csi_write_part(dev, R_CSI2_IPI_MODE, dev->hw.ipi_mode,
				       0, 1);
		v4l2_dbg(1, debug, &dev->sd, "IPI MODE: %d\n",
			 dev->hw.ipi_mode);

		dw_mipi_csi_write_part(dev, R_CSI2_IPI_MODE,
				       dev->hw.ipi_color_mode, 8, 1);
		v4l2_dbg(1, debug, &dev->sd, "Color Mode: %d\n",
			 dev->hw.ipi_color_mode);

		dw_mipi_csi_write(dev, R_CSI2_IPI_VCID, dev->hw.virtual_ch);
		v4l2_dbg(1, debug, &dev->sd, "Virtual Channel: %d\n",
			 dev->hw.virtual_ch);

		dw_mipi_csi_write_part(dev, R_CSI2_IPI_MEM_FLUSH,
				       dev->hw.ipi_auto_flush, 8, 1);
		v4l2_dbg(1, debug, &dev->sd, "Auto-flush: %d\n",
			 dev->hw.ipi_auto_flush);

		dw_mipi_csi_write(dev, R_CSI2_IPI_HSA_TIME, dev->hw.hsa);
		v4l2_dbg(1, debug, &dev->sd, "HSA: %d\n", dev->hw.hsa);

		dw_mipi_csi_write(dev, R_CSI2_IPI_HBP_TIME, dev->hw.hbp);
		v4l2_dbg(1, debug, &dev->sd, "HBP: %d\n", dev->hw.hbp);

		dw_mipi_csi_write(dev, R_CSI2_IPI_HSD_TIME, dev->hw.hsd);
		v4l2_dbg(1, debug, &dev->sd, "HSD: %d\n", dev->hw.hsd);

		if (dev->hw.ipi_mode == AUTO_TIMING) {
			dw_mipi_csi_write(dev, R_CSI2_IPI_HLINE_TIME,
					  dev->hw.htotal);
			v4l2_dbg(1, debug, &dev->sd, "H total: %d\n",
				 dev->hw.htotal);

			dw_mipi_csi_write(dev, R_CSI2_IPI_VSA_LINES,
					  dev->hw.vsa);
			v4l2_dbg(1, debug, &dev->sd, "VSA: %d\n", dev->hw.vsa);

			dw_mipi_csi_write(dev, R_CSI2_IPI_VBP_LINES,
					  dev->hw.vbp);
			v4l2_dbg(1, debug, &dev->sd, "VBP: %d\n", dev->hw.vbp);

			dw_mipi_csi_write(dev, R_CSI2_IPI_VFP_LINES,
					  dev->hw.vfp);
			v4l2_dbg(1, debug, &dev->sd, "VFP: %d\n", dev->hw.vfp);

			dw_mipi_csi_write(dev, R_CSI2_IPI_VACTIVE_LINES,
					  dev->hw.vactive);
			v4l2_dbg(1, debug, &dev->sd, "V Active: %d\n",
				 dev->hw.vactive);
		}
	}

	phy_power_on(dev->phy);
}

static int
dw_mipi_csi_enum_mbus_code(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(dw_mipi_csi_formats))
		return -EINVAL;

	code->code = dw_mipi_csi_formats[code->index].code;
	return 0;
}

static struct mipi_fmt const *
dw_mipi_csi_try_format(struct v4l2_mbus_framefmt *mf)
{
	struct mipi_fmt const *fmt;

	fmt = find_dw_mipi_csi_format(mf);
	if (fmt == NULL)
		fmt = &dw_mipi_csi_formats[0];

	mf->code = fmt->code;
	return fmt;
}

static struct v4l2_mbus_framefmt *
__dw_mipi_csi_get_format(struct mipi_csi_dev *dev,
			 struct v4l2_subdev_pad_config *cfg,
			 enum v4l2_subdev_format_whence which)
{
	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return cfg ? v4l2_subdev_get_try_format(&dev->sd, cfg,
							0) : NULL;

	return &dev->format;
}

static int
dw_mipi_csi_set_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_pad_config *cfg,
		    struct v4l2_subdev_format *fmt)
{
	struct mipi_csi_dev *dev = sd_to_mipi_csi_dev(sd);
	struct mipi_fmt const *dev_fmt;
	struct v4l2_mbus_framefmt *mf;
	int i = 0;
	const struct v4l2_bt_timings *bt_r = &v4l2_dv_timings_presets[0].bt;

	mf = __dw_mipi_csi_get_format(dev, cfg, fmt->which);

	dev_fmt = dw_mipi_csi_try_format(&fmt->format);
	if (dev_fmt) {
		*mf = fmt->format;
		if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
			dev->fmt = dev_fmt;
		dw_mipi_csi_set_ipi_fmt(dev);
	}
	while (v4l2_dv_timings_presets[i].bt.width) {
		const struct v4l2_bt_timings *bt =
		    &v4l2_dv_timings_presets[i].bt;
		if (mf->width == bt->width && mf->height == bt->width) {
			__dw_mipi_csi_fill_timings(dev, bt);
			return 0;
		}
		i++;
	}

	__dw_mipi_csi_fill_timings(dev, bt_r);
	return 0;

}

static int
dw_mipi_csi_get_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_pad_config *cfg,
		    struct v4l2_subdev_format *fmt)
{
	struct mipi_csi_dev *dev = sd_to_mipi_csi_dev(sd);
	struct v4l2_mbus_framefmt *mf;

	mf = __dw_mipi_csi_get_format(dev, cfg, fmt->which);
	if (!mf)
		return -EINVAL;

	mutex_lock(&dev->lock);
	fmt->format = *mf;
	mutex_unlock(&dev->lock);
	return 0;
}

static int
dw_mipi_csi_s_power(struct v4l2_subdev *sd, int on)
{
	struct mipi_csi_dev *dev = sd_to_mipi_csi_dev(sd);

	if (on) {
		dw_mipi_csi_hw_stdby(dev);
		dw_mipi_csi_start(dev);
	} else {
		dw_mipi_csi_mask_irq_power_off(dev);
	}

	return 0;
}

static int
dw_mipi_csi_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct v4l2_mbus_framefmt *format =
	    v4l2_subdev_get_try_format(sd, fh->pad, 0);

	format->colorspace = V4L2_COLORSPACE_SRGB;
	format->code = dw_mipi_csi_formats[0].code;
	format->width = MIN_WIDTH;
	format->height = MIN_HEIGHT;
	format->field = V4L2_FIELD_NONE;

	return 0;
}

static const struct v4l2_subdev_internal_ops dw_mipi_csi_sd_internal_ops = {
	.open = dw_mipi_csi_open,
};

static struct v4l2_subdev_core_ops dw_mipi_csi_core_ops = {
	.s_power = dw_mipi_csi_s_power,
};

static struct v4l2_subdev_pad_ops dw_mipi_csi_pad_ops = {
	.enum_mbus_code = dw_mipi_csi_enum_mbus_code,
	.get_fmt = dw_mipi_csi_get_fmt,
	.set_fmt = dw_mipi_csi_set_fmt,
};

static struct v4l2_subdev_ops dw_mipi_csi_subdev_ops = {
	.core = &dw_mipi_csi_core_ops,
	.pad = &dw_mipi_csi_pad_ops,
};

static irqreturn_t
dw_mipi_csi_irq1(int irq, void *dev_id)
{

	struct mipi_csi_dev *dev = dev_id;
	u32 int_status, ind_status;
	unsigned long flags;

	int_status = dw_mipi_csi_read(dev, R_CSI2_INTERRUPT);
	spin_lock_irqsave(&dev->slock, flags);

	if (int_status & CSI2_INT_PHY_FATAL) {
		ind_status = dw_mipi_csi_read(dev, R_CSI2_INT_PHY_FATAL);
		pr_info_ratelimited("%08X CSI INT PHY FATAL: %08X\n",
				    (uint32_t) dev->base_address, ind_status);
	}

	if (int_status & CSI2_INT_PKT_FATAL) {
		ind_status = dw_mipi_csi_read(dev, R_CSI2_INT_PKT_FATAL);
		pr_info_ratelimited("%08X CSI INT PKT FATAL: %08X\n",
				    (uint32_t) dev->base_address, ind_status);
	}

	if (int_status & CSI2_INT_FRAME_FATAL) {
		ind_status = dw_mipi_csi_read(dev, R_CSI2_INT_FRAME_FATAL);
		pr_info_ratelimited("%08X CSI INT FRAME FATAL: %08X\n",
				    (uint32_t) dev->base_address, ind_status);
	}

	if (int_status & CSI2_INT_PHY) {
		ind_status = dw_mipi_csi_read(dev, R_CSI2_INT_PHY);
		pr_info_ratelimited("%08X CSI INT PHY: %08X\n",
				    (uint32_t) dev->base_address, ind_status);
	}

	if (int_status & CSI2_INT_PKT) {
		ind_status = dw_mipi_csi_read(dev, R_CSI2_INT_PKT);
		pr_info_ratelimited("%08X CSI INT PKT: %08X\n",
				    (uint32_t) dev->base_address, ind_status);
	}

	if (int_status & CSI2_INT_LINE) {
		ind_status = dw_mipi_csi_read(dev, R_CSI2_INT_LINE);
		pr_info_ratelimited("%08X CSI INT LINE: %08X\n",
				    (uint32_t) dev->base_address, ind_status);
	}

	if (int_status & CSI2_INT_IPI) {
		ind_status = dw_mipi_csi_read(dev, R_CSI2_INT_IPI);
		pr_info_ratelimited("%08X CSI INT IPI: %08X\n",
				    (uint32_t) dev->base_address, ind_status);
	}
	spin_unlock_irqrestore(&dev->slock, flags);
	return IRQ_HANDLED;
}

static int
dw_mipi_csi_parse_dt(struct platform_device *pdev, struct mipi_csi_dev *dev)
{
	struct device_node *node = pdev->dev.of_node;
	int reg;
	int ret = 0;

	/* Device tree information */
	ret = of_property_read_u32(node, "data-lanes", &dev->hw.num_lanes);
	if (ret) {
		dev_err(&pdev->dev, "Couldn't read data-lanes\n");
		return ret;
	}

	ret = of_property_read_u32(node, "output-type", &dev->hw.output_type);
	if (ret) {
		dev_err(&pdev->dev, "Couldn't read output-type\n");
		return ret;
	}

	ret = of_property_read_u32(node, "ipi-mode", &dev->hw.ipi_mode);
	if (ret) {
		dev_err(&pdev->dev, "Couldn't read ipi-mode\n");
		return ret;
	}

	ret =
	    of_property_read_u32(node, "ipi-auto-flush",
				 &dev->hw.ipi_auto_flush);
	if (ret) {
		dev_err(&pdev->dev, "Couldn't read ipi-auto-flush\n");
		return ret;
	}

	ret =
	    of_property_read_u32(node, "ipi-color-mode",
				 &dev->hw.ipi_color_mode);
	if (ret) {
		dev_err(&pdev->dev, "Couldn't read ipi-color-mode\n");
		return ret;
	}

	ret =
	    of_property_read_u32(node, "virtual-channel", &dev->hw.virtual_ch);
	if (ret) {
		dev_err(&pdev->dev, "Couldn't read virtual-channel\n");
		return ret;
	}

	node = of_get_child_by_name(node, "port");
	if (!node)
		return -EINVAL;

	ret = of_property_read_u32(node, "reg", &reg);
	if (ret) {
		dev_err(&pdev->dev, "Couldn't read reg value\n");
		return ret;
	}
	dev->index = reg - 1;

	if (dev->index >= CSI_MAX_ENTITIES)
		return -ENXIO;

	return 0;
}

static const struct of_device_id dw_mipi_csi_of_match[];

/**
 * @short Initialization routine - Entry point of the driver
 * @param[in] pdev pointer to the platform device structure
 * @return 0 on success and a negative number on failure
 * Refer to Linux errors.
 */
static int mipi_csi_probe(struct platform_device *pdev)
{
	const struct of_device_id *of_id;
	struct device *dev = &pdev->dev;
	struct resource *res = NULL;
	struct mipi_csi_dev *mipi_csi;
	int ret = -ENOMEM;

	dev_info(&pdev->dev, "Installing MIPI CSI-2 module\n");

	dev_dbg(&pdev->dev, "Device registration\n");
	mipi_csi = devm_kzalloc(dev, sizeof(*mipi_csi), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	mutex_init(&mipi_csi->lock);
	spin_lock_init(&mipi_csi->slock);
	mipi_csi->pdev = pdev;

	of_id = of_match_node(dw_mipi_csi_of_match, dev->of_node);
	if (WARN_ON(of_id == NULL))
		return -EINVAL;

	ret = dw_mipi_csi_parse_dt(pdev, mipi_csi);
	if (ret < 0)
		return ret;

	dev_info(dev, "Request DPHY\n");
	mipi_csi->phy = devm_phy_get(dev, "csi2-dphy");
	if (IS_ERR(mipi_csi->phy))
		return PTR_ERR(mipi_csi->phy);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mipi_csi->base_address = devm_ioremap_resource(dev, res);

	if (IS_ERR(mipi_csi->base_address))
		return PTR_ERR(mipi_csi->base_address);

	mipi_csi->ctrl_irq_number = platform_get_irq(pdev, 0);
	if (mipi_csi->ctrl_irq_number <= 0) {
		dev_err(dev, "IRQ number not set.\n");
		return mipi_csi->ctrl_irq_number;
	}

	ret = devm_request_irq(dev, mipi_csi->ctrl_irq_number,
			       dw_mipi_csi_irq1, IRQF_SHARED,
			       dev_name(dev), mipi_csi);
	if (ret) {
		dev_err(dev, "IRQ failed\n");
		goto end;
	}

	v4l2_subdev_init(&mipi_csi->sd, &dw_mipi_csi_subdev_ops);
	mipi_csi->sd.owner = THIS_MODULE;
	snprintf(mipi_csi->sd.name, sizeof(mipi_csi->sd.name), "%s.%d",
		 CSI_DEVICE_NAME, mipi_csi->index);
	mipi_csi->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	mipi_csi->fmt = &dw_mipi_csi_formats[0];

	mipi_csi->format.code = dw_mipi_csi_formats[0].code;
	mipi_csi->format.width = MIN_WIDTH;
	mipi_csi->format.height = MIN_HEIGHT;

	mipi_csi->pads[CSI_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	mipi_csi->pads[CSI_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&mipi_csi->sd.entity,
				     CSI_PADS_NUM, mipi_csi->pads);

	if (ret < 0) {
		dev_err(dev, "Media Entity init failed\n");
		goto entity_cleanup;
	}

	/* This allows to retrieve the platform device id by the host driver */
	v4l2_set_subdevdata(&mipi_csi->sd, pdev);

	/* .. and a pointer to the subdev. */
	platform_set_drvdata(pdev, &mipi_csi->sd);

	dw_mipi_csi_mask_irq_power_off(mipi_csi);
	dev_info(dev, "DW MIPI CSI-2 Host registered successfully\n");
	return 0;

entity_cleanup:
	media_entity_cleanup(&mipi_csi->sd.entity);
end:
	return ret;
}

/**
 * @short Exit routine - Exit point of the driver
 * @param[in] pdev pointer to the platform device structure
 * @return 0 on success and a negative number on failure
 * Refer to Linux errors.
 */
static int mipi_csi_remove(struct platform_device *pdev)
{
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct mipi_csi_dev *mipi_csi = sd_to_mipi_csi_dev(sd);

	dev_dbg(&pdev->dev, "Removing MIPI CSI-2 module\n");
	media_entity_cleanup(&mipi_csi->sd.entity);

	return 0;
}

/**
 * @short of_device_id structure
 */
static const struct of_device_id dw_mipi_csi_of_match[] = {
	{
	 .compatible = "snps,dw-mipi-csi"},
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, dw_mipi_csi_of_match);

/**
 * @short Platform driver structure
 */
static struct platform_driver __refdata dw_mipi_csi_pdrv = {
	.remove = mipi_csi_remove,
	.probe = mipi_csi_probe,
	.driver = {
		   .name = CSI_DEVICE_NAME,
		   .owner = THIS_MODULE,
		   .of_match_table = dw_mipi_csi_of_match,
		   },
};

module_platform_driver(dw_mipi_csi_pdrv);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ramiro Oliveira <roliveir@synopsys.com>");
MODULE_DESCRIPTION("Synopsys DW MIPI CSI-2 Host driver");
