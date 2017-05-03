/*
 *  PWM vibrator driver
 *
 *  Copyright (C) 2017 Collabora Ltd.
 *
 *  Based on previous work from:
 *  Copyright (C) 2012 Dmitry Torokhov <dmitry.torokhov@gmail.com>
 *
 *  Based on PWM beeper driver:
 *  Copyright (C) 2010, Lars-Peter Clausen <lars@metafoo.de>
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under  the terms of the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the License, or (at your
 *  option) any later version.
 */

#define DEBUG

#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

/**
 * Motorola Droid 4 (also known as mapphone), has a vibrator, which pulses
 * 1x on rising edge. Increasing the pwm period results in more pulses per
 * second, but reduces intensity. There is also a second channel to control
 * the vibrator's rotation direction to increase effect. The following
 * numbers were determined manually. Going below 12.5 Hz means, clearly
 * noticeable pauses and at 30 Hz the vibration is just barely noticable
 * anymore.
 */
#define MAPPHONE_MIN_FREQ 125 /* 12.5 Hz */
#define MAPPHONE_MAX_FREQ 300 /* 30.0 Hz */

struct pwm_vibrator_hw {
	void (*setup_pwm)(u16 level, struct pwm_state *);
	void (*setup_pwm_dir)(u16 level, struct pwm_state *);
};

struct pwm_vibrator {
	struct input_dev *input;
	struct pwm_device *pwm;
	struct pwm_device *pwm_dir;
	struct regulator *vcc;

	struct work_struct play_work;
	u16 level;

	const struct pwm_vibrator_hw *hw;
};

static void pwm_vibrator_setup_generic(u16 level, struct pwm_state *state)
{
	/* period is configured by platform, duty cycle controls strength */
	pwm_set_relative_duty_cycle(state, level, 0xffff);
}

static void pwm_vibrator_setup_dir_generic(u16 level, struct pwm_state *state)
{
	/* period is configured by platform, duty cycle controls strength */
	pwm_set_relative_duty_cycle(state, 50, 100);
}

static struct pwm_vibrator_hw pwm_vib_hw_generic = {
	.setup_pwm = pwm_vibrator_setup_generic,
	.setup_pwm_dir = pwm_vibrator_setup_dir_generic,
};

static void pwm_vibrator_setup_mapphone(u16 level, struct pwm_state *state)
{
	unsigned int freq;

	/* convert [0, 0xffff] -> [MAPPHONE_MAX_FREQ, MAPPHONE_MIN_FREQ] */
	freq = 0xffff - level;
	freq *= MAPPHONE_MAX_FREQ - MAPPHONE_MIN_FREQ;
	freq /= 0xffff;
	freq += MAPPHONE_MIN_FREQ;

	state->period = DIV_ROUND_CLOSEST_ULL((u64) NSEC_PER_SEC * 10, freq);
	pwm_set_relative_duty_cycle(state, 50, 100);
}

static struct pwm_vibrator_hw pwm_vib_hw_mapphone = {
	.setup_pwm = pwm_vibrator_setup_mapphone,
	.setup_pwm_dir = pwm_vibrator_setup_mapphone,
};

static int pwm_vibrator_start(struct pwm_vibrator *vibrator)
{
	struct device *pdev = vibrator->input->dev.parent;
	struct pwm_state state;
	int err;

	dev_dbg(pdev, "start vibrator with level=0x%04x", vibrator->level);

	err = regulator_enable(vibrator->vcc);
	if (err) {
		dev_err(pdev, "failed to enable regulator: %d", err);
		return err;
	}

	pwm_get_state(vibrator->pwm, &state);
	state.enabled = true;

	vibrator->hw->setup_pwm(vibrator->level, &state);
	dev_dbg(pdev, "period=%u", state.period);

	err = pwm_apply_state(vibrator->pwm, &state);
	if (err) {
		dev_err(pdev, "failed to apply pwm state: %d", err);
		return err;
	}

	if (vibrator->pwm_dir) {
		pwm_get_state(vibrator->pwm_dir, &state);
		state.enabled = true;

		/* always control via period */
		vibrator->hw->setup_pwm_dir(vibrator->level, &state);

		err = pwm_apply_state(vibrator->pwm_dir, &state);
		if (err) {
			dev_err(pdev, "failed to apply dir-pwm state: %d", err);
			pwm_disable(vibrator->pwm);
			return err;
		}
	}

	return 0;
}

static void pwm_vibrator_stop(struct pwm_vibrator *vibrator)
{
	struct device *pdev = vibrator->input->dev.parent;

	dev_dbg(pdev, "stop vibrator");

	regulator_disable(vibrator->vcc);

	if (vibrator->pwm_dir)
		pwm_disable(vibrator->pwm_dir);
	pwm_disable(vibrator->pwm);
}

static void vibra_play_work(struct work_struct *work)
{
	struct pwm_vibrator *vibrator = container_of(work,
					struct pwm_vibrator, play_work);

	if (vibrator->level)
		pwm_vibrator_start(vibrator);
	else
		pwm_vibrator_stop(vibrator);
}

static int pwm_vibrator_play_effect(struct input_dev *dev, void *data,
				    struct ff_effect *effect)
{
	struct pwm_vibrator *vibrator = input_get_drvdata(dev);

	vibrator->level = effect->u.rumble.strong_magnitude;
	if (!vibrator->level)
		vibrator->level = effect->u.rumble.weak_magnitude;

	schedule_work(&vibrator->play_work);

	return 0;
}

static void pwm_vibrator_close(struct input_dev *input)
{
	struct pwm_vibrator *vibrator = input_get_drvdata(input);

	cancel_work_sync(&vibrator->play_work);
	pwm_vibrator_stop(vibrator);
}

static int pwm_vibrator_probe(struct platform_device *pdev)
{
	struct pwm_vibrator *vibrator;
	struct input_dev *input;
	struct pwm_state state;
	int err;

	vibrator = devm_kzalloc(&pdev->dev, sizeof(*vibrator), GFP_KERNEL);
	if (!vibrator)
		return -ENOMEM;

	input = devm_input_allocate_device(&pdev->dev);
	if (!input)
		return -ENOMEM;

	vibrator->input = input;

	vibrator->vcc = devm_regulator_get(&pdev->dev, "vcc");
	err = PTR_ERR_OR_ZERO(vibrator->vcc);
	if (err) {
		if (err != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Failed to request regulator: %d",
				err);
		return err;
	}

	vibrator->pwm = devm_pwm_get(&pdev->dev, "enable");
	err = PTR_ERR_OR_ZERO(vibrator->pwm);
	if (err) {
		if (err != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Failed to request main pwm: %d",
				err);
		return err;
	}

	INIT_WORK(&vibrator->play_work, vibra_play_work);

	/* Sync up PWM state and ensure it is off. */
	pwm_init_state(vibrator->pwm, &state);
	state.enabled = false;
	err = pwm_apply_state(vibrator->pwm, &state);
	if (err) {
		dev_err(&pdev->dev, "failed to apply initial PWM state: %d",
			err);
		return err;
	}

	vibrator->pwm_dir = devm_pwm_get(&pdev->dev, "direction");
	err = PTR_ERR_OR_ZERO(vibrator->pwm_dir);
	if (err == -ENODATA) {
		vibrator->pwm_dir = NULL;
	} else if (err == -EPROBE_DEFER) {
		return err;
	} else if (err) {
		dev_err(&pdev->dev, "Failed to request direction pwm: %d", err);
		return err;
	} else {
		/* Sync up PWM state and ensure it is off. */
		pwm_init_state(vibrator->pwm_dir, &state);
		state.enabled = false;
		err = pwm_apply_state(vibrator->pwm_dir, &state);
		if (err) {
			dev_err(&pdev->dev, "failed to apply initial PWM state: %d",
				err);
			return err;
		}
	}

	vibrator->hw = of_device_get_match_data(&pdev->dev);
	if (!vibrator->hw)
		vibrator->hw = &pwm_vib_hw_generic;

	input->name = "pwm-vibrator";
	input->id.bustype = BUS_HOST;
	input->dev.parent = &pdev->dev;
	input->close = pwm_vibrator_close;

	input_set_drvdata(input, vibrator);
	input_set_capability(input, EV_FF, FF_RUMBLE);

	err = input_ff_create_memless(input, NULL, pwm_vibrator_play_effect);
	if (err) {
		dev_err(&pdev->dev, "Couldn't create FF dev: %d", err);
		return err;
	}

	err = input_register_device(input);
	if (err) {
		dev_err(&pdev->dev, "Couldn't register input dev: %d", err);
		return err;
	}

	platform_set_drvdata(pdev, vibrator);

	return 0;
}

static int __maybe_unused pwm_vibrator_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct pwm_vibrator *vibrator = platform_get_drvdata(pdev);
	struct input_dev *input = vibrator->input;
	unsigned long flags;

	spin_lock_irqsave(&input->event_lock, flags);
	cancel_work_sync(&vibrator->play_work);
	if (vibrator->level)
		pwm_vibrator_stop(vibrator);
	spin_unlock_irqrestore(&input->event_lock, flags);

	return 0;
}

static int __maybe_unused pwm_vibrator_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct pwm_vibrator *vibrator = platform_get_drvdata(pdev);
	struct input_dev *input = vibrator->input;
	unsigned long flags;

	spin_lock_irqsave(&input->event_lock, flags);
	if (vibrator->level)
		pwm_vibrator_start(vibrator);
	spin_unlock_irqrestore(&input->event_lock, flags);

	return 0;
}

static SIMPLE_DEV_PM_OPS(pwm_vibrator_pm_ops,
			 pwm_vibrator_suspend, pwm_vibrator_resume);

#ifdef CONFIG_OF

#define PWM_VIB_COMPAT(of_compatible, cfg) {			\
			.compatible = of_compatible,		\
			.data = &cfg,	\
}

static const struct of_device_id pwm_vibra_dt_match_table[] = {
	PWM_VIB_COMPAT("pwm-vibrator", pwm_vib_hw_generic),
	PWM_VIB_COMPAT("motorola,mapphone-pwm-vibrator", pwm_vib_hw_mapphone),
	{},
};
MODULE_DEVICE_TABLE(of, pwm_vibra_dt_match_table);
#endif

static struct platform_driver pwm_vibrator_driver = {
	.probe	= pwm_vibrator_probe,
	.driver	= {
		.name	= "pwm-vibrator",
		.pm	= &pwm_vibrator_pm_ops,
		.of_match_table = of_match_ptr(pwm_vibra_dt_match_table),
	},
};
module_platform_driver(pwm_vibrator_driver);

MODULE_AUTHOR("Sebastian Reichel <sre@kernel.org>");
MODULE_DESCRIPTION("PWM vibrator driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:pwm-vibrator");
