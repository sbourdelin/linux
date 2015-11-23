/*
 * Copyright (C) 2015 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_opp.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

static struct clk *arm_clk;
static struct clk *pll_arm;
static struct clk *arm_src;
static struct clk *pll_sys_main;

static struct regulator *arm_reg;

static struct device *cpu_dev;
static bool free_opp;
static struct cpufreq_frequency_table *freq_table;
static unsigned int transition_latency;

static int imx7d_set_target(struct cpufreq_policy *policy, unsigned int index)
{
	struct dev_pm_opp *opp;
	unsigned long freq_hz, volt, volt_old;
	unsigned int old_freq, new_freq;
	int ret;

	new_freq = freq_table[index].frequency;
	freq_hz = new_freq * 1000;
	old_freq = clk_get_rate(arm_clk) / 1000;

	rcu_read_lock();
	opp = dev_pm_opp_find_freq_ceil(cpu_dev, &freq_hz);
	if (IS_ERR(opp)) {
		rcu_read_unlock();
		dev_err(cpu_dev, "failed to find OPP for %ld\n", freq_hz);
		return PTR_ERR(opp);
	}
	volt = dev_pm_opp_get_voltage(opp);
	rcu_read_unlock();
	volt_old = regulator_get_voltage(arm_reg);

	dev_dbg(cpu_dev, "%u MHz, %ld mV --> %u MHz, %ld mV\n",
		old_freq / 1000, volt_old / 1000,
		new_freq / 1000, volt / 1000);

	/* Scaling up? scale voltage before frequency */
	if (new_freq > old_freq) {
		ret = regulator_set_voltage_tol(arm_reg, volt, 0);
		if (ret) {
			dev_err(cpu_dev, "failed to scale vddarm up: %d\n", ret);
			return ret;
		}
	}

	/* before changing pll_arm rate, change the arm_src's soure
	 * to pll_sys_main clk first.
	 */
	clk_set_parent(arm_src, pll_sys_main);
	clk_set_rate(pll_arm, new_freq * 1000);
	clk_set_parent(arm_src, pll_arm);

	/* change the cpu frequency */
	ret = clk_set_rate(arm_clk, new_freq * 1000);
	if (ret) {
		dev_err(cpu_dev, " failed to set clock rate: %d\n", ret);
		regulator_set_voltage_tol(arm_reg, volt_old, 0);
		return ret;
	}

	/* scaling down? scaling voltage after frequency */
	if (new_freq < old_freq) {
		ret = regulator_set_voltage_tol(arm_reg, volt, 0);
		if (ret) {
			dev_warn(cpu_dev, "failed to scale vddarm down: %d\n", ret);
			ret = 0;
		}
	}

	return 0;
}

static int imx7d_cpufreq_init(struct cpufreq_policy *policy)
{
	policy->clk = arm_clk;
	policy->cur = clk_get_rate(arm_clk) / 1000;

	return  cpufreq_generic_init(policy, freq_table, transition_latency);
}

static struct cpufreq_driver imx7d_cpufreq_driver = {
	.flags = CPUFREQ_NEED_INITIAL_FREQ_CHECK,
	.verify = cpufreq_generic_frequency_table_verify,
	.target_index = imx7d_set_target,
	.get = cpufreq_generic_get,
	.init = imx7d_cpufreq_init,
	.name = "imx7d-cpufreq",
	.attr = cpufreq_generic_attr,
};

static int imx7d_cpufreq_probe(struct platform_device *pdev)
{
	struct device_node *np;
	struct dev_pm_opp *opp;
	unsigned long min_volt, max_volt;
	int num, ret;

	cpu_dev = get_cpu_device(0);
	if (!cpu_dev) {
		pr_err("failed to get cpu0 device\n");
		return -ENODEV;
	}

	np = of_node_get(cpu_dev->of_node);
	if (!np) {
		dev_err(cpu_dev, "failed to find the cpu0 node\n");
		return -ENOENT;
	}

	arm_clk = clk_get(cpu_dev, "arm");
	arm_src	= clk_get(cpu_dev, "arm_root_src");
	pll_arm = clk_get(cpu_dev, "pll_arm");
	pll_sys_main = clk_get(cpu_dev, "pll_sys_main");

	if (IS_ERR(arm_clk) || IS_ERR(arm_src) || IS_ERR(pll_arm) ||
	    IS_ERR(pll_sys_main)) {
		dev_err(cpu_dev, "failed to get clocks\n");
		ret = -ENOENT;
		goto put_clk;
	}

	arm_reg = devm_regulator_get(cpu_dev, "arm");
	if (IS_ERR(arm_reg)) {
		ret = PTR_ERR(arm_reg);
		if (ret != -EPROBE_DEFER)
			dev_err(cpu_dev, "failed to get the regulator\n");

		goto put_clk;
	}

	/* We expect an OPP table supplied by platform.
	 * Just incase the platform did not supply the OPP
	 * table, it will try to get it.
	 */
	num = dev_pm_opp_get_opp_count(cpu_dev);
	if (num < 0) {
		ret = dev_pm_opp_of_add_table(cpu_dev);
		if (ret < 0) {
			dev_err(cpu_dev, "failed to init OPP table: %d\n", ret);
			goto put_reg;
		}

		free_opp = true;

		num = dev_pm_opp_get_opp_count(cpu_dev);
		if (num < 0) {
			ret = num;
			dev_err(cpu_dev, "no OPP table is found: %d\n", ret);
			goto out_free_opp;
		}
	}

	ret = dev_pm_opp_init_cpufreq_table(cpu_dev, &freq_table);
	if (ret) {
		dev_err(cpu_dev, "failed to init cpufreq table: %d\n", ret);
		goto out_free_opp;
	}

	if (of_property_read_u32(np, "clock-latency", &transition_latency))
		transition_latency = CPUFREQ_ETERNAL;

	/* OPP is maintained in order of increasing frequency, and
	 * freq_table initialized from OPP is therefore sorted in the
	 * same order
	 */
	rcu_read_lock();
	opp = dev_pm_opp_find_freq_exact(cpu_dev,
				freq_table[0].frequency * 1000, true);
	min_volt = dev_pm_opp_get_voltage(opp);
	opp = dev_pm_opp_find_freq_exact(cpu_dev,
				freq_table[--num].frequency * 1000, true);
	max_volt = dev_pm_opp_get_voltage(opp);
	rcu_read_unlock();
	ret = regulator_set_voltage_time(arm_reg, min_volt, max_volt);
	if (ret > 0)
		transition_latency += ret * 1000;

	ret = cpufreq_register_driver(&imx7d_cpufreq_driver);
	if (ret) {
		dev_err(cpu_dev, "failed register driver: %d\n", ret);
		goto free_freq_table;
	 }

	of_node_put(np);
	return 0;

free_freq_table:
	dev_pm_opp_free_cpufreq_table(cpu_dev, &freq_table);
out_free_opp:
	if (free_opp)
		dev_pm_opp_of_remove_table(cpu_dev);
put_reg:
	if (!IS_ERR(arm_reg))
		regulator_put(arm_reg);
put_clk:
	if (!IS_ERR(arm_clk))
		clk_put(arm_clk);
	if (!IS_ERR(arm_src))
		clk_put(arm_src);
	if (!IS_ERR(pll_arm))
		clk_put(pll_arm);
	if (!IS_ERR(pll_sys_main))
		clk_put(pll_sys_main);
	of_node_put(np);

	return ret;
}

static int imx7d_cpufreq_remove(struct platform_device *pdev)
{
	cpufreq_unregister_driver(&imx7d_cpufreq_driver);
	dev_pm_opp_free_cpufreq_table(cpu_dev, &freq_table);
	dev_pm_opp_of_remove_table(cpu_dev);

	regulator_put(arm_reg);

	clk_put(arm_clk);
	clk_put(arm_src);
	clk_put(pll_arm);
	clk_put(pll_sys_main);

	return 0;
}

static struct platform_driver imx7d_cpufreq_platdrv = {
	.driver = {
		.name	= "imx7d-cpufreq",
		.owner	= THIS_MODULE,
	},
	.probe		= imx7d_cpufreq_probe,
	.remove		= imx7d_cpufreq_remove,
};

module_platform_driver(imx7d_cpufreq_platdrv);

MODULE_DESCRIPTION("Freescale i.MX7D cpufreq driver");
MODULE_LICENSE("GPL");
