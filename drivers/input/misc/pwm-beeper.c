/*
 *  Copyright (C) 2010, Lars-Peter Clausen <lars@metafoo.de>
 *
 *  Copyright (C) 2016, Frieder Schrempf <frieder.schrempf@exceet.de>
 *  (volume support)
 *
 *  PWM beeper driver
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under  the terms of the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the License, or (at your
 *  option) any later version.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <linux/input.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

struct pwm_beeper {
	struct input_dev *input;
	struct pwm_device *pwm;
	struct work_struct work;
	unsigned long period;
	unsigned int volume;
	unsigned int *volume_levels;
	unsigned int max_volume;
};

#define HZ_TO_NANOSECONDS(x) (1000000000UL/(x))

static ssize_t beeper_show_volume(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct pwm_beeper *beeper = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", beeper->volume);
}

static ssize_t beeper_show_max_volume(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct pwm_beeper *beeper = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", beeper->max_volume);
}

static ssize_t beeper_store_volume(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int rc;
	struct pwm_beeper *beeper = dev_get_drvdata(dev);
	unsigned int volume;

	rc = kstrtouint(buf, 0, &volume);
	if (rc)
		return rc;

	if (volume > beeper->max_volume)
		return -EINVAL;
	pr_debug("set volume to %u\n", volume);
	beeper->volume = volume;

	return count;
}

static DEVICE_ATTR(volume, 0644, beeper_show_volume, beeper_store_volume);
static DEVICE_ATTR(max_volume, 0644, beeper_show_max_volume, NULL);

static struct attribute *pwm_beeper_attributes[] = {
	&dev_attr_volume.attr,
	&dev_attr_max_volume.attr,
	NULL,
};

static struct attribute_group pwm_beeper_attribute_group = {
	.attrs = pwm_beeper_attributes,
};

static void __pwm_beeper_set(struct pwm_beeper *beeper)
{
	unsigned long period = beeper->period;

	if (period) {
		pwm_config(beeper->pwm,
			period / 1000 * beeper->volume_levels[beeper->volume],
			period);
		pwm_enable(beeper->pwm);
	} else
		pwm_disable(beeper->pwm);
}

static void pwm_beeper_work(struct work_struct *work)
{
	struct pwm_beeper *beeper =
		container_of(work, struct pwm_beeper, work);

	__pwm_beeper_set(beeper);
}

static int pwm_beeper_event(struct input_dev *input,
			    unsigned int type, unsigned int code, int value)
{
	struct pwm_beeper *beeper = input_get_drvdata(input);

	if (type != EV_SND || value < 0)
		return -EINVAL;

	switch (code) {
	case SND_BELL:
		value = value ? 1000 : 0;
		break;
	case SND_TONE:
		break;
	default:
		return -EINVAL;
	}

	if (value == 0)
		beeper->period = 0;
	else
		beeper->period = HZ_TO_NANOSECONDS(value);

	schedule_work(&beeper->work);

	return 0;
}

static void pwm_beeper_stop(struct pwm_beeper *beeper)
{
	cancel_work_sync(&beeper->work);

	if (beeper->period)
		pwm_disable(beeper->pwm);
}

static void pwm_beeper_close(struct input_dev *input)
{
	struct pwm_beeper *beeper = input_get_drvdata(input);

	pwm_beeper_stop(beeper);
}

static int pwm_beeper_probe(struct platform_device *pdev)
{
	unsigned long pwm_id = (unsigned long)dev_get_platdata(&pdev->dev);
	struct pwm_beeper *beeper;
	int error, length;
	size_t size;
	u32 value;

	beeper = kzalloc(sizeof(*beeper), GFP_KERNEL);
	if (!beeper)
		return -ENOMEM;

	beeper->pwm = pwm_get(&pdev->dev, NULL);
	if (IS_ERR(beeper->pwm)) {
		dev_dbg(&pdev->dev, "unable to request PWM, trying legacy API\n");
		beeper->pwm = pwm_request(pwm_id, "pwm beeper");
	}

	if (IS_ERR(beeper->pwm)) {
		error = PTR_ERR(beeper->pwm);
		dev_err(&pdev->dev, "Failed to request pwm device: %d\n", error);
		goto err_free;
	}

	/*
	 * FIXME: pwm_apply_args() should be removed when switching to
	 * the atomic PWM API.
	 */
	pwm_apply_args(beeper->pwm);

	INIT_WORK(&beeper->work, pwm_beeper_work);

	/* determine the number of volume levels */
	length = device_property_read_u32_array(&pdev->dev, "volume-levels", NULL, 0);
	if (length <= 0) {
		dev_dbg(&pdev->dev, "no volume levels specified, using max volume\n");
		beeper->max_volume = 1;
	} else
		beeper->max_volume = length;

	/* read volume levels from DT property */
	if (beeper->max_volume > 0) {
		size = sizeof(*beeper->volume_levels) *	beeper->max_volume;

		beeper->volume_levels = devm_kzalloc(&(pdev->dev), size,
			GFP_KERNEL);
		if (!beeper->volume_levels)
			return -ENOMEM;

		if (length > 0) {
			error = device_property_read_u32_array(&pdev->dev, "volume-levels",
						beeper->volume_levels,
						beeper->max_volume);

			if (error < 0)
				return error;

			error = device_property_read_u32(&pdev->dev, "default-volume-level",
						   &value);

			if (error < 0) {
				dev_dbg(&pdev->dev, "no default volume specified, using max volume\n");
				value = beeper->max_volume - 1;
			}
		} else {
			beeper->volume_levels[0] = 500;
			value = 0;
		}

		beeper->volume = value;
		beeper->max_volume--;
	}

	beeper->input = input_allocate_device();
	if (!beeper->input) {
		dev_err(&pdev->dev, "Failed to allocate input device\n");
		error = -ENOMEM;
		goto err_pwm_free;
	}
	beeper->input->dev.parent = &pdev->dev;

	beeper->input->name = "pwm-beeper";
	beeper->input->phys = "pwm/input0";
	beeper->input->id.bustype = BUS_HOST;
	beeper->input->id.vendor = 0x001f;
	beeper->input->id.product = 0x0001;
	beeper->input->id.version = 0x0100;

	beeper->input->evbit[0] = BIT(EV_SND);
	beeper->input->sndbit[0] = BIT(SND_TONE) | BIT(SND_BELL);

	beeper->input->event = pwm_beeper_event;
	beeper->input->close = pwm_beeper_close;

	input_set_drvdata(beeper->input, beeper);

	error = sysfs_create_group(&pdev->dev.kobj, &pwm_beeper_attribute_group);
	if (error) {
		dev_err(&pdev->dev, "Failed to create sysfs group: %d\n", error);
		goto err_pwm_free;
	}

	error = input_register_device(beeper->input);
	if (error) {
		dev_err(&pdev->dev, "Failed to register input device: %d\n", error);
		goto err_input_free;
	}

	platform_set_drvdata(pdev, beeper);

	return 0;

err_input_free:
	input_free_device(beeper->input);
err_pwm_free:
	pwm_free(beeper->pwm);
err_free:
	kfree(beeper);

	return error;
}

static int pwm_beeper_remove(struct platform_device *pdev)
{
	struct pwm_beeper *beeper = platform_get_drvdata(pdev);

	input_unregister_device(beeper->input);

	pwm_free(beeper->pwm);

	kfree(beeper);

	return 0;
}

static int __maybe_unused pwm_beeper_suspend(struct device *dev)
{
	struct pwm_beeper *beeper = dev_get_drvdata(dev);

	pwm_beeper_stop(beeper);

	return 0;
}

static int __maybe_unused pwm_beeper_resume(struct device *dev)
{
	struct pwm_beeper *beeper = dev_get_drvdata(dev);

	if (beeper->period)
		__pwm_beeper_set(beeper);

	return 0;
}

static SIMPLE_DEV_PM_OPS(pwm_beeper_pm_ops,
			 pwm_beeper_suspend, pwm_beeper_resume);

#ifdef CONFIG_OF
static const struct of_device_id pwm_beeper_match[] = {
	{ .compatible = "pwm-beeper", },
	{ },
};
MODULE_DEVICE_TABLE(of, pwm_beeper_match);
#endif

static struct platform_driver pwm_beeper_driver = {
	.probe	= pwm_beeper_probe,
	.remove = pwm_beeper_remove,
	.driver = {
		.name	= "pwm-beeper",
		.pm	= &pwm_beeper_pm_ops,
		.of_match_table = of_match_ptr(pwm_beeper_match),
	},
};
module_platform_driver(pwm_beeper_driver);

MODULE_AUTHOR("Lars-Peter Clausen <lars@metafoo.de>");
MODULE_DESCRIPTION("PWM beeper driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:pwm-beeper");
