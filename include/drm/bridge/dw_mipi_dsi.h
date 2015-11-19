/*
 * Copyright (C) 2014-2015 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef __DW_MIPI_DSI__
#define __DW_MIPI_DSI__

#include <drm/drmP.h>

struct dw_mipi_dsi_plat_data {
	unsigned int max_data_lanes;
	enum drm_mode_status (*mode_valid)(struct drm_connector *connector,
					   struct drm_display_mode *mode);
};

int dw_mipi_dsi_get_encoder_pixel_format(struct drm_encoder *encoder);

int dw_mipi_dsi_bind(struct device *dev, struct device *master,
		     void *data, struct drm_encoder *encoder,
		     const struct dw_mipi_dsi_plat_data *pdata);
void dw_mipi_dsi_unbind(struct device *dev, struct device *master, void *data);
#endif	/* __DW_MIPI_DSI__ */
