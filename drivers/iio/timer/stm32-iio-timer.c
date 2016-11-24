/*
 * stm32-iio-timer.c
 *
 * Copyright (C) STMicroelectronics 2016
 * Author: Benjamin Gaignard <benjamin.gaignard@st.com> for STMicroelectronics.
 * License terms:  GNU General Public License (GPL), version 2
 */

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/timer/stm32-iio-timers.h>
#include <linux/iio/trigger.h>
#include <linux/iio/triggered_event.h>
#include <linux/interrupt.h>
#include <linux/mfd/stm32-gptimer.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define DRIVER_NAME "stm32-iio-timer"

struct stm32_iio_timer_dev {
	struct device *dev;
	struct regmap *regmap;
	struct clk *clk;
	int irq;
	bool own_timer;
	unsigned int sampling_frequency;
	struct iio_trigger *active_trigger;
};

static ssize_t _store_frequency(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
	struct iio_trigger *trig = to_iio_trigger(dev);
	struct stm32_iio_timer_dev *stm32 = iio_trigger_get_drvdata(trig);
	unsigned int freq;
	int ret;

	ret = kstrtouint(buf, 10, &freq);
	if (ret)
		return ret;

	stm32->sampling_frequency = freq;

	return len;
}

static ssize_t _read_frequency(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct iio_trigger *trig = to_iio_trigger(dev);
	struct stm32_iio_timer_dev *stm32 = iio_trigger_get_drvdata(trig);
	unsigned long long freq = stm32->sampling_frequency;
	u32 psc, arr, cr1;

	regmap_read(stm32->regmap, TIM_CR1, &cr1);
	regmap_read(stm32->regmap, TIM_PSC, &psc);
	regmap_read(stm32->regmap, TIM_ARR, &arr);

	if (psc && arr && (cr1 & TIM_CR1_CEN)) {
		freq = (unsigned long long)clk_get_rate(stm32->clk);
		do_div(freq, psc);
		do_div(freq, arr);
	}

	return sprintf(buf, "%d\n", (unsigned int)freq);
}

static IIO_DEV_ATTR_SAMP_FREQ(S_IWUSR | S_IRUGO,
			      _read_frequency,
			      _store_frequency);

static struct attribute *stm32_trigger_attrs[] = {
	&iio_dev_attr_sampling_frequency.dev_attr.attr,
	NULL,
};

static const struct attribute_group stm32_trigger_attr_group = {
	.attrs = stm32_trigger_attrs,
};

static const struct attribute_group *stm32_trigger_attr_groups[] = {
	&stm32_trigger_attr_group,
	NULL,
};

static
ssize_t _show_master_mode(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct stm32_iio_timer_dev *stm32 = iio_priv(indio_dev);
	u32 cr2;

	regmap_read(stm32->regmap, TIM_CR2, &cr2);

	return snprintf(buf, PAGE_SIZE, "%d\n", (cr2 >> 4) & 0x7);
}

static
ssize_t _store_master_mode(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct stm32_iio_timer_dev *stm32 = iio_priv(indio_dev);
	u8 mode;
	int ret;

	ret = kstrtou8(buf, 10, &mode);
	if (ret)
		return ret;

	if (mode > 0x7)
		return -EINVAL;

	regmap_update_bits(stm32->regmap, TIM_CR2, TIM_CR2_MMS, mode << 4);

	return len;
}

static IIO_DEVICE_ATTR(master_mode, S_IRUGO | S_IWUSR,
		       _show_master_mode,
		       _store_master_mode,
		       0);

static
ssize_t _show_slave_mode(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct stm32_iio_timer_dev *stm32 = iio_priv(indio_dev);
	u32 smcr;

	regmap_read(stm32->regmap, TIM_SMCR, &smcr);

	return snprintf(buf, PAGE_SIZE, "%d\n", smcr & 0x3);
}

static
ssize_t _store_slave_mode(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct stm32_iio_timer_dev *stm32 = iio_priv(indio_dev);
	u8 mode;
	int ret;

	ret = kstrtou8(buf, 10, &mode);
	if (ret)
		return ret;

	if (mode > 0x7)
		return -EINVAL;

	regmap_update_bits(stm32->regmap, TIM_SMCR, TIM_SMCR_SMS, mode);

	return len;
}

static IIO_DEVICE_ATTR(slave_mode, S_IRUGO | S_IWUSR,
		       _show_slave_mode,
		       _store_slave_mode,
		       0);

static struct attribute *stm32_timer_attrs[] = {
	&iio_dev_attr_master_mode.dev_attr.attr,
	&iio_dev_attr_slave_mode.dev_attr.attr,
	NULL,
};

static const struct attribute_group stm32_timer_attr_group = {
	.attrs = stm32_timer_attrs,
};

static int stm32_timer_start(struct stm32_iio_timer_dev *stm32)
{
	unsigned long long prd, div;
	int prescaler = 0;
	u32 max_arr = 0xFFFF, cr1;

	if (stm32->sampling_frequency == 0)
		return 0;

	/* Period and prescaler values depends of clock rate */
	div = (unsigned long long)clk_get_rate(stm32->clk);

	do_div(div, stm32->sampling_frequency);

	prd = div;

	while (div > max_arr) {
		prescaler++;
		div = prd;
		do_div(div, (prescaler + 1));
	}
	prd = div;

	if (prescaler > MAX_TIM_PSC) {
		dev_err(stm32->dev, "prescaler exceeds the maximum value\n");
		return -EINVAL;
	}

	/* Check that we own the timer */
	regmap_read(stm32->regmap, TIM_CR1, &cr1);
	if ((cr1 & TIM_CR1_CEN) && !stm32->own_timer)
		return -EBUSY;

	if (!stm32->own_timer) {
		stm32->own_timer = true;
		clk_enable(stm32->clk);
	}

	regmap_write(stm32->regmap, TIM_PSC, prescaler);
	regmap_write(stm32->regmap, TIM_ARR, prd - 1);
	regmap_update_bits(stm32->regmap, TIM_CR1, TIM_CR1_ARPE, TIM_CR1_ARPE);

	/* Force master mode to update mode */
	regmap_update_bits(stm32->regmap, TIM_CR2, TIM_CR2_MMS, 0x20);

	/* Make sure that registers are updated */
	regmap_update_bits(stm32->regmap, TIM_EGR, TIM_EGR_UG, TIM_EGR_UG);

	/* Enable interrupt */
	regmap_write(stm32->regmap, TIM_SR, 0);
	regmap_update_bits(stm32->regmap, TIM_DIER, TIM_DIER_UIE, TIM_DIER_UIE);

	/* Enable controller */
	regmap_update_bits(stm32->regmap, TIM_CR1, TIM_CR1_CEN, TIM_CR1_CEN);

	return 0;
}

static int stm32_timer_stop(struct stm32_iio_timer_dev *stm32)
{
	if (!stm32->own_timer)
		return 0;

	/* Stop timer */
	regmap_update_bits(stm32->regmap, TIM_DIER, TIM_DIER_UIE, 0);
	regmap_update_bits(stm32->regmap, TIM_CR1, TIM_CR1_CEN, 0);
	regmap_write(stm32->regmap, TIM_PSC, 0);
	regmap_write(stm32->regmap, TIM_ARR, 0);

	clk_disable(stm32->clk);

	stm32->own_timer = false;
	stm32->active_trigger = NULL;

	return 0;
}

static int stm32_set_trigger_state(struct iio_trigger *trig, bool state)
{
	struct stm32_iio_timer_dev *stm32 = iio_trigger_get_drvdata(trig);

	stm32->active_trigger = trig;

	if (state)
		return stm32_timer_start(stm32);
	else
		return stm32_timer_stop(stm32);
}

static irqreturn_t stm32_timer_irq_handler(int irq, void *private)
{
	struct stm32_iio_timer_dev *stm32 = private;
	u32 sr;

	regmap_read(stm32->regmap, TIM_SR, &sr);
	regmap_write(stm32->regmap, TIM_SR, 0);

	if ((sr & TIM_SR_UIF) && stm32->active_trigger)
		iio_trigger_poll(stm32->active_trigger);

	return IRQ_HANDLED;
}

static const struct iio_trigger_ops timer_trigger_ops = {
	.owner = THIS_MODULE,
	.set_trigger_state = stm32_set_trigger_state,
};

static int stm32_setup_iio_triggers(struct stm32_iio_timer_dev *stm32)
{
	int ret;
	struct property *p;
	const char *cur = NULL;

	p = of_find_property(stm32->dev->of_node,
			     "st,output-triggers-names", NULL);

	while ((cur = of_prop_next_string(p, cur)) != NULL) {
		struct iio_trigger *trig;

		trig = devm_iio_trigger_alloc(stm32->dev, "%s", cur);
		if  (!trig)
			return -ENOMEM;

		trig->dev.parent = stm32->dev->parent;
		trig->ops = &timer_trigger_ops;
		trig->dev.groups = stm32_trigger_attr_groups;
		iio_trigger_set_drvdata(trig, stm32);

		ret = devm_iio_trigger_register(stm32->dev, trig);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * is_stm32_iio_timer_trigger
 * @trig: trigger to be checked
 *
 * return true if the trigger is a valid stm32 iio timer trigger
 * either return false
 */
bool is_stm32_iio_timer_trigger(struct iio_trigger *trig)
{
	return (trig->ops == &timer_trigger_ops);
}
EXPORT_SYMBOL(is_stm32_iio_timer_trigger);

static int stm32_validate_trigger(struct iio_dev *indio_dev,
				  struct iio_trigger *trig)
{
	struct stm32_iio_timer_dev *dev = iio_priv(indio_dev);
	int ret;

	if (!is_stm32_iio_timer_trigger(trig))
		return -EINVAL;

	ret = of_property_match_string(dev->dev->of_node,
				       "st,input-triggers-names",
				       trig->name);

	if (ret < 0)
		return ret;

	regmap_update_bits(dev->regmap, TIM_SMCR, TIM_SMCR_TS, ret << 4);

	return 0;
}

static const struct iio_info stm32_trigger_info = {
	.driver_module = THIS_MODULE,
	.validate_trigger = stm32_validate_trigger,
	.attrs = &stm32_timer_attr_group,
};

static struct stm32_iio_timer_dev *stm32_setup_iio_device(struct device *dev)
{
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(struct stm32_iio_timer_dev));
	if (!indio_dev)
		return NULL;

	indio_dev->name = dev_name(dev);
	indio_dev->dev.parent = dev;
	indio_dev->info = &stm32_trigger_info;
	indio_dev->modes = INDIO_EVENT_TRIGGERED;
	indio_dev->num_channels = 0;
	indio_dev->dev.of_node = dev->of_node;

	ret = iio_triggered_event_setup(indio_dev,
					NULL,
					stm32_timer_irq_handler);
	if (ret)
		return NULL;

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret) {
		iio_triggered_event_cleanup(indio_dev);
		return NULL;
	}

	return iio_priv(indio_dev);
}

static int stm32_iio_timer_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct stm32_iio_timer_dev *stm32;
	struct stm32_gptimer_dev *mfd = dev_get_drvdata(pdev->dev.parent);
	int ret;

	stm32 = stm32_setup_iio_device(dev);
	if (!stm32)
		return -ENOMEM;

	stm32->dev = dev;
	stm32->regmap = mfd->regmap;
	stm32->clk = mfd->clk;

	stm32->irq = platform_get_irq(pdev, 0);
	if (stm32->irq < 0)
		return -EINVAL;

	ret = devm_request_irq(stm32->dev, stm32->irq,
			       stm32_timer_irq_handler, IRQF_SHARED,
			       "iiotimer_event", stm32);
	if (ret)
		return ret;

	ret = stm32_setup_iio_triggers(stm32);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, stm32);

	return 0;
}

static int stm32_iio_timer_remove(struct platform_device *pdev)
{
	struct stm32_iio_timer_dev *stm32 = platform_get_drvdata(pdev);

	iio_triggered_event_cleanup((struct iio_dev *)stm32);

	return 0;
}

static const struct of_device_id stm32_trig_of_match[] = {
	{
		.compatible = "st,stm32-iio-timer",
	},
};
MODULE_DEVICE_TABLE(of, stm32_trig_of_match);

static struct platform_driver stm32_iio_timer_driver = {
	.probe = stm32_iio_timer_probe,
	.remove = stm32_iio_timer_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = stm32_trig_of_match,
	},
};
module_platform_driver(stm32_iio_timer_driver);

MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_DESCRIPTION("STMicroelectronics STM32 iio timer driver");
MODULE_LICENSE("GPL");
