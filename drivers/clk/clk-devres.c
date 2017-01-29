/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/export.h>
#include <linux/gfp.h>

static int devm_clk_create_devres(struct device *dev, struct clk *clk,
				  void (*release)(struct device *, void *))
{
	struct clk **ptr;

	ptr = devres_alloc(release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return -ENOMEM;

	*ptr = clk;
	devres_add(dev, ptr);

	return 0;
}

static int devm_clk_match(struct device *dev, void *res, void *data)
{
	struct clk **c = res;
	if (!c || !*c) {
		WARN_ON(!c || !*c);
		return 0;
	}
	return *c == data;
}

#define DEFINE_DEVM_CLK_DESTROY_OP(destroy_op)				\
static void devm_##destroy_op##_release(struct device *dev, void *res)	\
{									\
	destroy_op(*(struct clk **)res);				\
}									\
									\
void devm_##destroy_op(struct device *dev, struct clk *clk)		\
{									\
	WARN_ON(devres_release(dev, devm_##destroy_op##_release,	\
				devm_clk_match, clk));			\
}									\
EXPORT_SYMBOL(devm_##destroy_op)

#define DEFINE_DEVM_CLK_OP(create_op, destroy_op)			\
DEFINE_DEVM_CLK_DESTROY_OP(destroy_op);					\
int devm_##create_op(struct device *dev, struct clk *clk)		\
{									\
	int error;							\
									\
	error = create_op(clk);						\
	if (error)							\
		return error;						\
									\
	error = devm_clk_create_devres(dev, clk,			\
					devm_##destroy_op##_release);	\
	if (error) {							\
		destroy_op(clk);					\
		return error;						\
	}								\
									\
	return 0;							\
}									\
EXPORT_SYMBOL(devm_##create_op)

DEFINE_DEVM_CLK_DESTROY_OP(clk_put);
DEFINE_DEVM_CLK_OP(clk_prepare, clk_unprepare);
DEFINE_DEVM_CLK_OP(clk_prepare_enable, clk_disable_unprepare);

struct clk *devm_clk_get(struct device *dev, const char *id)
{
	struct clk *clk;
	int error;

	clk = clk_get(dev, id);
	if (!IS_ERR(clk)) {
		error = devm_clk_create_devres(dev, clk, devm_clk_put_release);
		if (error) {
			clk_put(clk);
			return ERR_PTR(error);
		}
	}

	return clk;
}
EXPORT_SYMBOL(devm_clk_get);

struct clk *devm_get_clk_from_child(struct device *dev,
				    struct device_node *np, const char *con_id)
{
	struct clk *clk;
	int error;

	clk = of_clk_get_by_name(np, con_id);
	if (!IS_ERR(clk)) {
		error = devm_clk_create_devres(dev, clk, devm_clk_put_release);
		if (error) {
			clk_put(clk);
			return ERR_PTR(error);
		}
	}

	return clk;
}
EXPORT_SYMBOL(devm_get_clk_from_child);
