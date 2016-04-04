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

	/* init DP */
	ret = intel_dp_init_minimum(intel_dig_port, intel_connector);
	if (ret) {
		DRM_ERROR("DP init for LSPCON failed\n");
		return ret;
	}

	/* init HDMI */
	ret = intel_hdmi_init_minimum(intel_dig_port, intel_connector);
	if (ret) {
		DRM_ERROR("HDMI init for LSPCON failed\n");
		return ret;
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
		return -EINVAL;
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
}
