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
#include <linux/of.h>
#include <linux/pm_domain.h>
#include <linux/pm_qos.h>
#include <linux/pm_runtime.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/tick.h>

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

static bool cpu_pd_down_ok(struct dev_pm_domain *pd)
{
	struct generic_pm_domain *genpd = pd_to_genpd(pd);
	struct cpu_pm_domain *cpu_pd = to_cpu_pd(genpd);
	int qos_ns = pm_qos_request(PM_QOS_CPU_DMA_LATENCY);
	u64 sleep_ns;
	ktime_t earliest, next_wakeup;
	int cpu;
	int i;

	/* Reset the last set genpd state, default to index 0 */
	genpd->state_idx = 0;

	/* We don't want to power down, if QoS is 0 */
	if (!qos_ns)
		return false;

	/*
	 * Find the sleep time for the cluster.
	 * The time between now and the first wake up of any CPU that
	 * are in this domain hierarchy is the time available for the
	 * domain to be idle.
	 *
	 * We only care about the next wakeup for any online CPU in that
	 * cluster. Hotplug off any of the CPUs that we care about will
	 * wait on the genpd lock, until we are done. Any other CPU hotplug
	 * is not of consequence to our sleep time.
	 */
	earliest = ktime_set(KTIME_SEC_MAX, 0);
	for_each_cpu_and(cpu, cpu_pd->cpus, cpu_online_mask) {
		next_wakeup = tick_nohz_get_next_wakeup(cpu);
		if (earliest.tv64 > next_wakeup.tv64)
			earliest = next_wakeup;
	}

	sleep_ns = ktime_to_ns(ktime_sub(earliest, ktime_get()));
	if (sleep_ns <= 0)
		return false;

	/*
	 * Find the deepest sleep state that satisfies the residency
	 * requirement and the QoS constraint
	 */
	for (i = genpd->state_count - 1; i >= 0; i--) {
		u64 state_sleep_ns;

		state_sleep_ns = genpd->states[i].power_off_latency_ns +
			genpd->states[i].power_on_latency_ns +
			genpd->states[i].residency_ns;

		/*
		 * If we can't sleep to save power in the state, move on
		 * to the next lower idle state.
		 */
		if (state_sleep_ns > sleep_ns)
			continue;

		/*
		 * We also don't want to sleep more than we should to
		 * gaurantee QoS.
		 */
		if (state_sleep_ns < (qos_ns * NSEC_PER_USEC))
			break;
	}

	if (i >= 0)
		genpd->state_idx = i;

	return (i >= 0);
}

static struct dev_power_governor cpu_pd_gov = {
	.power_down_ok = cpu_pd_down_ok,
};

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

	ret = pm_genpd_init(genpd, &cpu_pd_gov, false);
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

static struct generic_pm_domain *alloc_genpd(const char *name)
{
	struct generic_pm_domain *genpd;

	genpd = kzalloc(sizeof(*genpd), GFP_KERNEL);
	if (!genpd)
		return ERR_PTR(-ENOMEM);

	genpd->name = kstrndup(name, CPU_PD_NAME_MAX, GFP_KERNEL);
	if (!genpd->name) {
		kfree(genpd);
		return ERR_PTR(-ENOMEM);
	}

	return genpd;
}

/**
 * of_init_cpu_pm_domain() - Initialize a CPU PM domain from a device node
 *
 * @dn: The domain provider's device node
 * @ops: The power_on/_off callbacks for the domain
 *
 * Returns the generic_pm_domain (genpd) pointer to the domain on success
 */
static struct generic_pm_domain *of_init_cpu_pm_domain(struct device_node *dn,
				const struct cpu_pd_ops *ops)
{
	struct cpu_pm_domain *pd = NULL;
	struct generic_pm_domain *genpd = NULL;
	int ret = -ENOMEM;

	if (!of_device_is_available(dn))
		return ERR_PTR(-ENODEV);

	genpd = alloc_genpd(dn->full_name);
	if (IS_ERR(genpd))
		return genpd;

	genpd->of_node = dn;

	/* Populate platform specific states from DT */
	if (ops->populate_state_data) {
		struct device_node *np;
		int i;

		/* Initialize the arm,idle-state properties */
		ret = pm_genpd_of_parse_power_states(genpd);
		if (ret) {
			pr_warn("%s domain states not initialized (%d)\n",
					dn->full_name, ret);
			goto fail;
		}
		for (i = 0; i < genpd->state_count; i++) {
			ret = ops->populate_state_data(genpd->states[i].of_node,
						&genpd->states[i].param);
			of_node_put(np);
			if (ret)
				goto fail;
		}
	}

	genpd = cpu_pd_init(genpd, ops);
	if (IS_ERR(genpd))
		goto fail;

	ret = of_genpd_add_provider_simple(dn, genpd);
	if (ret)
		pr_warn("Unable to add genpd %s as provider\n",
				pd->genpd->name);

	return genpd;
fail:
	kfree(genpd->name);
	kfree(genpd);
	if (pd)
		kfree(pd->cpus);
	kfree(pd);
	return ERR_PTR(ret);
}

static struct generic_pm_domain *of_get_cpu_domain(struct device_node *dn,
		const struct cpu_pd_ops *ops, int cpu)
{
	struct of_phandle_args args;
	struct generic_pm_domain *genpd, *parent;
	int ret;

	/* Do we have this domain? If not, create the domain */
	args.np = dn;
	args.args_count = 0;

	genpd = of_genpd_get_from_provider(&args);
	if (!IS_ERR(genpd))
		return genpd;

	genpd = of_init_cpu_pm_domain(dn, ops);
	if (IS_ERR(genpd))
		return genpd;

	/* Is there a domain provider for this domain? */
	ret = of_parse_phandle_with_args(dn, "power-domains",
			"#power-domain-cells", 0, &args);
	if (ret < 0)
		goto skip_parent;

	/* Find its parent and attach this domain to it, recursively */
	parent = of_get_cpu_domain(args.np, ops, cpu);
	if (IS_ERR(parent))
		goto skip_parent;

	ret = cpu_pd_attach_domain(parent, genpd);
	if (ret)
		pr_err("Unable to attach domain %s to parent %s\n",
				genpd->name, parent->name);

skip_parent:
	of_node_put(dn);
	return genpd;
}

/**
 * of_setup_cpu_pd_single() - Setup the PM domains for a CPU
 *
 * @cpu: The CPU for which the PM domain is to be set up.
 * @ops: The PM domain suspend/resume ops for the CPU's domain
 *
 * If the CPU PM domain exists already, then the CPU is attached to
 * that CPU PD. If it doesn't, the domain is created, the @ops are
 * set for power_on/power_off callbacks and then the CPU is attached
 * to that domain. If the domain was created outside this framework,
 * then we do not attach the CPU to the domain.
 */
int of_setup_cpu_pd_single(int cpu, const struct cpu_pd_ops *ops)
{

	struct device_node *dn, *np;
	struct generic_pm_domain *genpd;
	struct cpu_pm_domain *cpu_pd;

	np = of_get_cpu_node(cpu, NULL);
	if (!np)
		return -ENODEV;

	dn = of_parse_phandle(np, "power-domains", 0);
	of_node_put(np);
	if (!dn)
		return -ENODEV;

	/* Find the genpd for this CPU, create if not found */
	genpd = of_get_cpu_domain(dn, ops, cpu);
	of_node_put(dn);
	if (IS_ERR(genpd))
		return PTR_ERR(genpd);

	cpu_pd = to_cpu_pd(genpd);
	if (!cpu_pd) {
		pr_err("%s: Genpd was created outside CPU PM domains\n",
				__func__);
		return -ENOENT;
	}

	return cpu_pd_attach_cpu(genpd, cpu);
}
EXPORT_SYMBOL(of_setup_cpu_pd_single);

/**
 * of_setup_cpu_pd() - Setup the PM domains for all CPUs
 *
 * @ops: The PM domain suspend/resume ops for all the domains
 *
 * Setup the CPU PM domain and attach all possible CPUs to their respective
 * domains. The domains are created if not already and then attached.
 */
int of_setup_cpu_pd(const struct cpu_pd_ops *ops)
{
	int cpu;
	int ret;

	for_each_possible_cpu(cpu) {
		ret = of_setup_cpu_pd_single(cpu, ops);
		if (ret)
			break;
	}

	return ret;
}
EXPORT_SYMBOL(of_setup_cpu_pd);
