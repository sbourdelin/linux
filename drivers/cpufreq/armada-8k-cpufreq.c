// SPDX-License-Identifier: GPL-2.0+
/*
 * CPUFreq support for Armada 7K/8K
 *
 * Copyright (C) 2018 Marvell
 *
 * Omri Itach <omrii@marvell.com>
 * Gregory Clement <gregory.clement@bootlin.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>

#define MIN_FREQ 100000000

/*
 * Setup the opps list with the divider for the max frequency, that
 * will be filled at runtime, 0 meaning 100Mhz
 */
static int opps_div[] __initdata = {1, 2, 3, 0};

struct opps_array {
	struct device *cpu_dev;
	unsigned int freq[ARRAY_SIZE(opps_div)];
};

/* If the CPUs share the same clock, then they are in the same cluster */
static void __init aramda_8k_get_sharing_cpus(struct clk *cur_clk,
				       struct cpumask *cpumask)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct device *cpu_dev = get_cpu_device(cpu);
		struct clk *clk = clk_get(cpu_dev, 0);

		if (IS_ERR(clk))
			dev_warn(cpu_dev, "Cannot get clock for CPU %d\n", cpu);

		if (clk_is_match(clk, cur_clk))
			cpumask_set_cpu(cpu, cpumask);
	}

}

static int __init armada_8k_cpufreq_init(void)
{
	struct opps_array *opps_arrays;
	struct platform_device *pdev;
	int ret, cpu, opps_index = 0;
	unsigned int cur_frequency;
	struct device_node *node;

	node = of_find_compatible_node(NULL, NULL, "marvell,ap806-cpu-clock");
	if (!node || !of_device_is_available(node))
		return -ENODEV;

	opps_arrays = kcalloc(num_possible_cpus(), sizeof(*opps_arrays),
			      GFP_KERNEL);
	/*
	 * For each CPU, this loop registers the operating points
	 * supported (which are the nominal CPU frequency and full integer
	 * divisions of it).
	 */
	for_each_possible_cpu(cpu) {
		struct device *cpu_dev;
		struct cpumask cpus;
		unsigned int freq;
		struct clk *clk;
		int i;

		cpu_dev = get_cpu_device(cpu);
		if (!cpu_dev) {
			dev_err(cpu_dev, "Cannot get CPU %d\n", cpu);
			continue;
		}

		clk = clk_get(cpu_dev, 0);
		if (IS_ERR(clk)) {
			dev_err(cpu_dev, "Cannot get clock for CPU %d\n", cpu);
			return PTR_ERR(clk);
		}

		/* Get nominal (current) CPU frequency */
		cur_frequency = clk_get_rate(clk);
		if (!cur_frequency) {
			dev_err(cpu_dev,
				"Failed to get clock rate for CPU %d\n", cpu);
			return -EINVAL;
		}

		opps_arrays[opps_index].cpu_dev = cpu_dev;
		for (i = 0; i < ARRAY_SIZE(opps_div); i++) {
			if (opps_div[i])
				freq = cur_frequency / opps_div[i];
			else
				freq = MIN_FREQ;

			ret = dev_pm_opp_add(cpu_dev, freq, 0);

			if (ret)
				goto remove_opp;
			opps_arrays[opps_index].freq[i] = freq;
		}

		cpumask_clear(&cpus);
		aramda_8k_get_sharing_cpus(clk, &cpus);
		dev_pm_opp_set_sharing_cpus(cpu_dev, &cpus);
	}

	pdev = platform_device_register_simple("cpufreq-dt", -1, NULL, 0);
	ret = PTR_ERR_OR_ZERO(pdev);
	if (ret)
		goto remove_opp;
	kfree(opps_arrays);
	return 0;
remove_opp:

	for (; opps_index >= 0; opps_index--) {
		int i = 0;

		while (opps_arrays[opps_index].freq[i]) {
			dev_pm_opp_remove(opps_arrays[opps_index].cpu_dev,
					  opps_arrays[opps_index].freq[i]);
			i++;
		}
	}
	kfree(opps_arrays);
	return ret;
}
module_init(armada_8k_cpufreq_init);

MODULE_AUTHOR("Gregory Clement <gregory.clement@bootlin.com>");
MODULE_DESCRIPTION("Armada 8K cpufreq driver");
MODULE_LICENSE("GPL");
