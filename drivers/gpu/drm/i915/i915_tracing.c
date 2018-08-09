/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright © 2018 Intel Corporation
 *
 */

#include "i915_tracing.h"

#include "i915_drv.h"
#include "intel_ringbuffer.h"

static DEFINE_MUTEX(driver_list_lock);
static LIST_HEAD(driver_list);
static bool notify_enabled;

static void __i915_enable_notify(struct drm_i915_private *i915)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	intel_runtime_pm_get(i915);

	for_each_engine(engine, i915, id)
		intel_engine_pin_breadcrumbs_irq(engine);

	intel_runtime_pm_put(i915);
}

static void __i915_disable_notify(struct drm_i915_private *i915)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	intel_runtime_pm_get(i915);

	for_each_engine(engine, i915, id)
		intel_engine_unpin_breadcrumbs_irq(engine);

	intel_runtime_pm_put(i915);
}

/**
 * i915_tracing_register - registers driver tracepoints support
 * @i915: the drm_i915_private device
 *
 * Registers the tracepoints support from the driver registration path.
 *
 * Puts the driver on the local list and enables the unconditional notifications
 * for the intel_engine_notify tracepoint if they should be enabled.
 */
void i915_tracing_register(struct drm_i915_private *i915)
{
	INIT_LIST_HEAD(&i915->tracing_link);

	mutex_lock(&driver_list_lock);

	list_add_tail(&i915->tracing_link, &driver_list);

	if (notify_enabled)
		__i915_enable_notify(i915);

	mutex_unlock(&driver_list_lock);
}

/**
 * i915_tracing_unregister - unregisters driver tracepoints support
 * @i915: the drm_i915_private device
 *
 * Un-registers the tracepoints support from the driver un-registration path.
 *
 * Removes the driver from the local list and disables the unconditional
 * notifications for the intel_engine_notify tracepoint if they were enabled.
 */
void i915_tracing_unregister(struct drm_i915_private *i915)
{
	mutex_lock(&driver_list_lock);

	if (notify_enabled)
		__i915_disable_notify(i915);

	list_del(&i915->tracing_link);

	mutex_unlock(&driver_list_lock);
}

/**
 * intel_engine_notify_tracepoint_register - tracepoint registration callback
 *
 * This is called as the intel_engine_notify registration callback, ie. when
 * the tracepoint is first activated.
 */
int intel_engine_notify_tracepoint_register(void)
{
	struct drm_i915_private *i915;

	mutex_lock(&driver_list_lock);

	GEM_BUG_ON(notify_enabled);

	/*
	 * Enable user interrupts / constant intel_engine_notify notifications.
	 */
	list_for_each_entry(i915, &driver_list, tracing_link)
		__i915_enable_notify(i915);

	notify_enabled = true;

	mutex_unlock(&driver_list_lock);

	return 0;
}

/**
 * intel_engine_notify_tracepoint_unregister - tracepoint unregistration callback
 *
 * This is called as the intel_engine_notify unregistration callback, ie. when
 * the last listener of this tracepoint goes away.
 */
void intel_engine_notify_tracepoint_unregister(void)
{
	struct drm_i915_private *i915;

	mutex_lock(&driver_list_lock);

	GEM_BUG_ON(!notify_enabled);

	/*
	 * Disable user interrupts / constant intel_engine_notify notifications.
	 */
	list_for_each_entry(i915, &driver_list, tracing_link)
		__i915_disable_notify(i915);

	notify_enabled = false;

	mutex_unlock(&driver_list_lock);
}
