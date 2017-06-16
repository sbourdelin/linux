/*
 * This file is part of STM32 low-power timer driver
 *
 * Copyright (C) STMicroelectronics 2017
 *
 * Author: Gerald Baeza <gerald.baeza@st.com>
 *
 * License terms: GNU General Public License (GPL), version 2
 *
 * Inspired by pwm-stm32.c from Gerald Baeza
 */

#include <linux/bitfield.h>
#include <linux/mfd/stm32-lptimer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

struct stm32_pwm_lp {
	struct pwm_chip chip;
	struct clk *clk;
	struct regmap *regmap;
};

static inline struct stm32_pwm_lp *to_stm32_pwm_lp(struct pwm_chip *chip)
{
	return container_of(chip, struct stm32_pwm_lp, chip);
}

static const u8 prescalers[] = {1, 2, 4, 8, 16, 32, 64, 128};

static int stm32_pwm_lp_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			      struct pwm_state *state)
{
	struct stm32_pwm_lp *priv = to_stm32_pwm_lp(chip);
	unsigned long long prd, div, dty;
	struct pwm_state cstate;
	u32 val, mask, cfgr, wavpol, presc = 0;
	bool reenable = false;
	int ret;

	pwm_get_state(pwm, &cstate);

	if (!state->enabled) {
		if (cstate.enabled) {
			/* Disable LP timer */
			ret = regmap_write(priv->regmap, STM32_LPTIM_CR, 0);
			if (ret)
				return ret;
			clk_disable(priv->clk);
		}
		return 0;
	}

	/* Calculate the period and prescaler value */
	div = (unsigned long long)clk_get_rate(priv->clk) * state->period;
	do_div(div, NSEC_PER_SEC);
	prd = div;
	while (div > STM32_LPTIM_MAX_ARR) {
		presc++;
		if (presc >= ARRAY_SIZE(prescalers)) {
			dev_err(priv->chip.dev, "max prescaler exceeded\n");
			return -EINVAL;
		}
		div = prd;
		do_div(div, prescalers[presc]);
	}
	prd = div;

	/* Calculate the duty cycle */
	dty = prd * state->duty_cycle;
	do_div(dty, state->period);

	wavpol = FIELD_PREP(STM32_LPTIM_WAVPOL, state->polarity);

	if (!cstate.enabled) {
		ret = clk_enable(priv->clk);
		if (ret)
			return ret;
	}

	ret = regmap_read(priv->regmap, STM32_LPTIM_CFGR, &cfgr);
	if (ret)
		goto err;

	if ((wavpol != FIELD_GET(STM32_LPTIM_WAVPOL, cfgr)) ||
	    (presc != FIELD_GET(STM32_LPTIM_PRESC, cfgr))) {
		val = FIELD_PREP(STM32_LPTIM_PRESC, presc) | wavpol;
		mask = STM32_LPTIM_PRESC | STM32_LPTIM_WAVPOL;

		/* Must disable LP timer to modify CFGR */
		ret = regmap_write(priv->regmap, STM32_LPTIM_CR, 0);
		if (ret)
			goto err;
		reenable = true;
		ret = regmap_update_bits(priv->regmap, STM32_LPTIM_CFGR, mask,
					 val);
		if (ret)
			goto err;
	}

	if (!cstate.enabled || reenable) {
		/* Must enable LP timer to modify CMP & ARR */
		ret = regmap_write(priv->regmap, STM32_LPTIM_CR,
				   STM32_LPTIM_ENABLE);
		if (ret)
			goto err;
	}

	ret = regmap_write(priv->regmap, STM32_LPTIM_ARR, prd - 1);
	if (ret)
		goto err;

	ret = regmap_write(priv->regmap, STM32_LPTIM_CMP, prd - (1 + dty));
	if (ret)
		goto err;

	/* ensure CMP & ARR registers are properly written */
	ret = regmap_read_poll_timeout(priv->regmap, STM32_LPTIM_ISR, val,
				       (val & STM32_LPTIM_CMPOK_ARROK),
				       100, 1000);
	if (ret) {
		dev_err(priv->chip.dev, "ARR/CMP registers write issue\n");
		goto err;
	}
	ret = regmap_write(priv->regmap, STM32_LPTIM_ICR,
			   STM32_LPTIM_CMPOKCF_ARROKCF);
	if (ret)
		goto err;

	if (!cstate.enabled || reenable) {
		/* Start LP timer in continuous mode */
		ret = regmap_update_bits(priv->regmap, STM32_LPTIM_CR,
					 STM32_LPTIM_CNTSTRT,
					 STM32_LPTIM_CNTSTRT);
		if (ret) {
			regmap_write(priv->regmap, STM32_LPTIM_CR, 0);
			goto err;
		}
	}

	return 0;
err:
	if (!cstate.enabled)
		clk_disable(priv->clk);

	return ret;
}

static const struct pwm_ops stm32_pwm_lp_ops = {
	.owner = THIS_MODULE,
	.apply = stm32_pwm_lp_apply,
};

static int stm32_pwm_lp_probe(struct platform_device *pdev)
{
	struct stm32_lptimer *ddata = dev_get_drvdata(pdev->dev.parent);
	struct stm32_pwm_lp *priv;
	int ret;

	if (IS_ERR_OR_NULL(ddata))
		return -EINVAL;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regmap = ddata->regmap;
	priv->clk = ddata->clk;
	if (!priv->regmap || !priv->clk)
		return -EINVAL;

	priv->chip.base = -1;
	priv->chip.dev = &pdev->dev;
	priv->chip.ops = &stm32_pwm_lp_ops;
	priv->chip.npwm = 1;

	ret = pwmchip_add(&priv->chip);
	if (ret < 0)
		return ret;

	platform_set_drvdata(pdev, priv);

	return 0;
}

static int stm32_pwm_lp_remove(struct platform_device *pdev)
{
	struct stm32_pwm_lp *priv = platform_get_drvdata(pdev);

	if (pwm_is_enabled(priv->chip.pwms))
		pwm_disable(priv->chip.pwms);

	return pwmchip_remove(&priv->chip);
}

static const struct of_device_id stm32_pwm_lp_of_match[] = {
	{ .compatible = "st,stm32-pwm-lp", },
	{ /* end node */ },
};
MODULE_DEVICE_TABLE(of, stm32_pwm_lp_of_match);

static struct platform_driver stm32_pwm_lp_driver = {
	.probe	= stm32_pwm_lp_probe,
	.remove	= stm32_pwm_lp_remove,
	.driver	= {
		.name = "stm32-pwm-lp",
		.of_match_table = of_match_ptr(stm32_pwm_lp_of_match),
	},
};
module_platform_driver(stm32_pwm_lp_driver);

MODULE_ALIAS("platform:stm32-pwm-lp");
MODULE_DESCRIPTION("STMicroelectronics STM32 PWM LP driver");
MODULE_LICENSE("GPL v2");
