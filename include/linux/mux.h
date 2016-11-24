/*
 * mux.h - definitions for the multiplexer interface
 *
 * Copyright (C) 2016 Axentia Technologies AB
 *
 * Author: Peter Rosin <peda@axentia.se>
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
struct platform_device;

struct mux_control_ops {
	int (*set)(struct mux_control *mux, int state);
};

/**
 * struct mux_control - Represents a mux controller.
 * @lock:		Protects the mux controller state.
 * @dev:		Device structure.
 * @id:			Used to identify the device internally.
 * @states:		The number of mux controller states.
 * @cached_state:	The current mux controller state, or -1 if none.
 * @idle_state:		The mux controller state to use when inactive, or -1
 *			for none.
 * @ops:		Mux controller operations.
 */
struct mux_control {
	struct rw_semaphore lock; /* protects the state of the mux */

	struct device dev;
	int id;
	struct platform_device *drv_pdev;

	unsigned int states;
	int cached_state;
	int idle_state;

	const struct mux_control_ops *ops;
};

#define to_mux_control(x) container_of((x), struct mux_control, dev)

/**
 * mux_control_priv() - Get the extra memory reserved by mux_control_alloc().
 * @mux: The mux-control to get the extra memory from.
 *
 * Return: Pointer to the private memory requested by the allocator.
 */
static inline void *mux_control_priv(struct mux_control *mux)
{
	return mux + 1;
}

/**
 * mux_control_alloc() - Allocate a mux-control.
 * @dev: The device implementing the mux interface.
 * @sizeof_priv: Size of extra memory area for private use by the caller.
 *
 * Return: A pointer to the new mux-control, NULL on failure.
 */
struct mux_control *mux_control_alloc(struct device *dev, size_t sizeof_priv);

/**
 * mux_control_register() - Register a mux-control, thus readying it for use.
 * @mux: The mux-control to register.
 *
 * Do not retry registration of the same mux-control on failure. You should
 * instead put it away with mux_control_put() and allocate a new one, if you
 * for some reason would like to retry registration.
 *
 * Return: Zero on success or a negative errno on error.
 */
int mux_control_register(struct mux_control *mux);

/**
 * mux_control_unregister() - Take the mux-control off-line.
 * @mux: The mux-control to unregister.
 *
 * mux_control_unregister() reverses the effects of mux_control_register().
 * But not completely, you should not try to call mux_control_register()
 * on a mux-control that has been registered before.
 */
void mux_control_unregister(struct mux_control *mux);

/**
 * mux_control_put() - Put away the mux-control for good.
 * @mux: The mux-control to put away.
 *
 * mux_control_put() reverses the effects of either mux_control_alloc() or
 * mux_control_get().
 */
void mux_control_put(struct mux_control *mux);

/**
 * mux_control_select() - Select the given multiplexer state.
 * @mux: The mux-control to request a change of state from.
 * @state: The new requested state.
 *
 * Make sure to call mux_control_deselect() when the operation is complete and
 * the mux-control is free for others to use, but do not call
 * mux_control_deselect() if mux_control_select() fails.
 *
 * Return: 0 if the requested state was already active, or 1 it the
 * mux-control state was changed to the requested state. Or a negavive
 * errno on error.
 *
 * Note that the difference in return value of zero or one is of
 * questionable value; especially if the mux-control has several independent
 * consumers, which is something the consumers should not be making
 * assumptions about.
 */
int mux_control_select(struct mux_control *mux, int state);

/**
 * mux_control_deselect() - Deselect the previously selected multiplexer state.
 * @mux: The mux-control to deselect.
 *
 * Return: 0 on success and a negative errno on error. An error can only
 * occur if the mux has an idle state. Note that even if an error occurs, the
 * mux-control is unlocked for others to access.
 */
int mux_control_deselect(struct mux_control *mux);

/**
 * mux_control_get() - Get the mux-control for a device.
 * @dev: The device that needs a mux-control.
 *
 * Return: A pointer to the mux-control, or an ERR_PTR with a negative errno.
 */
struct mux_control *mux_control_get(struct device *dev);

/**
 * devm_mux_control_get() - Get the mux-control for a device, with resource
 *			    management.
 * @dev: The device that needs a mux-control.
 *
 * Return: Pointer to the mux-control, or an ERR_PTR with a negative errno.
 */
struct mux_control *devm_mux_control_get(struct device *dev);

/**
 * devm_mux_control_put() - Resource-managed version mux_control_put().
 * @dev: The device that originally got the mux-control.
 * @mux: The mux-control to put away.
 *
 * Note that you do not normally need to call this function.
 */
void devm_mux_control_put(struct device *dev, struct mux_control *mux);

#endif /* _LINUX_MUX_H */
