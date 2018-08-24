// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2018 Linus Walleij <linus.walleij@linaro.org>
 */

#include <linux/module.h>
#include <drm/drmP.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_modes.h>
#include <video/of_display_timing.h>

struct virtenc {
	struct device *dev;
	struct drm_device *drm;
	struct drm_bridge bridge;
	struct drm_connector connector;
	struct drm_display_mode mode;
	u32 bus_flags;
};

static inline struct virtenc *bridge_to_virtenc(struct drm_bridge *bridge)
{
	return container_of(bridge, struct virtenc, bridge);
}

static inline struct virtenc *connector_to_virtenc(struct drm_connector *con)
{
	return container_of(con, struct virtenc, connector);
}

static enum drm_connector_status
virtenc_connector_detect(struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}

static const struct drm_connector_funcs virtenc_connector_funcs = {
	.detect = virtenc_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int virtenc_get_modes(struct drm_connector *connector)
{
	struct virtenc *virtenc = connector_to_virtenc(connector);
	struct drm_display_mode *mode = drm_mode_create(virtenc->drm);
	u32 bus_format = MEDIA_BUS_FMT_RGB888_1X24;
	int ret;

	drm_mode_copy(mode, &virtenc->mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	mode->width_mm = 80;
	mode->height_mm = 60;
	drm_mode_set_name(mode);

	drm_mode_probed_add(connector, mode);
	ret = drm_display_info_set_bus_formats(&connector->display_info,
					       &bus_format, 1);
	if (ret)
		return ret;

	return 1;
}

static enum drm_mode_status virtenc_mode_valid(struct drm_connector *connector,
					       struct drm_display_mode *mode)
{
	return MODE_OK;
}

static const struct drm_connector_helper_funcs
virtenc_connector_helper_funcs = {
	.get_modes = virtenc_get_modes,
	.mode_valid = virtenc_mode_valid,
};

static void virtenc_bridge_disable(struct drm_bridge *bridge)
{
}

static void virtenc_bridge_enable(struct drm_bridge *bridge)
{
}

static void virtenc_bridge_mode_set(struct drm_bridge *bridge,
				    struct drm_display_mode *mode,
				    struct drm_display_mode *adj)
{
}

static int virtenc_bridge_attach(struct drm_bridge *bridge)
{
	struct virtenc *virtenc = bridge_to_virtenc(bridge);
	struct drm_device *drm = bridge->dev;
	int ret;

	virtenc->drm = drm;
	drm_connector_helper_add(&virtenc->connector,
				 &virtenc_connector_helper_funcs);

	if (!drm_core_check_feature(drm, DRIVER_ATOMIC)) {
		dev_err(virtenc->dev,
			"Virtual Display bridge driver is only compatible with DRM devices supporting atomic updates\n");
		return -ENOTSUPP;
	}

	ret = drm_connector_init(drm, &virtenc->connector,
				 &virtenc_connector_funcs,
				 DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret)
		return ret;

	virtenc->connector.polled = DRM_CONNECTOR_POLL_CONNECT;

	drm_mode_connector_attach_encoder(&virtenc->connector, bridge->encoder);

	return 0;
}

static const struct drm_bridge_funcs virtenc_bridge_funcs = {
	.attach = virtenc_bridge_attach,
	.mode_set = virtenc_bridge_mode_set,
	.disable = virtenc_bridge_disable,
	.enable = virtenc_bridge_enable,
};

static int virtenc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct virtenc *virtenc;
	int ret;

	virtenc = devm_kzalloc(dev, sizeof(*virtenc), GFP_KERNEL);
	if (!virtenc)
		return -ENOMEM;

	ret = of_get_drm_display_mode(np, &virtenc->mode,
				      &virtenc->bus_flags,
				      0);
	if (ret)
		return ret;

	virtenc->dev = dev;
	virtenc->bridge.funcs = &virtenc_bridge_funcs;
	virtenc->bridge.of_node = dev->of_node;
	drm_bridge_add(&virtenc->bridge);
	platform_set_drvdata(pdev, virtenc);
	dev_info(dev, "added virtual display bridge\n");

	return 0;
}

static int virtenc_remove(struct platform_device *pdev)

{
	struct virtenc *virtenc = platform_get_drvdata(pdev);

	drm_bridge_remove(&virtenc->bridge);

	return 0;
}

static const struct of_device_id virtenc_dt_ids[] = {
	{ .compatible = "virtual-display-bridge", },
	{ }
};
MODULE_DEVICE_TABLE(of, virtenc_dt_ids);

static struct platform_driver virtenc_driver = {
	.driver = {
		.name = "virtenc",
		.of_match_table = virtenc_dt_ids,
	},
	.probe = virtenc_probe,
	.remove = virtenc_remove,
};
module_platform_driver(virtenc_driver);

MODULE_AUTHOR("Linus Walleij <linus.walleij@linaro.org>");
MODULE_DESCRIPTION("Virtual Display Bridge");
MODULE_LICENSE("GPL");
