/*
 * Driver for an envelope detector using a DAC and a comparator
 *
 * Copyright (C) 2016 Axentia Technologies AB
 *
 * Author: Peter Rosin <peda@axentia.se>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * The DAC is used to find the peak level of an alternating voltage input
 * signal by a binary search using the output of a comparator wired to
 * an interrupt pin. Like so:
 *                           _
 *                          | \
 *     input +------>-------|+ \
 *                          |   \
 *            .-------.     |    }---.
 *            |       |     |   /    |
 *            |    dac|-->--|- /     |
 *            |       |     |_/      |
 *            |       |              |
 *            |       |              |
 *            |    irq|------<-------'
 *            |       |
 *            '-------'
 */

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/iio/consumer.h>
#include <linux/iio/iio.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

struct envelope {
	struct iio_channel *dac;
	struct delayed_work comp_timeout;
	int comp_irq;

	spinlock_t comp_lock; /* protects comp */
	int comp;

	struct mutex read_lock; /* protects everything below */

	u32 dac_max;
	u32 comp_interval;
	bool invert;

	int high;
	int level;
	int low;

	struct completion done;
};

static int envelope_detector_latch(struct envelope *env)
{
	int comp;

	spin_lock_irq(&env->comp_lock);
	comp = env->comp;
	env->comp = 0;
	spin_unlock_irq(&env->comp_lock);

	if (comp)
		enable_irq(env->comp_irq);

	return comp;
}

static irqreturn_t envelope_detector_isr(int irq, void *ctx)
{
	struct envelope *env = ctx;

	spin_lock(&env->comp_lock);
	env->comp = 1;
	disable_irq_nosync(env->comp_irq);
	spin_unlock(&env->comp_lock);

	return IRQ_HANDLED;
}

static void envelope_detector_setup_compare(struct envelope *env)
{
	int ret;

	/*
	 * Do a binary search for the peak input level, and stop
	 * when that level is "trapped" between two adjacent DAC
	 * values.
	 * When invert is active, use the midpoint floor so that
	 * env->level ends up as env->low when the termination
	 * criteria below is fulfilled, and use the midpoint
	 * ceiling when invert is not active so that env->level
	 * ends up as env->high in that case.
	 */
	env->level = (env->high + env->low + !env->invert) / 2;

	if (env->high == env->low + 1) {
		complete(&env->done);
		return;
	}

	/* Set a "safe" DAC level (if there is such a thing)... */
	ret = iio_write_channel_raw(env->dac, env->invert ? 0 : env->dac_max);
	if (ret < 0)
		goto err;

	/* ...clear the comparison result... */
	envelope_detector_latch(env);

	/* ...set the real DAC level... */
	ret = iio_write_channel_raw(env->dac, env->level);
	if (ret < 0)
		goto err;

	/* ...and wait for a bit to see if the latch catches anything. */
	schedule_delayed_work(&env->comp_timeout,
			      msecs_to_jiffies(env->comp_interval));
	return;

err:
	env->level = ret;
	complete(&env->done);
}

static void envelope_detector_timeout(struct work_struct *work)
{
	struct envelope *env = container_of(work, struct envelope,
					    comp_timeout.work);

	/* Adjust low/high depending on the latch content... */
	if (!envelope_detector_latch(env) ^ !env->invert)
		env->low = env->level;
	else
		env->high = env->level;

	/* ...and continue the search. */
	envelope_detector_setup_compare(env);
}

static int envelope_detector_read_raw(struct iio_dev *indio_dev,
				      struct iio_chan_spec const *chan,
				      int *val, int *val2, long mask)
{
	struct envelope *env = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		/*
		 * When invert is active, start with high=max+1 and low=0
		 * since we will end up with the low value when the
		 * termination criteria is fulfilled (rounding down). And
		 * start with high=max and low=-1 when invert is not active
		 * since we will end up with the high value in that case.
		 * This ensures that we in both cases return a value in the
		 * same range as the DAC and that as not triggered the
		 * comparator.
		 */
		mutex_lock(&env->read_lock);
		env->high = env->dac_max + 1 - !env->invert;
		env->low = 0 - !env->invert;
		envelope_detector_setup_compare(env);
		wait_for_completion(&env->done);
		if (env->level < 0) {
			ret = env->level;
			goto err_unlock;
		}
		*val = env->invert ? env->dac_max - env->level : env->level;
		mutex_unlock(&env->read_lock);

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		return iio_read_channel_scale(env->dac, val, val2);
	}

	return -EINVAL;

err_unlock:
	mutex_unlock(&env->read_lock);
	return ret;
}

static const struct iio_chan_spec envelope_detector_iio_channel = {
	.type = IIO_ALTVOLTAGE,
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW)
			    | BIT(IIO_CHAN_INFO_SCALE),
	.output = 1,
};

static const struct iio_info envelope_detector_info = {
	.read_raw = &envelope_detector_read_raw,
	.driver_module = THIS_MODULE,
};

static int envelope_detector_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iio_dev *indio_dev;
	struct envelope *env;
	enum iio_chan_type type;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*env));
	if (!indio_dev)
		return -ENOMEM;

	platform_set_drvdata(pdev, indio_dev);
	env = iio_priv(indio_dev);

	spin_lock_init(&env->comp_lock);
	mutex_init(&env->read_lock);
	init_completion(&env->done);
	INIT_DELAYED_WORK(&env->comp_timeout, envelope_detector_timeout);

	indio_dev->name = dev_name(dev);
	indio_dev->dev.parent = dev;
	indio_dev->dev.of_node = dev->of_node;
	indio_dev->info = &envelope_detector_info;
	indio_dev->channels = &envelope_detector_iio_channel;
	indio_dev->num_channels = 1;

	env->dac = devm_iio_channel_get(dev, "dac");
	if (IS_ERR(env->dac)) {
		if (PTR_ERR(env->dac) != -EPROBE_DEFER)
			dev_err(dev, "failed to get dac input channel\n");
		return PTR_ERR(env->dac);
	}

	env->comp_irq = platform_get_irq_byname(pdev, "comp");
	if (env->comp_irq < 0) {
		if (env->comp_irq != -EPROBE_DEFER)
			dev_err(dev, "failed to get compare interrupt\n");
		return env->comp_irq;
	}

	ret = devm_request_irq(dev, env->comp_irq, envelope_detector_isr, 0,
			       "env-env-dac-comp", env);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to request interrupt\n");
		return ret;
	}

	ret = iio_get_channel_type(env->dac, &type);
	if (ret < 0)
		return ret;

	if (type != IIO_VOLTAGE) {
		dev_err(dev, "dac is of the wrong type\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(dev->of_node, "envelope-detector,dac-max",
				   &env->dac_max);
	if (ret) {
		dev_err(dev, "the dac-max property is missing\n");
		return ret;
	}

	ret = of_property_read_u32(dev->of_node,
				   "envelope-detector,comp-interval-ms",
				   &env->comp_interval);
	if (ret) {
		dev_err(dev, "the comp-interval-ms property is missing\n");
		return ret;
	}

	env->invert = of_property_read_bool(dev->of_node,
					    "envelope-detector,inverted");

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id envelope_detector_match[] = {
	{ .compatible = "envelope-detector" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, envelope_detector_match);

static struct platform_driver envelope_detector_driver = {
	.probe = envelope_detector_probe,
	.driver = {
		.name = "iio-envelope-detector",
		.of_match_table = envelope_detector_match,
	},
};
module_platform_driver(envelope_detector_driver);

MODULE_DESCRIPTION("Envelope detector using a DAC and a comparator");
MODULE_AUTHOR("Peter Rosin <peda@axentia.se>");
MODULE_LICENSE("GPL v2");
