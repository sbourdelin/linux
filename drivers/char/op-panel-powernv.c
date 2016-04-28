/*
 * OPAL Operator Panel Display Driver
 *
 * (C) Copyright IBM Corp. 2016
 *
 * Author: Suraj Jitindar Singh <sjitindarsingh@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>

#include <asm/opal.h>
#include <asm/opal-api.h>

/*
 * This driver creates a character device (/dev/oppanel) which exposes the
 * operator panel (2x16 character LCD display) on IBM Power Systems machines
 * with FSPs.
 * A 32 character buffer written to the device will be displayed on the
 * operator panel.
 */

static DEFINE_MUTEX(oppanel_mutex);

static oppanel_line_t	*oppanel_lines;
static char		*oppanel_data;
static u32		line_length, num_lines;

static loff_t oppanel_llseek(struct file *filp, loff_t offset, int whence)
{
	return fixed_size_llseek(filp, offset, whence, num_lines *
		line_length);
}

static ssize_t oppanel_read(struct file *filp, char __user *userbuf, size_t len,
		loff_t *f_pos)
{
	return simple_read_from_buffer(userbuf, len, f_pos, oppanel_data,
		(num_lines * line_length));
}

static int __op_panel_write(void)
{
	int rc, token;
	struct opal_msg msg;

	token = opal_async_get_token_interruptible();
	if (token < 0) {
		if (token != -ERESTARTSYS)
			pr_err("Couldn't get OPAL async token [token=%d]\n",
				token);
		return token;
	}

	rc = opal_write_oppanel_async(token, oppanel_lines, (u64) num_lines);
	switch (rc) {
	case OPAL_ASYNC_COMPLETION:
		rc = opal_async_wait_response(token, &msg);
		if (rc) {
			pr_err("Failed to wait for async response [rc=%d]\n",
				rc);
			goto out_token;
		}
		rc = be64_to_cpu(msg.params[1]);
		if (rc != OPAL_SUCCESS) {
			pr_err("OPAL async call returned failed [rc=%d]\n", rc);
			goto out_token;
		}
	case OPAL_SUCCESS:
		break;
	default:
		pr_err("OPAL write op-panel call failed [rc=%d]\n", rc);
	}

out_token:
	opal_async_release_token(token);
	return rc;
}

static ssize_t oppanel_write(struct file *filp, const char __user *userbuf,
		size_t len, loff_t *f_pos)
{
	ssize_t ret;
	loff_t f_pos_prev = *f_pos;
	int rc;

	if (*f_pos >= (num_lines * line_length))
		return -EFBIG;

	ret = simple_write_to_buffer(oppanel_data, (num_lines *
			line_length), f_pos, userbuf, len);
	if (ret > 0) {
		rc = __op_panel_write();
		if (rc != OPAL_SUCCESS) {
			pr_err("OPAL call failed to write to op panel display [rc=%d]\n",
				rc);
			*f_pos = f_pos_prev;
			return -EIO;
		}
	}
	return ret;
}

static int oppanel_open(struct inode *inode, struct file *filp)
{
	if (!mutex_trylock(&oppanel_mutex)) {
		pr_debug("Device Busy\n");
		return -EBUSY;
	}
	return 0;
}

static int oppanel_release(struct inode *inode, struct file *filp)
{
	mutex_unlock(&oppanel_mutex);
	return 0;
}

static const struct file_operations oppanel_fops = {
	.owner		= THIS_MODULE,
	.llseek		= oppanel_llseek,
	.read		= oppanel_read,
	.write		= oppanel_write,
	.open		= oppanel_open,
	.release	= oppanel_release
};

static struct miscdevice oppanel_dev = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= "oppanel",
	.fops		= &oppanel_fops
};

static int oppanel_probe(struct platform_device *pdev)
{
	int rc, i;
	struct device_node *dev_node = pdev->dev.of_node;
	const u32 *length_val, *lines_val;

	if (strncmp(dev_node->name, "oppanel", 7)) {
		pr_err("Operator panel not found\n");
		return -1;
	}

	length_val = of_get_property(dev_node, "#length", NULL);
	if (!length_val) {
		pr_err("Operator panel length property not found\n");
		return -1;
	}
	line_length = be32_to_cpu(*length_val);
	lines_val = of_get_property(dev_node, "#lines", NULL);
	if (!lines_val) {
		pr_err("Operator panel lines property not found\n");
		return -1;
	}
	num_lines = be32_to_cpu(*lines_val);

	pr_debug("Operator panel found with %u lines of length %u\n",
			num_lines, line_length);

	oppanel_data = kcalloc((num_lines * line_length), sizeof(char),
		GFP_KERNEL);
	if (!oppanel_data)
		return -ENOMEM;

	oppanel_lines = kcalloc(num_lines, sizeof(oppanel_line_t), GFP_KERNEL);
	if (!oppanel_lines) {
		kfree(oppanel_data);
		return -ENOMEM;
	}

	memset(oppanel_data, ' ', (num_lines * line_length));
	for (i = 0; i < num_lines; i++) {
		oppanel_lines[i].line_len = cpu_to_be64((u64) line_length);
		oppanel_lines[i].line = cpu_to_be64((u64) &oppanel_data[i *
						line_length]);
	}

	mutex_init(&oppanel_mutex);

	rc = misc_register(&oppanel_dev);
	if (rc) {
		pr_err("Failed to register as misc device\n");
		goto remove_mutex;
	}

	pr_info("Device Successfully Initialised\n");
	return 0;

remove_mutex:
	mutex_destroy(&oppanel_mutex);
	kfree(oppanel_lines);
	kfree(oppanel_data);
	return rc;
}

static int oppanel_remove(struct platform_device *pdev)
{
	misc_deregister(&oppanel_dev);
	mutex_destroy(&oppanel_mutex);
	kfree(oppanel_lines);
	kfree(oppanel_data);
	pr_info("Device Successfully Removed\n");
	return 0;
}

static const struct of_device_id oppanel_match[] = {
	{ .compatible = "ibm,opal-oppanel" },
	{ },
};

static struct platform_driver oppanel_driver = {
	.driver	= {
		.name		= "op-panel-powernv",
		.of_match_table	= oppanel_match,
	},
	.probe	= oppanel_probe,
	.remove	= oppanel_remove,
};

module_platform_driver(oppanel_driver);

MODULE_DEVICE_TABLE(of, oppanel_match);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("PowerNV Operator Panel LCD Display Driver");
MODULE_AUTHOR("Suraj Jitindar Singh <sjitindarsingh@gmail.com>");
