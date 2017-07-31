/*
 * Industrial I/O counter interface functions
 * Copyright (C) 2017 William Breathitt Gray
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/err.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linux/iio/iio.h>
#include <linux/iio/types.h>

#include <linux/iio/counter.h>

static struct iio_counter_signal *__iio_counter_signal_find_by_id(
	const struct iio_counter *const counter, const int id)
{
	struct iio_counter_signal *iter;

	list_for_each_entry(iter, &counter->signal_list, list)
		if (iter->id == id)
			return iter;

	return NULL;
}

static struct iio_counter_trigger *__iio_counter_trigger_find_by_id(
	const struct iio_counter_value *const value, const int id)
{
	struct iio_counter_trigger *iter;

	list_for_each_entry(iter, &value->trigger_list, list)
		if (iter->signal->id == id)
			return iter;

	return NULL;
}

static struct iio_counter_value *__iio_counter_value_find_by_id(
	const struct iio_counter *const counter, const int id)
{
	struct iio_counter_value *iter;

	list_for_each_entry(iter, &counter->value_list, list)
		if (iter->id == id)
			return iter;

	return NULL;
}

static void __iio_counter_trigger_unregister_all(
	struct iio_counter_value *const value)
{
	struct iio_counter_trigger *iter, *tmp_iter;

	mutex_lock(&value->trigger_list_lock);
	list_for_each_entry_safe(iter, tmp_iter, &value->trigger_list, list)
		list_del(&iter->list);
	mutex_unlock(&value->trigger_list_lock);
}

static void __iio_counter_signal_unregister_all(
	struct iio_counter *const counter)
{
	struct iio_counter_signal *iter, *tmp_iter;

	mutex_lock(&counter->signal_list_lock);
	list_for_each_entry_safe(iter, tmp_iter, &counter->signal_list, list)
		list_del(&iter->list);
	mutex_unlock(&counter->signal_list_lock);
}

static void __iio_counter_value_unregister_all(
	struct iio_counter *const counter)
{
	struct iio_counter_value *iter, *tmp_iter;

	mutex_lock(&counter->value_list_lock);
	list_for_each_entry_safe(iter, tmp_iter, &counter->value_list, list) {
		__iio_counter_trigger_unregister_all(iter);

		list_del(&iter->list);
	}
	mutex_unlock(&counter->value_list_lock);
}

static ssize_t __iio_counter_signal_name_read(struct iio_dev *indio_dev,
	uintptr_t priv, const struct iio_chan_spec *chan, char *buf)
{
	struct iio_counter *const counter = iio_priv(indio_dev);
	const struct iio_counter_signal *signal;

	mutex_lock(&counter->signal_list_lock);
	signal = __iio_counter_signal_find_by_id(counter, chan->channel2);
	mutex_unlock(&counter->signal_list_lock);
	if (!signal)
		return -EINVAL;

	return scnprintf(buf, PAGE_SIZE, "%s\n", signal->name);
}

static ssize_t __iio_counter_value_name_read(struct iio_dev *indio_dev,
	uintptr_t priv, const struct iio_chan_spec *chan, char *buf)
{
	struct iio_counter *const counter = iio_priv(indio_dev);
	const struct iio_counter_value *value;

	mutex_lock(&counter->value_list_lock);
	value = __iio_counter_value_find_by_id(counter, chan->channel2);
	mutex_unlock(&counter->value_list_lock);
	if (!value)
		return -EINVAL;

	return scnprintf(buf, PAGE_SIZE, "%s\n", value->name);
}

static ssize_t __iio_counter_value_triggers_read(struct iio_dev *indio_dev,
	uintptr_t priv, const struct iio_chan_spec *chan, char *buf)
{
	struct iio_counter *const counter = iio_priv(indio_dev);
	struct iio_counter_value *value;
	const struct iio_counter_trigger *trigger;
	ssize_t len = 0;

	mutex_lock(&counter->value_list_lock);
	value = __iio_counter_value_find_by_id(counter, chan->channel2);
	if (!value) {
		len = -EINVAL;
		goto err_find_value;
	}

	mutex_lock(&value->trigger_list_lock);
	list_for_each_entry(trigger, &value->trigger_list, list) {
		len += snprintf(buf + len, PAGE_SIZE - len, "%d\t%s\t%s\n",
			trigger->signal->id, trigger->signal->name,
			trigger->trigger_modes[trigger->mode]);
		if (len >= PAGE_SIZE) {
			len = -ENOMEM;
			goto err_no_buffer_space;
		}
	}
err_no_buffer_space:
	mutex_unlock(&value->trigger_list_lock);

err_find_value:
	mutex_unlock(&counter->value_list_lock);

	return len;
}

static ssize_t __iio_counter_trigger_mode_read(struct iio_dev *indio_dev,
	uintptr_t priv, const struct iio_chan_spec *chan, char *buf)
{
	struct iio_counter *const counter = iio_priv(indio_dev);
	struct iio_counter_value *value;
	ssize_t ret;
	struct iio_counter_trigger *trigger;
	const int signal_id = *((int *)priv);
	int mode;

	if (!counter->ops->trigger_mode_get)
		return -EINVAL;

	mutex_lock(&counter->value_list_lock);

	value = __iio_counter_value_find_by_id(counter, chan->channel2);
	if (!value) {
		ret = -EINVAL;
		goto err_value;
	}

	mutex_lock(&value->trigger_list_lock);

	trigger = __iio_counter_trigger_find_by_id(value, signal_id);
	if (!trigger) {
		ret = -EINVAL;
		goto err_trigger;
	}

	mode = counter->ops->trigger_mode_get(counter, value, trigger);

	if (mode < 0) {
		ret = mode;
		goto err_trigger;
	} else if (mode >= trigger->num_trigger_modes) {
		ret = -EINVAL;
		goto err_trigger;
	}

	trigger->mode = mode;

	ret = scnprintf(buf, PAGE_SIZE, "%s\n", trigger->trigger_modes[mode]);

	mutex_unlock(&value->trigger_list_lock);

	mutex_unlock(&counter->value_list_lock);

	return ret;

err_trigger:
	mutex_unlock(&value->trigger_list_lock);
err_value:
	mutex_unlock(&counter->value_list_lock);
	return ret;
}

static ssize_t __iio_counter_trigger_mode_write(struct iio_dev *indio_dev,
	uintptr_t priv, const struct iio_chan_spec *chan, const char *buf,
	size_t len)
{
	struct iio_counter *const counter = iio_priv(indio_dev);
	struct iio_counter_value *value;
	ssize_t err;
	struct iio_counter_trigger *trigger;
	const int signal_id = *(int *)((void *)priv);
	unsigned int mode;

	if (!counter->ops->trigger_mode_set)
		return -EINVAL;

	mutex_lock(&counter->value_list_lock);

	value = __iio_counter_value_find_by_id(counter, chan->channel2);
	if (!value) {
		err = -EINVAL;
		goto err_value;
	}

	mutex_lock(&value->trigger_list_lock);

	trigger = __iio_counter_trigger_find_by_id(value, signal_id);
	if (!trigger) {
		err = -EINVAL;
		goto err_trigger;
	}

	for (mode = 0; mode < trigger->num_trigger_modes; mode++)
		if (sysfs_streq(buf, trigger->trigger_modes[mode]))
			break;

	if (mode >= trigger->num_trigger_modes) {
		err = -EINVAL;
		goto err_trigger;
	}

	err = counter->ops->trigger_mode_set(counter, value, trigger, mode);
	if (err)
		goto err_trigger;

	trigger->mode = mode;

	mutex_unlock(&value->trigger_list_lock);

	mutex_unlock(&counter->value_list_lock);

	return len;

err_trigger:
	mutex_unlock(&value->trigger_list_lock);
err_value:
	mutex_unlock(&counter->value_list_lock);
	return err;
}

static ssize_t __iio_counter_trigger_mode_available_read(
	struct iio_dev *indio_dev, uintptr_t priv,
	const struct iio_chan_spec *chan, char *buf)
{
	struct iio_counter *const counter = iio_priv(indio_dev);
	struct iio_counter_value *value;
	ssize_t len = 0;
	struct iio_counter_trigger *trigger;
	const int signal_id = *(int *)((void *)priv);
	unsigned int i;

	mutex_lock(&counter->value_list_lock);

	value = __iio_counter_value_find_by_id(counter, chan->channel2);
	if (!value) {
		len = -EINVAL;
		goto err_no_value;
	}

	mutex_lock(&value->trigger_list_lock);

	trigger = __iio_counter_trigger_find_by_id(value, signal_id);
	if (!trigger) {
		len = -EINVAL;
		goto err_no_trigger;
	}

	for (i = 0; i < trigger->num_trigger_modes; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s ",
			trigger->trigger_modes[i]);

	mutex_unlock(&value->trigger_list_lock);

	mutex_unlock(&counter->value_list_lock);

	buf[len - 1] = '\n';

	return len;

err_no_trigger:
	mutex_unlock(&value->trigger_list_lock);
err_no_value:
	mutex_unlock(&counter->value_list_lock);
	return len;
}

static int __iio_counter_value_function_set(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan, unsigned int mode)
{
	struct iio_counter *const counter = iio_priv(indio_dev);
	struct iio_counter_value *value;
	int err;

	if (!counter->ops->trigger_mode_get)
		return -EINVAL;

	mutex_lock(&counter->value_list_lock);

	value = __iio_counter_value_find_by_id(counter, chan->channel2);
	if (!value) {
		err = -EINVAL;
		goto err_value;
	}

	err = counter->ops->value_function_set(counter, value, mode);
	if (err)
		goto err_value;

	value->mode = mode;

err_value:
	mutex_unlock(&counter->value_list_lock);

	return err;
}

static int __iio_counter_value_function_get(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan)
{
	struct iio_counter *const counter = iio_priv(indio_dev);
	struct iio_counter_value *value;
	int retval;

	if (!counter->ops->trigger_mode_get)
		return -EINVAL;

	mutex_lock(&counter->value_list_lock);

	value = __iio_counter_value_find_by_id(counter, chan->channel2);
	if (!value) {
		retval = -EINVAL;
		goto err_value;
	}

	retval = counter->ops->value_function_get(counter, value);
	if (retval < 0)
		goto err_value;
	else if (retval >= value->num_function_modes) {
		retval = -EINVAL;
		goto err_value;
	}

	value->mode = retval;

err_value:
	mutex_unlock(&counter->value_list_lock);

	return retval;
}

static int __iio_counter_value_ext_info_alloc(struct iio_chan_spec *const chan,
	struct iio_counter_value *const value)
{
	const struct iio_chan_spec_ext_info ext_info_default[] = {
		{
			.name = "name",
			.shared = IIO_SEPARATE,
			.read = __iio_counter_value_name_read
		},
		IIO_ENUM("function", IIO_SEPARATE, &value->function_enum),
		{
			.name = "function_available",
			.shared = IIO_SEPARATE,
			.read = iio_enum_available_read,
			.private = (uintptr_t)((void *)&value->function_enum)
		},
		{
			.name = "triggers",
			.shared = IIO_SEPARATE,
			.read = __iio_counter_value_triggers_read
		}
	};
	const size_t num_default = ARRAY_SIZE(ext_info_default);
	const struct iio_chan_spec_ext_info ext_info_trigger[] = {
		{
			.shared = IIO_SEPARATE,
			.read = __iio_counter_trigger_mode_read,
			.write = __iio_counter_trigger_mode_write
		},
		{
			.shared = IIO_SEPARATE,
			.read = __iio_counter_trigger_mode_available_read
		}
	};
	const size_t num_ext_info_trigger = ARRAY_SIZE(ext_info_trigger);
	const struct list_head *pos;
	size_t num_triggers = 0;
	size_t num_triggers_ext_info;
	size_t num_ext_info;
	int err;
	struct iio_chan_spec_ext_info *ext_info;
	const struct iio_counter_trigger *trigger_pos;
	size_t i;

	value->function_enum.items = value->function_modes;
	value->function_enum.num_items = value->num_function_modes;
	value->function_enum.set = __iio_counter_value_function_set;
	value->function_enum.get = __iio_counter_value_function_get;

	mutex_lock(&value->trigger_list_lock);

	list_for_each(pos, &value->trigger_list)
		num_triggers++;

	num_triggers_ext_info = num_ext_info_trigger * num_triggers;
	num_ext_info = num_default + num_triggers_ext_info + 1;

	ext_info = kmalloc_array(num_ext_info, sizeof(*ext_info), GFP_KERNEL);
	if (!ext_info) {
		err = -ENOMEM;
		goto err_ext_info_alloc;
	}
	ext_info[num_ext_info - 1].name = NULL;

	memcpy(ext_info, ext_info_default, sizeof(ext_info_default));
	for (i = 0; i < num_triggers_ext_info; i += num_ext_info_trigger)
		memcpy(ext_info + num_default + i, ext_info_trigger,
			sizeof(ext_info_trigger));

	i = num_default;
	list_for_each_entry(trigger_pos, &value->trigger_list, list) {
		ext_info[i].name = kasprintf(GFP_KERNEL, "trigger_signal%d-%d",
			chan->channel, trigger_pos->signal->id);
		if (!ext_info[i].name) {
			err = -ENOMEM;
			goto err_name_alloc;
		}
		ext_info[i].private = (void *)&trigger_pos->signal->id;
		i++;

		ext_info[i].name = kasprintf(GFP_KERNEL,
			"trigger_signal%d-%d_available",
			chan->channel, trigger_pos->signal->id);
		if (!ext_info[i].name) {
			err = -ENOMEM;
			goto err_name_alloc;
		}
		ext_info[i].private = (void *)&trigger_pos->signal->id;
		i++;
	}

	chan->ext_info = ext_info;

	mutex_unlock(&value->trigger_list_lock);

	return 0;

err_name_alloc:
	while (i-- > num_default)
		kfree(ext_info[i].name);
	kfree(ext_info);
err_ext_info_alloc:
	mutex_unlock(&value->trigger_list_lock);
	return err;
}

static void __iio_counter_value_ext_info_free(
	const struct iio_chan_spec *const channel)
{
	size_t i;
	const char *const prefix = "trigger_signal";
	const size_t prefix_len = strlen(prefix);

	for (i = 0; channel->ext_info[i].name; i++)
		if (!strncmp(channel->ext_info[i].name, prefix, prefix_len))
			kfree(channel->ext_info[i].name);
	kfree(channel->ext_info);
}

static const struct iio_chan_spec_ext_info __iio_counter_signal_ext_info[] = {
	{
		.name = "name",
		.shared = IIO_SEPARATE,
		.read = __iio_counter_signal_name_read
	},
	{}
};

static int __iio_counter_channels_alloc(struct iio_counter *const counter)
{
	const struct list_head *pos;
	size_t num_channels = 0;
	int err;
	struct iio_chan_spec *channels;
	struct iio_counter_value *value_pos;
	size_t i = counter->num_channels;
	const struct iio_counter_signal *signal_pos;

	mutex_lock(&counter->signal_list_lock);

	list_for_each(pos, &counter->signal_list)
		num_channels++;

	if (!num_channels) {
		err = -EINVAL;
		goto err_no_signals;
	}

	mutex_lock(&counter->value_list_lock);

	list_for_each(pos, &counter->value_list)
		num_channels++;

	num_channels += counter->num_channels;

	channels = kcalloc(num_channels, sizeof(*channels), GFP_KERNEL);
	if (!channels) {
		err = -ENOMEM;
		goto err_channels_alloc;
	}

	memcpy(channels, counter->channels,
		counter->num_channels * sizeof(*counter->channels));

	list_for_each_entry(value_pos, &counter->value_list, list) {
		channels[i].type = IIO_COUNT;
		channels[i].channel = counter->id;
		channels[i].channel2 = value_pos->id;
		channels[i].info_mask_separate = BIT(IIO_CHAN_INFO_RAW);
		channels[i].indexed = 1;
		channels[i].counter = 1;

		err = __iio_counter_value_ext_info_alloc(channels + i,
			value_pos);
		if (err)
			goto err_value_ext_info_alloc;

		i++;
	}

	mutex_unlock(&counter->value_list_lock);

	list_for_each_entry(signal_pos, &counter->signal_list, list) {
		channels[i].type = IIO_SIGNAL;
		channels[i].channel = counter->id;
		channels[i].channel2 = signal_pos->id;
		channels[i].info_mask_separate = BIT(IIO_CHAN_INFO_RAW);
		channels[i].indexed = 1;
		channels[i].counter = 1;
		channels[i].ext_info = __iio_counter_signal_ext_info;

		i++;
	}

	mutex_unlock(&counter->signal_list_lock);

	counter->indio_dev->num_channels = num_channels;
	counter->indio_dev->channels = channels;

	return 0;

err_value_ext_info_alloc:
	while (i-- > counter->num_channels)
		__iio_counter_value_ext_info_free(channels + i);
	kfree(channels);
err_channels_alloc:
	mutex_unlock(&counter->value_list_lock);
err_no_signals:
	mutex_unlock(&counter->signal_list_lock);
	return err;
}

static void __iio_counter_channels_free(const struct iio_counter *const counter)
{
	size_t i = counter->num_channels + counter->indio_dev->num_channels;
	const struct iio_chan_spec *const chans = counter->indio_dev->channels;

	while (i-- > counter->num_channels)
		if (chans[i].type == IIO_COUNT)
			__iio_counter_value_ext_info_free(chans + i);

	kfree(chans);
}

static int __iio_counter_read_raw(struct iio_dev *indio_dev,
	struct iio_chan_spec const *chan, int *val, int *val2, long mask)
{
	struct iio_counter *const counter = iio_priv(indio_dev);
	struct iio_counter_signal *signal;
	int retval;
	struct iio_counter_value *value;

	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;

	switch (chan->type) {
	case IIO_SIGNAL:
		if (!counter->ops->signal_read)
			return -EINVAL;

		mutex_lock(&counter->signal_list_lock);
		signal = __iio_counter_signal_find_by_id(counter,
			chan->channel2);
		if (!signal) {
			mutex_unlock(&counter->signal_list_lock);
			return -EINVAL;
		}

		retval = counter->ops->signal_read(counter, signal, val, val2);
		mutex_unlock(&counter->signal_list_lock);

		return retval;
	case IIO_COUNT:
		if (!counter->ops->value_read)
			return -EINVAL;

		mutex_lock(&counter->value_list_lock);
		value = __iio_counter_value_find_by_id(counter, chan->channel2);
		if (!value) {
			mutex_unlock(&counter->value_list_lock);
			return -EINVAL;
		}

		retval = counter->ops->value_read(counter, value, val, val2);
		mutex_unlock(&counter->value_list_lock);

		return retval;
	default:
		if (counter->info && counter->info->read_raw)
			return counter->info->read_raw(indio_dev, chan, val,
				val2, mask);
	}

	return -EINVAL;
}

static int __iio_counter_write_raw(struct iio_dev *indio_dev,
	struct iio_chan_spec const *chan, int val, int val2, long mask)
{
	struct iio_counter *const counter = iio_priv(indio_dev);
	struct iio_counter_signal *signal;
	int retval;
	struct iio_counter_value *value;

	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;

	switch (chan->type) {
	case IIO_SIGNAL:
		if (!counter->ops->signal_write)
			return -EINVAL;

		mutex_lock(&counter->signal_list_lock);
		signal = __iio_counter_signal_find_by_id(counter,
			chan->channel2);
		if (!signal) {
			mutex_unlock(&counter->signal_list_lock);
			return -EINVAL;
		}

		retval = counter->ops->signal_write(counter, signal, val, val2);
		mutex_unlock(&counter->signal_list_lock);

		return retval;
	case IIO_COUNT:
		if (!counter->ops->value_write)
			return -EINVAL;

		mutex_lock(&counter->value_list_lock);
		value = __iio_counter_value_find_by_id(counter, chan->channel2);
		if (!value) {
			mutex_unlock(&counter->value_list_lock);
			return -EINVAL;
		}

		retval = counter->ops->value_write(counter, value, val, val2);
		mutex_unlock(&counter->value_list_lock);

		return retval;
	default:
		if (counter->info && counter->info->write_raw)
			return counter->info->write_raw(indio_dev, chan, val,
				val2, mask);
	}

	return -EINVAL;
}

static int __iio_counter_signal_register(struct iio_counter *const counter,
	struct iio_counter_signal *const signal)
{
	int err;

	if (!counter || !signal)
		return -EINVAL;

	mutex_lock(&counter->signal_list_lock);
	if (__iio_counter_signal_find_by_id(counter, signal->id)) {
		pr_err("Duplicate counter signal ID '%d'\n", signal->id);
		err = -EEXIST;
		goto err_duplicate_id;
	}
	list_add_tail(&signal->list, &counter->signal_list);
	mutex_unlock(&counter->signal_list_lock);

	return 0;

err_duplicate_id:
	mutex_unlock(&counter->signal_list_lock);
	return err;
}

static void __iio_counter_signal_unregister(struct iio_counter *const counter,
	struct iio_counter_signal *const signal)
{
	if (!counter || !signal)
		return;

	mutex_lock(&counter->signal_list_lock);
	list_del(&signal->list);
	mutex_unlock(&counter->signal_list_lock);
}

static int __iio_counter_signals_register(struct iio_counter *const counter,
	struct iio_counter_signal *const signals, const size_t num_signals)
{
	size_t i;
	int err;

	if (!counter || !signals)
		return -EINVAL;

	for (i = 0; i < num_signals; i++) {
		err = __iio_counter_signal_register(counter, signals + i);
		if (err)
			goto err_signal_register;
	}

	return 0;

err_signal_register:
	while (i--)
		__iio_counter_signal_unregister(counter, signals + i);
	return err;
}

static void __iio_counter_signals_unregister(struct iio_counter *const counter,
	struct iio_counter_signal *signals, size_t num_signals)
{
	if (!counter || !signals)
		return;

	while (num_signals--) {
		__iio_counter_signal_unregister(counter, signals);
		signals++;
	}
}

/**
 * iio_counter_trigger_register - register Trigger to Value
 * @value: pointer to IIO Counter Value for association
 * @trigger: pointer to IIO Counter Trigger to register
 *
 * The Trigger is added to the Value's trigger_list. A check is first performed
 * to verify that the respective Signal is not already linked to the Value; if
 * the respective Signal is already linked to the Value, the Trigger is not
 * added to the Value's trigger_list.
 *
 * NOTE: This function will acquire and release the Value's trigger_list_lock
 * during execution.
 */
int iio_counter_trigger_register(struct iio_counter_value *const value,
	struct iio_counter_trigger *const trigger)
{
	if (!value || !trigger || !trigger->signal)
		return -EINVAL;

	mutex_lock(&value->trigger_list_lock);
	if (__iio_counter_trigger_find_by_id(value, trigger->signal->id)) {
		pr_err("Signal%d is already linked to counter value%d\n",
			trigger->signal->id, value->id);
		return -EEXIST;
	}
	list_add_tail(&trigger->list, &value->trigger_list);
	mutex_unlock(&value->trigger_list_lock);

	return 0;
}
EXPORT_SYMBOL(iio_counter_trigger_register);

/**
 * iio_counter_trigger_unregister - unregister Trigger from Value
 * @value: pointer to IIO Counter Value of association
 * @trigger: pointer to IIO Counter Trigger to unregister
 *
 * The Trigger is removed from the Value's trigger_list.
 *
 * NOTE: This function will acquire and release the Value's trigger_list_lock
 * during execution.
 */
void iio_counter_trigger_unregister(struct iio_counter_value *const value,
	struct iio_counter_trigger *const trigger)
{
	if (!value || !trigger || !trigger->signal)
		return;

	mutex_lock(&value->trigger_list_lock);
	list_del(&trigger->list);
	mutex_unlock(&value->trigger_list_lock);
}
EXPORT_SYMBOL(iio_counter_trigger_unregister);

/**
 * iio_counter_triggers_register - register an array of Triggers to Value
 * @value: pointer to IIO Counter Value for association
 * @triggers: array of pointers to IIO Counter Triggers to register
 *
 * The iio_counter_trigger_register function is called for each Trigger in the
 * array. The @triggers array is traversed for the first @num_triggers Triggers.
 *
 * NOTE: @num_triggers must not be greater than the size of the @triggers array.
 */
int iio_counter_triggers_register(struct iio_counter_value *const value,
	struct iio_counter_trigger *const triggers, const size_t num_triggers)
{
	size_t i;
	int err;

	if (!value || !triggers)
		return -EINVAL;

	for (i = 0; i < num_triggers; i++) {
		err = iio_counter_trigger_register(value, triggers + i);
		if (err)
			goto err_trigger_register;
	}

	return 0;

err_trigger_register:
	while (i--)
		iio_counter_trigger_unregister(value, triggers + i);
	return err;
}
EXPORT_SYMBOL(iio_counter_triggers_register);

/**
 * iio_counter_triggers_unregister - unregister Triggers from Value
 * @value: pointer to IIO Counter Value of association
 * @triggers: array of pointers to IIO Counter Triggers to unregister
 *
 * The iio_counter_trigger_unregister function is called for each Trigger in the
 * array. The @triggers array is traversed for the first @num_triggers Triggers.
 *
 * NOTE: @num_triggers must not be greater than the size of the @triggers array.
 */
void iio_counter_triggers_unregister(struct iio_counter_value *const value,
	struct iio_counter_trigger *triggers, size_t num_triggers)
{
	if (!value || !triggers)
		return;

	while (num_triggers--) {
		iio_counter_trigger_unregister(value, triggers);
		triggers++;
	}
}
EXPORT_SYMBOL(iio_counter_triggers_unregister);

/**
 * iio_counter_value_register - register Value to Counter
 * @counter: pointer to IIO Counter for association
 * @value: pointer to IIO Counter Value to register
 *
 * The registration process occurs in two major steps. First, the Value is
 * initialized: trigger_list_lock is initialized, trigger_list is initialized,
 * and init_triggers if not NULL is passed to iio_counter_triggers_register.
 * Second, the Value is added to the Counter's value_list. A check is first
 * performed to verify that the Value is not already associated to the Counter
 * (via the Value's unique ID); if the Value is already associated to the
 * Counter, the Value is not added to the Counter's value_list and all of the
 * Value's Triggers are unregistered.
 *
 * NOTE: This function will acquire and release the Counter's value_list_lock
 * during execution.
 */
int iio_counter_value_register(struct iio_counter *const counter,
	struct iio_counter_value *const value)
{
	int err;

	if (!counter || !value)
		return -EINVAL;

	mutex_init(&value->trigger_list_lock);
	INIT_LIST_HEAD(&value->trigger_list);

	if (value->init_triggers) {
		err = iio_counter_triggers_register(value,
			value->init_triggers, value->num_init_triggers);
		if (err)
			return err;
	}

	mutex_lock(&counter->value_list_lock);
	if (__iio_counter_value_find_by_id(counter, value->id)) {
		pr_err("Duplicate counter value ID '%d'\n", value->id);
		err = -EEXIST;
		goto err_duplicate_id;
	}
	list_add_tail(&value->list, &counter->value_list);
	mutex_unlock(&counter->value_list_lock);

	return 0;

err_duplicate_id:
	mutex_unlock(&counter->value_list_lock);
	__iio_counter_trigger_unregister_all(value);
	return err;
}
EXPORT_SYMBOL(iio_counter_value_register);

/**
 * iio_counter_value_unregister - unregister Value from Counter
 * @counter: pointer to IIO Counter of association
 * @value: pointer to IIO Counter Value to unregister
 *
 * The Value is removed from the Counter's value_list and all of the Value's
 * Triggers are unregistered.
 *
 * NOTE: This function will acquire and release the Counter's value_list_lock
 * during execution.
 */
void iio_counter_value_unregister(struct iio_counter *const counter,
	struct iio_counter_value *const value)
{
	if (!counter || !value)
		return;

	mutex_lock(&counter->value_list_lock);
	list_del(&value->list);
	mutex_unlock(&counter->value_list_lock);

	__iio_counter_trigger_unregister_all(value);
}
EXPORT_SYMBOL(iio_counter_value_unregister);

/**
 * iio_counter_values_register - register an array of Values to Counter
 * @counter: pointer to IIO Counter for association
 * @values: array of pointers to IIO Counter Values to register
 *
 * The iio_counter_value_register function is called for each Value in the
 * array. The @values array is traversed for the first @num_values Values.
 *
 * NOTE: @num_values must not be greater than the size of the @values array.
 */
int iio_counter_values_register(struct iio_counter *const counter,
	struct iio_counter_value *const values, const size_t num_values)
{
	size_t i;
	int err;

	if (!counter || !values)
		return -EINVAL;

	for (i = 0; i < num_values; i++) {
		err = iio_counter_value_register(counter, values + i);
		if (err)
			goto err_values_register;
	}

	return 0;

err_values_register:
	while (i--)
		iio_counter_value_unregister(counter, values + i);
	return err;
}
EXPORT_SYMBOL(iio_counter_values_register);

/**
 * iio_counter_values_unregister - unregister Values from Counter
 * @counter: pointer to IIO Counter of association
 * @values: array of pointers to IIO Counter Values to unregister
 *
 * The iio_counter_value_unregister function is called for each Value in the
 * array. The @values array is traversed for the first @num_values Values.
 *
 * NOTE: @num_values must not be greater than the size of the @values array.
 */
void iio_counter_values_unregister(struct iio_counter *const counter,
	struct iio_counter_value *values, size_t num_values)
{
	if (!counter || !values)
		return;

	while (num_values--) {
		iio_counter_value_unregister(counter, values);
		values++;
	}
}
EXPORT_SYMBOL(iio_counter_values_unregister);

/**
 * iio_counter_register - register Counter to the system
 * @counter: pointer to IIO Counter to register
 *
 * This function piggybacks off of iio_device_register. First, the relevant
 * Counter members are initialized; if init_signals is not NULL it is passed to
 * iio_counter_signals_register, and similarly if init_values is not NULL it is
 * passed to iio_counter_values_register. Next, a struct iio_dev is allocated by
 * a call to iio_device_alloc and initialized for the Counter, IIO channels are
 * allocated, the Counter is copied as the private data, and finally
 * iio_device_register is called.
 */
int iio_counter_register(struct iio_counter *const counter)
{
	const struct iio_info info_default = {
		.driver_module = THIS_MODULE,
		.read_raw = __iio_counter_read_raw,
		.write_raw = __iio_counter_write_raw
	};
	int err;
	struct iio_info *info;
	struct iio_counter *priv;

	if (!counter)
		return -EINVAL;

	mutex_init(&counter->signal_list_lock);
	INIT_LIST_HEAD(&counter->signal_list);

	if (counter->init_signals) {
		err = __iio_counter_signals_register(counter,
			counter->init_signals, counter->num_init_signals);
		if (err)
			return err;
	}

	mutex_init(&counter->value_list_lock);
	INIT_LIST_HEAD(&counter->value_list);

	if (counter->init_values) {
		err = iio_counter_values_register(counter,
			counter->init_values, counter->num_init_values);
		if (err)
			goto err_values_register;
	}

	counter->indio_dev = iio_device_alloc(sizeof(*counter));
	if (!counter->indio_dev) {
		err = -ENOMEM;
		goto err_iio_device_alloc;
	}

	info = kmalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		err = -ENOMEM;
		goto err_info_alloc;
	}
	if (counter->info) {
		memcpy(info, counter->info, sizeof(*counter->info));
		info->read_raw = __iio_counter_read_raw;
		info->write_raw = __iio_counter_write_raw;
	} else {
		memcpy(info, &info_default, sizeof(info_default));
	}

	counter->indio_dev->info = info;
	counter->indio_dev->modes = INDIO_DIRECT_MODE;
	counter->indio_dev->name = counter->name;
	counter->indio_dev->dev.parent = counter->dev;

	err = __iio_counter_channels_alloc(counter);
	if (err)
		goto err_channels_alloc;

	priv = iio_priv(counter->indio_dev);
	memcpy(priv, counter, sizeof(*priv));

	err = iio_device_register(priv->indio_dev);
	if (err)
		goto err_iio_device_register;

	return 0;

err_iio_device_register:
	__iio_counter_channels_free(counter);
err_channels_alloc:
	kfree(info);
err_info_alloc:
	iio_device_free(counter->indio_dev);
err_iio_device_alloc:
	iio_counter_values_unregister(counter, counter->init_values,
		counter->num_init_values);
err_values_register:
	__iio_counter_signals_unregister(counter, counter->init_signals,
		counter->num_init_signals);
	return err;
}
EXPORT_SYMBOL(iio_counter_register);

/**
 * iio_counter_unregister - unregister Counter from the system
 * @counter: pointer to IIO Counter to unregister
 *
 * The Counter is unregistered from the system. The indio_dev is unregistered,
 * allocated memory is freed, and all associated Values and Signals are
 * unregistered.
 */
void iio_counter_unregister(struct iio_counter *const counter)
{
	const struct iio_info *const info = counter->indio_dev->info;

	if (!counter)
		return;

	iio_device_unregister(counter->indio_dev);

	__iio_counter_channels_free(counter);

	kfree(info);
	iio_device_free(counter->indio_dev);

	__iio_counter_value_unregister_all(counter);
	__iio_counter_signal_unregister_all(counter);
}
EXPORT_SYMBOL(iio_counter_unregister);
