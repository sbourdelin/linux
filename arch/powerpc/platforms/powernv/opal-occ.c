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
#include <linux/highmem.h>
#include <linux/slab.h>
#include <asm/opal.h>

struct occ {
	struct miscdevice dev;
	struct opal_occ_rsp_data *rsp;
	atomic_t session;
	atomic_t cmd_in_progress;
	atomic_t rsp_consumed;
	int id;
	u8 request_id;
} *occs;
static int nr_occs;

static int __send_occ_command(struct opal_occ_cmd_rsp_msg *msg,
			      int chip_id, int token, bool retry)
{
	struct opal_msg async_msg;
	int rc;

	rc = opal_occ_command(chip_id, msg, token, retry);
	if (rc == OPAL_ASYNC_COMPLETION) {
		rc = opal_async_wait_response(token, &async_msg);
		if (rc) {
			pr_devel("Failed to wait for async response %d\n", rc);
			return rc;
		}
	}

	return rc ? rc : opal_get_async_rc(async_msg);
}

static int send_occ_command(struct opal_occ_cmd_rsp_msg *msg, struct occ *occ)
{
	int token, rc;

	token = opal_async_get_token_interruptible();
	if (token < 0) {
		pr_devel("Failed to get the token for OCC command %d (%d)\n",
			 msg->cmd, token);
		return token;
	}

	msg->request_id = occ->request_id++;
	rc = __send_occ_command(msg, occ->id, token, false);

	switch (rc) {
	case OPAL_OCC_CMD_TIMEOUT:
	case OPAL_OCC_RSP_MISMATCH:
		pr_devel("Failed OCC command with %d. Retrying it again\n", rc);
		msg->request_id = occ->request_id++;
		rc = __send_occ_command(msg, occ->id, token, true);
		break;
	default:
		break;
	}

	opal_async_release_token(token);
	return opal_error_code(rc);
}

static int opal_occ_cmd_prepare(struct opal_occ_cmd_data *cmd, struct occ *occ)
{
	struct opal_occ_cmd_rsp_msg msg;
	int rc;

	msg.cmd = cmd->cmd;
	msg.cdata = cpu_to_be64(__pa(cmd->data));
	msg.cdata_size = cpu_to_be16(cmd->size);
	msg.rdata = cpu_to_be64(__pa(occ->rsp->data));

	rc = send_occ_command(&msg, occ);
	if (rc) {
		pr_info("Failed OCC command %d with %d\n", cmd->cmd, rc);
		return rc;
	}

	occ->rsp->size = be16_to_cpu(msg.rdata_size);
	occ->rsp->status = msg.status;
	if (occ->rsp->size > MAX_OCC_RSP_DATA_LENGTH) {
		pr_devel("Bigger OCC response size, clipping to %d\n",
			 MAX_OCC_RSP_DATA_LENGTH);
		occ->rsp->size = MAX_OCC_RSP_DATA_LENGTH;
	}

	atomic_set(&occ->rsp_consumed, 1);
	return rc;
}

static ssize_t opal_occ_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct miscdevice *dev = file->private_data;
	struct occ *occ = container_of(dev, struct occ, dev);
	struct opal_occ_cmd_data *cmd;
	int rc;

	if (count < sizeof(*cmd))
		return -EINVAL;

	if (atomic_cmpxchg(&occ->cmd_in_progress, 0, 1))
		return -EBUSY;

	cmd = kmalloc(count, GFP_KERNEL);
	if (!cmd) {
		rc = -ENOMEM;
		goto out;
	}

	rc = copy_from_user(cmd, buf, count);
	if (rc) {
		pr_err("Failed to copy OCC command request message\n");
		rc = -EFAULT;
		goto free_cmd;
	}

	if (cmd->size > MAX_OPAL_CMD_DATA_LENGTH) {
		rc = -EINVAL;
		goto free_cmd;
	}

	rc = opal_occ_cmd_prepare(cmd, occ);
	if (!rc)
		rc = count;

free_cmd:
	kfree(cmd);
out:
	atomic_set(&occ->cmd_in_progress, 0);
	return rc;
}

static ssize_t opal_occ_read(struct file *file, char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct miscdevice *dev = file->private_data;
	struct occ *occ = container_of(dev, struct occ, dev);
	int rc;

	if (count < sizeof(*occ->rsp) + occ->rsp->size)
		return -EINVAL;

	if (atomic_cmpxchg(&occ->cmd_in_progress, 0, 1))
		return -EBUSY;

	if (!atomic_cmpxchg(&occ->rsp_consumed, 1, 0)) {
		rc = -EBUSY;
		goto out;
	}

	rc = copy_to_user((void __user *)buf, occ->rsp,
			  sizeof(occ->rsp) + occ->rsp->size);
	if (rc) {
		atomic_set(&occ->rsp_consumed, 1);
		pr_err("Failed to copy OCC response data to user\n");
	}

out:
	atomic_set(&occ->cmd_in_progress, 0);
	return rc ? rc : sizeof(*occ->rsp) + occ->rsp->size;
}

static int opal_occ_open(struct inode *inode, struct file *file)
{
	struct miscdevice *dev = file->private_data;
	struct occ *occ = container_of(dev, struct occ, dev);

	return atomic_cmpxchg(&occ->session, 0, 1) ? -EBUSY : 0;
}

static int opal_occ_release(struct inode *inode, struct file *file)
{
	struct miscdevice *dev = file->private_data;
	struct occ *occ = container_of(dev, struct occ, dev);

	atomic_set(&occ->session, 0);

	return 0;
}

static const struct file_operations opal_occ_fops = {
	.open		= opal_occ_open,
	.read		= opal_occ_read,
	.write		= opal_occ_write,
	.release	= opal_occ_release,
	.owner		= THIS_MODULE,
};

#define MAX_POSSIBLE_CHIPS	256

static int opal_occ_probe(struct platform_device *pdev)
{
	unsigned int chip[MAX_POSSIBLE_CHIPS];
	unsigned int cpu;
	unsigned int prev_chip_id = UINT_MAX;
	int i, rc;

	for_each_possible_cpu(cpu) {
		unsigned int id = cpu_to_chip_id(cpu);

		if (prev_chip_id != id) {
			int j = nr_occs;

			while (--j >= 0)
				if (chip[j] == id)
					continue;

			prev_chip_id = id;
			chip[nr_occs++] = id;
			WARN_ON_ONCE(nr_occs >= MAX_POSSIBLE_CHIPS - 1);
		}
	}

	occs = kcalloc(nr_occs, sizeof(*occs), GFP_KERNEL);
	if (!occs)
		return -ENOMEM;

	for (i = 0; i < nr_occs; i++) {
		char name[10];

		occs[i].id = chip[i];
		occs[i].dev.minor = MISC_DYNAMIC_MINOR;
		snprintf(name, 10, "occ%d", chip[i]);
		occs[i].dev.name = name;
		occs[i].dev.fops = &opal_occ_fops;
		occs[i].rsp = kmalloc(sizeof(occs[i].rsp) +
				      MAX_OCC_RSP_DATA_LENGTH,
				      GFP_KERNEL);
		if (!occs[i].rsp) {
			rc = -ENOMEM;
			goto free_occs;
		}

		rc = misc_register(&occs[i].dev);
		if (rc)
			goto free_occ_rsp_data;
	}

	return 0;

free_occ_rsp_data:
	kfree(occs[i].rsp);
free_occs:
	while (--i >= 0) {
		kfree(occs[i].rsp);
		misc_deregister(&occs[i].dev);
	}
	kfree(occs);

	return rc;
}

static int opal_occ_remove(struct platform_device *pdev)
{
	int i;

	for (i = 0; i < nr_occs; i++) {
		kfree(occs[i].rsp);
		misc_deregister(&occs[i].dev);
	}

	kfree(occs);
	return 0;
}

static const struct of_device_id opal_occ_match[] = {
	{ .compatible = "ibm,opal-occ-cmd-rsp-interface" },
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
