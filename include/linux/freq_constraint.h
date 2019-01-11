/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Frequency constraints header.
 *
 * Copyright (C) 2019 Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 */
#ifndef _LINUX_FREQ_CONSTRAINT_H
#define _LINUX_FREQ_CONSTRAINT_H

struct device;
struct freq_constraint;

enum freq_constraint_type {
	FREQ_CONSTRAINT_THERMAL,
	FREQ_CONSTRAINT_USER,
	FREQ_CONSTRAINT_MAX
};

struct freq_constraint *freq_constraint_add(struct device *dev,
					    enum freq_constraint_type type,
					    unsigned long min_freq,
					    unsigned long max_freq);
void freq_constraint_remove(struct device *dev,
			    struct freq_constraint *constraint);
int freq_constraint_update(struct device *dev,
			   struct freq_constraint *constraint,
			   unsigned long min_freq,
			   unsigned long max_freq);

int freq_constraint_set_dev_callback(struct device *dev,
				     void (*callback)(void *param),
				     void *callback_param);
void freq_constraint_remove_dev_callback(struct device *dev);
int freq_constraints_get(struct device *dev, unsigned long *min_freq,
			 unsigned long *max_freq);

#ifdef CONFIG_CPU_FREQ
int freq_constraint_set_cpumask_callback(const struct cpumask *cpumask,
					 void (*callback)(void *param),
					 void *callback_param);
void freq_constraint_remove_cpumask_callback(const struct cpumask *cpumask);
#endif /* CONFIG_CPU_FREQ */

#endif /* _LINUX_FREQ_CONSTRAINT_H */
