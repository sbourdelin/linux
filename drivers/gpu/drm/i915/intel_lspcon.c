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
 * Authors:
 * Shashank Sharma <shashank.sharma@intel.com>
 * Akashdeep Sharma <akashdeep.sharma@intel.com>
 *
 */
#include <drm/drm_edid.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_dp_dual_mode_helper.h>
#include "intel_drv.h"

#define LSPCON_I2C_ADDRESS			0x80
#define LSPCON_MODE_CHANGE_OFFSET		0x40
#define LSPCON_MODE_CHECK_OFFSET		0x41
#define LSPCON_ADAPTER_SIGN_OFFSET		0x00
#define LSPCON_IDENTIFIER_OFFSET		0x10
#define LSPCON_IDENTIFIER_LENGTH		0x10
#define LSPCON_MODE_MASK			0x1

struct intel_digital_port *lspcon_to_dig_port(struct intel_lspcon *lspcon)
{
	return container_of(lspcon, struct intel_digital_port, lspcon);
}

struct intel_hdmi *lspcon_to_hdmi(struct intel_lspcon *lspcon)
{
	return &lspcon_to_dig_port(lspcon)->hdmi;
}

struct intel_lspcon *enc_to_lspcon(struct drm_encoder *encoder)
{
	struct intel_digital_port *intel_dig_port =
		container_of(encoder, struct intel_digital_port, base.base);
	return &intel_dig_port->lspcon;
}

static struct intel_lspcon
*intel_attached_lspcon(struct drm_connector *connector)
{
	return enc_to_lspcon(&intel_attached_encoder(connector)->base);
}

struct edid *lspcon_get_edid(struct intel_lspcon *lspcon, struct drm_connector
						*connector)
{
	struct edid *edid = NULL;
	struct intel_digital_port *dig_port = lspcon_to_dig_port(lspcon);
	struct i2c_adapter *adapter = &dig_port->dp.aux.ddc;

	if (lspcon->mode_of_op != lspcon_mode_ls) {
		DRM_ERROR("EDID read supported in LS mode only\n");
		return false;
	}

	/* LS mode, getting EDID using I2C over Aux */
	edid = drm_do_get_edid(connector, drm_dp_dual_mode_get_edid,
			(void *)adapter);
	return edid;
}

static void
lspcon_unset_edid(struct drm_connector *connector)
{
	struct intel_lspcon *lspcon = intel_attached_lspcon(connector);
	struct intel_hdmi *intel_hdmi = lspcon_to_hdmi(lspcon);

	intel_hdmi->has_hdmi_sink = false;
	intel_hdmi->has_audio = false;
	intel_hdmi->rgb_quant_range_selectable = false;

	kfree(to_intel_connector(connector)->detect_edid);
	to_intel_connector(connector)->detect_edid = NULL;
}

static bool
lspcon_set_edid(struct drm_connector *connector, bool force)
{
	struct drm_i915_private *dev_priv = to_i915(connector->dev);
	struct intel_lspcon *lspcon = intel_attached_lspcon(connector);
	struct intel_hdmi *intel_hdmi = lspcon_to_hdmi(lspcon);
	struct edid *edid = NULL;
	bool connected = false;

	if (force) {
		intel_display_power_get(dev_priv, POWER_DOMAIN_GMBUS);
		edid = lspcon_get_edid(lspcon, connector);
		intel_display_power_put(dev_priv, POWER_DOMAIN_GMBUS);
	}

	to_intel_connector(connector)->detect_edid = edid;
	if (edid && edid->input & DRM_EDID_INPUT_DIGITAL) {
		intel_hdmi->rgb_quant_range_selectable =
			drm_rgb_quant_range_selectable(edid);

		intel_hdmi->has_audio = drm_detect_monitor_audio(edid);
		if (intel_hdmi->force_audio != HDMI_AUDIO_AUTO)
			intel_hdmi->has_audio =
				intel_hdmi->force_audio == HDMI_AUDIO_ON;

		if (intel_hdmi->force_audio != HDMI_AUDIO_OFF_DVI)
			intel_hdmi->has_hdmi_sink =
				drm_detect_hdmi_monitor(edid);

		connected = true;
	}
	return connected;
}

static enum drm_connector_status
lspcon_detect(struct drm_connector *connector, bool force)
{
	enum drm_connector_status status;
	struct drm_i915_private *dev_priv = to_i915(connector->dev);
	struct intel_lspcon *lspcon = intel_attached_lspcon(connector);
	struct intel_hdmi *intel_hdmi = lspcon_to_hdmi(lspcon);

	DRM_DEBUG_KMS("[CONNECTOR:%d:%s]\n",
		      connector->base.id, connector->name);
	intel_display_power_get(dev_priv, POWER_DOMAIN_GMBUS);

	lspcon_unset_edid(connector);
	if (lspcon_set_edid(connector, true)) {
		DRM_DEBUG_DRIVER("HDMI connected\n");
		hdmi_to_dig_port(intel_hdmi)->base.type = INTEL_OUTPUT_HDMI;
		status = connector_status_connected;
	} else {
		DRM_DEBUG_DRIVER("HDMI disconnected\n");
		status = connector_status_disconnected;
	}
	intel_display_power_put(dev_priv, POWER_DOMAIN_GMBUS);
	return status;
}

static int
lspcon_set_property(struct drm_connector *connector,
			struct drm_property *property,
			uint64_t val)
{
	return intel_hdmi_set_property(connector, property, val);
}

static int
lspcon_get_modes(struct drm_connector *connector)
{
	return intel_hdmi_get_modes(connector);
}

static void
lspcon_destroy(struct drm_connector *connector)
{
	intel_hdmi_destroy(connector);
}

static enum drm_mode_status
lspcon_mode_valid(struct drm_connector *connector,
		      struct drm_display_mode *mode)
{
	int clock = mode->clock;
	int max_dotclk = 675000; /* 4k@60 */
	struct drm_device *dev = connector->dev;

	if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
		return MODE_NO_DBLESCAN;

	if ((mode->flags & DRM_MODE_FLAG_3D_MASK) ==
		DRM_MODE_FLAG_3D_FRAME_PACKING)
		clock *= 2;

	if (mode->flags & DRM_MODE_FLAG_DBLCLK)
		clock *= 2;

	if (clock < 25000)
		return MODE_CLOCK_LOW;

	if (clock > max_dotclk)
		return MODE_CLOCK_HIGH;

	/* BXT DPLL can't generate 223-240 MHz */
	if (IS_BROXTON(dev) && clock > 223333 && clock < 240000)
		return MODE_CLOCK_RANGE;

	/* todo: check for 12bpc here */
	return MODE_OK;
}

void lspcon_add_properties(struct intel_digital_port *dig_port,
		struct drm_connector *connector)
{
	intel_hdmi_add_properties(&dig_port->hdmi, connector);
}

static const struct drm_connector_funcs lspcon_connector_funcs = {
	.dpms = drm_atomic_helper_connector_dpms,
	.detect = lspcon_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.set_property = lspcon_set_property,
	.atomic_get_property = intel_connector_atomic_get_property,
	.destroy = lspcon_destroy,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
};

static const struct drm_connector_helper_funcs lspcon_connector_helper_funcs = {
	.get_modes = lspcon_get_modes,
	.mode_valid = lspcon_mode_valid,
	.best_encoder = intel_best_encoder,
};

int intel_lspcon_init_connector(struct intel_digital_port *intel_dig_port)
{
	int ret;
	struct intel_encoder *intel_encoder = &intel_dig_port->base;
	struct drm_device *dev = intel_encoder->base.dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_connector *intel_connector;
	struct drm_connector *connector;
	enum port port = intel_dig_port->port;

	intel_connector = intel_connector_alloc();
	if (!intel_connector)
		return -ENOMEM;

	connector = &intel_connector->base;
	connector->interlace_allowed = true;
	connector->doublescan_allowed = 0;

	/* Load connector */
	drm_connector_init(dev, connector, &lspcon_connector_funcs,
			DRM_MODE_CONNECTOR_DisplayPort);
	drm_connector_helper_add(connector, &lspcon_connector_helper_funcs);
	intel_connector_attach_encoder(intel_connector, intel_encoder);
	drm_connector_register(connector);

	/* Add properties and functions */
	lspcon_add_properties(intel_dig_port, connector);
	intel_connector->get_hw_state = intel_ddi_connector_get_hw_state;
	i915_debugfs_connector_add(connector);

	/* init DP */
	ret = intel_dp_init_minimum(intel_dig_port, intel_connector);
	if (ret) {
		DRM_ERROR("DP init for LSPCON failed\n");
		goto fail;
	}

	/* init HDMI */
	ret = intel_hdmi_init_minimum(intel_dig_port, intel_connector);
	if (ret) {
		DRM_ERROR("HDMI init for LSPCON failed\n");
		goto fail;
	}

	/* Set up the hotplug pin. */
	switch (port) {
	case PORT_A:
		intel_encoder->hpd_pin = HPD_PORT_A;
		break;
	case PORT_B:
		intel_encoder->hpd_pin = HPD_PORT_B;
		if (IS_BXT_REVID(dev, 0, BXT_REVID_A1))
			intel_encoder->hpd_pin = HPD_PORT_A;
		break;
	case PORT_C:
		intel_encoder->hpd_pin = HPD_PORT_C;
		break;
	case PORT_D:
		intel_encoder->hpd_pin = HPD_PORT_D;
		break;
	case PORT_E:
		intel_encoder->hpd_pin = HPD_PORT_E;
		break;
	default:
		DRM_ERROR("Invalid port to configure LSPCON\n");
		ret = -EINVAL;
	}

	/*
	* On BXT A0/A1, sw needs to activate DDIA HPD logic and
	* interrupts to check the external panel connection.
	*/
	if (IS_BXT_REVID(dev, 0, BXT_REVID_A1) && port == PORT_B)
		dev_priv->hotplug.irq_port[PORT_A] = intel_dig_port;
	else
		dev_priv->hotplug.irq_port[port] = intel_dig_port;

	/* For G4X desktop chip, PEG_BAND_GAP_DATA 3:0 must first be written
	* 0xd.  Failure to do so will result in spurious interrupts being
	* generated on the port when a cable is not attached.
	*/
	if (IS_G4X(dev) && !IS_GM45(dev)) {
		u32 temp = I915_READ(PEG_BAND_GAP_DATA);

		I915_WRITE(PEG_BAND_GAP_DATA, (temp & ~0xf) | 0xd);
	}

	DRM_DEBUG_DRIVER("Success: LSPCON connector init\n");
	return 0;

fail:
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
	return ret;
}
