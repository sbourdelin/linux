// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) STMicroelectronics 2018 - All Rights Reserved
 * Author: Philippe Peurichard <philippe.peurichard@st.com>,
 * Pascal Paillet <p.paillet@st.com> for STMicroelectronics.
 */

#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/mfd/stpmu1.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

/**
 * struct stpmu1_onkey - OnKey data
 * @pmic:		pointer to STPMU1 PMIC device
 * @input_dev:		pointer to input device
 * @irq_falling:	irq that we are hooked on to
 * @irq_rising:		irq that we are hooked on to
 */
struct stpmu1_onkey {
	struct stpmu1_dev *pmic;
	struct input_dev *input_dev;
	int irq_falling;
	int irq_rising;
};

/**
 * struct pmic_onkey_config - configuration of pmic PONKEYn
 * @turnoff_enabled:		value to enable turnoff condition
 * @cc_flag_clear:		value to clear CC flag in case of PowerOff
 * trigger by longkey press
 * @onkey_pullup_val:		value of PONKEY PullUp (active or inactive)
 * @long_press_time_val:	value for long press h/w shutdown event
 */
struct pmic_onkey_config {
	bool turnoff_enabled;
	bool cc_flag_clear;
	u8 onkey_pullup_val;
	u8 long_press_time_val;
};

/**
 * onkey_falling_irq() - button press isr
 * @irq:		irq
 * @pmic_onkey:		onkey device
 *
 * Return: IRQ_HANDLED
 */
static irqreturn_t onkey_falling_irq(int irq, void *ponkey)
{
	struct stpmu1_onkey *onkey = ponkey;
	struct input_dev *input_dev = onkey->input_dev;

	input_report_key(input_dev, KEY_POWER, 1);
	pm_wakeup_event(input_dev->dev.parent, 0);
	input_sync(input_dev);

	dev_dbg(&input_dev->dev, "Pwr Onkey Falling Interrupt received\n");

	return IRQ_HANDLED;
}

/**
 * onkey_rising_irq() - button released isr
 * @irq:		irq
 * @pmic_onkey:		onkey device
 *
 * Return: IRQ_HANDLED
 */
static irqreturn_t onkey_rising_irq(int irq, void *ponkey)
{
	struct stpmu1_onkey *onkey = ponkey;
	struct input_dev *input_dev = onkey->input_dev;

	input_report_key(input_dev, KEY_POWER, 0);
	pm_wakeup_event(input_dev->dev.parent, 0);
	input_sync(input_dev);

	dev_dbg(&input_dev->dev, "Pwr Onkey Rising Interrupt received\n");

	return IRQ_HANDLED;
}

/**
 * stpmu1_onkey_dt_params() - device tree parameter parser
 * @pdev:	platform device for the PONKEY
 * @onkey:	pointer to onkey driver data
 * @config:	configuration params to be filled up
 */
static int stpmu1_onkey_dt_params(struct platform_device *pdev,
				  struct stpmu1_onkey *onkey,
				  struct pmic_onkey_config *config)
{
	struct device *dev = &pdev->dev;
	struct device_node *np;
	u32 val;

	np = dev->of_node;
	if (!np)
		return -EINVAL;

	onkey->irq_falling = platform_get_irq_byname(pdev, "onkey-falling");
	if (onkey->irq_falling < 0) {
		dev_err(dev, "failed: request IRQ onkey-falling %d",
			onkey->irq_falling);
		return onkey->irq_falling;
	}

	onkey->irq_rising = platform_get_irq_byname(pdev, "onkey-rising");
	if (onkey->irq_rising < 0) {
		dev_err(dev, "failed: request IRQ onkey-rising %d",
			onkey->irq_rising);
		return onkey->irq_rising;
	}

	if (!of_property_read_u32(np, "st,onkey-long-press-seconds", &val)) {
		if (val >= 1 && val <= 16) {
			config->long_press_time_val = (16 - val);
		} else {
			dev_warn(dev,
				 "Invalid range of long key pressed timer %d (<16)\n\r",
				 val);
		}
	}
	if (of_get_property(np, "st,onkey-pwroff-enabled", NULL))
		config->turnoff_enabled = true;

	if (of_get_property(np, "st,onkey-clear-cc-flag", NULL))
		config->cc_flag_clear = true;

	if (of_get_property(np, "st,onkey-pu-inactive", NULL))
		config->onkey_pullup_val = PONKEY_PU_ACTIVE;

	dev_dbg(dev, "onkey-switch-off duration=%d seconds\n",
		config->long_press_time_val);

	return 0;
}

/**
 * stpmu1_onkey_probe() - probe
 * @pdev:	platform device for the PONKEY
 *
 * Return: 0 for successful probe else appropriate error
 */
static int stpmu1_onkey_probe(struct platform_device *pdev)
{
	struct stpmu1_dev *pmic = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	struct input_dev *input_dev;
	struct stpmu1_onkey *onkey;
	struct pmic_onkey_config config;
	unsigned int val = 0;
	int ret;

	onkey = devm_kzalloc(dev, sizeof(*onkey), GFP_KERNEL);
	if (!onkey)
		return -ENOMEM;

	memset(&config, 0, sizeof(struct pmic_onkey_config));
	ret = stpmu1_onkey_dt_params(pdev, onkey, &config);
	if (ret < 0)
		goto err;

	input_dev = devm_input_allocate_device(dev);
	if (!input_dev) {
		dev_err(dev, "Can't allocate Pwr Onkey Input Device\n");
		ret = -ENOMEM;
		goto err;
	}

	input_dev->name = "pmic_onkey";
	input_dev->phys = "pmic_onkey/input0";
	input_dev->dev.parent = dev;

	input_set_capability(input_dev, EV_KEY, KEY_POWER);

	/* Setup Power Onkey Hardware parameters (long key press)*/
	val = (config.long_press_time_val & PONKEY_TURNOFF_TIMER_MASK);
	if (config.turnoff_enabled)
		val |= PONKEY_PWR_OFF;
	if (config.cc_flag_clear)
		val |= PONKEY_CC_FLAG_CLEAR;
	ret = regmap_update_bits(pmic->regmap, PKEY_TURNOFF_CR,
				 PONKEY_TURNOFF_MASK, val);
	if (ret) {
		dev_err(dev, "LONG_PRESS_KEY_UPDATE failed: %d\n", ret);
		goto err;
	}

	ret = regmap_update_bits(pmic->regmap, PADS_PULL_CR,
				 PONKEY_PU_ACTIVE,
				 config.onkey_pullup_val);

	if (ret) {
		dev_err(dev, "ONKEY Pads configuration failed: %d\n", ret);
		goto err;
	}

	onkey->pmic = pmic;
	onkey->input_dev = input_dev;

	ret = devm_request_threaded_irq(dev, onkey->irq_falling, NULL,
					onkey_falling_irq,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					dev_name(dev), onkey);
	if (ret) {
		dev_err(dev, "Can't get IRQ for Onkey Falling edge: %d\n", ret);
		goto err;
	}

	ret = devm_request_threaded_irq(dev, onkey->irq_rising, NULL,
					onkey_rising_irq,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					dev_name(dev), onkey);
	if (ret) {
		dev_err(dev, "Can't get IRQ for Onkey Rising edge: %d\n", ret);
		goto err;
	}

	ret = input_register_device(input_dev);
	if (ret) {
		dev_err(dev, "Can't register power button: %d\n", ret);
		goto err;
	}

	platform_set_drvdata(pdev, onkey);
	device_init_wakeup(dev, true);

	dev_dbg(dev, "PMIC Pwr Onkey driver probed\n");

err:
	return ret;
}

/**
 * stpmu1_onkey_remove() - Cleanup on removal
 * @pdev:	platform device for the button
 *
 * Return: 0
 */
static int stpmu1_onkey_remove(struct platform_device *pdev)
{
	struct stpmu1_onkey *onkey = platform_get_drvdata(pdev);

	input_unregister_device(onkey->input_dev);
	return 0;
}

#ifdef CONFIG_PM_SLEEP

/**
 * pmic_onkey_suspend() - suspend handler
 * @dev:	power button device
 *
 * Cancel all pending work items for the power button, setup irq for wakeup
 *
 * Return: 0
 */
static int __maybe_unused stpmu1_onkey_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct stpmu1_onkey *onkey = platform_get_drvdata(pdev);

	if (device_may_wakeup(dev)) {
		enable_irq_wake(onkey->irq_falling);
		enable_irq_wake(onkey->irq_rising);
	}
	return 0;
}

/**
 * pmic_onkey_resume() - resume handler
 * @dev:	power button device
 *
 * Just disable the wakeup capability of irq here.
 *
 * Return: 0
 */
static int __maybe_unused stpmu1_onkey_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct stpmu1_onkey *onkey = platform_get_drvdata(pdev);

	if (device_may_wakeup(dev)) {
		disable_irq_wake(onkey->irq_falling);
		disable_irq_wake(onkey->irq_rising);
	}
	return 0;
}

#endif

static SIMPLE_DEV_PM_OPS(stpmu1_onkey_pm,
			 stpmu1_onkey_suspend,
			 stpmu1_onkey_resume);

static const struct of_device_id of_stpmu1_onkey_match[] = {
	{ .compatible = "st,stpmu1-onkey" },
	{ },
};

MODULE_DEVICE_TABLE(of, of_stpmu1_onkey_match);

static struct platform_driver stpmu1_onkey_driver = {
	.probe	= stpmu1_onkey_probe,
	.remove	= stpmu1_onkey_remove,
	.driver	= {
		.name	= "stpmu1_onkey",
		.of_match_table = of_match_ptr(of_stpmu1_onkey_match),
		.pm	= &stpmu1_onkey_pm,
	},
};
module_platform_driver(stpmu1_onkey_driver);

MODULE_DESCRIPTION("Onkey driver for STPMU1");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("philippe.peurichard@st.com>");
