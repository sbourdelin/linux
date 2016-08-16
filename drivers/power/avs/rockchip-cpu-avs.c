/*
 * Rockchip CPU AVS support.
 *
 * Copyright (c) 2016 ROCKCHIP, Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>
#include <linux/types.h>
#include "../../base/power/opp/opp.h"

#define MAX_NAME_LEN		22
#define LEAKAGE_TABLE_END	~1
#define INVALID_VALUE		0xff

struct leakage_volt_table {
	int    min;
	int    max;
	int    volt;
};

struct leakage_volt_table *leakage_volt_table;

struct rockchip_cpu_avs {
	struct leakage_volt_table **volt_table;
	struct notifier_block   cpufreq_notify;
};

#define notifier_to_avs(_n) container_of(_n, struct rockchip_cpu_avs, \
	cpufreq_notify)

static unsigned char rockchip_fetch_leakage(struct device *dev)
{
	struct nvmem_cell *cell;
	unsigned char *buf;
	size_t len;
	unsigned char leakage = INVALID_VALUE;

	cell = nvmem_cell_get(dev, "cpu_leakage");
	if (IS_ERR(cell)) {
		pr_err("failed to get cpu_leakage cell\n");
		return INVALID_VALUE;
	}

	buf = (unsigned char *)nvmem_cell_read(cell, &len);

	nvmem_cell_put(cell);

	if (IS_ERR(buf)) {
		pr_err("failed to read nvmem cell\n");
		return INVALID_VALUE;
	}
	leakage = buf[0];
	kfree(buf);

	return leakage;
}

static int rockchip_fetch_leakage_volt_table(
	struct device_node *np,
	struct leakage_volt_table **table,
	const char *name)
{
	struct leakage_volt_table *volt_table = NULL;
	const struct property *prop;
	int count, i;

	prop = of_find_property(np, name, NULL);
	if (!prop) {
		pr_err("failed to find prop %s\n", name);
		return -EINVAL;
	}
	if (!prop->value) {
		pr_err("%s value is NULL\n", name);
		return -ENODATA;
	}

	count = of_property_count_u32_elems(np, name);
	if (count < 0) {
		pr_err("Invalid %s property (%d)\n", name, count);
		return -EINVAL;
	}
	if (count % 3) {
		pr_err("Invalid number of elements in %s property (%d)\n",
		       name, count);
		return -EINVAL;
	}

	volt_table = kzalloc(sizeof(*table) * (count / 3 + 1), GFP_KERNEL);
	if (!volt_table)
		return -ENOMEM;

	if (volt_table) {
		for (i = 0; i < count / 3; i++) {
			of_property_read_s32_index(np, name, 3 * i,
						   &volt_table[i].min);
			of_property_read_s32_index(np, name, 3 * i + 1,
						   &volt_table[i].max);
			of_property_read_s32_index(np, name, 3 * i + 2,
						   &volt_table[i].volt);
		}
		volt_table[i].min = 0;
		volt_table[i].max = 0;
		volt_table[i].volt = LEAKAGE_TABLE_END;
	}

	*table = volt_table;

	return 0;
}

static int rockchip_parse_leakage_volt(unsigned char leakage,
				       unsigned int cpu,
				       struct rockchip_cpu_avs *avs)
{
	struct leakage_volt_table *table;
	unsigned int i, j, id;
	int volt;

	id = topology_physical_package_id(cpu);
	if (id < 0)
		id = 0;

	table = avs->volt_table[id];
	if (!table)
		return 0;

	for (i = 0; table[i].volt != LEAKAGE_TABLE_END; i++) {
		if (leakage >= table[i].min)
			j = i;
	}

	volt = table[j].volt;

	return volt;
}

static void rockchip_adjust_opp_table(struct device *dev,
				      struct cpufreq_frequency_table *table,
				      int volt)
{
	struct opp_table *opp_table;
	struct cpufreq_frequency_table *pos;
	struct dev_pm_opp *opp;
	int ret;

	rcu_read_lock();

	opp_table = _find_opp_table(dev);
	if (IS_ERR(opp_table)) {
		pr_err("failed to find OPP table\n");
		rcu_read_unlock();
		return;
	}

	cpufreq_for_each_valid_entry(pos, table) {
		opp = dev_pm_opp_find_freq_exact(dev, pos->frequency * 1000,
						 true);
		if (IS_ERR(opp)) {
			pr_err("failed to find OPP for freq %d (%d)\n",
			       pos->frequency, ret);
			continue;
		}
		opp->u_volt += volt;
		opp->u_volt_min += volt;
		opp->u_volt_max += volt;
	}

	rcu_read_unlock();
}

static void rockchip_adjust_volt_by_leakage(
	struct device *dev,
	struct cpufreq_frequency_table *table,
	unsigned int cpu,
	struct rockchip_cpu_avs *avs)
{
	unsigned char leakage;
	int volt;

	/* fetch leakage from efuse */
	leakage = rockchip_fetch_leakage(dev);
	if (leakage == INVALID_VALUE) {
		pr_err("cpu%d leakage invalid\n", cpu);
		return;
	}

	/* fetch adjust volt from table */
	volt = rockchip_parse_leakage_volt(leakage, cpu, avs);
	if (volt)
		rockchip_adjust_opp_table(dev, table, volt);

	pr_debug("cpu%d, leakage=%d, adjust_volt=%d\n", cpu, leakage, volt);
}

static int rockchip_cpu_avs_notifier(struct notifier_block *nb,
				     unsigned long event, void *data)
{
	struct rockchip_cpu_avs *avs = notifier_to_avs(nb);
	struct cpufreq_policy *policy = data;
	struct device *dev;

	struct cpufreq_frequency_table *table;

	if (event != CPUFREQ_START)
		goto out;

	dev = get_cpu_device(policy->cpu);
	if (!dev) {
		pr_err("cpu%d Failed to get device\n", policy->cpu);
		goto out;
	}

	table = cpufreq_frequency_get_table(policy->cpu);
	if (!table) {
		pr_err("cpu%d CPUFreq table not found\n", policy->cpu);
		goto out;
	}

	rockchip_adjust_volt_by_leakage(dev, table, policy->cpu, avs);

out:

	return NOTIFY_OK;
}

static const struct of_device_id rockchip_cpu_avs_match[] = {
	{
		.compatible = "rockchip,rk3399-cpu-avs",
	},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, rockchip_cpu_avs_match);

static int rockchip_cpu_avs_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct rockchip_cpu_avs *avs;
	char name[MAX_NAME_LEN];
	int i, ret, cpu, id;
	int last_id = -1;
	int cluster_num = 0;

	avs = devm_kzalloc(&pdev->dev, sizeof(struct rockchip_cpu_avs),
			   GFP_KERNEL);
	if (!avs)
		return -ENOMEM;

	avs->cpufreq_notify.notifier_call = rockchip_cpu_avs_notifier;

	for_each_online_cpu(cpu) {
		id = topology_physical_package_id(cpu);
		if (id < 0)
			id = 0;
		if (id != last_id) {
			last_id = id;
			cluster_num++;
		}
	}

	avs->volt_table = devm_kzalloc(&pdev->dev,
		sizeof(struct leakage_volt_table) * cluster_num, GFP_KERNEL);
	if (!avs->volt_table)
		return -ENOMEM;

	for (i = 0; i < cluster_num; i++) {
		snprintf(name, MAX_NAME_LEN, "leakage-volt-cluster%d", i);
		ret = rockchip_fetch_leakage_volt_table(np, &avs->volt_table[i],
							name);
		if (ret)
			continue;
	}

	return cpufreq_register_notifier(&avs->cpufreq_notify,
		CPUFREQ_POLICY_NOTIFIER);
}

static struct platform_driver rockchip_cpu_avs_driver = {
	.probe   = rockchip_cpu_avs_probe,
	.driver  = {
		.name  = "rockchip-cpu-avs",
		.of_match_table = rockchip_cpu_avs_match,
		.suppress_bind_attrs = true,
	},
};

static int __init rockchip_cpu_avs_module_init(void)
{
	return platform_driver_probe(&rockchip_cpu_avs_driver,
				     rockchip_cpu_avs_probe);
}

subsys_initcall(rockchip_cpu_avs_module_init);

MODULE_DESCRIPTION("Rockchip CPU AVS driver");
MODULE_AUTHOR("Finley Xiao <finley.xiao@rock-chips.com>");
MODULE_LICENSE("GPL v2");
