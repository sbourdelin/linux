// SPDX-License-Identifier: GPL-2.0
/*
 * This manages frequency constraints on devices.
 *
 * Copyright (C) 2019 Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/freq_constraint.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

struct freq_constraint_dev {
	struct list_head node;
	struct device *dev;
};

struct freq_pair {
	unsigned long min;
	unsigned long max;
};

struct freq_constraint {
	struct list_head node;
	enum freq_constraint_type type;
	struct freq_pair freq;
};

struct freq_constraints {
	struct list_head node;
	struct list_head devices;
	struct list_head constraints;
	void (*callback)(void *param);
	void *callback_param;
	struct kref kref;
	struct mutex lock;
	struct work_struct work;

	/* Aggregated constraint values */
	struct freq_pair freq;
};

enum fc_event {
	ADD,
	REMOVE,
	UPDATE
};

/* List of all frequency constraints */
static LIST_HEAD(fcs_list);
static DEFINE_MUTEX(fc_mutex);

/* Return true if aggregated constraints are updated, else false */
static bool fcs_reevaluate(struct freq_constraints *fcs)
{
	struct freq_pair limits[FREQ_CONSTRAINT_MAX] = {
			[0 ... FREQ_CONSTRAINT_MAX - 1] = {0, ULONG_MAX} };
	struct freq_constraint *constraint;
	unsigned long min = 0, max = ULONG_MAX;
	bool updated = false;
	int i;

	/* Find min/max freq under each constraint type */
	list_for_each_entry(constraint, &fcs->constraints, node) {
		if (constraint->freq.min > limits[constraint->type].min)
			limits[constraint->type].min = constraint->freq.min;

		if (constraint->freq.max < limits[constraint->type].max)
			limits[constraint->type].max = constraint->freq.max;
	}

	/*
	 * Resolve possible 'internal' conflicts for each constraint type,
	 * the max limit wins over the min.
	 */
	for (i = 0; i < FREQ_CONSTRAINT_MAX; i++) {
		if (limits[i].min > limits[i].max)
			limits[i].min = limits[i].max;
	}

	/*
	 * Thermal constraints are always honored, adjust conflicting other
	 * constraints.
	 */
	if (limits[FREQ_CONSTRAINT_USER].min > limits[FREQ_CONSTRAINT_THERMAL].max)
		limits[FREQ_CONSTRAINT_USER].min = 0;

	if (limits[FREQ_CONSTRAINT_USER].max < limits[FREQ_CONSTRAINT_THERMAL].min)
		limits[FREQ_CONSTRAINT_USER].max = ULONG_MAX;

	for (i = 0; i < FREQ_CONSTRAINT_MAX; i++) {
		min = max(min, limits[i].min);
		max = min(max, limits[i].max);
	}

	WARN_ON(min > max);

	if (fcs->freq.min != min) {
		fcs->freq.min = min;
		updated = true;
	}

	if (fcs->freq.max != max) {
		fcs->freq.max = max;
		updated = true;
	}

	return updated;
}

/* Return true if aggregated constraints are updated, else false */
static bool _fcs_update(struct freq_constraints *fcs, struct freq_pair *freq,
			enum fc_event event)
{
	bool updated = false;

	switch (event) {
	case ADD:
		if (freq->min > fcs->freq.max || freq->max < fcs->freq.min)
			return fcs_reevaluate(fcs);

		if (freq->min > fcs->freq.min) {
			fcs->freq.min = freq->min;
			updated = true;
		}

		if (freq->max < fcs->freq.max) {
			fcs->freq.max = freq->max;
			updated = true;
		}

		return updated;

	case REMOVE:
		if (freq->min == fcs->freq.min || freq->max == fcs->freq.max)
			return fcs_reevaluate(fcs);

		return false;

	case UPDATE:
		return fcs_reevaluate(fcs);

	default:
		WARN_ON(1);
		return false;
	}
}

static void fcs_update(struct freq_constraints *fcs, struct freq_pair *freq,
		       enum fc_event event)
{
	mutex_lock(&fcs->lock);

	if (_fcs_update(fcs, freq, event)) {
		if (fcs->callback)
			schedule_work(&fcs->work);
	}

	mutex_unlock(&fcs->lock);
}

static void fcs_work_handler(struct work_struct *work)
{
	struct freq_constraints *fcs = container_of(work,
			struct freq_constraints, work);

	fcs->callback(fcs->callback_param);
}

static void free_fcdev(struct freq_constraint_dev *fcdev,
		       struct freq_constraints *fcs)
{
	mutex_lock(&fcs->lock);
	list_del(&fcdev->node);
	mutex_unlock(&fcs->lock);

	kfree(fcdev);
}

static struct freq_constraint_dev *alloc_fcdev(struct device *dev,
					       struct freq_constraints *fcs)
{
	struct freq_constraint_dev *fcdev;

	fcdev = kzalloc(sizeof(*fcdev), GFP_KERNEL);
	if (!fcdev)
		return ERR_PTR(-ENOMEM);

	fcdev->dev = dev;

	mutex_lock(&fcs->lock);
	list_add(&fcdev->node, &fcs->devices);
	mutex_unlock(&fcs->lock);

	return fcdev;
}

static struct freq_constraint_dev *find_fcdev(struct device *dev,
					      struct freq_constraints *fcs)
{
	struct freq_constraint_dev *fcdev;

	mutex_lock(&fcs->lock);
	list_for_each_entry(fcdev, &fcs->devices, node) {
		if (fcdev->dev == dev) {
			mutex_unlock(&fcs->lock);
			return fcdev;
		}
	}
	mutex_unlock(&fcs->lock);

	return NULL;
}

static void free_constraint(struct freq_constraints *fcs,
			    struct freq_constraint *constraint)
{
	mutex_lock(&fcs->lock);
	list_del(&constraint->node);
	mutex_unlock(&fcs->lock);

	kfree(constraint);
}

static struct freq_constraint *alloc_constraint(struct freq_constraints *fcs,
						enum freq_constraint_type type,
						unsigned long min_freq,
						unsigned long max_freq)
{
	struct freq_constraint *constraint;

	constraint = kzalloc(sizeof(*constraint), GFP_KERNEL);
	if (!constraint)
		return ERR_PTR(-ENOMEM);

	constraint->type = type;
	constraint->freq.min = min_freq;
	constraint->freq.max = max_freq;

	mutex_lock(&fcs->lock);
	list_add(&constraint->node, &fcs->constraints);
	mutex_unlock(&fcs->lock);

	return constraint;
}

static void free_fcs(struct freq_constraints *fcs)
{
	list_del(&fcs->node);
	mutex_destroy(&fcs->lock);
	kfree(fcs);
}

static void fcs_kref_release(struct kref *kref)
{
	struct freq_constraints *fcs = container_of(kref, struct freq_constraints, kref);
	struct freq_constraint_dev *fcdev, *temp;

	WARN_ON(!list_empty(&fcs->constraints));

	list_for_each_entry_safe(fcdev, temp, &fcs->devices, node)
		free_fcdev(fcdev, fcs);

	free_fcs(fcs);
	mutex_unlock(&fc_mutex);
}

static void put_fcs(struct freq_constraints *fcs)
{
	kref_put_mutex(&fcs->kref, fcs_kref_release, &fc_mutex);
}

static struct freq_constraints *alloc_fcs(struct device *dev)
{
	struct freq_constraints *fcs;
	struct freq_constraint_dev *fcdev;

	fcs = kzalloc(sizeof(*fcs), GFP_KERNEL);
	if (!fcs)
		return ERR_PTR(-ENOMEM);

	mutex_init(&fcs->lock);
	INIT_LIST_HEAD(&fcs->devices);
	INIT_LIST_HEAD(&fcs->constraints);
	INIT_WORK(&fcs->work, fcs_work_handler);
	kref_init(&fcs->kref);

	fcs->freq.min = 0;
	fcs->freq.max = ULONG_MAX;

	fcdev = alloc_fcdev(dev, fcs);
	if (IS_ERR(fcdev)) {
		free_fcs(fcs);
		return ERR_CAST(fcdev);
	}

	mutex_lock(&fc_mutex);
	list_add(&fcs->node, &fcs_list);
	mutex_unlock(&fc_mutex);

	return fcs;
}

static struct freq_constraints *find_fcs(struct device *dev)
{
	struct freq_constraints *fcs;

	mutex_lock(&fc_mutex);
	list_for_each_entry(fcs, &fcs_list, node) {
		if (find_fcdev(dev, fcs)) {
			kref_get(&fcs->kref);
			mutex_unlock(&fc_mutex);
			return fcs;
		}
	}
	mutex_unlock(&fc_mutex);

	return ERR_PTR(-ENODEV);
}

static struct freq_constraints *get_fcs(struct device *dev)
{
	struct freq_constraints *fcs;

	fcs = find_fcs(dev);
	if (!IS_ERR(fcs))
		return fcs;

	return alloc_fcs(dev);
}

struct freq_constraint *freq_constraint_add(struct device *dev,
					    enum freq_constraint_type type,
					    unsigned long min_freq,
					    unsigned long max_freq)
{
	struct freq_constraints *fcs;
	struct freq_constraint *constraint;

	if (!max_freq || min_freq > max_freq) {
		dev_err(dev, "freq-constraints: Invalid min/max frequency\n");
		return ERR_PTR(-EINVAL);
	}

	fcs = get_fcs(dev);
	if (IS_ERR(fcs))
		return ERR_CAST(fcs);

	constraint = alloc_constraint(fcs, type, min_freq, max_freq);
	if (IS_ERR(constraint)) {
		put_fcs(fcs);
		return constraint;
	}

	fcs_update(fcs, &constraint->freq, ADD);

	return constraint;
}
EXPORT_SYMBOL_GPL(freq_constraint_add);

void freq_constraint_remove(struct device *dev,
			    struct freq_constraint *constraint)
{
	struct freq_constraints *fcs;
	struct freq_pair freq = constraint->freq;

	fcs = find_fcs(dev);
	if (IS_ERR(fcs)) {
		dev_err(dev, "Failed to find freq-constraint\n");
		return;
	}

	free_constraint(fcs, constraint);
	fcs_update(fcs, &freq, REMOVE);

	/*
	 * Put the reference twice, once for the freed constraint and one for
	 * the above call to find_fcs().
	 */
	put_fcs(fcs);
	put_fcs(fcs);
}
EXPORT_SYMBOL_GPL(freq_constraint_remove);

int freq_constraint_update(struct device *dev,
			   struct freq_constraint *constraint,
			   unsigned long min_freq,
			   unsigned long max_freq)
{
	struct freq_constraints *fcs;

	if (!max_freq || min_freq > max_freq) {
		dev_err(dev, "freq-constraints: Invalid min/max frequency\n");
		return -EINVAL;
	}

	fcs = find_fcs(dev);
	if (IS_ERR(fcs)) {
		dev_err(dev, "Failed to find freq-constraint\n");
		return -ENODEV;
	}

	mutex_lock(&fcs->lock);
	constraint->freq.min = min_freq;
	constraint->freq.max = max_freq;
	mutex_unlock(&fcs->lock);

	fcs_update(fcs, &constraint->freq, UPDATE);

	put_fcs(fcs);

	return 0;
}
EXPORT_SYMBOL_GPL(freq_constraint_update);

int freq_constraints_get(struct device *dev, unsigned long *min_freq,
			 unsigned long *max_freq)
{
	struct freq_constraints *fcs;

	fcs = find_fcs(dev);
	if (IS_ERR(fcs))
		return -ENODEV;

	mutex_lock(&fcs->lock);
	*min_freq = fcs->freq.min;
	*max_freq = fcs->freq.max;
	mutex_unlock(&fcs->lock);

	put_fcs(fcs);
	return 0;
}

static int set_fcs_callback(struct device *dev, struct freq_constraints *fcs,
			    void (*callback)(void *param), void *callback_param)
{
	if (unlikely(fcs->callback)) {
		dev_err(dev, "freq-constraint: callback already registered\n");
		return -EBUSY;
	}

	fcs->callback = callback;
	fcs->callback_param = callback_param;
	return 0;
}

int freq_constraint_set_dev_callback(struct device *dev,
				     void (*callback)(void *param),
				     void *callback_param)
{
	struct freq_constraints *fcs;
	int ret;

	if (WARN_ON(!callback))
		return -ENODEV;

	fcs = get_fcs(dev);
	if (IS_ERR(fcs))
		return PTR_ERR(fcs);

	mutex_lock(&fcs->lock);
	ret = set_fcs_callback(dev, fcs, callback, callback_param);
	mutex_unlock(&fcs->lock);

	if (ret)
		put_fcs(fcs);

	return ret;
}
EXPORT_SYMBOL_GPL(freq_constraint_set_dev_callback);

/* Caller must call put_fcs() after using it */
static struct freq_constraints *remove_callback(struct device *dev)
{
	struct freq_constraints *fcs;

	fcs = find_fcs(dev);
	if (IS_ERR(fcs)) {
		dev_err(dev, "freq-constraint: device not registered\n");
		return fcs;
	}

	mutex_lock(&fcs->lock);

	cancel_work_sync(&fcs->work);

	if (fcs->callback) {
		fcs->callback = NULL;
		fcs->callback_param = NULL;
	} else {
		dev_err(dev, "freq-constraint: Call back not registered for device\n");
	}
	mutex_unlock(&fcs->lock);

	return fcs;
}

void freq_constraint_remove_dev_callback(struct device *dev)
{
	struct freq_constraints *fcs;

	fcs = remove_callback(dev);
	if (IS_ERR(fcs))
		return;

	/*
	 * Put the reference twice, once for the callback removal and one for
	 * the above call to remove_callback().
	 */
	put_fcs(fcs);
	put_fcs(fcs);
}
EXPORT_SYMBOL_GPL(freq_constraint_remove_dev_callback);

#ifdef CONFIG_CPU_FREQ
static void remove_cpumask_fcs(struct freq_constraints *fcs,
			       const struct cpumask *cpumask, int stop_cpu)
{
	struct device *cpu_dev;
	int cpu;

	for_each_cpu(cpu, cpumask) {
		if (unlikely(cpu == stop_cpu))
			return;

		cpu_dev = get_cpu_device(cpu);
		if (unlikely(!cpu_dev))
			continue;

		put_fcs(fcs);
	}
}

int freq_constraint_set_cpumask_callback(const struct cpumask *cpumask,
					 void (*callback)(void *param),
					 void *callback_param)
{
	struct freq_constraints *fcs = ERR_PTR(-ENODEV);
	struct device *cpu_dev, *first_cpu_dev = NULL;
	struct freq_constraint_dev *fcdev;
	int cpu, ret;

	if (WARN_ON(cpumask_empty(cpumask) || !callback))
		return -ENODEV;

	/* Find a CPU for which fcs already exists */
	for_each_cpu(cpu, cpumask) {
		cpu_dev = get_cpu_device(cpu);
		if (unlikely(!cpu_dev))
			continue;

		if (unlikely(!first_cpu_dev))
			first_cpu_dev = cpu_dev;

		fcs = find_fcs(cpu_dev);
		if (!IS_ERR(fcs))
			break;
	}

	/* Allocate fcs if it wasn't already present */
	if (IS_ERR(fcs)) {
		if (unlikely(!first_cpu_dev)) {
			pr_err("device structure not available for any CPU\n");
			return -ENODEV;
		}

		fcs = alloc_fcs(first_cpu_dev);
		if (IS_ERR(fcs))
			return PTR_ERR(fcs);
	}

	for_each_cpu(cpu, cpumask) {
		cpu_dev = get_cpu_device(cpu);
		if (unlikely(!cpu_dev))
			continue;

		if (!find_fcdev(cpu_dev, fcs)) {
			fcdev = alloc_fcdev(cpu_dev, fcs);
			if (IS_ERR(fcdev)) {
				remove_cpumask_fcs(fcs, cpumask, cpu);
				put_fcs(fcs);
				return PTR_ERR(fcdev);
			}
		}

		kref_get(&fcs->kref);
	}

	mutex_lock(&fcs->lock);
	ret = set_fcs_callback(first_cpu_dev, fcs, callback, callback_param);
	mutex_unlock(&fcs->lock);

	if (ret)
		remove_cpumask_fcs(fcs, cpumask, cpu);

	put_fcs(fcs);

	return ret;
}

void freq_constraint_remove_cpumask_callback(const struct cpumask *cpumask)
{
	struct freq_constraints *fcs;
	struct device *cpu_dev = NULL;
	int cpu;

	for_each_cpu(cpu, cpumask) {
		cpu_dev = get_cpu_device(cpu);
		if (likely(cpu_dev))
			break;
	}

	if (!cpu_dev)
		return;

	fcs = remove_callback(cpu_dev);
	if (IS_ERR(fcs))
		return;

	remove_cpumask_fcs(fcs, cpumask, -1);

	put_fcs(fcs);
}
#endif /* CONFIG_CPU_FREQ */
