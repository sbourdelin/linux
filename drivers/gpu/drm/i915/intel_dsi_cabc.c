/*
 * Copyright © 2006-2010 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Author: Deepak M <m.deepak at intel.com>
 */

#include "intel_drv.h"
#include "intel_dsi.h"
#include "i915_drv.h"
#include <drm/drm_mipi_dsi.h>

#define CABC_OFF                        (0 << 0)
#define CABC_USER_INTERFACE_IMAGE       (1 << 0)
#define CABC_STILL_PICTURE              (2 << 0)
#define CABC_VIDEO_MODE                 (3 << 0)

#define CABC_BACKLIGHT                  (1 << 2)
#define CABC_DIMMING_DISPLAY            (1 << 3)
#define CABC_BCTRL                      (1 << 5)

#define CABC_MAX_VALUE                  0xFF

#define MIPI_DCS_CABC_LEVEL_RD          0x52
#define MIPI_DCS_CABC_MIN_BRIGHTNESS_RD 0x5F
#define MIPI_DCS_CABC_CONTROL_RD        0x56
#define MIPI_DCS_CABC_CONTROL_BRIGHT_RD 0x54
#define MIPI_DCS_CABC_LEVEL_WR          0x51
#define MIPI_DCS_CABC_MIN_BRIGHTNESS_WR 0x5E
#define MIPI_DCS_CABC_CONTROL_WR        0x55
#define MIPI_DCS_CABC_CONTROL_BRIGHT_WR 0x53

static u32 cabc_get_backlight(struct intel_connector *connector)
{
	struct intel_dsi *intel_dsi = NULL;
	struct intel_encoder *encoder = NULL;
	struct mipi_dsi_device *dsi_device;
	u8 data[2] = {0};
	enum port port;

	encoder = connector->encoder;
	intel_dsi = enc_to_intel_dsi(&encoder->base);

	for_each_dsi_port(port, intel_dsi->bkl_dcs_ports) {
		dsi_device = intel_dsi->dsi_hosts[port]->device;
		mipi_dsi_dcs_read(dsi_device, MIPI_DCS_CABC_LEVEL_RD, data, 2);
	}

	return data[1];
}

static void cabc_set_backlight(struct intel_connector *connector, u32 level)
{
	struct intel_dsi *intel_dsi = NULL;
	struct intel_encoder *encoder = NULL;
	struct mipi_dsi_device *dsi_device;
	u8 data[2] = {0};
	enum port port;

	encoder = connector->encoder;
	intel_dsi = enc_to_intel_dsi(&encoder->base);

	for_each_dsi_port(port, intel_dsi->bkl_dcs_ports) {
		dsi_device = intel_dsi->dsi_hosts[port]->device;
		data[1] = level;
		data[0] = MIPI_DCS_CABC_LEVEL_WR;
		mipi_dsi_dcs_write_buffer(dsi_device, data, 2);
	}
}

static void cabc_disable_backlight(struct intel_connector *connector)
{
	struct intel_dsi *intel_dsi = NULL;
	struct intel_encoder *encoder = NULL;
	struct mipi_dsi_device *dsi_device;
	enum port port;
	u8 data[2] = {0};

	encoder = connector->encoder;
	intel_dsi = enc_to_intel_dsi(&encoder->base);

	cabc_set_backlight(connector, 0);

	for_each_dsi_port(port, intel_dsi->bkl_dcs_ports) {
		dsi_device = intel_dsi->dsi_hosts[port]->device;
		data[1] = CABC_OFF;
		data[0] = MIPI_DCS_CABC_CONTROL_WR;
		mipi_dsi_dcs_write_buffer(dsi_device, data, 2);
		data[0] = MIPI_DCS_CABC_CONTROL_BRIGHT_WR;
		mipi_dsi_dcs_write_buffer(dsi_device, data, 2);
	}
}

static void cabc_enable_backlight(struct intel_connector *connector)
{
	struct intel_dsi *intel_dsi = NULL;
	struct intel_encoder *encoder = NULL;
	struct intel_panel *panel = &connector->panel;
	struct mipi_dsi_device *dsi_device;
	enum port port;
	u8 data[2] = {0};

	encoder = connector->encoder;
	intel_dsi = enc_to_intel_dsi(&encoder->base);

	for_each_dsi_port(port, intel_dsi->bkl_dcs_ports) {
		dsi_device = intel_dsi->dsi_hosts[port]->device;
		data[0] = MIPI_DCS_CABC_CONTROL_BRIGHT_WR;
		data[1] = CABC_BACKLIGHT | CABC_DIMMING_DISPLAY | CABC_BCTRL;
		mipi_dsi_dcs_write_buffer(dsi_device, data, 2);
		data[0] = MIPI_DCS_CABC_CONTROL_WR;
		data[1] = CABC_STILL_PICTURE;
		mipi_dsi_dcs_write_buffer(dsi_device, data, 2);
	}

	cabc_set_backlight(connector, panel->backlight.level);
}

static int cabc_setup_backlight(struct intel_connector *connector,
		enum pipe unused)
{
	struct drm_device *dev = connector->base.dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_panel *panel = &connector->panel;

	if (dev_priv->vbt.backlight.present)
		panel->backlight.present = true;
	else {
		DRM_ERROR("no backlight present per VBT\n");
		return 0;
	}

	panel->backlight.max = CABC_MAX_VALUE;
	panel->backlight.level = CABC_MAX_VALUE;

	return 0;
}

int intel_dsi_cabc_init_backlight_funcs(struct intel_connector *intel_connector)
{
	struct drm_device *dev = intel_connector->base.dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_encoder *encoder = intel_connector->encoder;
	struct intel_panel *panel = &intel_connector->panel;

	if (!dev_priv->vbt.dsi.config->cabc_supported)
		return -EINVAL;

	if (encoder->type != INTEL_OUTPUT_DSI) {
		DRM_ERROR("Use DSI encoder for CABC\n");
		return -EINVAL;
	}

	panel->backlight.setup = cabc_setup_backlight;
	panel->backlight.enable = cabc_enable_backlight;
	panel->backlight.disable = cabc_disable_backlight;
	panel->backlight.set = cabc_set_backlight;
	panel->backlight.get = cabc_get_backlight;

	return 0;
}
