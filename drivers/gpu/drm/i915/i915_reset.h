/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2008-2018 Intel Corporation
 */

#ifndef I915_RESET_H
#define I915_RESET_H

#include <linux/compiler.h>
#include <linux/types.h>

struct drm_i915_private;
struct intel_engine_cs;
struct intel_guc;

__printf(4, 5)
void i915_handle_error(struct drm_i915_private *i915,
		       u32 engine_mask,
		       unsigned long flags,
		       const char *fmt, ...);
#define I915_ERROR_CAPTURE BIT(0)

void i915_reset(struct drm_i915_private *i915,
		unsigned int stalled_mask,
		const char *reason);
int i915_reset_engine(struct intel_engine_cs *engine,
		      const char *reason);

bool intel_has_gpu_reset(struct drm_i915_private *i915);
bool intel_has_reset_engine(struct drm_i915_private *i915);

int intel_gpu_reset(struct drm_i915_private *i915, u32 engine_mask);

int intel_reset_guc(struct drm_i915_private *i915);

#endif /* I915_RESET_H */
