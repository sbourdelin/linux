// SPDX-License-Identifier: GPL-2.0+
/*
 * dw-csi-plat.c
 *
 * Copyright(c) 2018-present, Synopsys, Inc. and/or its affiliates.
 * Luis Oliveira <Luis.Oliveira@synopsys.com>
 *
 */

#include "dw-csi-plat.h"

static const struct mipi_fmt *
find_dw_mipi_csi_format(struct v4l2_mbus_framefmt *mf)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dw_mipi_csi_formats); i++)
		if (mf->code == dw_mipi_csi_formats[i].code)
			return &dw_mipi_csi_formats[i];
	return NULL;
}

static int dw_mipi_csi_enum_mbus_code(struct v4l2_subdev *sd,
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
dw_mipi_csi_get_format(struct mipi_csi_dev *dev,
		struct v4l2_subdev_pad_config *cfg,
		enum v4l2_subdev_format_whence which)
{
	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return cfg ? v4l2_subdev_get_try_format(&dev->sd, cfg,
							0) : NULL;

	return &dev->format;
}

static int
dw_mipi_csi_set_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_format *fmt)
{
	struct mipi_csi_dev *dev = sd_to_mipi_csi_dev(sd);
	struct mipi_fmt const *dev_fmt;
	struct v4l2_mbus_framefmt *mf;
	unsigned int i = 0;
	const struct v4l2_bt_timings *bt_r = &v4l2_dv_timings_presets[0].bt;

	mf = dw_mipi_csi_get_format(dev, cfg, fmt->which);

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
			dw_mipi_csi_fill_timings(dev, bt);
			return 0;
		}
		i++;
	}

	dw_mipi_csi_fill_timings(dev, bt_r);
	return 0;

}

static int
dw_mipi_csi_get_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_format *fmt)
{
	struct mipi_csi_dev *dev = sd_to_mipi_csi_dev(sd);
	struct v4l2_mbus_framefmt *mf;

	mf = dw_mipi_csi_get_format(dev, cfg, fmt->which);
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
		phy_power_off(dev->phy);
		dw_mipi_csi_mask_irq_power_off(dev);
	}
	return 0;
}

static int
dw_mipi_csi_init_cfg(struct v4l2_subdev *sd, struct v4l2_subdev_pad_config *cfg)
{
	struct v4l2_mbus_framefmt *format =
	    v4l2_subdev_get_try_format(sd, cfg, 0);

	format->colorspace = V4L2_COLORSPACE_SRGB;
	format->code = dw_mipi_csi_formats[0].code;
	format->width = MIN_WIDTH;
	format->height = MIN_HEIGHT;
	format->field = V4L2_FIELD_NONE;

	return 0;
}

static struct v4l2_subdev_core_ops dw_mipi_csi_core_ops = {
	.s_power = dw_mipi_csi_s_power,
};

static struct v4l2_subdev_pad_ops dw_mipi_csi_pad_ops = {
	.init_cfg =	dw_mipi_csi_init_cfg,
	.enum_mbus_code = dw_mipi_csi_enum_mbus_code,
	.get_fmt = dw_mipi_csi_get_fmt,
	.set_fmt = dw_mipi_csi_set_fmt,
};

static struct v4l2_subdev_ops dw_mipi_csi_subdev_ops = {
	.core = &dw_mipi_csi_core_ops,
	.pad = &dw_mipi_csi_pad_ops,
};

static irqreturn_t dw_mipi_csi_irq1(int irq, void *dev_id)
{
	struct mipi_csi_dev *csi_dev = dev_id;

	dw_mipi_csi_irq_handler(csi_dev);

	return IRQ_HANDLED;
}

static int
dw_mipi_csi_parse_dt(struct platform_device *pdev, struct mipi_csi_dev *dev)
{
	struct device_node *node = pdev->dev.of_node;
	struct v4l2_fwnode_endpoint endpoint;
	int ret;

	ret = of_property_read_u32(node, "snps,output-type", &dev->hw.output);
	if (ret) {
		dev_err(&pdev->dev, "Couldn't read output-type\n");
		return ret;
	}

	ret = of_property_read_u32(node, "snps,ipi-mode", &dev->hw.ipi_mode);
	if (ret) {
		dev_err(&pdev->dev, "Couldn't read ipi-mode\n");
		return ret;
	}

	ret = of_property_read_u32(node, "snps,ipi-auto-flush",
				 &dev->hw.ipi_auto_flush);
	if (ret) {
		dev_err(&pdev->dev, "Couldn't read ipi-auto-flush\n");
		return ret;
	}

	ret = of_property_read_u32(node, "snps,ipi-color-mode",
				 &dev->hw.ipi_color_mode);
	if (ret) {
		dev_err(&pdev->dev, "Couldn't read ipi-color-mode\n");
		return ret;
	}

	ret = of_property_read_u32(node, "snps,virtual-channel",
				&dev->hw.virtual_ch);
	if (ret) {
		dev_err(&pdev->dev, "Couldn't read virtual-channel\n");
		return ret;
	}

	node = of_graph_get_next_endpoint(node, NULL);
	if (!node) {
		dev_err(&pdev->dev, "No port node at %s\n",
				pdev->dev.of_node->full_name);
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_parse(of_fwnode_handle(node), &endpoint);
	if (ret)
		goto err;

	dev->index = endpoint.base.port - 1;
	if (dev->index >= CSI_MAX_ENTITIES) {
		ret = -ENXIO;
		goto err;
	}
	dev->hw.num_lanes = endpoint.bus.mipi_csi2.num_data_lanes;

err:
	of_node_put(node);
	return ret;
}

static ssize_t csih_version_show(struct device *dev,
						struct device_attribute *attr,
						char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct mipi_csi_dev *csi_dev = sd_to_mipi_csi_dev(sd);

	char buffer[10];

	snprintf(buffer, 10, "v.%d.%d*\n", csi_dev->hw_version_major,
					csi_dev->hw_version_minor);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t n_lanes_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	int ret;
	unsigned long lanes;

	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct mipi_csi_dev *csi_dev = sd_to_mipi_csi_dev(sd);

	ret = kstrtoul(buf, 10, &lanes);
	if (ret < 0)
		return ret;

	if (lanes > 8) {
		dev_err(dev, "Invalid number of lanes %lu\n", lanes);
		return count;
	}

	dev_info(dev, "Lanes %lu\n", lanes);
	csi_dev->hw.num_lanes = lanes;

	return count;
}
static ssize_t n_lanes_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct mipi_csi_dev *csi_dev = sd_to_mipi_csi_dev(sd);

	char buffer[10];

	snprintf(buffer, 10, "Lanes %d\n", csi_dev->hw.num_lanes);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t csih_reset_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct mipi_csi_dev *csi_dev = sd_to_mipi_csi_dev(sd);

	char buffer[10];

	/* Reset Controller and DPHY */
	phy_reset(csi_dev->phy);
	dw_mipi_csi_reset(csi_dev);

	snprintf(buffer, 10, "Reset\n");

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static ssize_t dt_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	int ret;
	unsigned long dt;

	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct mipi_csi_dev *csi_dev = sd_to_mipi_csi_dev(sd);

	ret = kstrtoul(buf, 16, &dt);
	if (ret < 0)
		return ret;

	if ((dt < 0x18) || (dt > 0x2F)) {
		dev_err(dev, "Invalid data type %lx\n", dt);
		return count;
	}

	dev_info(dev, "Data type %lx\n", dt);
	csi_dev->ipi_dt = dt;

	return count;
}

static ssize_t dt_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct mipi_csi_dev *csi_dev = sd_to_mipi_csi_dev(sd);

	char buffer[10];

	snprintf(buffer, 10, "DT %x\n", csi_dev->ipi_dt);

	return strlcpy(buf, buffer, PAGE_SIZE);
}

static DEVICE_ATTR_RO(csih_version);
static DEVICE_ATTR_RO(csih_reset);
static DEVICE_ATTR_RW(n_lanes);
static DEVICE_ATTR_RW(dt);

static const struct of_device_id dw_mipi_csi_of_match[];

static int csi_plat_probe(struct platform_device *pdev)
{
	const struct of_device_id *of_id;
	struct device *dev = &pdev->dev;
	struct resource *res = NULL;
	struct mipi_csi_dev *mipi_csi;
	int ret = -ENOMEM;

	mipi_csi = devm_kzalloc(dev, sizeof(*mipi_csi), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	mutex_init(&mipi_csi->lock);
	spin_lock_init(&mipi_csi->slock);
	mipi_csi->dev = dev;

	of_id = of_match_node(dw_mipi_csi_of_match, dev->of_node);
	if (WARN_ON(of_id == NULL))
		return -EINVAL;

	ret = dw_mipi_csi_parse_dt(pdev, mipi_csi);
	if (ret < 0)
		return ret;

	mipi_csi->phy = devm_of_phy_get(dev, dev->of_node, NULL);
	if (IS_ERR(mipi_csi->phy)) {
		dev_err(dev, "No DPHY available\n");
		return PTR_ERR(mipi_csi->phy);
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mipi_csi->base_address = devm_ioremap_resource(dev, res);

	if (IS_ERR(mipi_csi->base_address)) {
		dev_err(dev, "Base address not set.\n");
		return PTR_ERR(mipi_csi->base_address);
	}

	mipi_csi->ctrl_irq_number = platform_get_irq(pdev, 0);
	if (mipi_csi->ctrl_irq_number <= 0) {
		dev_err(dev, "IRQ number not set.\n");
		return mipi_csi->ctrl_irq_number;
	}

	mipi_csi->rst = devm_reset_control_get_optional_shared(dev, NULL);
	if (IS_ERR(mipi_csi->rst)) {
		ret = PTR_ERR(mipi_csi->rst);
		dev_err(dev, "error getting reset control %d\n", ret);
		return ret;
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
		 CSI_HOST_NAME, mipi_csi->index);
	mipi_csi->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	mipi_csi->fmt = &dw_mipi_csi_formats[0];

	mipi_csi->format.code = dw_mipi_csi_formats[0].code;
	mipi_csi->format.width = MIN_WIDTH;
	mipi_csi->format.height = MIN_HEIGHT;

	mipi_csi->sd.entity.function = MEDIA_ENT_F_IO_V4L;
	mipi_csi->pads[CSI_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	mipi_csi->pads[CSI_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&mipi_csi->sd.entity,
				     CSI_PADS_NUM, mipi_csi->pads);

	if (ret < 0) {
		dev_err(dev, "Media Entity init failed\n");
		goto entity_cleanup;
	}

	v4l2_set_subdevdata(&mipi_csi->sd, pdev);

	platform_set_drvdata(pdev, &mipi_csi->sd);

	device_create_file(&pdev->dev, &dev_attr_csih_version);
	device_create_file(&pdev->dev, &dev_attr_csih_reset);
	device_create_file(&pdev->dev, &dev_attr_n_lanes);
	device_create_file(&pdev->dev, &dev_attr_dt);

	if (mipi_csi->rst)
		reset_control_deassert(mipi_csi->rst);

	dw_mipi_csi_get_version(mipi_csi);
	dw_mipi_csi_specific_mappings(mipi_csi);
	dw_mipi_csi_mask_irq_power_off(mipi_csi);

	dev_info(dev, "DW MIPI CSI-2 Host registered successfully HW v%u.%u\n",
		mipi_csi->hw_version_major, mipi_csi->hw_version_minor);
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
static int csi_plat_remove(struct platform_device *pdev)
{
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct mipi_csi_dev *mipi_csi = sd_to_mipi_csi_dev(sd);

	dev_dbg(&pdev->dev, "Removing MIPI CSI-2 module\n");

	if (mipi_csi->rst)
		reset_control_assert(mipi_csi->rst);

	media_entity_cleanup(&mipi_csi->sd.entity);

	return 0;
}

/**
 * @short of_device_id structure
 */
static const struct of_device_id dw_mipi_csi_of_match[] = {
	{
	 .compatible = "snps,dw-csi-plat"},
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, dw_mipi_csi_of_match);

/**
 * @short Platform driver structure
 */
static struct platform_driver __refdata dw_mipi_csi_pdrv = {
	.remove = csi_plat_remove,
	.probe = csi_plat_probe,
	.driver = {
		   .name = CSI_HOST_NAME,
		   .owner = THIS_MODULE,
		   .of_match_table = dw_mipi_csi_of_match,
		   },
};

module_platform_driver(dw_mipi_csi_pdrv);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Luis Oliveira <lolivei@synopsys.com>");
MODULE_DESCRIPTION("Synopsys DesignWare MIPI CSI-2 Host Platform driver");
