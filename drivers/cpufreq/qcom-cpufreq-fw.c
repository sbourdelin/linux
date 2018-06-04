// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 */

#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>

#define INIT_RATE			300000000UL
#define XO_RATE				19200000UL
#define LUT_MAX_ENTRIES			40U
#define CORE_COUNT_VAL(val)		(((val) & (GENMASK(18, 16))) >> 16)
#define LUT_ROW_SIZE			32

struct cpufreq_qcom {
	struct cpufreq_frequency_table *table;
	struct device *dev;
	void __iomem *perf_base;
	void __iomem *lut_base;
	cpumask_t related_cpus;
	unsigned int max_cores;
};

static struct cpufreq_qcom *qcom_freq_domain_map[NR_CPUS];

static int
qcom_cpufreq_fw_target_index(struct cpufreq_policy *policy,
			     unsigned int index)
{
	struct cpufreq_qcom *c = policy->driver_data;

	writel_relaxed(index, c->perf_base);

	return 0;
}

static unsigned int qcom_cpufreq_fw_get(unsigned int cpu)
{
	struct cpufreq_qcom *c;
	unsigned int index;

	c = qcom_freq_domain_map[cpu];
	if (!c)
		return 0;

	index = readl_relaxed(c->perf_base);
	index = min(index, LUT_MAX_ENTRIES - 1);

	return c->table[index].frequency;
}

static int qcom_cpufreq_fw_cpu_init(struct cpufreq_policy *policy)
{
	struct cpufreq_qcom *c;

	c = qcom_freq_domain_map[policy->cpu];
	if (!c) {
		pr_err("No scaling support for CPU%d\n", policy->cpu);
		return -ENODEV;
	}

	cpumask_copy(policy->cpus, &c->related_cpus);

	policy->freq_table = c->table;
	policy->driver_data = c;

	return 0;
}

static struct freq_attr *qcom_cpufreq_fw_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	&cpufreq_freq_attr_scaling_boost_freqs,
	NULL
};

static struct cpufreq_driver cpufreq_qcom_fw_driver = {
	.flags		= CPUFREQ_STICKY | CPUFREQ_NEED_INITIAL_FREQ_CHECK |
			  CPUFREQ_HAVE_GOVERNOR_PER_POLICY,
	.verify		= cpufreq_generic_frequency_table_verify,
	.target_index	= qcom_cpufreq_fw_target_index,
	.get		= qcom_cpufreq_fw_get,
	.init		= qcom_cpufreq_fw_cpu_init,
	.name		= "qcom-cpufreq-fw",
	.attr		= qcom_cpufreq_fw_attr,
	.boost_enabled	= true,
};

static int qcom_read_lut(struct platform_device *pdev,
			 struct cpufreq_qcom *c)
{
	struct device *dev = &pdev->dev;
	u32 data, src, lval, i, core_count, prev_cc, prev_freq, cur_freq;

	c->table = devm_kcalloc(dev, LUT_MAX_ENTRIES + 1,
				sizeof(*c->table), GFP_KERNEL);
	if (!c->table)
		return -ENOMEM;

	for (i = 0; i < LUT_MAX_ENTRIES; i++) {
		data = readl_relaxed(c->lut_base + i * LUT_ROW_SIZE);
		src = ((data & GENMASK(31, 30)) >> 30);
		lval = (data & GENMASK(7, 0));
		core_count = CORE_COUNT_VAL(data);

		if (!src)
			c->table[i].frequency = INIT_RATE / 1000;
		else
			c->table[i].frequency = XO_RATE * lval / 1000;

		cur_freq = c->table[i].frequency;

		dev_dbg(dev, "index=%d freq=%d, core_count %d\n",
			i, c->table[i].frequency, core_count);

		if (core_count != c->max_cores)
			cur_freq = CPUFREQ_ENTRY_INVALID;

		/*
		 * Two of the same frequencies with the same core counts means
		 * end of table.
		 */
		if (i > 0 && c->table[i - 1].frequency ==
		   c->table[i].frequency && prev_cc == core_count) {
			struct cpufreq_frequency_table *prev = &c->table[i - 1];

			if (prev_freq == CPUFREQ_ENTRY_INVALID)
				prev->flags = CPUFREQ_BOOST_FREQ;
			break;
		}
		prev_cc = core_count;
		prev_freq = cur_freq;
	}

	c->table[i].frequency = CPUFREQ_TABLE_END;

	return 0;
}

static int qcom_get_related_cpus(struct device_node *np, struct cpumask *m)
{
	struct device_node  *cpu_dev;
	int cpu;

	for_each_possible_cpu(cpu) {
		cpu_dev = of_cpu_device_node_get(cpu);
		if (!cpu_dev)
			continue;
		cpu_dev = of_parse_phandle(cpu_dev, "qcom,freq-domain", 0);
		if (!cpu_dev)
			continue;
		if (cpu_dev == np)
			cpumask_set_cpu(cpu, m);
	}

	if (cpumask_empty(m))
		return -ENOENT;

	return 0;
}

static int qcom_cpu_resources_init(struct platform_device *pdev,
				   struct device_node *np)
{
	struct cpufreq_qcom *c;
	struct resource res;
	struct device *dev = &pdev->dev;
	void __iomem *en_base;
	int cpu, index, ret;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	index = of_property_match_string(np, "reg-names", "enable");
	if (index < 0)
		return index;

	if (of_address_to_resource(np, index, &res))
		return -ENOMEM;

	en_base = devm_ioremap(dev, res.start, resource_size(&res));
	if (!en_base) {
		dev_err(dev, "Unable to map %s enable-base\n", np->name);
		return -ENOMEM;
	}

	/* FW should be in enabled state to proceed */
	if (!(readl_relaxed(en_base) & 0x1)) {
		dev_err(dev, "%s firmware not enabled\n", np->name);
		return -ENODEV;
	}
	devm_iounmap(&pdev->dev, en_base);

	index = of_property_match_string(np, "reg-names", "perf");
	if (index < 0)
		return index;

	if (of_address_to_resource(np, index, &res))
		return -ENOMEM;

	c->perf_base = devm_ioremap(dev, res.start, resource_size(&res));
	if (!c->perf_base) {
		dev_err(dev, "Unable to map %s perf-base\n", np->name);
		return -ENOMEM;
	}

	index = of_property_match_string(np, "reg-names", "lut");
	if (index < 0)
		return index;

	if (of_address_to_resource(np, index, &res))
		return -ENOMEM;

	c->lut_base = devm_ioremap(dev, res.start, resource_size(&res));
	if (!c->lut_base) {
		dev_err(dev, "Unable to map %s lut-base\n", np->name);
		return -ENOMEM;
	}

	ret = qcom_get_related_cpus(np, &c->related_cpus);
	if (ret) {
		dev_err(dev, "%s failed to get core phandles\n", np->name);
		return ret;
	}

	c->max_cores = cpumask_weight(&c->related_cpus);

	ret = qcom_read_lut(pdev, c);
	if (ret) {
		dev_err(dev, "%s failed to read LUT\n", np->name);
		return ret;
	}
	for_each_cpu(cpu, &c->related_cpus)
		qcom_freq_domain_map[cpu] = c;

	return 0;
}

static int qcom_resources_init(struct platform_device *pdev)
{
	struct device_node *np, *cpu_dev;
	unsigned int cpu;
	int ret;

	for_each_possible_cpu(cpu) {
		cpu_dev = of_cpu_device_node_get(cpu);
		if (!cpu_dev) {
			dev_err(&pdev->dev, "Failed to get cpu %d device\n",
				cpu);
			continue;
		}

		np = of_parse_phandle(cpu_dev, "qcom,freq-domain", 0);
		if (!np) {
			dev_err(&pdev->dev, "Failed to get freq-domain device\n");
			continue;
		}

		of_node_put(cpu_dev);

		ret = qcom_cpu_resources_init(pdev, np);
		if (ret)
			return ret;
	}

	return 0;
}

static int qcom_cpufreq_fw_driver_probe(struct platform_device *pdev)
{
	int rc;

	/* Get the bases of cpufreq for domains */
	rc = qcom_resources_init(pdev);
	if (rc) {
		dev_err(&pdev->dev, "CPUFreq resource init failed\n");
		return rc;
	}

	rc = cpufreq_register_driver(&cpufreq_qcom_fw_driver);
	if (rc) {
		dev_err(&pdev->dev, "CPUFreq FW driver failed to register\n");
		return rc;
	}

	dev_info(&pdev->dev, "QCOM CPUFreq FW driver inited\n");

	return 0;
}

static const struct of_device_id match_table[] = {
	{ .compatible = "qcom,cpufreq-fw" },
	{}
};

static struct platform_driver qcom_cpufreq_fw_driver = {
	.probe = qcom_cpufreq_fw_driver_probe,
	.driver = {
		.name = "qcom-cpufreq-fw",
		.of_match_table = match_table,
		.owner = THIS_MODULE,
	},
};

static int __init qcom_cpufreq_fw_init(void)
{
	return platform_driver_register(&qcom_cpufreq_fw_driver);
}
subsys_initcall(qcom_cpufreq_fw_init);

MODULE_DESCRIPTION("QCOM CPU Frequency FW");
MODULE_LICENSE("GPL v2");
