/*
 * mux.h - definitions for the multiplexer interface
 *
 * Copyright (C) 2016 Axentia Technologies AB
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _LINUX_MUX_H
#define _LINUX_MUX_H

#include <linux/device.h>
#include <linux/rwsem.h>

struct mux_control;

struct mux_control_ops {
	int (*set)(struct mux_control *mux, int reg);
};

struct mux_control {
	struct rw_semaphore lock; /* protects the state of the mux */

	struct device dev;
	int id;

	unsigned int states;
	int cached_state;
	int idle_state;

	const struct mux_control_ops *ops;
};

#define to_mux_control(x) container_of((x), struct mux_control, dev)

static inline void *mux_control_priv(struct mux_control *mux)
{
	return mux + 1;
}

struct mux_control *mux_control_alloc(size_t sizeof_priv);
int mux_control_register(struct mux_control *mux);
void mux_control_unregister(struct mux_control *mux);
void mux_control_put(struct mux_control *mux);

int mux_control_select(struct mux_control *mux, int state);
int mux_control_deselect(struct mux_control *mux);

struct mux_control *mux_control_get(struct device *dev);
struct mux_control *devm_mux_control_get(struct device *dev);
void devm_mux_control_put(struct device *dev, struct mux_control *mux);

#endif /* _LINUX_MUX_H */
