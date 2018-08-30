/*
 * Copyright (C) 2010 Google, Inc.
 *
 * Author:
 *	Colin Cross <ccross@google.com>
 *	Based on arch/arm/plat-omap/cpu-omap.c, (C) 2005 Nokia Corporation
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpu_cooling.h>
#include <linux/cpufreq.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/types.h>

#define PLLX_PREPARE		BIT(0)
#define PLLX_PREPARED		BIT(1)

struct tegra20_cpufreq {
	struct device *dev;
	struct device *cpu_dev;
	struct cpumask cpu_mask;
	struct cpufreq_driver driver;
	struct thermal_cooling_device *cdev;
	struct cpufreq_frequency_table *freq_table;
	struct clk *cpu_clk;
	struct clk *pll_x_clk;
	struct clk *backup_clk;
	unsigned long backup_rate;
	unsigned int state;
};

static unsigned int tegra_get_intermediate(struct cpufreq_policy *policy,
					   unsigned int index)
{
	struct tegra20_cpufreq *cpufreq = cpufreq_get_driver_data();
	struct clk *cpu_parent = clk_get_parent(cpufreq->cpu_clk);
	unsigned long new_rate = cpufreq->freq_table[index].frequency * 1000;
	int err;

	/*
	 * Make sure that backup clock rate stays consistent during
	 * transition by entering into critical section of the backup clock.
	 */
	err = clk_rate_exclusive_get(cpufreq->backup_clk);
	/* this shouldn't fail */
	WARN_ON_ONCE(err);

	/*
	 * When target rate is equal to backup rate, we don't need to
	 * switch to backup clock and so the intermediate routine isn't
	 * called.  Also, we wouldn't be using PLLX anymore and must not
	 * take extra reference to it, as it can be disabled to save some
	 * power.
	 */
	cpufreq->backup_rate = clk_get_rate(cpufreq->backup_clk);

	if (new_rate == cpufreq->backup_rate)
		cpufreq->state &= ~PLLX_PREPARE;
	else
		cpufreq->state |= PLLX_PREPARE;

	/* don't switch to intermediate freq if we are already at it */
	if (clk_is_match(cpu_parent, cpufreq->backup_clk))
		return 0;

	return cpufreq->backup_rate / 1000;
}

static int tegra_target_intermediate(struct cpufreq_policy *policy,
				     unsigned int index)
{
	struct tegra20_cpufreq *cpufreq = cpufreq_get_driver_data();
	unsigned int state = cpufreq->state;
	int err;

	/*
	 * Take an extra reference to the main PLLX so it doesn't turn off
	 * when we move the CPU clock to backup clock as enabling it again
	 * while we switch to it from tegra_target() would take additional
	 * time.
	 */
	if ((state & (PLLX_PREPARED | PLLX_PREPARE)) == PLLX_PREPARE) {
		err = clk_prepare_enable(cpufreq->pll_x_clk);
		if (err)
			goto err_exclusive_put;

		cpufreq->state |= PLLX_PREPARED;
	}

	err = clk_set_parent(cpufreq->cpu_clk, cpufreq->backup_clk);
	if (err)
		goto err_exclusive_put;

	return 0;

err_exclusive_put:
	clk_rate_exclusive_put(cpufreq->backup_clk);

	if (cpufreq->state & PLLX_PREPARED) {
		clk_disable_unprepare(cpufreq->pll_x_clk);
		cpufreq->state &= ~PLLX_PREPARED;
	}

	/* this shouldn't fail */
	return WARN_ON_ONCE(err);
}

static int tegra_target(struct cpufreq_policy *policy, unsigned int index)
{
	struct tegra20_cpufreq *cpufreq = cpufreq_get_driver_data();
	unsigned long new_rate = cpufreq->freq_table[index].frequency * 1000;
	unsigned int state = cpufreq->state;
	int ret;

	/*
	 * Drop refcount to PLLX only if we switched to backup clock earlier
	 * during transitioning to a target frequency and we are going to
	 * stay with the backup clock.
	 */
	if ((state & (PLLX_PREPARED | PLLX_PREPARE)) == PLLX_PREPARED) {
		clk_disable_unprepare(cpufreq->pll_x_clk);
		state &= ~PLLX_PREPARED;
	}

	/*
	 * Switch to new OPP, note that this will change PLLX rate and
	 * not the CCLK.
	 */
	ret = dev_pm_opp_set_rate(cpufreq->cpu_dev, new_rate);
	if (ret)
		goto exclusive_put;

	/*
	 * Target rate == backup rate leaves PLLX turned off, CPU is kept
	 * running off the backup clock. This should save us some power by
	 * keeping one more PLL disabled because the backup clock assumed
	 * to be always-on. In this case PLLX_PREPARE flag will be omitted.
	 */
	if (state & PLLX_PREPARE) {
		/*
		 * CCF doesn't return error if clock-enabling fails on
		 * re-parent, hence enable it now.
		 */
		ret = clk_prepare_enable(cpufreq->pll_x_clk);
		if (ret)
			goto exclusive_put;

		ret = clk_set_parent(cpufreq->cpu_clk, cpufreq->pll_x_clk);

		clk_disable_unprepare(cpufreq->pll_x_clk);
	}

	/*
	 * Drop refcount to PLLX only if we switched to backup clock earlier
	 * during transitioning to a target frequency.
	 */
	if (state & PLLX_PREPARED) {
		clk_disable_unprepare(cpufreq->pll_x_clk);
		state &= ~PLLX_PREPARED;
	}

exclusive_put:
	clk_rate_exclusive_put(cpufreq->backup_clk);

	cpufreq->state = state;

	/* this shouldn't fail */
	return WARN_ON_ONCE(ret);
}

static int tegra_cpu_setup_opp(struct tegra20_cpufreq *cpufreq)
{
	struct device *dev = cpufreq->cpu_dev;
	int err;

	err = dev_pm_opp_of_cpumask_add_table(cpu_possible_mask);
	if (err)
		return err;

	err = dev_pm_opp_init_cpufreq_table(dev, &cpufreq->freq_table);
	if (err)
		goto err_remove_table;

	return 0;

err_remove_table:
	dev_pm_opp_of_cpumask_remove_table(cpu_possible_mask);

	return err;
}

static void tegra_cpu_release_opp(struct tegra20_cpufreq *cpufreq)
{
	dev_pm_opp_free_cpufreq_table(cpufreq->cpu_dev, &cpufreq->freq_table);
	dev_pm_opp_of_cpumask_remove_table(cpu_possible_mask);
}

static int tegra_cpu_init_clk(struct tegra20_cpufreq *cpufreq)
{
	unsigned long backup_rate;
	int ret;

	ret = clk_rate_exclusive_get(cpufreq->backup_clk);
	if (ret)
		return ret;

	ret = clk_set_parent(cpufreq->cpu_clk, cpufreq->backup_clk);
	if (ret)
		goto exclusive_put;

	backup_rate = clk_get_rate(cpufreq->backup_clk);

	/*
	 * The CCLK has its own clock divider, that divider isn't getting
	 * disabled on clock reparent. Hence set CCLK parent to backup clock
	 * in order to disable the divider if it happens to be enabled,
	 * otherwise clk_set_rate() has no effect.
	 */
	ret = clk_set_rate(cpufreq->cpu_clk, backup_rate);

exclusive_put:
	clk_rate_exclusive_put(cpufreq->backup_clk);

	return ret;
}

static int tegra_cpu_init(struct cpufreq_policy *policy)
{
	struct tegra20_cpufreq *cpufreq = cpufreq_get_driver_data();
	struct device *cpu = cpufreq->cpu_dev;
	int err;

	err = tegra_cpu_setup_opp(cpufreq);
	if (err) {
		dev_err(cpufreq->dev, "Failed to setup OPP: %d\n", err);
		return err;
	}

	err = clk_prepare_enable(cpufreq->cpu_clk);
	if (err) {
		dev_err(cpufreq->dev,
			"Failed to enable CPU clock: %d\n", err);
		goto er_release_opp;
	}

	err = clk_prepare_enable(cpufreq->backup_clk);
	if (err) {
		dev_err(cpufreq->dev,
			"Failed to enable backup clock: %d\n", err);
		goto err_cpu_disable;
	}

	err = clk_rate_exclusive_get(cpufreq->cpu_clk);
	if (err) {
		dev_err(cpufreq->dev,
			"Failed to make CPU clock exclusive: %d\n", err);
		goto err_backup_disable;
	}

	err = tegra_cpu_init_clk(cpufreq);
	if (err) {
		dev_err(cpufreq->dev,
			"Failed to initialize CPU clock: %d\n", err);
		goto err_exclusive_put;
	}

	err = cpufreq_generic_init(policy, cpufreq->freq_table,
				   dev_pm_opp_get_max_transition_latency(cpu));
	if (err)
		goto err_exclusive_put;

	policy->clk = cpufreq->cpu_clk;
	policy->suspend_freq = dev_pm_opp_get_suspend_opp_freq(cpu) / 1000;

	return 0;

err_exclusive_put:
	clk_rate_exclusive_put(cpufreq->cpu_clk);
err_backup_disable:
	clk_disable_unprepare(cpufreq->backup_clk);
err_cpu_disable:
	clk_disable_unprepare(cpufreq->cpu_clk);
er_release_opp:
	tegra_cpu_release_opp(cpufreq);

	return err;
}

static int tegra_cpu_exit(struct cpufreq_policy *policy)
{
	struct tegra20_cpufreq *cpufreq = cpufreq_get_driver_data();

	cpufreq_cooling_unregister(cpufreq->cdev);
	clk_rate_exclusive_put(cpufreq->cpu_clk);
	clk_disable_unprepare(cpufreq->backup_clk);
	clk_disable_unprepare(cpufreq->cpu_clk);
	tegra_cpu_release_opp(cpufreq);

	return 0;
}

static void tegra_cpu_ready(struct cpufreq_policy *policy)
{
	struct tegra20_cpufreq *cpufreq = cpufreq_get_driver_data();

	cpufreq->cdev = of_cpufreq_cooling_register(policy);
}

static int tegra20_cpufreq_probe(struct platform_device *pdev)
{
	struct tegra20_cpufreq *cpufreq;
	struct device_node *np;
	int err;

	cpufreq = devm_kzalloc(&pdev->dev, sizeof(*cpufreq), GFP_KERNEL);
	if (!cpufreq)
		return -ENOMEM;

	cpufreq->cpu_dev = get_cpu_device(0);
	if (!cpufreq->cpu_dev)
		return -ENODEV;

	np = cpufreq->cpu_dev->of_node;

	cpufreq->cpu_clk = devm_get_clk_from_child(&pdev->dev, np, "cpu");
	if (IS_ERR(cpufreq->cpu_clk)) {
		err = PTR_ERR(cpufreq->cpu_clk);
		dev_err(&pdev->dev, "Failed to get cpu clock: %d\n", err);
		dev_err(&pdev->dev, "Please update your device tree\n");
		return err;
	}

	cpufreq->pll_x_clk = devm_get_clk_from_child(&pdev->dev, np, "pll_x");
	if (IS_ERR(cpufreq->pll_x_clk)) {
		err = PTR_ERR(cpufreq->pll_x_clk);
		dev_err(&pdev->dev, "Failed to get pll_x clock: %d\n", err);
		return err;
	}

	cpufreq->backup_clk = devm_get_clk_from_child(&pdev->dev, np, "backup");
	if (IS_ERR(cpufreq->backup_clk)) {
		err = PTR_ERR(cpufreq->backup_clk);
		dev_err(&pdev->dev, "Failed to get backup clock: %d\n", err);
		return err;
	}

	cpufreq->dev = &pdev->dev;
	cpufreq->driver.get = cpufreq_generic_get;
	cpufreq->driver.attr = cpufreq_generic_attr;
	cpufreq->driver.init = tegra_cpu_init;
	cpufreq->driver.exit = tegra_cpu_exit;
	cpufreq->driver.ready = tegra_cpu_ready;
	cpufreq->driver.flags = CPUFREQ_NEED_INITIAL_FREQ_CHECK;
	cpufreq->driver.verify = cpufreq_generic_frequency_table_verify;
	cpufreq->driver.suspend = cpufreq_generic_suspend;
	cpufreq->driver.driver_data = cpufreq;
	cpufreq->driver.target_index = tegra_target;
	cpufreq->driver.get_intermediate = tegra_get_intermediate;
	cpufreq->driver.target_intermediate = tegra_target_intermediate;
	snprintf(cpufreq->driver.name, CPUFREQ_NAME_LEN, "tegra");

	err = cpufreq_register_driver(&cpufreq->driver);
	if (err)
		return err;

	platform_set_drvdata(pdev, cpufreq);

	return 0;
}

static int tegra20_cpufreq_remove(struct platform_device *pdev)
{
	struct tegra20_cpufreq *cpufreq = platform_get_drvdata(pdev);

	cpufreq_unregister_driver(&cpufreq->driver);

	return 0;
}

static struct platform_driver tegra20_cpufreq_driver = {
	.probe		= tegra20_cpufreq_probe,
	.remove		= tegra20_cpufreq_remove,
	.driver		= {
		.name	= "tegra20-cpufreq",
	},
};
module_platform_driver(tegra20_cpufreq_driver);

MODULE_ALIAS("platform:tegra20-cpufreq");
MODULE_AUTHOR("Colin Cross <ccross@android.com>");
MODULE_DESCRIPTION("NVIDIA Tegra20 cpufreq driver");
MODULE_LICENSE("GPL");
