/*
 * This takes care of boot time constraints, normally set by the Bootloader.
 *
 * Copyright (C) 2017 Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 *
 * This file is released under the GPLv2.
 */

#define pr_fmt(fmt) "Boot Constraints: " fmt

#include <linux/boot_constraint.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

struct constraint {
	struct constraint_dev *cdev;
	struct list_head node;
	enum boot_constraint_type type;

	int (*add)(struct constraint *constraint, void *data);
	void (*remove)(struct constraint *constraint);
	void *private;
};

struct constraint_dev {
	struct device *dev;
	struct list_head node;
	struct list_head constraints;
};

#define for_each_constraint(_constraint, _temp, _cdev)		\
	list_for_each_entry_safe(_constraint, _temp, &_cdev->constraints, node)

/* Global list of all constraint devices currently registered */
static LIST_HEAD(constraint_devices);
static DEFINE_MUTEX(constraint_devices_mutex);

/* Forward declarations of constraints */
static int constraint_supply_add(struct constraint *constraint, void *data);
static void constraint_supply_remove(struct constraint *constraint);

static bool constraints_disabled;

static int __init constraints_disable(char *str)
{
	constraints_disabled = true;
	pr_debug("disabled\n");

	return 0;
}
early_param("boot_constraints_disable", constraints_disable);


/* Boot constraints core */

static struct constraint_dev *constraint_device_find(struct device *dev)
{
	struct constraint_dev *cdev;

	list_for_each_entry(cdev, &constraint_devices, node) {
		if (cdev->dev == dev)
			return cdev;
	}

	return NULL;
}

static struct constraint_dev *constraint_device_allocate(struct device *dev)
{
	struct constraint_dev *cdev;

	cdev = kzalloc(sizeof(*cdev), GFP_KERNEL);
	if (!cdev)
		return ERR_PTR(-ENOMEM);

	cdev->dev = dev;
	INIT_LIST_HEAD(&cdev->node);
	INIT_LIST_HEAD(&cdev->constraints);

	list_add(&cdev->node, &constraint_devices);

	return cdev;
}

static void constraint_device_free(struct constraint_dev *cdev)
{
	list_del(&cdev->node);
	kfree(cdev);
}

static struct constraint_dev *constraint_device_get(struct device *dev)
{
	struct constraint_dev *cdev;

	cdev = constraint_device_find(dev);
	if (cdev)
		return cdev;

	cdev = constraint_device_allocate(dev);
	if (IS_ERR(cdev)) {
		dev_err(dev, "Failed to add constraint dev (%ld)\n",
			PTR_ERR(cdev));
	}

	return cdev;
}

static void constraint_device_put(struct constraint_dev *cdev)
{
	if (!list_empty(&cdev->constraints))
		return;

	constraint_device_free(cdev);
}

static struct constraint *constraint_allocate(struct constraint_dev *cdev,
					      enum boot_constraint_type type)
{
	struct constraint *constraint;
	int (*add)(struct constraint *constraint, void *data);
	void (*remove)(struct constraint *constraint);

	switch (type) {
	case BOOT_CONSTRAINT_SUPPLY:
		add = constraint_supply_add;
		remove = constraint_supply_remove;
		break;
	default:
		return ERR_PTR(-EINVAL);
	}

	constraint = kzalloc(sizeof(*constraint), GFP_KERNEL);
	if (!constraint)
		return ERR_PTR(-ENOMEM);

	constraint->cdev = cdev;
	constraint->type = type;
	constraint->add = add;
	constraint->remove = remove;
	INIT_LIST_HEAD(&constraint->node);

	list_add(&constraint->node, &cdev->constraints);

	return constraint;
}

static void constraint_free(struct constraint *constraint)
{
	list_del(&constraint->node);
	kfree(constraint);
}

int boot_constraint_add(struct device *dev, enum boot_constraint_type type,
			void *data)
{
	struct constraint_dev *cdev;
	struct constraint *constraint;
	int ret;

	if (constraints_disabled)
		return -ENODEV;

	mutex_lock(&constraint_devices_mutex);

	/* Find or add the cdev type first */
	cdev = constraint_device_get(dev);
	if (IS_ERR(cdev)) {
		ret = PTR_ERR(cdev);
		goto unlock;
	}

	constraint = constraint_allocate(cdev, type);
	if (IS_ERR(constraint)) {
		dev_err(dev, "Failed to add constraint type: %d (%ld)\n", type,
			PTR_ERR(constraint));
		ret = PTR_ERR(constraint);
		goto put_cdev;
	}

	/* Set constraint */
	ret = constraint->add(constraint, data);
	if (ret)
		goto free_constraint;

	dev_dbg(dev, "Added boot constraint-type (%d)\n", type);

	mutex_unlock(&constraint_devices_mutex);

	return 0;

free_constraint:
	constraint_free(constraint);
put_cdev:
	constraint_device_put(cdev);
unlock:
	mutex_unlock(&constraint_devices_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(boot_constraint_add);

static void constraint_remove(struct constraint *constraint)
{
	constraint->remove(constraint);
	constraint_free(constraint);
}

void boot_constraints_remove(struct device *dev)
{
	struct constraint_dev *cdev;
	struct constraint *constraint, *temp;

	if (constraints_disabled)
		return;

	mutex_lock(&constraint_devices_mutex);

	cdev = constraint_device_find(dev);
	if (!cdev)
		goto unlock;

	for_each_constraint(constraint, temp, cdev)
		constraint_remove(constraint);

	constraint_device_put(cdev);
unlock:
	mutex_unlock(&constraint_devices_mutex);
}


/* Boot constraints */

/* Boot constraint - Supply */

struct constraint_supply {
	struct boot_constraint_supply_info supply;
	struct regulator *reg;
};

static int constraint_supply_add(struct constraint *constraint, void *data)
{
	struct boot_constraint_supply_info *supply = data;
	struct constraint_supply *csupply;
	struct device *dev = constraint->cdev->dev;
	int ret;

	csupply = kzalloc(sizeof(*csupply), GFP_KERNEL);
	if (!csupply)
		return -ENOMEM;

	csupply->reg = regulator_get(dev, supply->name);
	if (IS_ERR(csupply->reg)) {
		ret = PTR_ERR(csupply->reg);
		if (ret != -EPROBE_DEFER) {
			dev_err(dev, "regulator_get() failed for %s (%d)\n",
				supply->name, ret);
		}
		goto free;
	}

	ret = regulator_set_voltage(csupply->reg, supply->u_volt_min,
				    supply->u_volt_max);
	if (ret) {
		dev_err(dev, "regulator_set_voltage %s failed (%d)\n",
			supply->name, ret);
		goto free_regulator;
	}

	if (supply->enable) {
		ret = regulator_enable(csupply->reg);
		if (ret) {
			dev_err(dev, "regulator_enable %s failed (%d)\n",
				supply->name, ret);
			goto remove_voltage;
		}
	}

	memcpy(&csupply->supply, supply, sizeof(*supply));
	csupply->supply.name = kstrdup_const(supply->name, GFP_KERNEL);
	constraint->private = csupply;

	return 0;

remove_voltage:
	regulator_set_voltage(csupply->reg, 0, INT_MAX);
free_regulator:
	regulator_put(csupply->reg);
free:
	kfree(csupply);

	return ret;
}

static void constraint_supply_remove(struct constraint *constraint)
{
	struct constraint_supply *csupply = constraint->private;
	struct device *dev = constraint->cdev->dev;
	int ret;

	if (csupply->supply.enable) {
		ret = regulator_disable(csupply->reg);
		if (ret)
			dev_err(dev, "regulator_disable failed (%d)\n", ret);
	}

	ret = regulator_set_voltage(csupply->reg, 0, INT_MAX);
	if (ret)
		dev_err(dev, "regulator_set_voltage failed (%d)\n", ret);

	regulator_put(csupply->reg);
	kfree_const(csupply->supply.name);
	kfree(csupply);
}
