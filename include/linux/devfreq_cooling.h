/*
 * devfreq_cooling: Thermal cooling device implementation for devices using
 *                  devfreq
 *
 * Copyright (C) 2014-2015 ARM Limited
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __DEVFREQ_COOLING_H__
#define __DEVFREQ_COOLING_H__

#include <linux/devfreq.h>
#include <linux/thermal.h>
#include <linux/bitops.h>

/* Flags for the devfreq cooling interface. */
#define GET_DIRECT_DYNAMIC_POWER BIT(0)

/**
 * struct devfreq_cooling_power - Devfreq cooling power ops
 * @get_static_power:	Take voltage, in mV, and return the static power
 *			in mW.  If NULL, the static power is assumed
 *			to be 0.
 * @get_dynamic_power:	Take voltage, in mV, and frequency, in HZ, and return
 *			the dynamic power draw in mW. This function is called
 *			every time when the GET_DIRECT_DYNAMIC_POWER flag is
 *			set and the thermal framework calculates the current
 *			power for this device. If the flag is not set and this
 *			is NULL, a simple power model is used.
 * @power2state:	It receives the maximum power that the device should
 *			consume and it should return the needed 'state'.
 *			This function should be registered when the flag
 *			GET_DIRECT_DYNAMIC_POWER is set.
 * @dyn_power_coeff:	Coefficient for the simple dynamic power model in
 *			mW/(MHz mV mV).
 *			If get_dynamic_power() is NULL, then the
 *			dynamic power is calculated as
 *			@dyn_power_coeff * frequency * voltage^2
 */
struct devfreq_cooling_power {
	unsigned long (*get_static_power)(struct devfreq *devfreq,
					  unsigned long voltage);
	unsigned long (*get_dynamic_power)(struct devfreq *devfreq,
					   unsigned long freq,
					   unsigned long voltage);
	unsigned long (*power2state)(struct devfreq *devfreq, u32 power);
	unsigned long dyn_power_coeff;
};

#ifdef CONFIG_DEVFREQ_THERMAL

struct thermal_cooling_device *
of_devfreq_cooling_register_power(struct device_node *np, struct devfreq *df,
				  struct devfreq_cooling_power *dfc_power,
				  unsigned long flags);
struct thermal_cooling_device *
of_devfreq_cooling_register(struct device_node *np, struct devfreq *df);
struct thermal_cooling_device *devfreq_cooling_register(struct devfreq *df);
void devfreq_cooling_unregister(struct thermal_cooling_device *dfc);

#else /* !CONFIG_DEVFREQ_THERMAL */

struct inline thermal_cooling_device *
of_devfreq_cooling_register_power(struct device_node *np, struct devfreq *df,
				  struct devfreq_cooling_power *dfc_power,
				  unsigned long flags)
{
	return ERR_PTR(-EINVAL);
}

static inline struct thermal_cooling_device *
of_devfreq_cooling_register(struct device_node *np, struct devfreq *df)
{
	return ERR_PTR(-EINVAL);
}

static inline struct thermal_cooling_device *
devfreq_cooling_register(struct devfreq *df)
{
	return ERR_PTR(-EINVAL);
}

static inline void
devfreq_cooling_unregister(struct thermal_cooling_device *dfc)
{
}

#endif /* CONFIG_DEVFREQ_THERMAL */
#endif /* __DEVFREQ_COOLING_H__ */
