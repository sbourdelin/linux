/*
 * ARC PGU DRM driver.
 *
 * Copyright (C) 2016 Synopsys, Inc. (www.synopsys.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_encoder_slave.h>
#include <drm/drm_atomic_helper.h>

#include "arcpgu.h"

static void arcpgu_drm_i2c_encoder_mode_set(struct drm_encoder *encoder,
				struct drm_display_mode *mode,
				struct drm_display_mode *adjusted_mode)
{
	struct drm_bridge *bridge = encoder->bridge;

	bridge->funcs->mode_set(bridge, mode, adjusted_mode);
}

static struct drm_encoder_helper_funcs arcpgu_drm_encoder_helper_funcs = {
	.dpms = drm_i2c_encoder_dpms,
	.mode_fixup = drm_i2c_encoder_mode_fixup,
	.mode_set = arcpgu_drm_i2c_encoder_mode_set,
	.prepare = drm_i2c_encoder_prepare,
	.commit = drm_i2c_encoder_commit,
};

static struct drm_encoder_funcs arcpgu_drm_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static void arcpgu_drm_i2c_encoder_dpms(struct drm_encoder *encoder, int mode)
{
	struct drm_bridge *bridge = encoder->bridge;

	if (mode == DRM_MODE_DPMS_ON)
		bridge->funcs->enable(bridge);
	else
		bridge->funcs->disable(bridge);
}

static struct drm_encoder_slave_funcs arcpgu_drm_encoder_slave_funcs = {
	.dpms = arcpgu_drm_i2c_encoder_dpms,
};

int arcpgu_drm_hdmi_init(struct drm_device *drm, struct device_node *np)
{
	struct drm_encoder_slave *encoder;
	struct i2c_client *i2c_slave;
	struct drm_bridge *bridge;

	int ret;

	encoder = devm_kzalloc(drm->dev, sizeof(*encoder), GFP_KERNEL);
	if (encoder == NULL)
		return -ENOMEM;

	i2c_slave = of_find_i2c_device_by_node(np);
	if (!i2c_slave || !i2c_get_clientdata(i2c_slave)) {
		dev_err(drm->dev, "failed to find i2c slave encoder\n");
		return -EPROBE_DEFER;
	}

	if (i2c_slave->dev.driver == NULL) {
		dev_err(drm->dev, "failed to find i2c slave driver\n");
		return -EPROBE_DEFER;
	}

	/* Locate drm bridge from the hdmi encoder DT node */
	bridge = of_drm_find_bridge(np);
	if (!bridge)
		return -EPROBE_DEFER;

	encoder->base.possible_crtcs = 1;
	encoder->base.possible_clones = 0;
	encoder->slave_funcs = &arcpgu_drm_encoder_slave_funcs;
	ret = drm_encoder_init(drm, &encoder->base, &arcpgu_drm_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ret;

	drm_encoder_helper_add(&encoder->base,
			       &arcpgu_drm_encoder_helper_funcs);

	/* Link drm_bridge to encoder */
	bridge->encoder = &encoder->base;
	encoder->base.bridge = bridge;

	ret = drm_bridge_attach(drm, bridge);
	if (ret) {
		drm_encoder_cleanup(&encoder->base);
		return ret;
	}

	return 0;
}
