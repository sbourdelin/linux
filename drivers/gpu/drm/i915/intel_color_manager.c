/*
 * Copyright © 2015 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 * Shashank Sharma <shashank.sharma@intel.com>
 * Kausal Malladi <Kausal.Malladi@intel.com>
 */

#include "intel_color_manager.h"

void intel_attach_color_properties_to_crtc(struct drm_device *dev,
		struct drm_crtc *crtc)
{
	struct drm_mode_config *config = &dev->mode_config;
	struct drm_mode_object *mode_obj = &crtc->base;

	/*
	* Register:
	* =========
	* Gamma correction as palette_after_ctm property
	* Degamma correction as palette_before_ctm property
	*
	* Load:
	* =====
	* no. of coefficients supported on this platform for gamma
	* and degamma with the query properties. A user
	* space agent should read these query property, and prepare
	* the color correction values accordingly. Its expected from the
	* driver to load the right number of coefficients during the init
	* phase.
	*/
	if (config->cm_coeff_after_ctm_property) {
		drm_object_attach_property(mode_obj,
			config->cm_coeff_after_ctm_property,
		INTEL_INFO(dev)->num_samples_after_ctm);
		DRM_DEBUG_DRIVER("Gamma query property initialized\n");
	}

	if (config->cm_coeff_before_ctm_property) {
		drm_object_attach_property(mode_obj,
			config->cm_coeff_before_ctm_property,
		INTEL_INFO(dev)->num_samples_before_ctm);
		DRM_DEBUG_DRIVER("Degamma query property initialized\n");
	}
}
