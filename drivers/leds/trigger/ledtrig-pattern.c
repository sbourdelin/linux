// SPDX-License-Identifier: GPL-2.0

/*
 * LED pattern trigger
 *
 * Idea discussed with Pavel Machek. Raphael Teysseyre implemented
 * the first version, Baolin Wang simplified and improved the approach.
 */

#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/timer.h>

#define MAX_PATTERNS		1024
#define PATTERN_SEPARATOR	","

struct pattern_trig_data {
	struct led_classdev *led_cdev;
	struct led_pattern patterns[MAX_PATTERNS];
	struct led_pattern *curr;
	struct led_pattern *next;
	struct mutex lock;
	u32 npatterns;
	u32 repeat;
	bool is_indefinite;
	bool hardware_pattern;
	struct timer_list timer;
};

static void pattern_trig_update_patterns(struct pattern_trig_data *data)
{
	data->curr = data->next;
	if (!data->is_indefinite && data->curr == data->patterns)
		data->repeat--;

	if (data->next == data->patterns + data->npatterns - 1)
		data->next = data->patterns;
	else
		data->next++;
}

static void pattern_trig_timer_function(struct timer_list *t)
{
	struct pattern_trig_data *data = from_timer(data, t, timer);

	mutex_lock(&data->lock);

	if (!data->is_indefinite && !data->repeat) {
		mutex_unlock(&data->lock);
		return;
	}

	led_set_brightness(data->led_cdev, data->curr->brightness);
	mod_timer(&data->timer, jiffies + msecs_to_jiffies(data->curr->delta_t));
	pattern_trig_update_patterns(data);

	mutex_unlock(&data->lock);
}

static int pattern_trig_start_pattern(struct pattern_trig_data *data,
				      struct led_classdev *led_cdev)
{
	if (!data->npatterns)
		return 0;

	if (data->hardware_pattern) {
		return led_cdev->pattern_set(led_cdev, data->patterns,
					     data->npatterns, data->repeat);
	}

	data->curr = data->patterns;
	data->next = data->patterns + 1;
	data->timer.expires = jiffies;
	add_timer(&data->timer);

	return 0;
}

static ssize_t pattern_trig_show_repeat(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct pattern_trig_data *data = led_cdev->trigger_data;
	u32 repeat;

	mutex_lock(&data->lock);

	repeat = data->repeat;

	mutex_unlock(&data->lock);

	return scnprintf(buf, PAGE_SIZE, "%u\n", repeat);
}

static ssize_t pattern_trig_store_repeat(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct pattern_trig_data *data = led_cdev->trigger_data;
	unsigned long res;
	int err;

	err = kstrtoul(buf, 10, &res);
	if (err)
		return err;

	if (!data->hardware_pattern)
		del_timer_sync(&data->timer);

	mutex_lock(&data->lock);

	data->repeat = res;

	/* 0 means repeat indefinitely */
	if (!data->repeat)
		data->is_indefinite = true;
	else
		data->is_indefinite = false;

	err = pattern_trig_start_pattern(data, led_cdev);

	mutex_unlock(&data->lock);
	return err < 0 ? err : count;;
}

static DEVICE_ATTR(repeat, 0644, pattern_trig_show_repeat,
		   pattern_trig_store_repeat);

static ssize_t pattern_trig_show_pattern(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct pattern_trig_data *data = led_cdev->trigger_data;
	ssize_t count = 0;
	int i;

	mutex_lock(&data->lock);

	if (!data->npatterns)
		goto out;

	for (i = 0; i < data->npatterns; i++) {
		count += scnprintf(buf + count, PAGE_SIZE - count,
				   "%d %d" PATTERN_SEPARATOR,
				   data->patterns[i].brightness,
				   data->patterns[i].delta_t);
	}

	buf[count - 1] = '\n';

out:
	mutex_unlock(&data->lock);
	return count;
}

static ssize_t pattern_trig_store_pattern(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct pattern_trig_data *data = led_cdev->trigger_data;
	int cr, ccount, offset = 0, err = 0;

	if (!data->hardware_pattern)
		del_timer_sync(&data->timer);

	mutex_lock(&data->lock);

	data->npatterns = 0;
	while (offset < count - 1 && data->npatterns < MAX_PATTERNS) {
		cr = 0;
		ccount = sscanf(buf + offset, "%d %d " PATTERN_SEPARATOR "%n",
				&data->patterns[data->npatterns].brightness,
				&data->patterns[data->npatterns].delta_t, &cr);
		if (ccount != 2) {
			err = -EINVAL;
			goto out;
		}

		offset += cr;
		data->npatterns++;
		/* end of pattern */
		if (!cr)
			break;
	}

	err = pattern_trig_start_pattern(data, led_cdev);

out:
	mutex_unlock(&data->lock);
	return err < 0 ? err : count;
}

static DEVICE_ATTR(pattern, 0644, pattern_trig_show_pattern,
		   pattern_trig_store_pattern);

static struct attribute *pattern_trig_attrs[] = {
	&dev_attr_pattern.attr,
	&dev_attr_repeat.attr,
	NULL
};
ATTRIBUTE_GROUPS(pattern_trig);

static int pattern_trig_activate(struct led_classdev *led_cdev)
{
	struct pattern_trig_data *data;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	if (led_cdev->pattern_set && led_cdev->pattern_clear)
		data->hardware_pattern = true;
	else
		data->hardware_pattern = false;

	data->is_indefinite = true;
	mutex_init(&data->lock);
	data->led_cdev = led_cdev;
	led_set_trigger_data(led_cdev, data);
	timer_setup(&data->timer, pattern_trig_timer_function, 0);
	led_cdev->activated = true;

	return 0;
}

static void pattern_trig_deactivate(struct led_classdev *led_cdev)
{
	struct pattern_trig_data *data = led_cdev->trigger_data;

	if (!led_cdev->activated)
		return;

	if (data->hardware_pattern)
		led_cdev->pattern_clear(led_cdev);
	else
		del_timer_sync(&data->timer);

	led_set_brightness(led_cdev, LED_OFF);
	kfree(data);
	led_cdev->activated = false;
}

static struct led_trigger pattern_led_trigger = {
	.name = "pattern",
	.activate = pattern_trig_activate,
	.deactivate = pattern_trig_deactivate,
	.groups = pattern_trig_groups,
};

static int __init pattern_trig_init(void)
{
	return led_trigger_register(&pattern_led_trigger);
}

static void __exit pattern_trig_exit(void)
{
	led_trigger_unregister(&pattern_led_trigger);
}

module_init(pattern_trig_init);
module_exit(pattern_trig_exit);

MODULE_AUTHOR("Raphael Teysseyre <rteysseyre@gmail.com");
MODULE_AUTHOR("Baolin Wang <baolin.wang@linaro.org");
MODULE_DESCRIPTION("LED Pattern trigger");
MODULE_LICENSE("GPL v2");
