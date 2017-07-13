/*
 * mux/consumer.h - definitions for the multiplexer consumer interface
 *
 * Copyright (C) 2017 Axentia Technologies AB
 *
 * Author: Peter Rosin <peda@axentia.se>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _LINUX_MUX_CONSUMER_H
#define _LINUX_MUX_CONSUMER_H

struct device;
struct mux_control;

#if IS_ENABLED(CONFIG_MULTIPLEXER)

unsigned int mux_control_states(struct mux_control *mux);
int __must_check mux_control_select(struct mux_control *mux,
				    unsigned int state);
int __must_check mux_control_try_select(struct mux_control *mux,
					unsigned int state);
int mux_control_deselect(struct mux_control *mux);

struct mux_control *mux_control_get(struct device *dev, const char *mux_name);
void mux_control_put(struct mux_control *mux);

struct mux_control *devm_mux_control_get(struct device *dev,
					 const char *mux_name);

#else

static inline unsigned int mux_control_states(struct mux_control *mux)
{
	return 0;
}

static inline int __must_check mux_control_select(struct mux_control *mux,
						  unsigned int state)
{
	return -EINVAL;
}

static inline int __must_check mux_control_try_select(struct mux_control *mux,
						      unsigned int state)
{
	return -EINVAL;
}

static inline int mux_control_deselect(struct mux_control *mux)
{
	return -EINVAL;
}

static inline struct mux_control *mux_control_get(struct device *dev,
						  const char *mux_name)
{
	return ERR_PTR(-ENODEV);
}

static inline void mux_control_put(struct mux_control *mux) {}

static inline struct mux_control *devm_mux_control_get(struct device *dev,
						       const char *mux_name)
{
	return ERR_PTR(-ENODEV);
}

#endif /* CONFIG_MULTIPLEXER */

#endif /* _LINUX_MUX_CONSUMER_H */
