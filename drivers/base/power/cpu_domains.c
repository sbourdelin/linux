/*
 * drivers/base/power/cpu_domains.c - Helper functions to create CPU PM domains.
 *
 * Copyright (C) 2016 Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpu_domains.h>
#include <linux/cpu_pm.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/pm_domain.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>

#define CPU_PD_NAME_MAX 36

struct cpu_pm_domain {
	struct list_head link;
	struct cpu_pd_ops ops;
	struct generic_pm_domain *genpd;
	struct cpu_pm_domain *parent;
	cpumask_var_t cpus;
};

/* List of CPU PM domains we care about */
static LIST_HEAD(of_cpu_pd_list);
static DEFINE_MUTEX(cpu_pd_list_lock);

static inline struct cpu_pm_domain *to_cpu_pd(struct generic_pm_domain *d)
{
	struct cpu_pm_domain *pd;
	struct cpu_pm_domain *res = NULL;

	rcu_read_lock();
	list_for_each_entry_rcu(pd, &of_cpu_pd_list, link)
		if (pd->genpd == d) {
			res = pd;
			break;
		}
	rcu_read_unlock();

	return res;
}

static int cpu_pd_power_on(struct generic_pm_domain *genpd)
{
	struct cpu_pm_domain *pd = to_cpu_pd(genpd);

	return pd->ops.power_on ? pd->ops.power_on() : 0;
}

static int cpu_pd_power_off(struct generic_pm_domain *genpd)
{
	struct cpu_pm_domain *pd = to_cpu_pd(genpd);

	return pd->ops.power_off ? pd->ops.power_off(genpd->state_idx,
					genpd->states[genpd->state_idx].param,
					pd->cpus) : 0;
}

/**
 * cpu_pd_attach_domain:  Attach a child CPU PM to its parent
 *
 * @parent: The parent generic PM domain
 * @child: The child generic PM domain
 *
 * Generally, the child PM domain is the one to which CPUs are attached.
 */
int cpu_pd_attach_domain(struct generic_pm_domain *parent,
				struct generic_pm_domain *child)
{
	struct cpu_pm_domain *cpu_pd, *parent_cpu_pd;
	int ret;

	ret = pm_genpd_add_subdomain(parent, child);
	if (ret) {
		pr_err("%s: Unable to add sub-domain (%s) to %s.\n err=%d",
				__func__, child->name, parent->name, ret);
		return ret;
	}

	cpu_pd = to_cpu_pd(child);
	parent_cpu_pd = to_cpu_pd(parent);

	if (cpu_pd && parent_cpu_pd)
		cpu_pd->parent = parent_cpu_pd;

	return ret;
}
EXPORT_SYMBOL(cpu_pd_attach_domain);

/**
 * cpu_pd_attach_cpu:  Attach a CPU to its CPU PM domain.
 *
 * @genpd: The parent generic PM domain
 * @cpu: The CPU number
 */
int cpu_pd_attach_cpu(struct generic_pm_domain *genpd, int cpu)
{
	int ret;
	struct device *cpu_dev;
	struct cpu_pm_domain *cpu_pd = to_cpu_pd(genpd);

	cpu_dev = get_cpu_device(cpu);
	if (!cpu_dev) {
		pr_warn("%s: Unable to get device for CPU%d\n",
				__func__, cpu);
		return -ENODEV;
	}

	ret = genpd_dev_pm_attach(cpu_dev);
	if (ret)
		dev_warn(cpu_dev,
			"%s: Unable to attach to power-domain: %d\n",
			__func__, ret);
	else
		dev_dbg(cpu_dev, "Attached to domain\n");

	while (!ret && cpu_pd) {
		cpumask_set_cpu(cpu, cpu_pd->cpus);
		cpu_pd = cpu_pd->parent;
	};

	return ret;
}
EXPORT_SYMBOL(cpu_pd_attach_cpu);

/**
 * cpu_pd_init: Initialize a CPU PM domain for a genpd
 *
 * @genpd: The initialized generic PM domain object.
 * @ops: The power_on/power_off ops for the domain controller.
 *
 * Initialize a CPU PM domain based on a generic PM domain. The platform driver
 * is expected to setup the genpd object and the states associated with the
 * generic PM domain, before calling this function.
 */
struct generic_pm_domain *cpu_pd_init(struct generic_pm_domain *genpd,
				const struct cpu_pd_ops *ops)
{
	int ret = -ENOMEM;
	struct cpu_pm_domain *pd;

	if (IS_ERR_OR_NULL(genpd))
		return ERR_PTR(-EINVAL);

	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (!pd)
		goto fail;

	if (!zalloc_cpumask_var(&pd->cpus, GFP_KERNEL))
		goto fail;

	genpd->power_off = cpu_pd_power_off;
	genpd->power_on = cpu_pd_power_on;
	genpd->flags |= GENPD_FLAG_IRQ_SAFE;
	pd->genpd = genpd;
	pd->ops.power_on = ops->power_on;
	pd->ops.power_off = ops->power_off;

	INIT_LIST_HEAD_RCU(&pd->link);
	mutex_lock(&cpu_pd_list_lock);
	list_add_rcu(&pd->link, &of_cpu_pd_list);
	mutex_unlock(&cpu_pd_list_lock);

	ret = pm_genpd_init(genpd, &simple_qos_governor, false);
	if (ret) {
		pr_err("Unable to initialize domain %s\n", genpd->name);
		goto fail;
	}

	pr_debug("adding %s as CPU PM domain\n", pd->genpd->name);

	return genpd;
fail:
	kfree(genpd->name);
	kfree(genpd);
	if (pd)
		kfree(pd->cpus);
	kfree(pd);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(cpu_pd_init);
