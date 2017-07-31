/*
 * Copyright IBM Corporation 2017
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "opal-occ: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <asm/opal.h>
#include <asm/opal-occ.h>

DEFINE_MUTEX(opal_occ_mutex);

int opal_sensor_groups_clear_history(u32 handle)
{
	struct opal_msg async_msg;
	int token, rc;

	token = opal_async_get_token_interruptible();
	if (token < 0) {
		pr_devel("Failed to get the token %d\n", token);
		return token;
	}

	rc = mutex_lock_interruptible(&opal_occ_mutex);
	if (rc)
		goto out_token;

	rc = opal_sensor_groups_clear(handle, token);
	if (rc == OPAL_ASYNC_COMPLETION) {
		rc = opal_async_wait_response(token, &async_msg);
		if (rc) {
			pr_devel("Failed to wait for async response\n");
			rc = -EIO;
			goto out;
		}
		rc = opal_get_async_rc(async_msg);
	}

	rc = opal_error_code(rc);
out:
	mutex_unlock(&opal_occ_mutex);
out_token:
	opal_async_release_token(token);
	return rc;
}
EXPORT_SYMBOL_GPL(opal_sensor_groups_clear_history);

static long opal_occ_ioctl(struct file *file, unsigned int cmd,
			   unsigned long param)
{
	int rc = -EINVAL;

	switch (cmd) {
	case OPAL_OCC_IOCTL_CLEAR_SENSOR_GROUPS:
		rc = opal_sensor_groups_clear_history(param);
		break;
	default:
		break;
	}

	return rc;
}

static const struct file_operations opal_occ_fops = {
	.unlocked_ioctl = opal_occ_ioctl,
	.owner		= THIS_MODULE,
};

static struct miscdevice occ_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "occ",
	.fops	= &opal_occ_fops,
};

static int opal_occ_probe(struct platform_device *pdev)
{
	return misc_register(&occ_dev);
}

static int opal_occ_remove(struct platform_device *pdev)
{
	misc_deregister(&occ_dev);
	return 0;
}

static const struct of_device_id opal_occ_match[] = {
	{ .compatible = "ibm,opal-occ-sensor-group" },
	{ },
};

static struct platform_driver opal_occ_driver = {
	.driver = {
		.name           = "opal-occ",
		.of_match_table = opal_occ_match,
	},
	.probe	= opal_occ_probe,
	.remove	= opal_occ_remove,
};

module_platform_driver(opal_occ_driver);

MODULE_DESCRIPTION("PowerNV OPAL-OCC driver");
MODULE_LICENSE("GPL");
