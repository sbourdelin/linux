/*
 * GPIO Testing Device Driver
 *
 * Copyright (C) 2014  Kamlakant Patel <kamlakant.patel@broadcom.com>
 * Copyright (C) 2015-2016  Bamvor Jian Zhang <bamvor.zhangjian@linaro.org>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/gpio/driver.h>
#include <linux/platform_device.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include "gpiolib.h"

#define GPIO_NAME	"gpio-mockup"
#define	MAX_GC		10

enum direction {
	OUT,
	IN
};

/*
 * struct gpio_pin_status - structure describing a GPIO status
 * @dir:       Configures direction of gpio as "in" or "out", 0=in, 1=out
 * @value:     Configures status of the gpio as 0(low) or 1(high)
 */
struct gpio_pin_status {
	enum direction dir;
	bool value;
};

struct mockup_gpio_controller {
	struct gpio_chip gc;
	struct gpio_pin_status *stats;
	struct dentry *dbg_dir;
};

static int gpio_mockup_ranges[MAX_GC << 1];
static int gpio_mockup_params_nr;
module_param_array(gpio_mockup_ranges, int, &gpio_mockup_params_nr, 0400);

static bool gpio_mockup_named_lines;
module_param_named(gpio_mockup_named_lines,
		   gpio_mockup_named_lines, bool, 0400);

static const char pins_name_start = 'A';
static struct dentry *dbg_dir;

static int mockup_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct mockup_gpio_controller *cntr = gpiochip_get_data(gc);

	return cntr->stats[offset].value;
}

static void mockup_gpio_set(struct gpio_chip *gc, unsigned int offset,
			    int value)
{
	struct mockup_gpio_controller *cntr = gpiochip_get_data(gc);

	cntr->stats[offset].value = !!value;
}

static int mockup_gpio_dirout(struct gpio_chip *gc, unsigned int offset,
			      int value)
{
	struct mockup_gpio_controller *cntr = gpiochip_get_data(gc);

	mockup_gpio_set(gc, offset, value);
	cntr->stats[offset].dir = OUT;

	return 0;
}

static int mockup_gpio_dirin(struct gpio_chip *gc, unsigned int offset)
{
	struct mockup_gpio_controller *cntr = gpiochip_get_data(gc);

	cntr->stats[offset].dir = IN;

	return 0;
}

static int mockup_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct mockup_gpio_controller *cntr = gpiochip_get_data(gc);

	return cntr->stats[offset].dir;
}

static ssize_t mockup_gpio_event_write(struct file *file,
				       const char __user *usr_buf,
				       size_t size, loff_t *ppos)
{
	struct gpio_desc *desc;
	struct seq_file *sfile;
	int status, val;
	char buf;

	sfile = file->private_data;
	desc = sfile->private;

	status = copy_from_user(&buf, usr_buf, 1);
	if (status)
		return status;

	if (buf == '0')
		val = 0;
	else if (buf == '1')
		val = 1;
	else
		return -EINVAL;

	gpiod_set_value_cansleep(desc, val);
	gpiod_inject_event(desc);

	return size;
}

static int mockup_gpio_event_open(struct inode *inode, struct file *file)
{
	return single_open(file, NULL, inode->i_private);
}

static const struct file_operations mockup_gpio_event_ops = {
	.owner = THIS_MODULE,
	.open = mockup_gpio_event_open,
	.write = mockup_gpio_event_write,
	.llseek = no_llseek,
};

static void mockup_gpio_debugfs_setup(struct mockup_gpio_controller *cntr)
{
	struct dentry *evfile;
	struct gpio_chip *gc;
	struct device *dev;
	char *name;
	int i;

	gc = &cntr->gc;
	dev = &gc->gpiodev->dev;

	cntr->dbg_dir = debugfs_create_dir(gc->label, dbg_dir);
	if (!cntr->dbg_dir)
		goto err;

	for (i = 0; i < gc->ngpio; i++) {
		name = devm_kasprintf(dev, GFP_KERNEL, "%d", i);
		if (!name)
			goto err;

		evfile = debugfs_create_file(name, 0200, cntr->dbg_dir,
					     &gc->gpiodev->descs[i],
					     &mockup_gpio_event_ops);
		if (!evfile)
			goto err;
	}

	return;

err:
	dev_err(dev, "error creating debugfs directory\n");
}

static int mockup_gpio_add(struct device *dev,
			   struct mockup_gpio_controller *cntr,
			   const char *name, int base, int ngpio)
{
	struct gpio_chip *gc = &cntr->gc;
	char **names;
	int ret, i;

	gc->base = base;
	gc->ngpio = ngpio;
	gc->label = name;
	gc->owner = THIS_MODULE;
	gc->parent = dev;
	gc->get = mockup_gpio_get;
	gc->set = mockup_gpio_set;
	gc->direction_output = mockup_gpio_dirout;
	gc->direction_input = mockup_gpio_dirin;
	gc->get_direction = mockup_gpio_get_direction;
	gc->mockup = true;
	cntr->stats = devm_kzalloc(dev, sizeof(*cntr->stats) * gc->ngpio,
				   GFP_KERNEL);
	if (!cntr->stats) {
		ret = -ENOMEM;
		goto err;
	}

	if (gpio_mockup_named_lines) {
		names = devm_kzalloc(dev,
				     sizeof(char *) * gc->ngpio, GFP_KERNEL);
		if (!names) {
			ret = -ENOMEM;
			goto err;
		}

		for (i = 0; i < gc->ngpio; i++) {
			names[i] = devm_kasprintf(dev, GFP_KERNEL,
						  "%s-%d", gc->label, i);
			if (!names[i]) {
				ret = -ENOMEM;
				goto err;
			}
		}

		gc->names = (const char *const*)names;
	}

	ret = devm_gpiochip_add_data(dev, gc, cntr);
	if (ret)
		goto err;

	if (dbg_dir)
		mockup_gpio_debugfs_setup(cntr);

	dev_info(dev, "gpio<%d..%d> add successful!", base, base + ngpio);
	return 0;
err:
	dev_err(dev, "gpio<%d..%d> add failed!", base, base + ngpio);
	return ret;
}

static int mockup_gpio_probe(struct platform_device *pdev)
{
	struct mockup_gpio_controller *cntr;
	struct device *dev = &pdev->dev;
	int ret, i, base, ngpio;
	char *chip_name;

	if (gpio_mockup_params_nr < 2)
		return -EINVAL;

	cntr = devm_kzalloc(dev, sizeof(*cntr) * (gpio_mockup_params_nr >> 1),
			    GFP_KERNEL);
	if (!cntr)
		return -ENOMEM;

	platform_set_drvdata(pdev, cntr);

	for (i = 0; i < gpio_mockup_params_nr >> 1; i++) {
		base = gpio_mockup_ranges[i * 2];
		if (base == -1)
			ngpio = gpio_mockup_ranges[i * 2 + 1];
		else
			ngpio = gpio_mockup_ranges[i * 2 + 1] - base;

		if (ngpio >= 0) {
			chip_name = devm_kasprintf(dev, GFP_KERNEL,
						   "%s-%c", GPIO_NAME,
						   pins_name_start + i);
			if (!chip_name)
				return -ENOMEM;

			ret = mockup_gpio_add(dev, &cntr[i],
					      chip_name, base, ngpio);
		} else {
			ret = -1;
		}
		if (ret) {
			if (base < 0)
				dev_err(dev, "gpio<%d..%d> add failed\n",
					base, ngpio);
			else
				dev_err(dev, "gpio<%d..%d> add failed\n",
					base, base + ngpio);

			return ret;
		}
	}

	return 0;
}

static struct platform_driver mockup_gpio_driver = {
	.driver = {
		.name = GPIO_NAME,
	},
	.probe = mockup_gpio_probe,
};

static struct platform_device *pdev;
static int __init mock_device_init(void)
{
	int err;

	dbg_dir = debugfs_create_dir("gpio-mockup-event", NULL);
	if (!dbg_dir)
		pr_err("%s: error creating debugfs directory\n", GPIO_NAME);

	pdev = platform_device_alloc(GPIO_NAME, -1);
	if (!pdev)
		return -ENOMEM;

	err = platform_device_add(pdev);
	if (err) {
		platform_device_put(pdev);
		return err;
	}

	err = platform_driver_register(&mockup_gpio_driver);
	if (err) {
		platform_device_unregister(pdev);
		return err;
	}

	return 0;
}

static void __exit mock_device_exit(void)
{
	debugfs_remove_recursive(dbg_dir);
	platform_driver_unregister(&mockup_gpio_driver);
	platform_device_unregister(pdev);
}

module_init(mock_device_init);
module_exit(mock_device_exit);

MODULE_AUTHOR("Kamlakant Patel <kamlakant.patel@broadcom.com>");
MODULE_AUTHOR("Bamvor Jian Zhang <bamvor.zhangjian@linaro.org>");
MODULE_DESCRIPTION("GPIO Testing driver");
MODULE_LICENSE("GPL v2");
