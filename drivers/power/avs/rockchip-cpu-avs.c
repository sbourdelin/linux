/*
  * Rockchip CPU AVS support.
  *
  * Copyright (c) 2016 Rockchip Electronics Co. Ltd.
  * Author: Finley Xiao <finley.xiao@rock-chips.com>
  *
  * This program is free software; you can redistribute it and/or modify it
  * under the terms of version 2 of the GNU General Public License as
  * published by the Free Software Foundation.
  *
  * This program is distributed in the hope that it will be useful, but WITHOUT
  * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
  * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
  * more details.
  */

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
	int min;
	int max;
	int volt;
};

struct cluster_info {
	int adjust_volt;
	unsigned char leakage;
	struct leakage_volt_table *table;
};

struct rockchip_cpu_avs {
	struct device *dev;
	struct cluster_info *cluster;
	struct notifier_block cpufreq_notify;
};

#define notifier_to_avs(_n) container_of(_n, struct rockchip_cpu_avs, \
	cpufreq_notify)

static int rockchip_get_leakage(struct device *cpu_dev, unsigned char *leakage)
{
	struct nvmem_cell *cell;
	unsigned char *buf;
	size_t len;

	cell = nvmem_cell_get(cpu_dev, "cpu_leakage");
	if (IS_ERR(cell)) {
		dev_err(cpu_dev, "avs failed to get cpu_leakage cell\n");
		return PTR_ERR(cell);
	}

	buf = (unsigned char *)nvmem_cell_read(cell, &len);

	nvmem_cell_put(cell);

	if (IS_ERR(buf))
		return PTR_ERR(buf);

	if (buf[0] == INVALID_VALUE)
		return -EINVAL;

	*leakage = buf[0];
	kfree(buf);

	return 0;
}

static int rockchip_get_offset_volt(unsigned char leakage,
				    struct leakage_volt_table *table, int *volt)
{
	unsigned int i, j;

	if (!table)
		return -EINVAL;

	for (i = 0; table[i].volt != LEAKAGE_TABLE_END; i++) {
		if (leakage >= table[i].min)
			j = i;
	}

	*volt = table[j].volt;

	return 0;
}

static int rockchip_adjust_opp_table(struct device *cpu_dev,
				     struct cpufreq_frequency_table *table,
				     int volt)
{
	struct opp_table *opp_table;
	struct cpufreq_frequency_table *pos;
	struct dev_pm_opp *opp;

	if (!volt)
		return 0;

	rcu_read_lock();

	opp_table = _find_opp_table(cpu_dev);
	if (IS_ERR(opp_table)) {
		rcu_read_unlock();
		return PTR_ERR(opp_table);
	}

	cpufreq_for_each_valid_entry(pos, table) {
		opp = dev_pm_opp_find_freq_exact(cpu_dev, pos->frequency * 1000,
						 true);
		if (IS_ERR(opp))
			continue;

		opp->u_volt += volt;
		opp->u_volt_min += volt;
		opp->u_volt_max += volt;
	}

	rcu_read_unlock();

	return 0;
}

static void rockchip_adjust_volt_by_leakage(struct device *cpu_dev,
					    struct cpufreq_policy *policy,
					    struct rockchip_cpu_avs *avs,
					    int id)
{
	struct cluster_info *cluster = &avs->cluster[id];
	int ret;

	if (cluster->leakage)
		goto next;

	ret = rockchip_get_leakage(cpu_dev, &cluster->leakage);
	if (ret) {
		dev_err(avs->dev, "cpu%d leakage invalid\n", policy->cpu);
		return;
	}

	ret = rockchip_get_offset_volt(cluster->leakage, cluster->table,
				       &cluster->adjust_volt);
	if (ret) {
		dev_err(avs->dev, "cpu%d leakage volt table err\n",
			policy->cpu);
		return;
	}

next:
	ret = rockchip_adjust_opp_table(cpu_dev, policy->freq_table,
					cluster->adjust_volt);
	if (ret)
		dev_err(avs->dev, "cpu%d failed to adjust volt\n", policy->cpu);

	dev_dbg(avs->dev, "cpu%d, leakage=%d, adjust_volt=%d\n", policy->cpu,
		cluster->leakage, cluster->adjust_volt);
}

static int rockchip_cpu_avs_notifier(struct notifier_block *nb,
				     unsigned long event, void *data)
{
	struct rockchip_cpu_avs *avs = notifier_to_avs(nb);
	struct cpufreq_policy *policy = data;
	struct device *cpu_dev;
	int cluster_id;

	if (event != CPUFREQ_START)
		goto out;

	cluster_id = topology_physical_package_id(policy->cpu);
	if (cluster_id < 0) {
		dev_err(avs->dev, "cpu%d invalid cluster id\n", policy->cpu);
		goto out;
	}

	if (!policy->freq_table) {
		dev_err(avs->dev, "cpu%d freq table not found\n", policy->cpu);
		goto out;
	}

	cpu_dev = get_cpu_device(policy->cpu);
	if (!cpu_dev) {
		dev_err(avs->dev, "cpu%d failed to get device\n", policy->cpu);
		goto out;
	}

	rockchip_adjust_volt_by_leakage(cpu_dev, policy, avs, cluster_id);

out:

	return NOTIFY_OK;
}

static int rockchip_get_leakage_volt_table(struct device *dev,
					   struct leakage_volt_table **table,
					   const char *name)
{
	struct device_node *np = dev->of_node;
	struct leakage_volt_table *volt_table;
	const struct property *prop;
	int count, i;

	prop = of_find_property(np, name, NULL);
	if (!prop) {
		dev_err(dev, "failed to find prop %s\n", name);
		return -EINVAL;
	}
	if (!prop->value) {
		dev_err(dev, "%s value is NULL\n", name);
		return -ENODATA;
	}

	count = of_property_count_u32_elems(np, name);
	if (count < 0) {
		dev_err(dev, "Invalid %s property (%d)\n", name, count);
		return -EINVAL;
	}
	if (count % 3) {
		dev_err(dev, "Invalid number of elements in %s property (%d)\n",
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

static const struct of_device_id rockchip_cpu_avs_match[] = {
	{
		.compatible = "rockchip,rk3399-cpu-avs",
	},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, rockchip_cpu_avs_match);

static int rockchip_cpu_avs_probe(struct platform_device *pdev)
{
	struct rockchip_cpu_avs *avs;
	char name[MAX_NAME_LEN];
	int i, ret, cpu, id;
	int last_id = -1;
	int cluster_num = 0;

	for_each_online_cpu(cpu) {
		id = topology_physical_package_id(cpu);
		if (id < 0)
			return -EINVAL;
		if (id != last_id) {
			last_id = id;
			cluster_num++;
		}
	}

	avs = devm_kzalloc(&pdev->dev, sizeof(struct rockchip_cpu_avs),
			   GFP_KERNEL);
	if (!avs)
		return -ENOMEM;

	avs->dev = &pdev->dev;
	avs->cpufreq_notify.notifier_call = rockchip_cpu_avs_notifier;
	avs->cluster = devm_kzalloc(&pdev->dev,
		sizeof(struct cluster_info) * cluster_num, GFP_KERNEL);
	if (!avs->cluster)
		return -ENOMEM;

	for (i = 0; i < cluster_num; i++) {
		snprintf(name, MAX_NAME_LEN, "leakage-volt-cluster%d", i);
		ret = rockchip_get_leakage_volt_table(&pdev->dev,
						      &avs->cluster[i].table,
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
