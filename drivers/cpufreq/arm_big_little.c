/*
 * ARM big.LITTLE Platforms CPUFreq support
 *
 * Copyright (C) 2013 ARM Ltd.
 * Sudeep KarkadaNagesha <sudeep.karkadanagesha@arm.com>
 *
 * Copyright (C) 2013 Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/cpu_cooling.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_platform.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>
#include <linux/topology.h>
#include <linux/types.h>

#include "arm_big_little.h"

/* Currently we support only two clusters */
#define MAX_CLUSTERS	2

static struct thermal_cooling_device *cdev[MAX_CLUSTERS];
static struct cpufreq_arm_bL_ops *arm_bL_ops;
static struct clk *clk[MAX_CLUSTERS];
static struct cpufreq_frequency_table *freq_table[MAX_CLUSTERS];
static atomic_t cluster_usage[MAX_CLUSTERS];

static inline int raw_cpu_to_cluster(int cpu)
{
	return topology_physical_package_id(cpu);
}

static unsigned int clk_get_cpu_rate(unsigned int cpu)
{
	u32 cur_cluster = raw_cpu_to_cluster(cpu);
	u32 rate = clk_get_rate(clk[cur_cluster]) / 1000;

	pr_debug("%s: cpu: %d, cluster: %d, freq: %u\n", __func__, cpu,
			cur_cluster, rate);

	return rate;
}

static unsigned int
bL_cpufreq_set_rate(u32 cpu, u32 old_cluster, u32 new_cluster, u32 rate)
{
	u32 new_rate;
	int ret;

	new_rate = rate;

	pr_debug("%s: cpu: %d, old cluster: %d, new cluster: %d, freq: %d\n",
			__func__, cpu, old_cluster, new_cluster, new_rate);

	ret = clk_set_rate(clk[new_cluster], new_rate * 1000);
	if (!ret) {
		/*
		 * FIXME: clk_set_rate hasn't returned an error here however it
		 * may be that clk_change_rate failed due to hardware or
		 * firmware issues and wasn't able to report that due to the
		 * current design of the clk core layer. To work around this
		 * problem we will read back the clock rate and check it is
		 * correct. This needs to be removed once clk core is fixed.
		 */
		if (clk_get_rate(clk[new_cluster]) != new_rate * 1000)
			ret = -EIO;
	}

	if (WARN_ON(ret)) {
		pr_err("clk_set_rate failed: %d, new cluster: %d\n", ret,
				new_cluster);

		return ret;
	}

	return 0;
}

/* Set clock frequency */
static int bL_cpufreq_set_target(struct cpufreq_policy *policy,
		unsigned int index)
{
	u32 cpu = policy->cpu, cur_cluster;
	unsigned int freqs_new;

	cur_cluster = raw_cpu_to_cluster(cpu);

	freqs_new = freq_table[cur_cluster][index].frequency;

	return bL_cpufreq_set_rate(cpu, cur_cluster, cur_cluster, freqs_new);
}

static void _put_cluster_clk_and_freq_table(struct device *cpu_dev,
					    const struct cpumask *cpumask)
{
	u32 cluster = raw_cpu_to_cluster(cpu_dev->id);

	if (!freq_table[cluster])
		return;

	clk_put(clk[cluster]);
	dev_pm_opp_free_cpufreq_table(cpu_dev, &freq_table[cluster]);
	if (arm_bL_ops->free_opp_table)
		arm_bL_ops->free_opp_table(cpumask);
	dev_dbg(cpu_dev, "%s: cluster: %d\n", __func__, cluster);
}

static void put_cluster_clk_and_freq_table(struct device *cpu_dev,
					   const struct cpumask *cpumask)
{
	u32 cluster = raw_cpu_to_cluster(cpu_dev->id);

	if (atomic_dec_return(&cluster_usage[cluster]))
		return;

	return _put_cluster_clk_and_freq_table(cpu_dev, cpumask);
}

static int _get_cluster_clk_and_freq_table(struct device *cpu_dev,
					   const struct cpumask *cpumask)
{
	u32 cluster = raw_cpu_to_cluster(cpu_dev->id);
	int ret;

	if (freq_table[cluster])
		return 0;

	ret = arm_bL_ops->init_opp_table(cpumask);
	if (ret) {
		dev_err(cpu_dev, "%s: init_opp_table failed, cpu: %d, err: %d\n",
				__func__, cpu_dev->id, ret);
		goto out;
	}

	ret = dev_pm_opp_init_cpufreq_table(cpu_dev, &freq_table[cluster]);
	if (ret) {
		dev_err(cpu_dev, "%s: failed to init cpufreq table, cpu: %d, err: %d\n",
				__func__, cpu_dev->id, ret);
		goto free_opp_table;
	}

	clk[cluster] = clk_get(cpu_dev, NULL);
	if (!IS_ERR(clk[cluster])) {
		dev_dbg(cpu_dev, "%s: clk: %p & freq table: %p, cluster: %d\n",
				__func__, clk[cluster], freq_table[cluster],
				cluster);
		return 0;
	}

	dev_err(cpu_dev, "%s: Failed to get clk for cpu: %d, cluster: %d\n",
			__func__, cpu_dev->id, cluster);
	ret = PTR_ERR(clk[cluster]);
	dev_pm_opp_free_cpufreq_table(cpu_dev, &freq_table[cluster]);

free_opp_table:
	if (arm_bL_ops->free_opp_table)
		arm_bL_ops->free_opp_table(cpumask);
out:
	dev_err(cpu_dev, "%s: Failed to get data for cluster: %d\n", __func__,
			cluster);
	return ret;
}

static int get_cluster_clk_and_freq_table(struct device *cpu_dev,
					  const struct cpumask *cpumask)
{
	u32 cluster = raw_cpu_to_cluster(cpu_dev->id);
	int ret;

	if (atomic_inc_return(&cluster_usage[cluster]) != 1)
		return 0;

	ret = _get_cluster_clk_and_freq_table(cpu_dev, cpumask);
	if (ret)
		atomic_dec(&cluster_usage[cluster]);
	return ret;
}

/* Per-CPU initialization */
static int bL_cpufreq_init(struct cpufreq_policy *policy)
{
	u32 cur_cluster = raw_cpu_to_cluster(policy->cpu);
	struct device *cpu_dev;
	int ret;

	cpu_dev = get_cpu_device(policy->cpu);
	if (!cpu_dev) {
		pr_err("%s: failed to get cpu%d device\n", __func__,
				policy->cpu);
		return -ENODEV;
	}

	cpumask_copy(policy->cpus, topology_core_cpumask(policy->cpu));

	ret = get_cluster_clk_and_freq_table(cpu_dev, policy->cpus);
	if (ret)
		return ret;

	ret = cpufreq_table_validate_and_show(policy, freq_table[cur_cluster]);
	if (ret) {
		dev_err(cpu_dev, "CPU %d, cluster: %d invalid freq table\n",
			policy->cpu, cur_cluster);
		put_cluster_clk_and_freq_table(cpu_dev, policy->cpus);
		return ret;
	}

	if (arm_bL_ops->get_transition_latency)
		policy->cpuinfo.transition_latency =
			arm_bL_ops->get_transition_latency(cpu_dev);
	else
		policy->cpuinfo.transition_latency = CPUFREQ_ETERNAL;

	dev_info(cpu_dev, "%s: CPU %d initialized\n", __func__, policy->cpu);
	return 0;
}

static int bL_cpufreq_exit(struct cpufreq_policy *policy)
{
	struct device *cpu_dev;
	int cur_cluster = raw_cpu_to_cluster(policy->cpu);

	cpufreq_cooling_unregister(cdev[cur_cluster]);
	cdev[cur_cluster] = NULL;

	cpu_dev = get_cpu_device(policy->cpu);
	if (!cpu_dev) {
		pr_err("%s: failed to get cpu%d device\n", __func__,
				policy->cpu);
		return -ENODEV;
	}

	put_cluster_clk_and_freq_table(cpu_dev, policy->related_cpus);
	dev_dbg(cpu_dev, "%s: Exited, cpu: %d\n", __func__, policy->cpu);

	return 0;
}

static void bL_cpufreq_ready(struct cpufreq_policy *policy)
{
	struct device *cpu_dev = get_cpu_device(policy->cpu);
	int cur_cluster = raw_cpu_to_cluster(policy->cpu);
	struct device_node *np;

	np = of_node_get(cpu_dev->of_node);
	if (WARN_ON(!np))
		return;

	if (of_find_property(np, "#cooling-cells", NULL)) {
		u32 power_coefficient = 0;

		of_property_read_u32(np, "dynamic-power-coefficient",
				     &power_coefficient);

		cdev[cur_cluster] = of_cpufreq_power_cooling_register(np,
				policy->related_cpus, power_coefficient, NULL);
		if (IS_ERR(cdev[cur_cluster])) {
			dev_err(cpu_dev,
				"running cpufreq without cooling device: %ld\n",
				PTR_ERR(cdev[cur_cluster]));
			cdev[cur_cluster] = NULL;
		}
	}
	of_node_put(np);
}

static struct cpufreq_driver bL_cpufreq_driver = {
	.name			= "arm-big-little",
	.flags			= CPUFREQ_STICKY |
					CPUFREQ_HAVE_GOVERNOR_PER_POLICY |
					CPUFREQ_NEED_INITIAL_FREQ_CHECK,
	.verify			= cpufreq_generic_frequency_table_verify,
	.target_index		= bL_cpufreq_set_target,
	.get			= clk_get_cpu_rate,
	.init			= bL_cpufreq_init,
	.exit			= bL_cpufreq_exit,
	.ready			= bL_cpufreq_ready,
	.attr			= cpufreq_generic_attr,
};

int bL_cpufreq_register(struct cpufreq_arm_bL_ops *ops)
{
	int ret;

	if (arm_bL_ops) {
		pr_debug("%s: Already registered: %s, exiting\n", __func__,
				arm_bL_ops->name);
		return -EBUSY;
	}

	if (!ops || !strlen(ops->name) || !ops->init_opp_table) {
		pr_err("%s: Invalid arm_bL_ops, exiting\n", __func__);
		return -ENODEV;
	}

	arm_bL_ops = ops;

	ret = cpufreq_register_driver(&bL_cpufreq_driver);
	if (ret) {
		pr_info("%s: Failed registering platform driver: %s, err: %d\n",
				__func__, ops->name, ret);
		arm_bL_ops = NULL;
	} else {
		pr_info("%s: Registered platform driver: %s\n",
				__func__, ops->name);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(bL_cpufreq_register);

void bL_cpufreq_unregister(struct cpufreq_arm_bL_ops *ops)
{
	if (arm_bL_ops != ops) {
		pr_err("%s: Registered with: %s, can't unregister, exiting\n",
				__func__, arm_bL_ops->name);
		return;
	}

	cpufreq_unregister_driver(&bL_cpufreq_driver);
	pr_info("%s: Un-registered platform driver: %s\n", __func__,
			arm_bL_ops->name);
	arm_bL_ops = NULL;
}
EXPORT_SYMBOL_GPL(bL_cpufreq_unregister);

MODULE_AUTHOR("Viresh Kumar <viresh.kumar@linaro.org>");
MODULE_DESCRIPTION("Generic ARM big LITTLE cpufreq driver");
MODULE_LICENSE("GPL v2");
