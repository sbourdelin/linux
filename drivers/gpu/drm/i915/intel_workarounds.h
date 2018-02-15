/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2014-2018 Intel Corporation
 */

#ifndef _I915_WORKAROUNDS_H_
#define _I915_WORKAROUNDS_H_

void intel_ctx_workarounds_apply(struct drm_i915_private *dev_priv);

void intel_gt_workarounds_apply(struct drm_i915_private *dev_priv);

void intel_display_workarounds_apply(struct drm_i915_private *dev_priv);

int intel_whitelist_workarounds_apply(struct intel_engine_cs *engine);

int intel_engine_init_bb_workarounds(struct intel_engine_cs *engine);
void intel_engine_fini_bb_workarounds(struct intel_engine_cs *engine);

#endif
