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

/*
 * Allocate a mux-control, plus an extra memory area for private use
 * by the caller.
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
	mux->priv = mux + 1;

	mux->id = ida_simple_get(&mux_ida, 0, 0, GFP_KERNEL);
	if (mux->id < 0) {
		pr_err("mux-controlX failed to get device id\n");
		kfree(mux);
		return NULL;
	}
	dev_set_name(&mux->dev, "mux:control%d", mux->id);

	return mux;
}
EXPORT_SYMBOL_GPL(mux_control_alloc);

/*
 * Register the mux-control, thus readying it for use.
 */
int mux_control_register(struct mux_control *mux)
{
	/* If the calling driver did not initialize of_node, do it here */
	if (!mux->dev.of_node && mux->dev.parent)
		mux->dev.of_node = mux->dev.parent->of_node;

	return device_add(&mux->dev);
}
EXPORT_SYMBOL_GPL(mux_control_register);

/*
 * Take the mux-control off-line.
 */
void mux_control_unregister(struct mux_control *mux)
{
	device_del(&mux->dev);
}
EXPORT_SYMBOL_GPL(mux_control_unregister);

/*
 * Put away the mux-control for good.
 */
void mux_control_put(struct mux_control *mux)
{
	if (!mux)
		return;
	put_device(&mux->dev);
}
EXPORT_SYMBOL_GPL(mux_control_free);

/*
 * Select the given multiplexer channel. Call mux_control_deselect()
 * when the operation is complete on the multiplexer channel, and the
 * multiplexer is free for others to use.
 */
int mux_control_select(struct mux_control *mux, int reg)
{
	int ret;

	if (down_read_trylock(&mux->lock)) {
		if (mux->cache == reg)
			return 0;

		/* Sigh, the mux needs updating... */
		up_read(&mux->lock);
	}

	/* ...or it's just contended. */
	down_write(&mux->lock);

	if (mux->cache == reg) {
		/*
		 * Hmmm, someone else changed the mux to my liking.
		 * That makes me wonder how long I waited for nothing...
		 */
		downgrade_write(&mux->lock);
		return 0;
	}

	ret = mux->ops->set(mux, reg);
	if (ret < 0) {
		up_write(&mux->lock);
		return ret;
	}

	mux->cache = reg;
	downgrade_write(&mux->lock);

	return 1;
}
EXPORT_SYMBOL_GPL(mux_control_select);

/*
 * Deselect the previously selected multiplexer channel.
 */
int mux_control_deselect(struct mux_control *mux)
{
	if (mux->do_idle && mux->cache != mux->idle_state) {
		mux->ops->set(mux, mux->idle_state);
		mux->cache = mux->idle_state;
	}

	up_read(&mux->lock);

	return 0;
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

static struct mux_control *of_mux_control_get(struct device_node *np, int index)
{
	struct device_node *mux_np;
	struct mux_control *mux;

	mux_np = of_parse_phandle(np, "control-muxes", index);
	if (!mux_np)
		return NULL;

	mux = of_find_mux_by_node(mux_np);
	of_node_put(mux_np);

	return mux;
}

/*
 * Get a named mux.
 */
struct mux_control *mux_control_get(struct device *dev, const char *mux_name)
{
	struct device_node *np = dev->of_node;
	struct mux_control *mux;
	int index;

	index = of_property_match_string(np, "control-mux-names", mux_name);
	if (index < 0) {
		dev_err(dev, "failed to get control-mux %s:%s(%i)\n",
			np->full_name, mux_name ?: "", index);
		return ERR_PTR(index);
	}

	mux = of_mux_control_get(np, index);
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

/*
 * Get a named mux, with resource management.
 */
struct mux_control *devm_mux_control_get(struct device *dev,
					 const char *mux_name)
{
	struct mux_control **ptr, *mux;

	ptr = devres_alloc(devm_mux_control_free, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	mux = mux_control_get(dev, mux_name);
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

/*
 * Resource-managed version mux_control_put.
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
