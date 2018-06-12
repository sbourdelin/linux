/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2018 Linaro Ltd
 *
 * Author: Daniel Lezcano <daniel.lezcano@linaro.org>
 *
 */
#ifndef __IDLE_INJECTION_H__
#define __IDLE_INJECTION_H__

/* private idle injection device structure */
struct idle_injection_device;

struct idle_injection_device *idle_injection_register(struct cpumask *cpumask);

void idle_injection_unregister(struct idle_injection_device *ii_dev);

int idle_injection_start(struct idle_injection_device *ii_dev);

void idle_injection_stop(struct idle_injection_device *ii_dev);

void idle_injection_set_duration(struct idle_injection_device *ii_dev,
				 unsigned int run_duration_ms,
				 unsigned int idle_duration_ms);

void idle_injection_get_duration(struct idle_injection_device *ii_dev,
				 unsigned int *run_duration_ms,
				 unsigned int *idle_duration_ms);
#endif /* __IDLE_INJECTION_H__ */
