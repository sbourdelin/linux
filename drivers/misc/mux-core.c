/*
 * Multiplexer subsystem
 *
 * Copyright (C) 2016 Axentia Technologies AB
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) "mux-core: " fmt

#include <linux/device.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/module.h>
#include <linux/mux.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/slab.h>

static struct bus_type mux_bus_type = {
	.name = "mux",
};

static int __init mux_init(void)
{
	return bus_register(&mux_bus_type);
}

static void __exit mux_exit(void)
{
	bus_unregister(&mux_bus_type);
}

static DEFINE_IDA(mux_ida);

static void mux_control_release(struct device *dev)
{
	struct mux_control *mux = to_mux_control(dev);

	ida_simple_remove(&mux_ida, mux->id);
	kfree(mux);
}

static struct device_type mux_control_type = {
	.name = "mux-control",
	.release = mux_control_release,
};

/**
 * mux_control_alloc - allocate a mux-control
 * @sizeof_priv: Size of extra memory area for private use by the caller.
 *
 * Returns the new mux-control.
 */
struct mux_control *mux_control_alloc(size_t sizeof_priv)
{
	struct mux_control *mux;

	mux = kzalloc(sizeof(*mux) + sizeof_priv, GFP_KERNEL);
	if (!mux)
		return NULL;

	mux->dev.bus = &mux_bus_type;
	mux->dev.type = &mux_control_type;
	device_initialize(&mux->dev);
	dev_set_drvdata(&mux->dev, mux);

	init_rwsem(&mux->lock);

	mux->id = ida_simple_get(&mux_ida, 0, 0, GFP_KERNEL);
	if (mux->id < 0) {
		pr_err("mux-controlX failed to get device id\n");
		kfree(mux);
		return NULL;
	}
	dev_set_name(&mux->dev, "mux:control%d", mux->id);

	mux->cached_state = -1;
	mux->idle_state = -1;

	return mux;
}
EXPORT_SYMBOL_GPL(mux_control_alloc);

/**
 * mux_control_register - register a mux-control, thus readying it for use
 * @mux: The mux-control to register.
 *
 * Returns zero on success or a negative errno on error.
 */
int mux_control_register(struct mux_control *mux)
{
	int ret;

	/* If the calling driver did not initialize of_node, do it here */
	if (!mux->dev.of_node && mux->dev.parent)
		mux->dev.of_node = mux->dev.parent->of_node;

	ret = device_add(&mux->dev);
	if (ret < 0)
		return ret;

	ret = of_platform_populate(mux->dev.of_node, NULL, NULL, &mux->dev);
	if (ret < 0)
		device_del(&mux->dev);

	return ret;
}
EXPORT_SYMBOL_GPL(mux_control_register);

/**
 * mux_control_unregister - take the mux-control off-line, reversing the
 *			    effexts of mux_control_register
 * @mux: the mux-control to unregister.
 */
void mux_control_unregister(struct mux_control *mux)
{
	of_platform_depopulate(&mux->dev);
	device_del(&mux->dev);
}
EXPORT_SYMBOL_GPL(mux_control_unregister);

/**
 * mux_control_put - put away the mux-control for good, reversing the
 *		     effects of either mux_control_alloc or mux_control_get
 * @mux: The mux-control to put away.
 */
void mux_control_put(struct mux_control *mux)
{
	if (!mux)
		return;
	put_device(&mux->dev);
}
EXPORT_SYMBOL_GPL(mux_control_put);

static int mux_control_set(struct mux_control *mux, int state)
{
	int ret = mux->ops->set(mux, state);

	mux->cached_state = ret < 0 ? -1 : state;

	return ret;
}

/**
 * mux_control_select - select the given multiplexer state
 * @mux: The mux-control to request a change of state from.
 * @state: The new requested state.
 *
 * Returns 0 if the requested state was already active, or 1 it the
 * mux-control state was changed to the requested state. Or a negavive
 * errno on error.
 * Note that the difference in return value of zero or one is of
 * questionable value; especially if the mux-control has several independent
 * consumers, which is something the consumers should not be making
 * assumptions about.
 *
 * Make sure to call mux_control_deselect when the operation is complete and
 * the mux-control is free for others to use, but do not call
 * mux_control_deselect if mux_control_select fails.
 */
int mux_control_select(struct mux_control *mux, int state)
{
	int ret;

	if (down_read_trylock(&mux->lock)) {
		if (mux->cached_state == state)
			return 0;

		/* Sigh, the mux needs updating... */
		up_read(&mux->lock);
	}

	/* ...or it's just contended. */
	down_write(&mux->lock);

	if (mux->cached_state == state) {
		/*
		 * Hmmm, someone else changed the mux to my liking.
		 * That makes me wonder how long I waited for nothing?
		 */
		downgrade_write(&mux->lock);
		return 0;
	}

	ret = mux_control_set(mux, state);
	if (ret < 0) {
		if (mux->idle_state != -1)
			mux_control_set(mux, mux->idle_state);

		up_write(&mux->lock);
		return ret;
	}

	downgrade_write(&mux->lock);

	return 1;
}
EXPORT_SYMBOL_GPL(mux_control_select);

/**
 * mux_control_deselect - deselect the previously selected multiplexer state
 * @mux: The mux-control to deselect.
 *
 * Returns 0 on success and a negative errno on error. An error can only
 * occur if the mux has an idle state. Note that even if an error occurs, the
 * mux-control is unlocked for others to access.
 */
int mux_control_deselect(struct mux_control *mux)
{
	int ret = 0;

	if (mux->idle_state != -1 && mux->cached_state != mux->idle_state)
		ret = mux_control_set(mux, mux->idle_state);

	up_read(&mux->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mux_control_deselect);

static int of_dev_node_match(struct device *dev, void *data)
{
	return dev->of_node == data;
}

static struct mux_control *of_find_mux_by_node(struct device_node *np)
{
	struct device *dev;

	dev = bus_find_device(&mux_bus_type, NULL, np, of_dev_node_match);

	return dev ? to_mux_control(dev) : NULL;
}

/**
 * mux_control_get - get the mux-control for a device
 * @dev: The device that needs a mux-control.
 *
 * Returns the mux-control.
 */
struct mux_control *mux_control_get(struct device *dev)
{
	struct mux_control *mux;

	if (!dev->of_node)
		return ERR_PTR(-ENODEV);

	mux = of_find_mux_by_node(dev->of_node->parent);
	if (!mux)
		return ERR_PTR(-EPROBE_DEFER);

	return mux;
}
EXPORT_SYMBOL_GPL(mux_control_get);

static void devm_mux_control_free(struct device *dev, void *res)
{
	struct mux_control *mux = *(struct mux_control **)res;

	mux_control_put(mux);
}

/**
 * devm_mux_control_get - get the mux-control for a device, with resource
 *			  management
 * @dev: The device that needs a mux-control.
 *
 * Returns the mux-control.
 */
struct mux_control *devm_mux_control_get(struct device *dev)
{
	struct mux_control **ptr, *mux;

	ptr = devres_alloc(devm_mux_control_free, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	mux = mux_control_get(dev);
	if (IS_ERR(mux)) {
		devres_free(ptr);
		return mux;
	}

	*ptr = mux;
	devres_add(dev, ptr);

	return mux;
}
EXPORT_SYMBOL_GPL(devm_mux_control_get);

static int devm_mux_control_match(struct device *dev, void *res, void *data)
{
	struct mux_control **r = res;

	if (!r || !*r) {
		WARN_ON(!r || !*r);
		return 0;
	}

	return *r == data;
}

/**
 * devm_mux_control_put - resource-managed version mux_control_put
 * @dev: The device that originally got the mux-control.
 * @mux: The mux-control to put away.
 *
 * Note that you do not normally need to call this function.
 */
void devm_mux_control_put(struct device *dev, struct mux_control *mux)
{
	WARN_ON(devres_release(dev, devm_mux_control_free,
			       devm_mux_control_match, mux));
}
EXPORT_SYMBOL_GPL(devm_mux_control_put);

subsys_initcall(mux_init);
module_exit(mux_exit);

MODULE_AUTHOR("Peter Rosin <peda@axentia.se");
MODULE_DESCRIPTION("MUX subsystem");
MODULE_LICENSE("GPL v2");
