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

enum irqreturn
lspcon_hpd_pulse(struct intel_digital_port *intel_dig_port, bool long_hpd)
{
	struct drm_device *dev = intel_dig_port->base.base.dev;
	struct intel_encoder *intel_encoder = &intel_dig_port->base;
	struct intel_connector *intel_connector;
	bool changed = false;

	mutex_lock(&dev->mode_config.mutex);
	if (intel_encoder->hot_plug)
		intel_encoder->hot_plug(intel_encoder);

	for_each_intel_connector(dev, intel_connector) {
		if (intel_connector->encoder == intel_encoder) {
			struct drm_connector *connector =
				&intel_connector->base;

			DRM_DEBUG_DRIVER("Hptplug: Connector %s (pin %i).\n",
				connector->name, intel_encoder->hpd_pin);
			if (intel_hpd_irq_event(dev, connector))
				changed = true;
		}
	}
	mutex_unlock(&dev->mode_config.mutex);

	if (changed) {
		DRM_DEBUG_DRIVER("Sending event for change\n");
		drm_kms_helper_hotplug_event(dev);
	}
	return IRQ_HANDLED;
}

enum lspcon_mode lspcon_get_current_mode(struct intel_lspcon *lspcon)
{
	u8 data;
	int err = 0;
	struct intel_digital_port *dig_port = lspcon_to_dig_port(lspcon);
	struct i2c_adapter *adapter = &dig_port->dp.aux.ddc;

	/* Read Status: i2c over aux */
	err = drm_dp_dual_mode_ioa_read(adapter, &data,
		LSPCON_MODE_CHECK_OFFSET, sizeof(data));
	if (err < 0) {
		DRM_ERROR("LSPCON read mode ioa (0x80, 0x41) failed\n");
		return lspcon_mode_invalid;
	}

	DRM_DEBUG_DRIVER("LSPCON mode (0x80, 0x41) = %x\n", (unsigned int)data);
	return data & LSPCON_MODE_MASK ? lspcon_mode_pcon : lspcon_mode_ls;
}

int lspcon_change_mode(struct intel_lspcon *lspcon,
	enum lspcon_mode mode, bool force)
{
	u8 data;
	int err;
	int time_out = 200;
	enum lspcon_mode current_mode;
	struct intel_digital_port *dig_port = lspcon_to_dig_port(lspcon);

	current_mode = lspcon_get_current_mode(lspcon);
	if (current_mode == lspcon_mode_invalid) {
		DRM_ERROR("Failed to get current LSPCON mode\n");
		return -EFAULT;
	}

	if (current_mode == mode && !force) {
		DRM_DEBUG_DRIVER("Current mode = desired LSPCON mode\n");
		return 0;
	}

	if (mode == lspcon_mode_ls)
		data = ~LSPCON_MODE_MASK;
	else
		data = LSPCON_MODE_MASK;

	/* Change mode */
	err = drm_dp_dual_mode_ioa_write(&dig_port->dp.aux.ddc, &data,
			LSPCON_MODE_CHANGE_OFFSET, sizeof(data));
	if (err < 0) {
		DRM_ERROR("LSPCON mode change failed\n");
		return err;
	}

	/*
	* Confirm mode change by reading the status bit.
	* Sometimes, it takes a while to change the mode,
	* so wait and retry until time out or done.
	*/
	while (time_out) {
		current_mode = lspcon_get_current_mode(lspcon);
		if (current_mode != mode) {
			mdelay(10);
			time_out -= 10;
		} else {
			lspcon->mode_of_op = mode;
			DRM_DEBUG_DRIVER("LSPCON mode changed to %s\n",
				mode == lspcon_mode_ls ? "LS" : "PCON");
			return 0;
		}
	}

	DRM_ERROR("LSPCON mode change timed out\n");
	return -EFAULT;
}

bool lspcon_detect_identifier(struct intel_lspcon *lspcon)
{
	enum drm_dp_dual_mode_type adaptor_type;
	struct intel_digital_port *dig_port = lspcon_to_dig_port(lspcon);
	struct i2c_adapter *adapter = &dig_port->dp.aux.ddc;

	/* Lets probe the adaptor and check its type */
	adaptor_type = drm_dp_dual_mode_detect(adapter);
	if (adaptor_type != DRM_DP_DUAL_MODE_TYPE2_LSPCON) {
		DRM_DEBUG_DRIVER("No LSPCON detected, found %s\n",
			drm_dp_get_dual_mode_type_name(adaptor_type));
		return false;
	}

	/* Yay ... got a LSPCON device */
	DRM_DEBUG_DRIVER("LSPCON detected\n");
	return true;
}

enum lspcon_mode lspcon_probe(struct intel_lspcon *lspcon)
{
	enum lspcon_mode current_mode;

	/* Detect a valid lspcon */
	if (!lspcon_detect_identifier(lspcon)) {
		DRM_DEBUG_DRIVER("Failed to find LSPCON identifier\n");
		return false;
	}

	/* LSPCON's mode of operation */
	current_mode = lspcon_get_current_mode(lspcon);
	if (current_mode == lspcon_mode_invalid) {
		DRM_ERROR("Failed to read LSPCON mode\n");
		return false;
	}

	/* All is well */
	lspcon->mode_of_op = current_mode;
	lspcon->active = true;
	return current_mode;
}

bool lspcon_device_init(struct intel_lspcon *lspcon)
{

	/* Lets check LSPCON now, probe the HW status */
	lspcon->active = false;
	lspcon->mode_of_op = lspcon_mode_invalid;
	if (!lspcon_probe(lspcon)) {
		DRM_ERROR("Failed to probe lspcon");
		return false;
	}

	/* We wish to keep LSPCON in LS mode */
	if (lspcon->active && lspcon->mode_of_op != lspcon_mode_ls) {
		if (lspcon_change_mode(lspcon, lspcon_mode_ls, true) < 0) {
			DRM_ERROR("LSPCON mode change to LS failed\n");
			return false;
		}
	}
	DRM_DEBUG_DRIVER("LSPCON init success\n");
	return true;
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
	struct intel_lspcon *lspcon = &intel_dig_port->lspcon;
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

	/* HPD handler */
	intel_dig_port->hpd_pulse = lspcon_hpd_pulse;

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

	/* Now initialize the LSPCON device */
	if (!lspcon_device_init(lspcon)) {
		DRM_ERROR("LSPCON device init failed\n");
		goto fail;
	}

	DRM_DEBUG_DRIVER("Success: LSPCON connector init\n");
	return 0;

fail:
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
	return ret;
}
