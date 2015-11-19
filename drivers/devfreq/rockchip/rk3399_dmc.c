/*
 * Copyright (c) 2014, Fuzhou Rockchip Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/devfreq.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/rwsem.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>
#include <soc/rockchip/rk3399-dmc-clk.h>

#define RK3399_DMC_NUM_CH	2

/* DDRMON_CTRL */
#define DDRMON_CTRL	0x04
#define LPDDR4_EN	(1 << 4)
#define HARDWARE_EN	(1 << 3)
#define LPDDR3_EN	(1 << 2)
#define SOFTWARE_EN	(1 << 1)
#define TIME_CNT_EN	(1 << 0)

#define DDRMON_CH0_COUNT_NUM		0x28
#define DDRMON_CH0_DFI_ACCESS_NUM	0x2c
#define DDRMON_CH1_COUNT_NUM		0x3c
#define DDRMON_CH1_DFI_ACCESS_NUM	0x40

struct dmc_usage {
	u32 access;
	u32 total;
};

struct rk3399_dmcfreq {
	struct device *clk_dev;
	struct devfreq *devfreq;
	struct devfreq_simple_ondemand_data ondemand_data;
	struct clk *dmc_clk;
	struct regulator *vdd_logic;
	struct dmc_usage ch_usage[RK3399_DMC_NUM_CH];
	struct rk3399_dmcclk *dmc_data;
	struct mutex lock;
	unsigned long rate, target_rate;
	unsigned long volt, target_volt;
};

static struct rk3399_dmcfreq dmcfreq;

static int rk3399_dmcfreq_target(struct device *dev, unsigned long *freq,
				 u32 flags)
{
	struct dev_pm_opp *opp;
	unsigned long old_clk_rate = dmcfreq.rate;
	unsigned long target_volt, target_rate;
	int err;

	rcu_read_lock();
	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp)) {
		rcu_read_unlock();
		return PTR_ERR(opp);
	}
	target_rate = dev_pm_opp_get_freq(opp);
	target_volt = dev_pm_opp_get_voltage(opp);

	opp = devfreq_recommended_opp(dev, &dmcfreq.rate, flags);
	if (IS_ERR(opp)) {
		rcu_read_unlock();
		return PTR_ERR(opp);
	}
	dmcfreq.volt = dev_pm_opp_get_voltage(opp);
	rcu_read_unlock();

	if (dmcfreq.rate == dmcfreq.target_rate)
		return 0;

	mutex_lock(&dmcfreq.lock);

	if (old_clk_rate < target_rate) {
		err = regulator_set_voltage(dmcfreq.vdd_logic, target_volt,
					    target_volt);
		if (err) {
			dev_err(dev, "Unable to set vol %lu\n", target_volt);
			goto out;
		}
	}

	err = clk_set_rate(dmcfreq.dmc_clk, target_rate);
	if (err) {
		dev_err(dev,
			"Unable to set freq %lu. Current freq %lu. Error %d\n",
			target_rate, old_clk_rate, err);
		regulator_set_voltage(dmcfreq.vdd_logic, dmcfreq.volt,
				      dmcfreq.volt);
		goto out;
	}

	dmcfreq.rate = target_rate;

	if (old_clk_rate > target_rate)
		err = regulator_set_voltage(dmcfreq.vdd_logic, target_volt,
					    target_volt);
	if (err)
		dev_err(dev, "Unable to set vol %lu\n", target_volt);
out:
	mutex_unlock(&dmcfreq.lock);
	return err;
}

static void rk3399_dmc_start_hardware_counter(void)
{
	void __iomem *ctrl_regs = dmcfreq.dmc_data->ctrl_regs;
	void __iomem *dfi_regs = dmcfreq.dmc_data->dfi_regs;
	u32 val;
	u32 ddr_type;

	/* get ddr type */
	val = readl_relaxed(ctrl_regs + DENALI_CTL_00);
	ddr_type = (val >> DRAM_CLASS_SHIFT) & DRAM_CLASS_MASK;

	/* set ddr type to dfi */
	val = readl_relaxed(dfi_regs + DDRMON_CTRL);
	if (ddr_type == LPDDR3) {
		val &= ~LPDDR4_EN;
		val |= LPDDR3_EN;
	} else if (ddr_type == LPDDR4) {
		val &= ~LPDDR3_EN;
		val |= LPDDR4_EN;
	} else
		val &= ~(LPDDR3_EN | LPDDR4_EN);

	/* enable count, use software mode */
	val &= ~HARDWARE_EN;
	val |= SOFTWARE_EN;

	writel_relaxed(val, dfi_regs + DDRMON_CTRL);
}

static void rk3399_dmc_stop_hardware_counter(void)
{
	void __iomem *dfi_regs = dmcfreq.dmc_data->dfi_regs;
	u32 val;

	val = readl_relaxed(dfi_regs + DDRMON_CTRL);
	val &= ~SOFTWARE_EN;
	writel_relaxed(val, dfi_regs + DDRMON_CTRL);
}

static int rk3399_dmc_get_busier_ch(void)
{
	u32 tmp, max = 0;
	u32 i, busier_ch = 0;
	void __iomem *dfi_regs = dmcfreq.dmc_data->dfi_regs;

	rk3399_dmc_stop_hardware_counter();

	/* Find out which channel is busier */
	for (i = 0; i < RK3399_DMC_NUM_CH; i++) {
		dmcfreq.ch_usage[i].access = readl_relaxed(dfi_regs +
				DDRMON_CH0_DFI_ACCESS_NUM + i * 20);
		dmcfreq.ch_usage[i].total = readl_relaxed(dfi_regs +
				DDRMON_CH0_COUNT_NUM + i * 20);
		tmp = dmcfreq.ch_usage[i].access;
		if (tmp > max) {
			busier_ch = i;
			max = tmp;
		}
	}
	rk3399_dmc_start_hardware_counter();

	return busier_ch;
}

static int rk3399_dmcfreq_get_dev_status(struct device *dev,
					 struct devfreq_dev_status *stat)
{
	int busier_ch;

	busier_ch = rk3399_dmc_get_busier_ch();
	stat->current_frequency = dmcfreq.rate;
	stat->busy_time = dmcfreq.ch_usage[busier_ch].access;
	stat->total_time = dmcfreq.ch_usage[busier_ch].total;

	return 0;
}

static int rk3399_dmcfreq_get_cur_freq(struct device *dev, unsigned long *freq)
{
	*freq = dmcfreq.rate;
	return 0;
}

static void rk3399_dmcfreq_exit(struct device *dev)
{
	devfreq_unregister_opp_notifier(dmcfreq.clk_dev, dmcfreq.devfreq);
}

static struct devfreq_dev_profile rk3399_devfreq_dmc_profile = {
	.polling_ms	= 200,
	.target		= rk3399_dmcfreq_target,
	.get_dev_status	= rk3399_dmcfreq_get_dev_status,
	.get_cur_freq	= rk3399_dmcfreq_get_cur_freq,
	.exit		= rk3399_dmcfreq_exit,
};

static __maybe_unused int rk3399_dmcfreq_suspend(struct device *dev)
{
	unsigned long freq = ULONG_MAX;

	dev_info(dmcfreq.clk_dev, "suspending DVFS and going to max freq\n");
	devfreq_suspend_device(dmcfreq.devfreq);
	rk3399_dmc_stop_hardware_counter();
	rk3399_dmcfreq_target(dmcfreq.clk_dev, &freq, 0);

	return 0;
}

static __maybe_unused int rk3399_dmcfreq_resume(struct device *dev)
{
	dev_info(dmcfreq.clk_dev, "resuming DVFS\n");
	rk3399_dmc_start_hardware_counter();
	devfreq_resume_device(dmcfreq.devfreq);

	return 0;
}

static SIMPLE_DEV_PM_OPS(rk3399_dmcfreq_pm, rk3399_dmcfreq_suspend,
			 rk3399_dmcfreq_resume);

static void rk3399_dmcfreq_shutdown(void)
{
	devfreq_suspend_device(dmcfreq.devfreq);
}

static struct syscore_ops rk3399_dmcfreq_syscore_ops = {
	.shutdown = rk3399_dmcfreq_shutdown,
};

static int rk3399_dmcfreq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	dmcfreq.clk_dev = dev->parent;
	mutex_init(&dmcfreq.lock);

	dmcfreq.dmc_data =
		(struct rk3399_dmcclk *)dev_get_drvdata(dmcfreq.clk_dev);

	dmcfreq.vdd_logic = regulator_get(dmcfreq.clk_dev, "logic");
	if (IS_ERR(dmcfreq.vdd_logic)) {
		dev_err(dev, "Cannot get the regulator \"vdd_logic\"\n");
		return PTR_ERR(dmcfreq.vdd_logic);
	}

	dmcfreq.dmc_clk = devm_clk_get(dev, "dmc_clk");
	if (IS_ERR(dmcfreq.dmc_clk)) {
		dev_err(dev, "Cannot get the clk dmc_clk\n");
		return PTR_ERR(dmcfreq.dmc_clk);
	};

	/*
	 * We add a devfreq driver to our parent since it has a device tree node
	 * with operating points.
	 */
	if (dev_pm_opp_of_add_table(dmcfreq.clk_dev)) {
		dev_err(dev, "Invalid operating-points in device tree.\n");
		return -EINVAL;
	}

	of_property_read_u32(dmcfreq.clk_dev->of_node, "upthreshold",
				&dmcfreq.ondemand_data.upthreshold);

	of_property_read_u32(dmcfreq.clk_dev->of_node, "downdifferential",
				&dmcfreq.ondemand_data.downdifferential);

	dmcfreq.devfreq = devfreq_add_device(dmcfreq.clk_dev,
					     &rk3399_devfreq_dmc_profile,
					     "simple_ondemand",
					     &dmcfreq.ondemand_data);
	if (IS_ERR(dmcfreq.devfreq))
		return PTR_ERR(dmcfreq.devfreq);

	devfreq_register_opp_notifier(dmcfreq.clk_dev, dmcfreq.devfreq);

	register_syscore_ops(&rk3399_dmcfreq_syscore_ops);

	return 0;
}

static int rk3399_dmcfreq_remove(struct platform_device *pdev)
{
	devfreq_remove_device(dmcfreq.devfreq);
	regulator_put(dmcfreq.vdd_logic);

	return 0;
}

static struct platform_driver rk3399_dmcfreq_driver = {
	.probe	= rk3399_dmcfreq_probe,
	.remove	= rk3399_dmcfreq_remove,
	.driver = {
		.name	= "rk3399-dmc-freq",
		.pm	= &rk3399_dmcfreq_pm,
	},
};
module_platform_driver(rk3399_dmcfreq_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("RK3399 dmcfreq driver with devfreq framework");
