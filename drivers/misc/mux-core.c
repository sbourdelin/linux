/*
 * Multiplexer subsystem
 *
 * Copyright (C) 2016 Axentia Technologies AB
 *
 * Author: Peter Rosin <peda@axentia.se>
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

static struct class mux_class = {
	.name = "mux",
	.owner = THIS_MODULE,
};

static int __init mux_init(void)
{
	return class_register(&mux_class);
}

static void __exit mux_exit(void)
{
	class_unregister(&mux_class);
}

static DEFINE_IDA(mux_ida);

static void mux_control_release(struct device *dev)
{
	struct mux_control *mux = to_mux_control(dev);

	ida_simple_remove(&mux_ida, mux->id);
	kfree(mux);
}

static struct device_type mux_type = {
	.name = "mux-control",
	.release = mux_control_release,
};

struct mux_control *mux_control_alloc(struct device *dev, size_t sizeof_priv)
{
	struct mux_control *mux;

	mux = kzalloc(sizeof(*mux) + sizeof_priv, GFP_KERNEL);
	if (!mux)
		return NULL;

	mux->dev.class = &mux_class;
	mux->dev.type = &mux_type;
	mux->dev.parent = dev;
	mux->dev.of_node = dev->of_node;
	dev_set_drvdata(&mux->dev, mux);

	mux->id = ida_simple_get(&mux_ida, 0, 0, GFP_KERNEL);
	if (mux->id < 0) {
		pr_err("muxX failed to get a device id\n");
		kfree(mux);
		return NULL;
	}
	dev_set_name(&mux->dev, "mux%d", mux->id);

	init_rwsem(&mux->lock);
	mux->cached_state = -1;
	mux->idle_state = -1;

	device_initialize(&mux->dev);

	return mux;
}
EXPORT_SYMBOL_GPL(mux_control_alloc);

int mux_control_register(struct mux_control *mux)
{
	int ret;

	ret = device_add(&mux->dev);
	if (ret < 0)
		return ret;

	if (mux->drv_pdev)
		return ret;

	ret = of_platform_populate(mux->dev.of_node, NULL, NULL, &mux->dev);
	if (ret < 0)
		device_del(&mux->dev);

	return ret;
}
EXPORT_SYMBOL_GPL(mux_control_register);

void mux_control_unregister(struct mux_control *mux)
{
	if (!mux->drv_pdev)
		of_platform_depopulate(&mux->dev);

	device_del(&mux->dev);
}
EXPORT_SYMBOL_GPL(mux_control_unregister);

void mux_control_put(struct mux_control *mux)
{
	struct platform_device *drv_pdev;

	if (!mux)
		return;
	put_device(&mux->dev);

	if (!mux->drv_pdev)
		return;

	if (atomic_read(&mux->dev.kobj.kref.refcount) != 1)
		return;

	/*
	 * Only one ref left, and the mux core created the driver
	 * that presumably holds it. Time to release the driver so
	 * that it can let go of the final ref.
	 */
	drv_pdev = mux->drv_pdev;
	mux->drv_pdev = NULL;
	platform_device_unregister(drv_pdev);
}
EXPORT_SYMBOL_GPL(mux_control_put);

static int mux_control_set(struct mux_control *mux, int state)
{
	int ret = mux->ops->set(mux, state);

	mux->cached_state = ret < 0 ? -1 : state;

	return ret;
}

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

int mux_control_deselect(struct mux_control *mux)
{
	int ret = 0;

	if (mux->idle_state != -1 && mux->cached_state != mux->idle_state)
		ret = mux_control_set(mux, mux->idle_state);

	up_read(&mux->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mux_control_deselect);

static int of_dev_node_match(struct device *dev, const void *data)
{
	return dev->of_node == data;
}

static struct mux_control *of_find_mux_by_node(struct device_node *np)
{
	struct device *dev;

	dev = class_find_device(&mux_class, NULL, np, of_dev_node_match);

	return dev ? to_mux_control(dev) : NULL;
}

struct mux_control *mux_control_get(struct device *dev)
{
	struct device_node *mux_np;
	struct platform_device *drv_pdev;
	struct mux_control *mux;

	if (!dev->of_node)
		return ERR_PTR(-ENODEV);

	mux_np = of_get_child_by_name(dev->of_node, "mux-controller");
	if (!mux_np) {
		mux = of_find_mux_by_node(dev->of_node->parent);
		if (!mux)
			return ERR_PTR(-EPROBE_DEFER);

		return mux;
	}

	drv_pdev = of_platform_device_create(mux_np, "mux-controller", dev);
	of_node_put(mux_np);

	if (!drv_pdev)
		return ERR_PTR(-EPROBE_DEFER);

	mux = of_find_mux_by_node(mux_np);
	if (!mux) {
		platform_device_unregister(drv_pdev);
		return ERR_PTR(-ENODEV);
	}

	/*
	 * Aiee, holding a reference to the driver that holds a
	 * reference back. Circular deps, and refcounts never
	 * hit zero -> leak.
	 * So, watch for the mux-controller refcount to hit one
	 * and release the driver-ref then, knowing that the
	 * driver will (probably) not let go of its back-ref as
	 * long as the mux core holds a ref to it.
	 */

	mux->drv_pdev = drv_pdev;
	return mux;
}
EXPORT_SYMBOL_GPL(mux_control_get);

static void devm_mux_control_free(struct device *dev, void *res)
{
	struct mux_control *mux = *(struct mux_control **)res;

	mux_control_put(mux);
}

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
