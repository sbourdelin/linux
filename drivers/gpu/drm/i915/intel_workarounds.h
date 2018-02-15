/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2014-2018 Intel Corporation
 */

#ifndef _I915_WORKAROUNDS_H_
#define _I915_WORKAROUNDS_H_

void intel_ctx_workarounds_apply(struct drm_i915_private *dev_priv);

void intel_gt_workarounds_apply(struct drm_i915_private *dev_priv);

int intel_whitelist_workarounds_apply(struct intel_engine_cs *engine);

#endif
