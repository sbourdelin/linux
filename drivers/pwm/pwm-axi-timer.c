/*
 * Copyright 2017 Alvaro Gamez Machado <alvaro.gamez@hazent.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2.
 *
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/regmap.h>

struct axi_timer_pwm_chip {
	struct pwm_chip chip;
	struct clk *clk;
	void __iomem *regs;  /* virt. address of the control registers */
};

#define TCSR0   (0x00)
#define TLR0    (0x04)
#define TCR0    (0x08)
#define TCSR1   (0x10)
#define TLR1    (0x14)
#define TCR1    (0x18)

#define TCSR_MDT        BIT(0)
#define TCSR_UDT        BIT(1)
#define TCSR_GENT       BIT(2)
#define TCSR_CAPT       BIT(3)
#define TCSR_ARHT       BIT(4)
#define TCSR_LOAD       BIT(5)
#define TCSR_ENIT       BIT(6)
#define TCSR_ENT        BIT(7)
#define TCSR_TINT       BIT(8)
#define TCSR_PWMA       BIT(9)
#define TCSR_ENALL      BIT(10)
#define TCSR_CASC       BIT(11)

#define to_axi_timer_pwm_chip(_chip) \
	container_of(_chip, struct axi_timer_pwm_chip, chip)

static int axi_timer_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
				int duty_ns, int period_ns)
{
	struct axi_timer_pwm_chip *axi_timer = to_axi_timer_pwm_chip(chip);
	unsigned long long c;
	int period_cycles, duty_cycles;

	c = clk_get_rate(axi_timer->clk);

	/* When counters are configured to count down, UDT=1 (see datasheet):
	 * PWM_PERIOD = (TLR0 + 2) * AXI_CLOCK_PERIOD
	 * PWM_HIGH_TIME = (TLR1 + 2) * AXI_CLOCK_PERIOD
	 */
	period_cycles = div64_u64(c * period_ns, NSEC_PER_SEC) - 2;
	duty_cycles = div64_u64(c * duty_ns, NSEC_PER_SEC) - 2;

	iowrite32(period_cycles, axi_timer->regs + TLR0);
	iowrite32(duty_cycles, axi_timer->regs + TLR1);

	/* Load timer values */
	u32 tcsr;

	tcsr = ioread32(axi_timer->regs + TCSR0);
	iowrite32(tcsr | TCSR_LOAD, axi_timer->regs + TCSR0);
	iowrite32(tcsr, axi_timer->regs + TCSR0);

	tcsr = ioread32(axi_timer->regs + TCSR1);
	iowrite32(tcsr | TCSR_LOAD, axi_timer->regs + TCSR1);
	iowrite32(tcsr, axi_timer->regs + TCSR1);

	return 0;
}

static int axi_timer_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct axi_timer_pwm_chip *axi_timer = to_axi_timer_pwm_chip(chip);

	/* see timer data sheet for detail
	 * !CASC - disable cascaded operation
	 * ENALL - enable all
	 * PWMA - enable PWM
	 * !TINT - don't care about interrupts
	 * ENT- enable timer itself
	 * !ENIT - disable interrupt
	 * !LOAD - clear the bit to let go
	 * ARHT - auto reload
	 * !CAPT - no external trigger
	 * GENT - required for PWM
	 * UDT - set the timer as down counter
	 * !MDT - generate mode
	 */
	iowrite32(TCSR_ENALL | TCSR_PWMA | TCSR_ENT | TCSR_ARHT |
		  TCSR_GENT | TCSR_UDT, axi_timer->regs + TCSR0);
	iowrite32(TCSR_ENALL | TCSR_PWMA | TCSR_ENT | TCSR_ARHT |
		  TCSR_GENT | TCSR_UDT, axi_timer->regs + TCSR1);

	return 0;
}

static void axi_timer_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct axi_timer_pwm_chip *axi_timer = to_axi_timer_pwm_chip(chip);

	u32 tcsr;

	tcsr = ioread32(axi_timer->regs + TCSR0);
	iowrite32(tcsr & ~TCSR_PWMA, axi_timer->regs + TCSR0);

	tcsr = ioread32(axi_timer->regs + TCSR1);
	iowrite32(tcsr & ~TCSR_PWMA, axi_timer->regs + TCSR1);
}

static const struct pwm_ops axi_timer_pwm_ops = {
	.config = axi_timer_pwm_config,
	.enable = axi_timer_pwm_enable,
	.disable = axi_timer_pwm_disable,
	.owner = THIS_MODULE,
};

static int axi_timer_pwm_probe(struct platform_device *pdev)
{
	struct axi_timer_pwm_chip *axi_timer;
	struct resource *res;
	int ret;

	axi_timer = devm_kzalloc(&pdev->dev, sizeof(*axi_timer), GFP_KERNEL);
	if (!axi_timer)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	axi_timer->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(axi_timer->regs))
		return PTR_ERR(axi_timer->regs);

	axi_timer->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(axi_timer->clk))
		return PTR_ERR(axi_timer->clk);

	axi_timer->chip.dev = &pdev->dev;
	axi_timer->chip.ops = &axi_timer_pwm_ops;
	axi_timer->chip.npwm = 1;
	axi_timer->chip.base = -1;

	dev_info(&pdev->dev, "at 0x%08llX mapped to 0x%p\n",
		 (unsigned long long)res->start, axi_timer->regs);

	ret = pwmchip_add(&axi_timer->chip);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to add PWM chip, error %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, axi_timer);

	return 0;
}

static int axi_timer_pwm_remove(struct platform_device *pdev)
{
	struct axi_timer_pwm_chip *axi_timer = platform_get_drvdata(pdev);
	unsigned int i;

	for (i = 0; i < axi_timer->chip.npwm; i++)
		pwm_disable(&axi_timer->chip.pwms[i]);

	return pwmchip_remove(&axi_timer->chip);
}

static const struct of_device_id axi_timer_pwm_dt_ids[] = {
	{ .compatible = "xlnx,axi-timer-2.0", },
};
MODULE_DEVICE_TABLE(of, axi_timer_pwm_dt_ids);

static struct platform_driver axi_timer_pwm_driver = {
	.driver = {
		.name = "axi_timer-pwm",
		.of_match_table = axi_timer_pwm_dt_ids,
	},
	.probe = axi_timer_pwm_probe,
	.remove = axi_timer_pwm_remove,
};
module_platform_driver(axi_timer_pwm_driver);

MODULE_ALIAS("platform:axi_timer-pwm");
MODULE_AUTHOR("Alvaro Gamez Machado <alvaro.gamez@hazent.com>");
MODULE_DESCRIPTION("AXI TIMER PWM Driver");
MODULE_LICENSE("GPL v2");
