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
	int id;
	u8 last_token;
} *occs;
static int nr_occs;

static int __send_occ_command(struct opal_occ_cmd_rsp_msg *msg,
			      int chip_id, bool retry)
{
	struct opal_msg async_msg;
	int rc;

	if (!retry) {
		msg->cdata = cpu_to_be64(__pa(msg->cdata));
		msg->cdata_size = cpu_to_be16(msg->cdata_size);
		msg->rdata = cpu_to_be64(__pa(msg->rdata));
	}

	rc = opal_occ_command(chip_id, msg, retry);
	if (rc == OPAL_ASYNC_COMPLETION) {
		rc = opal_async_wait_response(msg->request_id, &async_msg);
		if (rc) {
			pr_info("Failed to wait for async response %d\n", rc);
			return rc;
		}
	} else if (rc) {
		pr_info("Failed to send opal_occ_command %d\n", rc);
		return rc;
	}

	rc = opal_get_async_rc(async_msg);
	if (rc) {
		pr_info("opal_occ_command failed with %d\n", rc);
		return rc;
	}

	msg->rdata_size = be16_to_cpu(msg->rdata_size);
	msg->rdata = be64_to_cpu(__va(msg->rdata));
	msg->cdata_size = be16_to_cpu(msg->cdata_size);
	msg->cdata = be64_to_cpu(__va(msg->cdata));

	if (msg->rdata_size > MAX_OCC_RSP_DATA_LENGTH) {
		pr_info("Opal sent bigger data, clipping to the max response size\n");
		msg->rdata_size = MAX_OCC_RSP_DATA_LENGTH;
	}

	return rc;
}

static int send_occ_command(struct opal_occ_cmd_rsp_msg *msg, struct occ *occ)
{
	int token, rc;

	token = opal_async_get_unique_token_interruptible(occ->last_token);
	if (token < 0) {
		pr_info("Failed to get the request_id/token for command %d (%d)\n",
			msg->cmd, token);
		return token;
	}

	msg->request_id = token;
	rc = __send_occ_command(msg, occ->id, false);

	switch (rc) {
	case OPAL_OCC_CMD_TIMEOUT:
	case OPAL_OCC_RSP_MISMATCH:
		occ->last_token = token;
		opal_async_release_token(token);
		token = opal_async_get_unique_token_interruptible(token);
		if (token < 0) {
			pr_info("Failed to get the request_id/token for retry command %d (%d)\n",
				msg->cmd, token);

			return opal_error_code(rc);
		}

		msg->request_id = token;
		rc = __send_occ_command(msg, occ->id, true);
		break;
	default:
		break;
	}

	occ->last_token = token;
	opal_async_release_token(token);
	return opal_error_code(rc);
}

static int opal_occ_cmd_prepare(struct opal_occ_cmd_data *cmd, struct occ *occ)
{
	struct opal_occ_cmd_rsp_msg msg;
	int rc;

	msg.cmd = cmd->cmd;
	msg.cdata_size = cmd->size;
	msg.rdata = (u64)occ->rsp->data;
	msg.cdata = (u64)cmd->data;
	rc = send_occ_command(&msg, occ);
	if (rc)
		return rc;

	occ->rsp->size = msg.rdata_size;
	occ->rsp->status = msg.status;

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

	if (atomic_xchg(&occ->cmd_in_progress, 1) == 1)
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
	atomic_xchg(&occ->cmd_in_progress, 0);
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

	rc = copy_to_user((void __user *)buf, occ->rsp,
			  sizeof(occ->rsp) + occ->rsp->size);
	if (rc) {
		pr_err("Failed to copy OCC response data to user\n");
		return rc;
	}

	return sizeof(*occ->rsp) + occ->rsp->size;
}

static int opal_occ_open(struct inode *inode, struct file *file)
{
	struct miscdevice *dev = file->private_data;
	struct occ *occ = container_of(dev, struct occ, dev);

	if (atomic_xchg(&occ->session, 1) == 1)
		return -EBUSY;

	return 0;
}

static int opal_occ_release(struct inode *inode, struct file *file)
{
	struct miscdevice *dev = file->private_data;
	struct occ *occ = container_of(dev, struct occ, dev);

	atomic_xchg(&occ->session, 0);

	return 0;
}

static const struct file_operations opal_occ_fops = {
	.open		= opal_occ_open,
	.read		= opal_occ_read,
	.write		= opal_occ_write,
	.release	= opal_occ_release,
	.owner		= THIS_MODULE,
};

static int opal_occ_probe(struct platform_device *pdev)
{
	unsigned int chip[256];
	unsigned int cpu;
	unsigned int prev_chip_id = UINT_MAX;
	int i, rc;

	for_each_possible_cpu(cpu) {
		unsigned int id = cpu_to_chip_id(cpu);

		if (prev_chip_id != id) {
			prev_chip_id = id;
			chip[nr_occs++] = id;
			WARN_ON(nr_occs > 254);
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
		occs[i].last_token = -1;
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
