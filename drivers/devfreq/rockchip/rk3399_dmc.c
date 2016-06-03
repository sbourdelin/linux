/*
 * Copyright (c) 2016, Fuzhou Rockchip Electronics Co., Ltd
 * Author: Lin Huang <hl@rock-chips.com>
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
#include <linux/devfreq-event.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/regulator/consumer.h>
#include <linux/rwsem.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>

#include <soc/rockchip/rockchip_dmc.h>

struct rk3399_dmcfreq {
	struct device *dev;
	struct devfreq *devfreq;
	struct devfreq_simple_ondemand_data ondemand_data;
	struct clk *dmc_clk;
	struct completion dcf_hold_completion;
	struct devfreq_event_dev *edev;
	struct mutex lock;
	struct notifier_block dmc_nb;
	int irq;
	struct regulator *vdd_center;
	unsigned long rate, target_rate;
	unsigned long volt, target_volt;
};

static int rk3399_dmcfreq_target(struct device *dev, unsigned long *freq,
				 u32 flags)
{
	struct platform_device *pdev = container_of(dev, struct platform_device,
						    dev);
	struct rk3399_dmcfreq *dmcfreq = platform_get_drvdata(pdev);
	struct dev_pm_opp *opp;
	unsigned long old_clk_rate = dmcfreq->rate;
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
	opp = devfreq_recommended_opp(dev, &dmcfreq->rate, flags);
	if (IS_ERR(opp)) {
		rcu_read_unlock();
		return PTR_ERR(opp);
	}
	dmcfreq->volt = dev_pm_opp_get_voltage(opp);
	rcu_read_unlock();

	if (dmcfreq->rate == target_rate)
		return 0;

	mutex_lock(&dmcfreq->lock);

	/*
	 * if frequency scaling from low to high, adjust voltage first;
	 * if frequency scaling from high to low, adjuset frequency first;
	 */
	if (old_clk_rate < target_rate) {
		err = regulator_set_voltage(dmcfreq->vdd_center, target_volt,
					    target_volt);
		if (err) {
			dev_err(dev, "Unable to set vol %lu\n", target_volt);
			goto out;
		}
	}

	dmc_event(DMCFREQ_ADJUST);
	reinit_completion(&dmcfreq->dcf_hold_completion);
	err = clk_set_rate(dmcfreq->dmc_clk, target_rate);
	if (err) {
		dev_err(dev,
			"Unable to set freq %lu. Current freq %lu. Error %d\n",
			target_rate, old_clk_rate, err);
		regulator_set_voltage(dmcfreq->vdd_center, dmcfreq->volt,
				      dmcfreq->volt);
		dmc_event(DMCFREQ_FINISH);
		goto out;
	}

	/* wait until bcf irq happen, it means freq scaling finish in bl31 */
	wait_for_completion(&dmcfreq->dcf_hold_completion);
	dmc_event(DMCFREQ_FINISH);

	/*
	 * check the rate we get whether is correct
	 * in bl31 dcf, there only two result we will get,
	 * 1. ddr frequency scaling fail, we still get the old rate
	 * 2, ddr frequency scaling sucessful, we get the rate we set
	 */
	dmcfreq->rate = clk_get_rate(dmcfreq->dmc_clk);

	/* do not get the right rate, set voltage to old value */
	if (dmcfreq->rate != target_rate) {
		dev_err(dev, "get wrong ddr frequency, Request freq %lu,\
			Current freq %lu\n", target_rate, dmcfreq->rate);
		regulator_set_voltage(dmcfreq->vdd_center, dmcfreq->volt,
				      dmcfreq->volt);
	} else if (old_clk_rate > target_rate)
		err = regulator_set_voltage(dmcfreq->vdd_center, target_volt,
					    target_volt);
	if (err)
		dev_err(dev, "Unable to set vol %lu\n", target_volt);

out:
	mutex_unlock(&dmcfreq->lock);
	return err;
}

static int rk3399_dmcfreq_get_dev_status(struct device *dev,
					 struct devfreq_dev_status *stat)
{
	struct platform_device *pdev = container_of(dev, struct platform_device,
						    dev);
	struct rk3399_dmcfreq *dmcfreq = platform_get_drvdata(pdev);
	struct devfreq_event_data edata;

	devfreq_event_get_event(dmcfreq->edev, &edata);

	stat->current_frequency = dmcfreq->rate;
	stat->busy_time = edata.load_count;
	stat->total_time = edata.total_count;

	return 0;
}

static int rk3399_dmcfreq_get_cur_freq(struct device *dev, unsigned long *freq)
{
	struct platform_device *pdev = container_of(dev, struct platform_device,
						    dev);
	struct rk3399_dmcfreq *dmcfreq = platform_get_drvdata(pdev);

	*freq = dmcfreq->rate;

	return 0;
}

static void rk3399_dmcfreq_exit(struct device *dev)
{
	struct platform_device *pdev = container_of(dev, struct platform_device,
						    dev);
	struct rk3399_dmcfreq *dmcfreq = platform_get_drvdata(pdev);

	devfreq_unregister_opp_notifier(dev, dmcfreq->devfreq);
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
	rockchip_dmc_disable();
	return 0;
}

static __maybe_unused int rk3399_dmcfreq_resume(struct device *dev)
{
	rockchip_dmc_enable();
	return 0;
}

static SIMPLE_DEV_PM_OPS(rk3399_dmcfreq_pm, rk3399_dmcfreq_suspend,
			 rk3399_dmcfreq_resume);

static int rk3399_dmc_enable_notify(struct notifier_block *nb,
				    unsigned long event, void *data)
{
	struct rk3399_dmcfreq *dmcfreq =
			      container_of(nb, struct rk3399_dmcfreq, dmc_nb);
	unsigned long freq = ULONG_MAX;

	if (event == DMC_ENABLE) {
		devfreq_event_enable_edev(dmcfreq->edev);
		devfreq_resume_device(dmcfreq->devfreq);
		return NOTIFY_OK;
	} else if (event == DMC_DISABLE) {
		devfreq_event_disable_edev(dmcfreq->edev);
		devfreq_suspend_device(dmcfreq->devfreq);

		/* when disable dmc, set sdram to max frequency */
		rk3399_dmcfreq_target(dmcfreq->dev, &freq, 0);
		return NOTIFY_OK;
	}

	return NOTIFY_DONE;
}

static irqreturn_t rk3399_dmc_irq(int irq, void *dev_id)
{
	struct rk3399_dmcfreq *dmcfreq = dev_id;

	complete(&dmcfreq->dcf_hold_completion);

	return IRQ_HANDLED;
}

static int rk3399_dmcfreq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rk3399_dmcfreq *data;
	int ret, irq;
	struct device_node *np = pdev->dev.of_node;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "no dmc irq resource\n");
		return -EINVAL;
	}

	data = devm_kzalloc(dev, sizeof(struct rk3399_dmcfreq), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	mutex_init(&data->lock);

	data->vdd_center = devm_regulator_get(dev, "center");
	if (IS_ERR(data->vdd_center)) {
		dev_err(dev, "Cannot get the regulator \"center\"\n");
		return PTR_ERR(data->vdd_center);
	}

	data->dmc_clk = devm_clk_get(dev, "dmc_clk");
	if (IS_ERR(data->dmc_clk)) {
		dev_err(dev, "Cannot get the clk dmc_clk\n");
		return PTR_ERR(data->dmc_clk);
	};

	data->edev = devfreq_event_get_edev_by_phandle(dev, 0);
	if (IS_ERR(data->edev))
		return -EPROBE_DEFER;

	ret = devfreq_event_enable_edev(data->edev);
	if (ret < 0) {
		dev_err(dev, "failed to enable devfreq-event devices\n");
		return ret;
	}

	/*
	 * We add a devfreq driver to our parent since it has a device tree node
	 * with operating points.
	 */
	if (dev_pm_opp_of_add_table(dev)) {
		dev_err(dev, "Invalid operating-points in device tree.\n");
		return -EINVAL;
	}

	of_property_read_u32(np, "upthreshold",
			     &data->ondemand_data.upthreshold);

	of_property_read_u32(np, "downdifferential",
			     &data->ondemand_data.downdifferential);

	data->devfreq = devfreq_add_device(dev,
					   &rk3399_devfreq_dmc_profile,
					   "simple_ondemand",
					   &data->ondemand_data);
	if (IS_ERR(data->devfreq))
		return PTR_ERR(data->devfreq);

	devfreq_register_opp_notifier(dev, data->devfreq);

	data->dmc_nb.notifier_call = rk3399_dmc_enable_notify;
	dmc_register_notifier(&data->dmc_nb);

	init_completion(&data->dcf_hold_completion);

	data->irq = irq;
	ret = devm_request_irq(dev, irq, rk3399_dmc_irq, 0,
			       dev_name(dev), data);
	if (ret) {
		dev_err(dev, "failed to request dmc irq: %d\n", ret);
		return ret;
	}

	data->dev = dev;
	platform_set_drvdata(pdev, data);

	return 0;
}

static int rk3399_dmcfreq_remove(struct platform_device *pdev)
{
	struct rk3399_dmcfreq *dmcfreq = platform_get_drvdata(pdev);

	devfreq_remove_device(dmcfreq->devfreq);
	regulator_put(dmcfreq->vdd_center);

	return 0;
}

static const struct of_device_id rk3399dmc_devfreq_of_match[] = {
	{ .compatible = "rockchip,rk3399-dmc" },
	{ },
};

static struct platform_driver rk3399_dmcfreq_driver = {
	.probe	= rk3399_dmcfreq_probe,
	.remove	= rk3399_dmcfreq_remove,
	.driver = {
		.name	= "rk3399-dmc-freq",
		.pm	= &rk3399_dmcfreq_pm,
		.of_match_table = rk3399dmc_devfreq_of_match,
	},
};
module_platform_driver(rk3399_dmcfreq_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("RK3399 dmcfreq driver with devfreq framework");
