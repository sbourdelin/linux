/*
 * Boot constraints header.
 *
 * Copyright (C) 2017 Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 *
 * This file is released under the GPLv2
 */

#include <linux/err.h>
#include <linux/types.h>

struct device;

enum boot_constraint_type {
	BOOT_CONSTRAINT_SUPPLY,
};

struct boot_constraint_supply_info {
	bool enable;
	const char *name;
	unsigned long u_volt_min;
	unsigned long u_volt_max;
};

#ifdef CONFIG_BOOT_CONSTRAINTS
int boot_constraint_add(struct device *dev, enum boot_constraint_type type,
			void *data);
void boot_constraints_remove(struct device *dev);
#else
static inline int boot_constraint_add(struct device *dev,
				      enum boot_constraint_type type, void *data)
{ return -EINVAL; }
static inline void boot_constraints_remove(struct device *dev) {}
#endif
