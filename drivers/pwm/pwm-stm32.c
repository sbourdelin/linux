/*
 * Copyright (C) STMicroelectronics 2016
 * Author:  Gerald Baeza <gerald.baeza@st.com>
 * License terms:  GNU General Public License (GPL), version 2
 *
 * Inspired by timer-stm32.c from Maxime Coquelin
 *             pwm-atmel.c from Bo Shen
 */

#include <linux/of.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

#include <linux/mfd/stm32-gptimer.h>

#define DRIVER_NAME "stm32-pwm"

#define CAP_COMPLEMENTARY	BIT(0)
#define CAP_32BITS_COUNTER	BIT(1)
#define CAP_BREAKINPUT		BIT(2)
#define CAP_BREAKINPUT_POLARITY BIT(3)

struct stm32_pwm_dev {
	struct device *dev;
	struct clk *clk;
	struct regmap *regmap;
	struct pwm_chip chip;
	int caps;
	int npwm;
	u32 polarity;
};

#define to_stm32_pwm_dev(x) container_of(chip, struct stm32_pwm_dev, chip)

static u32 __active_channels(struct stm32_pwm_dev *pwm_dev)
{
	u32 ccer;

	regmap_read(pwm_dev->regmap, TIM_CCER, &ccer);

	return ccer & TIM_CCER_CCXE;
}

static int write_ccrx(struct stm32_pwm_dev *dev, struct pwm_device *pwm,
		      u32 ccr)
{
	switch (pwm->hwpwm) {
	case 0:
		return regmap_write(dev->regmap, TIM_CCR1, ccr);
	case 1:
		return regmap_write(dev->regmap, TIM_CCR2, ccr);
	case 2:
		return regmap_write(dev->regmap, TIM_CCR3, ccr);
	case 3:
		return regmap_write(dev->regmap, TIM_CCR4, ccr);
	}
	return -EINVAL;
}

static int stm32_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
			    int duty_ns, int period_ns)
{
	struct stm32_pwm_dev *dev = to_stm32_pwm_dev(chip);
	unsigned long long prd, div, dty;
	int prescaler = 0;
	u32 max_arr = 0xFFFF, ccmr, mask, shift, bdtr;

	if (dev->caps & CAP_32BITS_COUNTER)
		max_arr = 0xFFFFFFFF;

	/* Period and prescaler values depends of clock rate */
	div = (unsigned long long)clk_get_rate(dev->clk) * period_ns;

	do_div(div, NSEC_PER_SEC);
	prd = div;

	while (div > max_arr) {
		prescaler++;
		div = prd;
		do_div(div, (prescaler + 1));
	}
	prd = div;

	if (prescaler > MAX_TIM_PSC) {
		dev_err(chip->dev, "prescaler exceeds the maximum value\n");
		return -EINVAL;
	}

	/* All channels share the same prescaler and counter so
	 * when two channels are active at the same we can't change them
	 */
	if (__active_channels(dev) & ~(1 << pwm->hwpwm * 4)) {
		u32 psc, arr;

		regmap_read(dev->regmap, TIM_PSC, &psc);
		regmap_read(dev->regmap, TIM_ARR, &arr);

		if ((psc != prescaler) || (arr != prd - 1))
			return -EINVAL;
	}

	regmap_write(dev->regmap, TIM_PSC, prescaler);
	regmap_write(dev->regmap, TIM_ARR, prd - 1);
	regmap_update_bits(dev->regmap, TIM_CR1, TIM_CR1_ARPE, TIM_CR1_ARPE);

	/* Calculate the duty cycles */
	dty = prd * duty_ns;
	do_div(dty, period_ns);

	write_ccrx(dev, pwm, dty);

	/* Configure output mode */
	shift = (pwm->hwpwm & 0x1) * 8;
	ccmr = (TIM_CCMR_PE | TIM_CCMR_M1) << shift;
	mask = 0xFF << shift;

	if (pwm->hwpwm & 0x2)
		regmap_update_bits(dev->regmap, TIM_CCMR2, mask, ccmr);
	else
		regmap_update_bits(dev->regmap, TIM_CCMR1, mask, ccmr);

	if (!(dev->caps & CAP_BREAKINPUT))
		return 0;

	bdtr = TIM_BDTR_MOE | TIM_BDTR_AOE;

	if (dev->caps & CAP_BREAKINPUT_POLARITY)
		bdtr |= TIM_BDTR_BKE;

	if (dev->polarity)
		bdtr |= TIM_BDTR_BKP;

	regmap_update_bits(dev->regmap, TIM_BDTR,
			   TIM_BDTR_MOE | TIM_BDTR_AOE |
			   TIM_BDTR_BKP | TIM_BDTR_BKE,
			   bdtr);

	return 0;
}

static int stm32_pwm_set_polarity(struct pwm_chip *chip, struct pwm_device *pwm,
				  enum pwm_polarity polarity)
{
	u32 mask;
	struct stm32_pwm_dev *dev = to_stm32_pwm_dev(chip);

	mask = TIM_CCER_CC1P << (pwm->hwpwm * 4);
	if (dev->caps & CAP_COMPLEMENTARY)
		mask |= TIM_CCER_CC1NP << (pwm->hwpwm * 4);

	regmap_update_bits(dev->regmap, TIM_CCER, mask,
			   polarity == PWM_POLARITY_NORMAL ? 0 : mask);

	return 0;
}

static int stm32_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	u32 mask;
	struct stm32_pwm_dev *dev = to_stm32_pwm_dev(chip);

	clk_enable(dev->clk);

	/* Enable channel */
	mask = TIM_CCER_CC1E << (pwm->hwpwm * 4);
	if (dev->caps & CAP_COMPLEMENTARY)
		mask |= TIM_CCER_CC1NE << (pwm->hwpwm * 4);

	regmap_update_bits(dev->regmap, TIM_CCER, mask, mask);

	/* Make sure that registers are updated */
	regmap_update_bits(dev->regmap, TIM_EGR, TIM_EGR_UG, TIM_EGR_UG);

	/* Enable controller */
	regmap_update_bits(dev->regmap, TIM_CR1, TIM_CR1_CEN, TIM_CR1_CEN);

	return 0;
}

static void stm32_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	u32 mask;
	struct stm32_pwm_dev *dev = to_stm32_pwm_dev(chip);

	/* Disable channel */
	mask = TIM_CCER_CC1E << (pwm->hwpwm * 4);
	if (dev->caps & CAP_COMPLEMENTARY)
		mask |= TIM_CCER_CC1NE << (pwm->hwpwm * 4);

	regmap_update_bits(dev->regmap, TIM_CCER, mask, 0);

	/* When all channels are disabled, we can disable the controller */
	if (!__active_channels(dev))
		regmap_update_bits(dev->regmap, TIM_CR1, TIM_CR1_CEN, 0);

	clk_disable(dev->clk);
}

static const struct pwm_ops stm32pwm_ops = {
	.config = stm32_pwm_config,
	.set_polarity = stm32_pwm_set_polarity,
	.enable = stm32_pwm_enable,
	.disable = stm32_pwm_disable,
};

static int stm32_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct stm32_gptimer_dev *mfd = dev_get_drvdata(pdev->dev.parent);
	struct stm32_pwm_dev *pwm;
	int ret;

	pwm = devm_kzalloc(dev, sizeof(*pwm), GFP_KERNEL);
	if (!pwm)
		return -ENOMEM;

	pwm->regmap = mfd->regmap;
	pwm->clk = mfd->clk;

	if (!pwm->regmap || !pwm->clk)
		return -EINVAL;

	if (of_property_read_bool(np, "st,complementary"))
		pwm->caps |= CAP_COMPLEMENTARY;

	if (of_property_read_bool(np, "st,32bits-counter"))
		pwm->caps |= CAP_32BITS_COUNTER;

	if (of_property_read_bool(np, "st,breakinput"))
		pwm->caps |= CAP_BREAKINPUT;

	if (!of_property_read_u32(np, "st,breakinput-polarity", &pwm->polarity))
		pwm->caps |= CAP_BREAKINPUT_POLARITY;

	of_property_read_u32(np, "st,pwm-num-chan", &pwm->npwm);

	pwm->chip.base = -1;
	pwm->chip.dev = dev;
	pwm->chip.ops = &stm32pwm_ops;
	pwm->chip.npwm = pwm->npwm;

	ret = pwmchip_add(&pwm->chip);
	if (ret < 0)
		return ret;

	platform_set_drvdata(pdev, pwm);

	return 0;
}

static int stm32_pwm_remove(struct platform_device *pdev)
{
	struct stm32_pwm_dev *pwm = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < pwm->npwm; i++)
		pwm_disable(&pwm->chip.pwms[i]);

	pwmchip_remove(&pwm->chip);

	return 0;
}

static const struct of_device_id stm32_pwm_of_match[] = {
	{
		.compatible = "st,stm32-pwm",
	},
};
MODULE_DEVICE_TABLE(of, stm32_pwm_of_match);

static struct platform_driver stm32_pwm_driver = {
	.probe		= stm32_pwm_probe,
	.remove		= stm32_pwm_remove,
	.driver	= {
		.name	= DRIVER_NAME,
		.of_match_table = stm32_pwm_of_match,
	},
};
module_platform_driver(stm32_pwm_driver);

MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_DESCRIPTION("STMicroelectronics STM32 PWM driver");
MODULE_LICENSE("GPL");
