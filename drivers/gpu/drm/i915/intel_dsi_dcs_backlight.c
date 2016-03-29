/*
 * Copyright © 2016 Intel Corporation
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
#include <video/mipi_display.h>
#include <drm/drm_mipi_dsi.h>

#define PANEL_PWM_BKL_EN		(1 << 2)
#define PANEL_PWM_DISP_DIMMING		(1 << 3)
#define PANEL_PWM_BCTRL			(1 << 5)

#define CABC_OFF			(0 << 0)
#define CABC_USER_INTERFACE_IMAGE	(1 << 0)
#define CABC_STILL_PICTURE		(2 << 0)
#define CABC_VIDEO_MODE			(3 << 0)

#define PANEL_PWM_MAX_VALUE		0xFF

static u32 panel_pwm_get_backlight(struct intel_connector *connector)
{
	struct intel_encoder *encoder = connector->encoder;
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	struct mipi_dsi_device *dsi_device;
	u8 data;
	enum port port;

	/* FIXME Need to take care of 16 bit brightness level */
	/*
	 * Sending the DCS commands to the ports to which Panel PWM
	 * On/Off commands were send
	 */
	for_each_dsi_port(port, intel_dsi->panel_pwm_dcs_ports) {
		dsi_device = intel_dsi->dsi_hosts[port]->device;
		mipi_dsi_dcs_read(dsi_device, MIPI_DCS_GET_DISPLAY_BRIGHTNESS,
					&data, sizeof(data));
		break;
	}

	return data;
}

static void panel_pwm_set_backlight(struct intel_connector *connector, u32 level)
{
	struct intel_encoder *encoder = connector->encoder;
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	struct mipi_dsi_device *dsi_device;
	u8 data;
	enum port port;

	/* FIXME Need to take care of 16 bit brightness level */
	/*
	 * Sending the DCS commands to the ports to which Panel PWM
	 * On/Off commands were send
	 */
	for_each_dsi_port(port, intel_dsi->panel_pwm_dcs_ports) {
		dsi_device = intel_dsi->dsi_hosts[port]->device;
		data = level;
		mipi_dsi_dcs_write(dsi_device, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
					&data, sizeof(data));
	}
}

static void panel_pwm_disable_backlight(struct intel_connector *connector)
{
	struct drm_device *dev = connector->base.dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_encoder *encoder = connector->encoder;
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	struct mipi_dsi_device *dsi_device;
	enum port port;
	u8 data;

	panel_pwm_set_backlight(connector, 0);

	if (dev_priv->vbt.dsi.config->cabc_supported) {
		data = 0;
		for_each_dsi_port(port, intel_dsi->cabc_dcs_ports) {
			dsi_device = intel_dsi->dsi_hosts[port]->device;
			data = CABC_OFF;
			mipi_dsi_dcs_write(dsi_device, MIPI_DCS_WRITE_POWER_SAVE,
					&data, sizeof(data));
		}
	}

	for_each_dsi_port(port, intel_dsi->panel_pwm_dcs_ports) {
		dsi_device = intel_dsi->dsi_hosts[port]->device;
		data &= ~PANEL_PWM_BKL_EN; /* Turn Off Backlight */
		data &= ~PANEL_PWM_DISP_DIMMING; /* Display Dimming Off */
		data &= ~PANEL_PWM_BCTRL; /* Brightness control Block Off */
		mipi_dsi_dcs_write(dsi_device, MIPI_DCS_WRITE_CONTROL_DISPLAY,
					&data, sizeof(data));
	}
}

static void panel_pwm_enable_backlight(struct intel_connector *connector)
{
	struct drm_device *dev = connector->base.dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_encoder *encoder = connector->encoder;
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	struct intel_panel *panel = &connector->panel;
	struct mipi_dsi_device *dsi_device;
	enum port port;
	u8 data;

	for_each_dsi_port(port, intel_dsi->panel_pwm_dcs_ports) {
		dsi_device = intel_dsi->dsi_hosts[port]->device;
		data = PANEL_PWM_BKL_EN /* Turn on backlight */
			| PANEL_PWM_DISP_DIMMING /* Display Dimming On */
			| PANEL_PWM_BCTRL; /* Brightness control Block On */
		mipi_dsi_dcs_write(dsi_device, MIPI_DCS_WRITE_CONTROL_DISPLAY,
					&data, sizeof(data));
	}

	if (dev_priv->vbt.dsi.config->cabc_supported) {
		data = 0;
		for_each_dsi_port(port, intel_dsi->cabc_dcs_ports) {
			dsi_device = intel_dsi->dsi_hosts[port]->device;
			/* Enabling CABC in still mode */
			data = CABC_STILL_PICTURE;
			mipi_dsi_dcs_write(dsi_device, MIPI_DCS_WRITE_POWER_SAVE,
					&data, sizeof(data));
		}
	}

	panel_pwm_set_backlight(connector, panel->backlight.level);
}

static int panel_pwm_setup_backlight(struct intel_connector *connector,
		enum pipe unused)
{
	struct intel_panel *panel = &connector->panel;

	panel->backlight.max = PANEL_PWM_MAX_VALUE;
	/* Assigning the MAX value during the setup */
	panel->backlight.level = PANEL_PWM_MAX_VALUE;

	return 0;
}

int intel_dsi_panel_pwm_init_backlight_funcs(struct intel_connector *intel_connector)
{
	struct drm_device *dev = intel_connector->base.dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_encoder *encoder = intel_connector->encoder;
	struct intel_panel *panel = &intel_connector->panel;

	/*
	 * Continue initalizing only if the PWM source is
	 * from the panel
	 */
	if (dev_priv->vbt.backlight.pwm_pin !=
			BLC_CONTROL_PIN_PANEL_PWM)
		return -ENODEV;

	if (WARN_ON(encoder->type != INTEL_OUTPUT_DSI))
		return -EINVAL;

	panel->backlight.setup = panel_pwm_setup_backlight;
	panel->backlight.enable = panel_pwm_enable_backlight;
	panel->backlight.disable = panel_pwm_disable_backlight;
	panel->backlight.set = panel_pwm_set_backlight;
	panel->backlight.get = panel_pwm_get_backlight;

	return 0;
}
