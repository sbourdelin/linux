/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright © 2018 Intel Corporation
 *
 */
#ifndef _I915_TRACING_H_
#define _I915_TRACING_H_

struct drm_i915_private;

#if IS_ENABLED(CONFIG_TRACEPOINTS)

void i915_tracing_register(struct drm_i915_private *i915);
void i915_tracing_unregister(struct drm_i915_private *i915);

int intel_engine_notify_tracepoint_register(void);
void intel_engine_notify_tracepoint_unregister(void);

#else

static inline void i915_tracing_register(struct drm_i915_private *i915) { }
static inline void i915_tracing_unregister(struct drm_i915_private *i915) { }

static inline int intel_engine_notify_tracepoint_register(void) { }
static inline void intel_engine_notify_tracepoint_unregister(void) { }

#endif

#endif
