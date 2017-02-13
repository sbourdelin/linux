/*
 * Copyright (c) 2017 Yang Ling <gnaygnil@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <loongson1.h>

struct ls1x_pwm_chip {
	struct clk *clk;
	void __iomem *base;
	struct pwm_chip chip;
};

struct ls1x_pwm_channel {
	u32 period_ns;
	u32 duty_ns;
};

static inline struct ls1x_pwm_chip *to_ls1x_pwm_chip(struct pwm_chip *chip)
{
	return container_of(chip, struct ls1x_pwm_chip, chip);
}

static int ls1x_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct ls1x_pwm_channel *chan = NULL;

	chan = devm_kzalloc(chip->dev, sizeof(*chan), GFP_KERNEL);
	if (!chan)
		return -ENOMEM;

	pwm_set_chip_data(pwm, chan);

	return 0;
}

static void ls1x_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	devm_kfree(chip->dev, pwm_get_chip_data(pwm));
	pwm_set_chip_data(pwm, NULL);
}

static int ls1x_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
			int duty_ns, int period_ns)
{
	struct ls1x_pwm_chip *pc = to_ls1x_pwm_chip(chip);
	struct ls1x_pwm_channel *chan = pwm_get_chip_data(pwm);
	unsigned long long tmp;
	unsigned long period, duty;

	if (period_ns == chan->period_ns && duty_ns == chan->duty_ns)
		return 0;

	tmp = (unsigned long long)clk_get_rate(pc->clk) * period_ns;
	do_div(tmp, 1000000000);
	period = tmp;

	tmp = (unsigned long long)period * duty_ns;
	do_div(tmp, period_ns);
	duty = period - tmp;

	if (duty >= period)
		duty = period - 1;

	if (duty >> 24 || period >> 24)
		return -EINVAL;

	chan->period_ns = period_ns;
	chan->duty_ns = duty_ns;

	writel(duty, pc->base + PWM_HRC(pwm->hwpwm));
	writel(period, pc->base + PWM_LRC(pwm->hwpwm));
	writel(0x00, pc->base + PWM_CNT(pwm->hwpwm));

	return 0;
}

static int ls1x_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct ls1x_pwm_chip *pc = to_ls1x_pwm_chip(chip);

	writel(CNT_RST, pc->base + PWM_CTRL(pwm->hwpwm));
	writel(CNT_EN, pc->base + PWM_CTRL(pwm->hwpwm));

	return 0;
}

static void ls1x_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct ls1x_pwm_chip *pc = to_ls1x_pwm_chip(chip);

	writel(PWM_OE, pc->base + PWM_CTRL(pwm->hwpwm));
}

static const struct pwm_ops ls1x_pwm_ops = {
	.request = ls1x_pwm_request,
	.free = ls1x_pwm_free,
	.config = ls1x_pwm_config,
	.enable = ls1x_pwm_enable,
	.disable = ls1x_pwm_disable,
	.owner = THIS_MODULE,
};

static int ls1x_pwm_probe(struct platform_device *pdev)
{
	struct ls1x_pwm_chip *pc = NULL;
	struct resource *res = NULL;

	pc = devm_kzalloc(&pdev->dev, sizeof(*pc), GFP_KERNEL);
	if (!pc)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pc->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pc->base))
		return PTR_ERR(pc->base);

	pc->clk = devm_clk_get(&pdev->dev, "ls1x-pwmtimer");
	if (IS_ERR(pc->clk)) {
		dev_err(&pdev->dev, "failed to get %s clock\n", pdev->name);
		return PTR_ERR(pc->clk);
	}
	clk_prepare_enable(pc->clk);

	pc->chip.ops = &ls1x_pwm_ops;
	pc->chip.dev = &pdev->dev;
	pc->chip.base = -1;
	pc->chip.npwm = 4;

	platform_set_drvdata(pdev, pc);

	return pwmchip_add(&pc->chip);
}

static int ls1x_pwm_remove(struct platform_device *pdev)
{
	struct ls1x_pwm_chip *pc = platform_get_drvdata(pdev);
	int ret;

	ret = pwmchip_remove(&pc->chip);
	if (ret < 0)
		return ret;

	clk_disable_unprepare(pc->clk);

	return 0;
}

static struct platform_driver ls1x_pwm_driver = {
	.driver = {
		.name = "ls1x-pwm",
	},
	.probe = ls1x_pwm_probe,
	.remove = ls1x_pwm_remove,
};
module_platform_driver(ls1x_pwm_driver);

MODULE_AUTHOR("Yang Ling <gnaygnil@gmail.com>");
MODULE_DESCRIPTION("Loongson1 PWM driver");
MODULE_ALIAS("platform:loongson1-pwm");
MODULE_LICENSE("GPL");
