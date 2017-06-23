 /*
  * Goodix GTx5 Touchscreen Driver
  * Core layer of touchdriver architecture.
  *
  * Copyright (C) 2015 - 2016 Goodix, Inc.
  * Authors:  Wang Yafei <wangyafei@goodix.com>
  *
  * This program is free software; you can redistribute it and/or modify
  * it under the terms of the GNU General Public License as published by
  * the Free Software Foundation; either version 2 of the License, or
  * (at your option) any later version.
  *
  * This program is distributed in the hope that it will be a reference
  * to you, when you are integrating the GOODiX's CTP IC into your system,
  * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  * General Public License for more details.
  *
  */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/of_platform.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/of_irq.h>
#include <linux/regulator/consumer.h>
#include <linux/input/mt.h>
#include "gtx5_core.h"

#define INPUT_TYPE_B_PROTOCOL

#define GOOIDX_INPUT_PHYS	"gtx5_ts/input0"
#define PINCTRL_STATE_ACTIVE    "pmx_ts_active"
#define PINCTRL_STATE_SUSPEND   "pmx_ts_suspend"

/*
 * struct gtx5_modules - external modules container
 * @head: external modules list
 * @initilized: whether this struct is initilized
 * @mutex: module mutex lock
 * @count: current number of registered external module
 * @wq: workqueue to do register work
 * @core_exit: if gtx5 touch core exit, then no
 *   registration is allowed.
 * @core_data: core_data pointer
 */
struct gtx5_modules {
	struct list_head head;
	bool initilized;
	struct mutex mutex;
	unsigned int count;
	struct workqueue_struct *wq;
	bool core_exit;
	struct completion core_comp;
	struct gtx5_ts_core *core_data;
};

static struct gtx5_modules gtx5_modules;

/**
 * __do_register_ext_module - register external module
 * to register into touch core modules structure
 */
static void  __do_register_ext_module(struct work_struct *work)
{
	struct gtx5_ext_module *module =
			container_of(work, struct gtx5_ext_module, work);
	struct gtx5_ext_module *ext_module;
	struct list_head *insert_point = &gtx5_modules.head;

	/* waitting for core layer */
	if (!wait_for_completion_timeout(&gtx5_modules.core_comp, 5 * HZ))
		return;

	/* driver probe failed */
	if (gtx5_modules.core_exit)
		return;

	/* prority level *must* be set */
	if (module->priority == EXTMOD_PRIO_RESERVED)
		return;

	mutex_lock(&gtx5_modules.mutex);
	if (!list_empty(&gtx5_modules.head)) {
		list_for_each_entry(ext_module, &gtx5_modules.head, list) {
			if (ext_module == module) {
				mutex_unlock(&gtx5_modules.mutex);
				return;
			}
		}

		list_for_each_entry(ext_module, &gtx5_modules.head, list) {
			/* small value of priority have higher priority level */
			if (ext_module->priority >= module->priority) {
				insert_point = &ext_module->list;
				break;
			}
		}
		/* else module will be inserted to gtx5_modules->head */
	}

	if (module->funcs && module->funcs->init) {
		if (module->funcs->init(gtx5_modules.core_data,
					module) < 0) {
			mutex_unlock(&gtx5_modules.mutex);
			return;
		}
	}

	list_add(&module->list, insert_point->prev);
	gtx5_modules.count++;
	mutex_unlock(&gtx5_modules.mutex);
}

/**
 * gtx5_register_ext_module - interface for external module
 * to register into touch core modules structure
 *
 * @module: pointer to external module to be register
 * return: 0 ok, <0 failed
 */
int gtx5_register_ext_module(struct gtx5_ext_module *module)
{
	if (!module)
		return -EINVAL;

	if (!gtx5_modules.initilized) {
		gtx5_modules.initilized = true;
		INIT_LIST_HEAD(&gtx5_modules.head);
		mutex_init(&gtx5_modules.mutex);
		init_completion(&gtx5_modules.core_comp);
	}

	if (gtx5_modules.core_exit)
		return -EFAULT;

	INIT_WORK(&module->work, __do_register_ext_module);
	schedule_work(&module->work);

	return 0;
}
EXPORT_SYMBOL_GPL(gtx5_register_ext_module);

/**
 * gtx5_unregister_ext_module - interface for external module
 * to unregister external modules
 *
 * @module: pointer to external module
 * return: 0 ok, <0 failed
 */
int gtx5_unregister_ext_module(struct gtx5_ext_module *module)
{
	struct gtx5_ext_module *ext_module;
	bool found = false;

	if (!module)
		return -EINVAL;

	if (!gtx5_modules.initilized)
		return -EINVAL;

	if (!gtx5_modules.core_data)
		return -ENODEV;

	mutex_lock(&gtx5_modules.mutex);
	if (!list_empty(&gtx5_modules.head)) {
		list_for_each_entry(ext_module, &gtx5_modules.head, list) {
			if (ext_module == module) {
				found = true;
				break;
			}
		}
	} else {
		mutex_unlock(&gtx5_modules.mutex);
		return -EFAULT;
	}

	if (!found) {
		mutex_unlock(&gtx5_modules.mutex);
		return -EFAULT;
	}

	list_del(&module->list);
	mutex_unlock(&gtx5_modules.mutex);

	if (module->funcs && module->funcs->exit)
		module->funcs->exit(gtx5_modules.core_data, module);
	gtx5_modules.count--;

	return 0;
}
EXPORT_SYMBOL_GPL(gtx5_unregister_ext_module);

static void gtx5_ext_sysfs_release(struct kobject *kobj)
{
	return;
}

#define to_ext_module(kobj) container_of(kobj,\
				struct gtx5_ext_module, kobj)
#define to_ext_attr(attr) container_of(attr,\
				struct gtx5_ext_attribute, attr)

static ssize_t gtx5_ext_sysfs_show(struct kobject *kobj,
				     struct attribute *attr, char *buf)
{
	struct gtx5_ext_module *module = to_ext_module(kobj);
	struct gtx5_ext_attribute *ext_attr = to_ext_attr(attr);

	if (ext_attr->show)
		return ext_attr->show(module, buf);

	return -EIO;
}

static ssize_t gtx5_ext_sysfs_store(struct kobject *kobj,
		struct attribute *attr, const char *buf, size_t count)
{
	struct gtx5_ext_module *module = to_ext_module(kobj);
	struct gtx5_ext_attribute *ext_attr = to_ext_attr(attr);

	if (ext_attr->store)
		return ext_attr->store(module, buf, count);

	return -EIO;
}

static const struct sysfs_ops gtx5_ext_ops = {
	.show = gtx5_ext_sysfs_show,
	.store = gtx5_ext_sysfs_store
};

static struct kobj_type gtx5_ext_ktype = {
	.release = gtx5_ext_sysfs_release,
	.sysfs_ops = &gtx5_ext_ops,
};

struct kobj_type *gtx5_get_default_ktype(void)
{
	return &gtx5_ext_ktype;
}
EXPORT_SYMBOL_GPL(gtx5_get_default_ktype);

struct kobject *gtx5_get_default_kobj(void)
{
	struct kobject *kobj = NULL;

	if (gtx5_modules.core_data &&
	    gtx5_modules.core_data->pdev)
		kobj = &gtx5_modules.core_data->pdev->dev.kobj;
	return kobj;
}
EXPORT_SYMBOL_GPL(gtx5_get_default_kobj);

/* show external module information */
static ssize_t gtx5_ts_extmod_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct gtx5_ext_module *module;
	size_t offset = 0;
	int r;

	mutex_lock(&gtx5_modules.mutex);
	if (!list_empty(&gtx5_modules.head)) {
		list_for_each_entry(module, &gtx5_modules.head, list) {
			r = snprintf(&buf[offset], PAGE_SIZE,
				     "priority:%u module:%s\n",
				     module->priority, module->name);
			if (r < 0) {
				mutex_unlock(&gtx5_modules.mutex);
				return -EINVAL;
			}
			offset += r;
		}
	}

	mutex_unlock(&gtx5_modules.mutex);
	return offset;
}

/* show driver information */
static ssize_t gtx5_ts_driver_info_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	return snprintf(buf, PAGE_SIZE, "DriverVersion:%s\n",
			GTX5_DRIVER_VERSION);
}

/* show chip infoamtion */
static ssize_t gtx5_ts_chip_info_show(struct device  *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct gtx5_ts_core *core_data = dev_get_drvdata(dev);
	struct gtx5_ts_device *ts_dev = core_data->ts_dev;
	struct gtx5_ts_version chip_ver;
	int r, cnt = 0;

	cnt += snprintf(buf, PAGE_SIZE,
			"TouchDeviceName:%s\n", ts_dev->name);
	if (ts_dev->hw_ops->read_version) {
		r = ts_dev->hw_ops->read_version(ts_dev, &chip_ver);
		if (!r && chip_ver.valid) {
			cnt += snprintf(&buf[cnt], PAGE_SIZE,
					"PID:%s\nVID:%04x\nSensorID:%02x\n",
					chip_ver.pid, chip_ver.vid,
					chip_ver.sensor_id);
		}
	}

	return cnt;
}

/* show chip configuration data */
static ssize_t gtx5_ts_config_data_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct gtx5_ts_core *core_data =
		dev_get_drvdata(dev);
	struct gtx5_ts_device *ts_dev = core_data->ts_dev;
	struct gtx5_ts_config *ncfg = ts_dev->normal_cfg;
	u8 *data;
	int i, r, offset = 0;

	if (ncfg && ncfg->initialized && ncfg->length < PAGE_SIZE) {
		data = kmalloc(ncfg->length, GFP_KERNEL);
		if (!data)
			return -ENOMEM;

		r = ts_dev->hw_ops->read(ts_dev, ncfg->reg_base,
				&data[0], ncfg->length);
		if (r < 0) {
			kfree(data);
			return -EINVAL;
		}

		for (i = 0; i < ncfg->length; i++) {
			if (i != 0 && i % 20 == 0)
				buf[offset++] = '\n';
			offset += snprintf(&buf[offset], PAGE_SIZE - offset,
					"%02x ", data[i]);
		}
		buf[offset++] = '\n';
		buf[offset++] = '\0';
		kfree(data);
		return offset;
	}

	return -EINVAL;
}

/* reset chip */
static ssize_t gtx5_ts_reset_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf,
				     size_t count)
{
	struct gtx5_ts_core *core_data =
		dev_get_drvdata(dev);
	struct gtx5_ts_device *ts_dev = core_data->ts_dev;
	int en;

	if (kstrtoint(buf, 10, &en))
		return -EINVAL;

	if (en != 1)
		return -EINVAL;

	if (ts_dev->hw_ops->reset)
		ts_dev->hw_ops->reset(ts_dev);

	return count;
}

/* show irq information */
static ssize_t gtx5_ts_irq_info_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct gtx5_ts_core *core_data = dev_get_drvdata(dev);
	struct irq_desc *desc;
	size_t offset = 0;
	int r;

	r = snprintf(&buf[offset], PAGE_SIZE, "irq:%u\n",
		     core_data->irq);
	if (r < 0)
		return -EINVAL;

	offset += r;
	r = snprintf(&buf[offset], PAGE_SIZE - offset, "state:%s\n",
		     atomic_read(&core_data->irq_enabled) ?
		     "enabled" : "disabled");
	if (r < 0)
		return -EINVAL;

	desc = irq_to_desc(core_data->irq);
	offset += r;
	r = snprintf(&buf[offset], PAGE_SIZE - offset, "disable-depth:%d\n",
		     desc->depth);
	if (r < 0)
		return -EINVAL;

	offset += r;
	r = snprintf(&buf[offset], PAGE_SIZE - offset, "trigger-count:%zu\n",
		     core_data->irq_trig_cnt);
	if (r < 0)
		return -EINVAL;

	offset += r;
	r = snprintf(&buf[offset], PAGE_SIZE - offset,
		     "echo 0/1 > irq_info to disable/enable irq");
	if (r < 0)
		return -EINVAL;

	offset += r;
	return offset;
}

/* enable/disable irq */
static ssize_t gtx5_ts_irq_info_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf,
					size_t count)
{
	struct gtx5_ts_core *core_data =
		dev_get_drvdata(dev);
	int en;

	if (kstrtoint(buf, 10, &en))
		return -EINVAL;

	gtx5_ts_irq_enable(core_data, en);
	return count;
}

static DEVICE_ATTR(extmod_info, 0444, gtx5_ts_extmod_show, NULL);
static DEVICE_ATTR(driver_info, 0444, gtx5_ts_driver_info_show, NULL);
static DEVICE_ATTR(chip_info, 0444, gtx5_ts_chip_info_show, NULL);
static DEVICE_ATTR(config_data, 0444, gtx5_ts_config_data_show, NULL);
static DEVICE_ATTR(reset, 0200, NULL, gtx5_ts_reset_store);
static DEVICE_ATTR(irq_info, 0644,
		gtx5_ts_irq_info_show, gtx5_ts_irq_info_store);

static struct attribute *sysfs_attrs[] = {
	&dev_attr_extmod_info.attr,
	&dev_attr_driver_info.attr,
	&dev_attr_chip_info.attr,
	&dev_attr_config_data.attr,
	&dev_attr_reset.attr,
	&dev_attr_irq_info.attr,
	NULL,
};

static const struct attribute_group sysfs_group = {
	.attrs = sysfs_attrs,
};

static int gtx5_ts_sysfs_init(struct gtx5_ts_core *core_data)
{
	return sysfs_create_group(&core_data->pdev->dev.kobj, &sysfs_group);
}

static void gtx5_ts_sysfs_exit(struct gtx5_ts_core *core_data)
{
	sysfs_remove_group(&core_data->pdev->dev.kobj, &sysfs_group);
}

/* event notifier */
static BLOCKING_NOTIFIER_HEAD(ts_notifier_list);
/**
 * gtx5_ts_register_client - register a client notifier
 * @nb: notifier block to callback on events
 *  see enum ts_notify_event in gtx5_ts_core.h
 */
int gtx5_ts_register_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&ts_notifier_list, nb);
}
EXPORT_SYMBOL(gtx5_ts_register_notifier);

/**
 * gtx5_ts_unregister_client - unregister a client notifier
 * @nb: notifier block to callback on events
 *	see enum ts_notify_event in gtx5_ts_core.h
 */
int gtx5_ts_unregister_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&ts_notifier_list, nb);
}
EXPORT_SYMBOL(gtx5_ts_unregister_notifier);

/**
 * gtx5_ts_blocking_notify - notify clients of certain events
 *	see enum ts_notify_event in gtx5_ts_core.h
 */
int gtx5_ts_blocking_notify(enum ts_notify_event evt, void *v)
{
	return blocking_notifier_call_chain(&ts_notifier_list,
			(unsigned long)evt, v);
}
EXPORT_SYMBOL_GPL(gtx5_ts_blocking_notify);

/**
 * gtx5_ts_input_report - report touch event to input subsystem
 *
 * @dev: input device pointer
 * @touch_data: touch data pointer
 * return: 0 ok, <0 failed
 */
static int gtx5_ts_input_report(struct input_dev *dev,
				  struct gtx5_touch_data *touch_data)
{
	struct gtx5_ts_coords *coords = &touch_data->coords[0];
	struct gtx5_ts_core *core_data = input_get_drvdata(dev);
	struct gtx5_ts_board_data *ts_bdata = board_data(core_data);
	unsigned int touch_num = touch_data->touch_num, x, y;
	static u16 pre_fin;
	int i, id;

	/* report touch-key */
	if (unlikely(touch_data->key_value)) {
		for (i = 0; i < ts_bdata->panel_max_key; i++) {
			input_report_key(dev, ts_bdata->panel_key_map[i],
					 touch_data->key_value & (1 << i));
		}
	}

	/* first touch down and last touch up condition */
	if (touch_num != 0 && pre_fin == 0x0000) {
		/* first touch down event */
		input_report_key(dev, BTN_TOUCH, 1);
		input_report_key(dev, BTN_TOOL_FINGER, 1);
	} else if (touch_num == 0 && pre_fin != 0x0000) {
		/* no finger exist */
		input_report_key(dev, BTN_TOUCH, 0);
		input_report_key(dev, BTN_TOOL_FINGER, 0);
	} else if (touch_num == 0 && pre_fin == 0x0000) {
		return 0;
	}

	/* report abs */
	id = coords->id;
	for (i = 0; i < ts_bdata->panel_max_id; i++) {
		if (touch_num && i == id) {
			/* this is a valid touch down event */
#ifdef INPUT_TYPE_B_PROTOCOL
			input_mt_slot(dev, id);
			input_mt_report_slot_state(dev, MT_TOOL_FINGER, true);
#else
			input_report_abs(dev, ABS_MT_TRACKING_ID, id);
#endif
			if (unlikely(ts_bdata->swap_axis)) {
				x = coords->y;
				y = coords->x;
			} else {
				x = coords->x;
				y = coords->y;
			}
			input_report_abs(dev, ABS_MT_POSITION_X, x);
			input_report_abs(dev, ABS_MT_POSITION_Y, y);
			input_report_abs(dev, ABS_MT_TOUCH_MAJOR, coords->w);
			pre_fin |= 1 << i;
			id = (++coords)->id;
#ifndef INPUT_TYPE_B_PROTOCOL
			input_mt_sync(dev);
#endif
		} else {
			if (pre_fin & (1 << i)) {/* release touch */
#ifdef INPUT_TYPE_B_PROTOCOL
				input_mt_slot(dev, i);
				input_mt_report_slot_state(dev, MT_TOOL_FINGER,
							   false);
#endif
				pre_fin &= ~(1 << i);
			}
		}
	}

#ifndef INPUT_TYPE_B_PROTOCOL
	if (!pre_fin)
		input_mt_sync(dev);
#endif
	input_sync(dev);
	return 0;
}

/**
 * gtx5_ts_threadirq_func - Bottom half of interrupt
 * This functions is excuted in thread context,
 * sleep in this function is permit.
 *
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
static irqreturn_t gtx5_ts_threadirq_func(int irq, void *data)
{
	struct gtx5_ts_core *core_data = data;
	struct gtx5_ts_device *ts_dev =  core_data->ts_dev;
	struct gtx5_ext_module *ext_module;
	struct gtx5_ts_event *ts_event = &core_data->ts_event;
	int r;

	core_data->irq_trig_cnt++;
	/* inform external module */
	list_for_each_entry(ext_module, &gtx5_modules.head, list) {
		if (!ext_module->funcs->irq_event)
			continue;
		r = ext_module->funcs->irq_event(core_data, ext_module);
		if (r == EVT_CANCEL_IRQEVT)
			return IRQ_HANDLED;
	}

	/* read touch data from touch device */
	r = ts_dev->hw_ops->event_handler(ts_dev, ts_event);
	if (likely(r >= 0)) {
		if (ts_event->event_type == EVENT_TOUCH) {
			/* report touch */
			gtx5_ts_input_report(core_data->input_dev,
					&ts_event->event_data.touch_data);
		}
	}

	return IRQ_HANDLED;
}

/**
 * gtx5_ts_init_irq - Requset interrupt line from system
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
static int gtx5_ts_irq_setup(struct gtx5_ts_core *core_data)
{
	const struct gtx5_ts_board_data *ts_bdata =
			board_data(core_data);
	const struct device *dev = &core_data->pdev->dev;
	int r;

	/* if ts_bdata->irq is invalid get it from irq-gpio */
	if (ts_bdata->irq <= 0)
		core_data->irq = gpiod_to_irq(ts_bdata->irq_gpiod);
	else
		core_data->irq = ts_bdata->irq;

	dev_info(dev, "IRQ:%u,flags:%d\n",
		 core_data->irq, (int)ts_bdata->irq_flags);
	r = devm_request_threaded_irq(&core_data->pdev->dev,
				      core_data->irq, NULL,
				      gtx5_ts_threadirq_func,
				      ts_bdata->irq_flags | IRQF_ONESHOT,
				      GTX5_CORE_DRIVER_NAME,
				      core_data);
	if (r < 0)
		dev_err(dev, "Failed to requeset threaded irq:%d\n", r);
	else
		atomic_set(&core_data->irq_enabled, 1);

	return r;
}

/**
 * gtx5_ts_irq_enable - Enable/Disable a irq
 * @core_data: pointer to touch core data
 * enable: enable or disable irq
 * return: 0 ok, <0 failed
 */
int gtx5_ts_irq_enable(struct gtx5_ts_core *core_data,
			 bool enable)
{
	const struct device *dev = &core_data->pdev->dev;

	if (enable) {
		if (!atomic_cmpxchg(&core_data->irq_enabled, 0, 1)) {
			enable_irq(core_data->irq);
			dev_dbg(dev, "Irq enabled\n");
		}
	} else {
		if (atomic_cmpxchg(&core_data->irq_enabled, 1, 0)) {
			disable_irq(core_data->irq);
			dev_dbg(dev, "Irq disabled\n");
		}
	}

	return 0;
}
EXPORT_SYMBOL(gtx5_ts_irq_enable);
/**
 * gtx5_ts_power_init - Get regulator for touch device
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
static int gtx5_ts_power_init(struct gtx5_ts_core *core_data)
{
	struct device *dev = NULL;
	struct gtx5_ts_board_data *ts_bdata;

	/* dev:i2c client device or spi slave device*/
	dev =  core_data->ts_dev->dev;
	ts_bdata = board_data(core_data);

	if (ts_bdata->avdd_name) {
		core_data->avdd = devm_regulator_get(dev, ts_bdata->avdd_name);
		if (IS_ERR_OR_NULL(core_data->avdd)) {
			core_data->avdd = NULL;
			return -ENOENT;
		}
	} else {
		return -EINVAL;
	}

	return 0;
}

/**
 * gtx5_ts_power_on - Turn on power to the touch device
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
static int gtx5_ts_power_on(struct gtx5_ts_core *core_data)
{
	struct gtx5_ts_board_data *ts_bdata =
			board_data(core_data);
	const struct device *dev = &core_data->pdev->dev;
	int r;

	dev_info(dev, "Device power on\n");
	if (core_data->power_on)
		return 0;

	if (core_data->avdd) {
		r = regulator_enable(core_data->avdd);
		if (!r) {
			if (ts_bdata->power_on_delay_us)
				usleep_range(ts_bdata->power_on_delay_us,
					     ts_bdata->power_on_delay_us);
		} else {
			dev_err(dev, "Failed to enable analog power:%d\n", r);
			return r;
		}
	}

	core_data->power_on = 1;
	return 0;
}

/**
 * gtx5_ts_power_off - Turn off power to the touch device
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
static int gtx5_ts_power_off(struct gtx5_ts_core *core_data)
{
	struct gtx5_ts_board_data *ts_bdata =
			board_data(core_data);
	const struct device *dev = &core_data->pdev->dev;
	int r;

	dev_info(dev, "Device power off\n");
	if (!core_data->power_on)
		return 0;

	if (core_data->avdd) {
		r = regulator_disable(core_data->avdd);
		if (!r) {
			if (ts_bdata->power_off_delay_us)
				usleep_range(ts_bdata->power_off_delay_us,
					     ts_bdata->power_off_delay_us);
		} else {
			dev_err(dev, "Failed to disable analog power:%d\n", r);
			return r;
		}
	}

	core_data->power_on = 0;
	return 0;
}

/**
 * gtx5_ts_gpio_setup - Request gpio resources from GPIO subsysten
 *	reset_gpio and irq_gpio number are obtained from gtx5_ts_device
 *  which created in hardware layer driver. e.g.gtx5_xx_i2c.c
 *	A gtx5_ts_device should set those two fileds to right value
 *	before registed to touch core driver.
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
static void gtx5_ts_gpio_setup(struct gtx5_ts_core *core_data)
{
	struct gtx5_ts_board_data *ts_bdata = board_data(core_data);
	struct gtx5_ts_device *ts_dev = core_data->ts_dev;
	const struct device *dev = &core_data->pdev->dev;

	ts_bdata->reset_gpiod = devm_gpiod_get_optional(ts_dev->dev,
			"reset", GPIOD_OUT_LOW);
	if (!ts_bdata->reset_gpiod)
		dev_info(dev, "No reset gpio found\n");

	ts_bdata->irq_gpiod = devm_gpiod_get_optional(ts_dev->dev,
			"irq", GPIOD_IN);
	if (!ts_bdata->irq_gpiod)
		dev_info(dev, "No irq gpio found\n");
}

/**
 * gtx5_input_set_params - set input parameters
 */
static void gtx5_ts_set_input_params(struct input_dev *input_dev,
				       struct gtx5_ts_board_data *ts_bdata)
{
	int i;

	if (ts_bdata->swap_axis)
		swap(ts_bdata->panel_max_x, ts_bdata->panel_max_y);

	input_set_abs_params(input_dev, ABS_MT_TRACKING_ID,
			     0, ts_bdata->panel_max_id, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_X,
			     0, ts_bdata->panel_max_x, 0, 0);

	input_set_abs_params(input_dev, ABS_MT_POSITION_Y,
			     0, ts_bdata->panel_max_y, 0, 0);

	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR,
			     0, ts_bdata->panel_max_w, 0, 0);
	if (ts_bdata->panel_max_key) {
		for (i = 0; i < ts_bdata->panel_max_key; i++)
			input_set_capability(input_dev, EV_KEY,
					     ts_bdata->panel_key_map[i]);
	}
}

/**
 * gtx5_ts_input_dev_config - Requset and config a input device
 *  then register it to input sybsystem.
 *  NOTE that some hardware layer may provide a input device
 *  (ts_dev->input_dev not NULL).
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
static int gtx5_ts_input_dev_config(struct gtx5_ts_core *core_data)
{
	struct gtx5_ts_board_data *ts_bdata = board_data(core_data);
	struct device *dev = &core_data->pdev->dev;
	struct input_dev *input_dev = NULL;
	int r;

	input_dev = devm_input_allocate_device(dev);
	if (!input_dev) {
		dev_err(dev, "Failed to allocated input device\n");
		return -ENOMEM;
	}

	core_data->input_dev = input_dev;
	input_set_drvdata(input_dev, core_data);

	input_dev->name = GTX5_CORE_DRIVER_NAME;
	input_dev->phys = GOOIDX_INPUT_PHYS;
	input_dev->id.product = 0xDEAD;
	input_dev->id.vendor = 0xBEEF;
	input_dev->id.version = 10427;

	__set_bit(EV_SYN, input_dev->evbit);
	__set_bit(EV_KEY, input_dev->evbit);
	__set_bit(EV_ABS, input_dev->evbit);
	__set_bit(BTN_TOUCH, input_dev->keybit);
	__set_bit(BTN_TOOL_FINGER, input_dev->keybit);

#ifdef INPUT_PROP_DIRECT
	__set_bit(INPUT_PROP_DIRECT, input_dev->propbit);
#endif

	/* set input parameters */
	gtx5_ts_set_input_params(input_dev, ts_bdata);

#ifdef INPUT_TYPE_B_PROTOCOL
	input_mt_init_slots(input_dev, ts_bdata->panel_max_id,
			    INPUT_MT_DIRECT);
#endif

	input_set_capability(input_dev, EV_KEY, KEY_POWER);
	r = input_register_device(input_dev);
	if (r < 0) {
		dev_err(dev, "Unable to register input device\n");
		return r;
	}

	return 0;
}

/**
 * gtx5_ts_hw_init - Hardware initialize
 *  poweron - hardware reset - sendconfig
 * @core_data: pointer to touch core data
 * return: 0 intilize ok, <0 failed
 */
static int gtx5_ts_hw_init(struct gtx5_ts_core *core_data)
{
	const struct gtx5_ts_hw_ops *hw_ops =
		ts_hw_ops(core_data);
	int r;

	r = gtx5_ts_power_on(core_data);
	if (r < 0)
		goto exit;

	/* reset touch device */
	if (hw_ops->reset)
		hw_ops->reset(core_data->ts_dev);

	/* init */
	if (hw_ops->init) {
		r = hw_ops->init(core_data->ts_dev);
		if (r < 0) {
			core_data->hw_err = true;
			goto exit;
		}
	}

exit:
	/* if bus communication error occurred then exit driver binding, other
	 * errors will be ignored
	 */
	if (r != -EBUS)
		r = 0;
	return r;
}

/**
 * gtx5_ts_esd_work - check hardware status and recovery
 *  the hardware if needed.
 */
static void gtx5_ts_esd_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct gtx5_ts_esd *ts_esd = container_of(dwork,
			struct gtx5_ts_esd, esd_work);
	struct gtx5_ts_core *core = container_of(ts_esd,
			struct gtx5_ts_core, ts_esd);
	const struct gtx5_ts_hw_ops *hw_ops = ts_hw_ops(core);
	int r = 0;

	if (ts_esd->esd_on == false)
		return;

	if (hw_ops->check_hw)
		r = hw_ops->check_hw(core->ts_dev);
	if (r < 0) {
		gtx5_ts_power_off(core);
		gtx5_ts_power_on(core);
		if (hw_ops->reset)
			hw_ops->reset(core->ts_dev);
	}

	mutex_lock(&ts_esd->esd_mutex);
	if (ts_esd->esd_on)
		schedule_delayed_work(&ts_esd->esd_work, 2 * HZ);
	mutex_unlock(&ts_esd->esd_mutex);
}

/**
 * gtx5_ts_esd_on - turn on esd protection
 */
static void gtx5_ts_esd_on(struct gtx5_ts_core *core_data)
{
	struct gtx5_ts_esd *ts_esd = &core_data->ts_esd;
	const struct device *dev = &core_data->pdev->dev;

	mutex_lock(&ts_esd->esd_mutex);
	if (ts_esd->esd_on == false) {
		ts_esd->esd_on = true;
		schedule_delayed_work(&ts_esd->esd_work, 2 * HZ);
		mutex_unlock(&ts_esd->esd_mutex);
		dev_info(dev, "Esd on\n");
		return;
	}
	mutex_unlock(&ts_esd->esd_mutex);
}

/**
 * gtx5_ts_esd_off - turn off esd protection
 */
static void gtx5_ts_esd_off(struct gtx5_ts_core *core_data)
{
	struct gtx5_ts_esd *ts_esd = &core_data->ts_esd;
	const struct device *dev = &core_data->pdev->dev;

	mutex_lock(&ts_esd->esd_mutex);
	if (ts_esd->esd_on == true) {
		ts_esd->esd_on = false;
		cancel_delayed_work(&ts_esd->esd_work);
		mutex_unlock(&ts_esd->esd_mutex);
		dev_info(dev, "Esd off\n");
		return;
	}
	mutex_unlock(&ts_esd->esd_mutex);
}

/**
 * gtx5_esd_notifier_callback - notification callback
 *  under certain condition, we need to turn off/on the esd
 *  protector, we use kernel notify call chain to achieve this.
 *
 *  for example: before firmware update we need to turn off the
 *  esd protector and after firmware update finished, we should
 *  turn on the esd protector.
 */
static int gtx5_esd_notifier_callback(struct notifier_block *nb,
					unsigned long action, void *data)
{
	struct gtx5_ts_esd *ts_esd = container_of(nb,
			struct gtx5_ts_esd, esd_notifier);

	switch (action) {
	case NOTIFY_FWUPDATE_START:
	case NOTIFY_SUSPEND:
		gtx5_ts_esd_off(ts_esd->ts_core);
		break;
	case NOTIFY_FWUPDATE_END:
	case NOTIFY_RESUME:
		gtx5_ts_esd_on(ts_esd->ts_core);
		break;
	}

	return 0;
}

/**
 * gtx5_ts_esd_init - initialize esd protection
 */
static int gtx5_ts_esd_init(struct gtx5_ts_core *core)
{
	struct gtx5_ts_esd *ts_esd = &core->ts_esd;

	INIT_DELAYED_WORK(&ts_esd->esd_work, gtx5_ts_esd_work);
	mutex_init(&ts_esd->esd_mutex);
	ts_esd->ts_core = core;
	ts_esd->esd_on = false;
	ts_esd->esd_notifier.notifier_call = gtx5_esd_notifier_callback;
	gtx5_ts_register_notifier(&ts_esd->esd_notifier);

	if (core->ts_dev->board_data->esd_default_on == true &&
			core->ts_dev->hw_ops->check_hw)
		gtx5_ts_esd_on(core);
	return 0;
}

/**
 * gtx5_ts_suspend - Touchscreen suspend function
 */
static int gtx5_ts_suspend(struct gtx5_ts_core *core_data)
{
	struct gtx5_ext_module *ext_module;
	struct gtx5_ts_device *ts_dev = core_data->ts_dev;
	const struct device *dev = &core_data->pdev->dev;
	int r;

	dev_dbg(dev, "Suspend start\n");
	/*
	 * notify suspend event, inform the esd protector
	 * and charger detector to turn off the work
	 */
	gtx5_ts_blocking_notify(NOTIFY_SUSPEND, NULL);

	/* inform external module */
	mutex_lock(&gtx5_modules.mutex);
	if (!list_empty(&gtx5_modules.head)) {
		list_for_each_entry(ext_module, &gtx5_modules.head, list) {
			if (!ext_module->funcs->before_suspend)
				continue;

			r = ext_module->funcs->before_suspend(core_data, ext_module);
			if (r == EVT_CANCEL_SUSPEND) {
				mutex_unlock(&gtx5_modules.mutex);
				dev_dbg(dev, "Canceled by module:%s\n",
					ext_module->name);
				goto out;
			}
		}
	}
	mutex_unlock(&gtx5_modules.mutex);

	/* disable irq */
	gtx5_ts_irq_enable(core_data, false);

	/* let touch ic work in sleep mode */
	if (ts_dev && ts_dev->hw_ops->suspend)
		ts_dev->hw_ops->suspend(ts_dev);
	atomic_set(&core_data->suspended, 1);

	/* inform exteranl modules */
	mutex_lock(&gtx5_modules.mutex);
	if (!list_empty(&gtx5_modules.head)) {
		list_for_each_entry(ext_module, &gtx5_modules.head, list) {
			if (!ext_module->funcs->after_suspend)
				continue;

			r = ext_module->funcs->after_suspend(core_data, ext_module);
			if (r == EVT_CANCEL_SUSPEND) {
				mutex_unlock(&gtx5_modules.mutex);
				dev_dbg(dev, "Canceled by module:%s\n",
					ext_module->name);
				goto out;
			}
		}
	}
	mutex_unlock(&gtx5_modules.mutex);

out:
	/* release all the touch IDs */
	core_data->ts_event.event_data.touch_data.touch_num = 0;
	gtx5_ts_input_report(core_data->input_dev,
			&core_data->ts_event.event_data.touch_data);
	dev_dbg(dev, "Suspend end\n");
	return 0;
}

/**
 * gtx5_ts_resume - Touchscreen resume function
 * Called by PM/FB/EARLYSUSPEN module to wakeup device
 */
static int gtx5_ts_resume(struct gtx5_ts_core *core_data)
{
	struct gtx5_ext_module *ext_module;
	struct gtx5_ts_device *ts_dev = core_data->ts_dev;
	const struct device *dev = &core_data->pdev->dev;
	int r;

	dev_dbg(dev, "Resume start\n");
	mutex_lock(&gtx5_modules.mutex);
	if (!list_empty(&gtx5_modules.head)) {
		list_for_each_entry(ext_module, &gtx5_modules.head, list) {
			if (!ext_module->funcs->before_resume)
				continue;

			r = ext_module->funcs->before_resume(core_data, ext_module);
			if (r == EVT_CANCEL_RESUME) {
				mutex_unlock(&gtx5_modules.mutex);
				dev_dbg(dev, "Canceled by module:%s\n",
					ext_module->name);
				goto out;
			}
		}
	}
	mutex_unlock(&gtx5_modules.mutex);

	atomic_set(&core_data->suspended, 0);
	/* resume device */
	if (ts_dev && ts_dev->hw_ops->resume)
		ts_dev->hw_ops->resume(ts_dev);

	gtx5_ts_irq_enable(core_data, true);

	mutex_lock(&gtx5_modules.mutex);
	if (!list_empty(&gtx5_modules.head)) {
		list_for_each_entry(ext_module, &gtx5_modules.head, list) {
			if (!ext_module->funcs->after_resume)
				continue;

			r = ext_module->funcs->after_resume(core_data,
							    ext_module);
			if (r == EVT_CANCEL_RESUME) {
				mutex_unlock(&gtx5_modules.mutex);
				dev_dbg(dev, "Canceled by module:%s\n",
					ext_module->name);
				goto out;
			}
		}
	}
	mutex_unlock(&gtx5_modules.mutex);

out:
	/*
	 * notify resume event, inform the esd protector
	 * and charger detector to turn on the work
	 */
	gtx5_ts_blocking_notify(NOTIFY_RESUME, NULL);
	dev_dbg(dev, "Resume end\n");
	return 0;
}

/**
 * gtx5_ts_pm_suspend - PM suspend function
 * Called by kernel during system suspend phrase
 */
static int __maybe_unused gtx5_ts_pm_suspend(struct device *dev)
{
	struct gtx5_ts_core *core_data =
		dev_get_drvdata(dev);

	return gtx5_ts_suspend(core_data);
}

/**
 * gtx5_ts_pm_resume - PM resume function
 * Called by kernel during system wakeup
 */
static int __maybe_unused gtx5_ts_pm_resume(struct device *dev)
{
	struct gtx5_ts_core *core_data =
		dev_get_drvdata(dev);

	return gtx5_ts_resume(core_data);
}

/**
 * gtx5_generic_noti_callback - generic notifier callback
 *  for gtx5 touch notification event.
 */
static int gtx5_generic_noti_callback(struct notifier_block *self,
					unsigned long action, void *data)
{
	struct gtx5_ts_core *ts_core = container_of(self,
			struct gtx5_ts_core, ts_notifier);
	const struct gtx5_ts_hw_ops *hw_ops = ts_hw_ops(ts_core);
	int r;

	switch (action) {
	case NOTIFY_FWUPDATE_END:
		if (ts_core->hw_err && hw_ops->init) {
			/* Firmware has been updated, we need to reinit
			 * the chip, read the sensor ID and send the
			 * correct config data based on sensor ID.
			 * The input parameters also needs to be updated.
			 */
			r = hw_ops->init(ts_core->ts_dev);
			if (r < 0)
				goto exit;

			gtx5_ts_set_input_params(ts_core->input_dev,
						   ts_core->ts_dev->board_data);
			ts_core->hw_err = false;
		}
		break;
	}

exit:
	return 0;
}

/**
 * gtx5_ts_probe - called by kernel when a Goodix touch
 *  platform driver is added.
 */
static int gtx5_ts_probe(struct platform_device *pdev)
{
	struct gtx5_ts_core *core_data = NULL;
	struct gtx5_ts_device *ts_device;
	int r;

	ts_device = pdev->dev.platform_data;
	if (!ts_device || !ts_device->hw_ops || !ts_device->board_data) {
		dev_err(&pdev->dev, "Invalid touch device\n");
		return -ENODEV;
	}

	core_data = devm_kzalloc(&pdev->dev, sizeof(struct gtx5_ts_core),
				 GFP_KERNEL);
	if (!core_data)
		return -ENOMEM;

	/* touch core layer is a platform driver */
	core_data->pdev = pdev;
	core_data->ts_dev = ts_device;
	platform_set_drvdata(pdev, core_data);

	r = gtx5_ts_power_init(core_data);
	if (r < 0)
		dev_err(&pdev->dev, "Failed power init\n");

	/* get GPIO resource if have */
	gtx5_ts_gpio_setup(core_data);

	/* initialize firmware */
	r = gtx5_ts_hw_init(core_data);
	if (r < 0)
		goto out;

	/* alloc/config/register input device */
	r = gtx5_ts_input_dev_config(core_data);
	if (r < 0)
		goto out;

	/* request irq line */
	r = gtx5_ts_irq_setup(core_data);
	if (r < 0)
		goto out;

	/* inform the external module manager that
	 * touch core layer is ready now
	 */
	gtx5_modules.core_data = core_data;
	complete_all(&gtx5_modules.core_comp);

	/* create sysfs files */
	gtx5_ts_sysfs_init(core_data);

	/* esd protector */
	gtx5_ts_esd_init(core_data);

	/* generic notifier callback */
	core_data->ts_notifier.notifier_call = gtx5_generic_noti_callback;
	gtx5_ts_register_notifier(&core_data->ts_notifier);

	return 0;
	/* we use resource managed api(devm_), no need to free resource */
out:
	gtx5_modules.core_exit = true;
	complete_all(&gtx5_modules.core_comp);
	dev_err(&pdev->dev, "Core layer probe failed");
	return r;
}

static int gtx5_ts_remove(struct platform_device *pdev)
{
	struct gtx5_ts_core *core_data =
		platform_get_drvdata(pdev);

	gtx5_ts_power_off(core_data);
	gtx5_ts_sysfs_exit(core_data);
	return 0;
}

static SIMPLE_DEV_PM_OPS(dev_pm_ops, gtx5_ts_pm_suspend, gtx5_ts_pm_resume);

static const struct platform_device_id ts_core_ids[] = {
	{.name = GTX5_CORE_DRIVER_NAME},
	{}
};
MODULE_DEVICE_TABLE(platform, ts_core_ids);

static struct platform_driver gtx5_ts_driver = {
	.driver = {
		.name = GTX5_CORE_DRIVER_NAME,
		.pm = &dev_pm_ops,
	},
	.probe = gtx5_ts_probe,
	.remove = gtx5_ts_remove,
	.id_table = ts_core_ids,
};

static int __init gtx5_ts_core_init(void)
{
	if (!gtx5_modules.initilized) {
		gtx5_modules.initilized = true;
		INIT_LIST_HEAD(&gtx5_modules.head);
		mutex_init(&gtx5_modules.mutex);
		init_completion(&gtx5_modules.core_comp);
	}

	return platform_driver_register(&gtx5_ts_driver);
}

static void __exit gtx5_ts_core_exit(void)
{
	platform_driver_unregister(&gtx5_ts_driver);
}

module_init(gtx5_ts_core_init);
module_exit(gtx5_ts_core_exit);

MODULE_DESCRIPTION("Goodix Touchscreen Core Module");
MODULE_AUTHOR("Goodix, Inc.");
MODULE_LICENSE("GPL v2");
