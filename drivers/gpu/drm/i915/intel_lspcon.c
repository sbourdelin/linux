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
