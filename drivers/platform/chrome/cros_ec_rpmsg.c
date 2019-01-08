// SPDX-License-Identifier: GPL-2.0
//
// Copyright 2018 Google LLC.

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mfd/cros_ec.h>
#include <linux/mfd/cros_ec_commands.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>

/**
 * cros_ec_cmd_xfer_rpmsg - Transfer a message over rpmsg and receive the reply
 *
 * This is only used for old EC proto version, and is not supported for this
 * driver.
 *
 * @ec_dev: ChromeOS EC device
 * @ec_msg: Message to transfer
 */
static int cros_ec_cmd_xfer_rpmsg(struct cros_ec_device *ec_dev,
				  struct cros_ec_command *ec_msg)
{
	return -EINVAL;
}

/**
 * cros_ec_pkt_xfer_rpmsg - Transfer a packet over rpmsg and receive the reply
 *
 * @ec_dev: ChromeOS EC device
 * @ec_msg: Message to transfer
 */
static int cros_ec_pkt_xfer_rpmsg(struct cros_ec_device *ec_dev,
				  struct cros_ec_command *ec_msg)
{
	struct ec_host_response *response;
	struct rpmsg_device *rpdev = ec_dev->priv;
	int len;
	u8 sum;
	int ret;
	int i;

	ec_msg->result = 0;
	len = cros_ec_prepare_tx(ec_dev, ec_msg);
	dev_dbg(ec_dev->dev, "prepared, len=%d\n", len);

	/*
	 * TODO: This currently relies on that mtk_rpmsg send actually blocks
	 * until ack. Should do the wait here instead.
	 */
	ret = rpmsg_send(rpdev->ept, ec_dev->dout, len);
	if (ret) {
		dev_err(ec_dev->dev, "rpmsg send failed\n");
		return ret;
	}

	/* check response error code */
	response = (struct ec_host_response *)ec_dev->din;
	ec_msg->result = response->result;

	ret = cros_ec_check_result(ec_dev, ec_msg);
	if (ret)
		goto exit;

	if (response->data_len > ec_msg->insize) {
		dev_err(ec_dev->dev, "packet too long (%d bytes, expected %d)",
			response->data_len, ec_msg->insize);
		ret = -EMSGSIZE;
		goto exit;
	}

	/* copy response packet payload and compute checksum */
	memcpy(ec_msg->data, ec_dev->din + sizeof(*response),
	       response->data_len);

	sum = 0;
	for (i = 0; i < sizeof(*response) + response->data_len; i++)
		sum += ec_dev->din[i];

	if (sum) {
		dev_err(ec_dev->dev, "bad packet checksum, calculated %x\n",
			sum);
		ret = -EBADMSG;
		goto exit;
	}

	ret = response->data_len;
exit:
	if (ec_msg->command == EC_CMD_REBOOT_EC)
		msleep(EC_REBOOT_DELAY_MS);

	return ret;
}

static int cros_ec_rpmsg_callback(struct rpmsg_device *rpdev, void *data,
				  int len, void *priv, u32 src)
{
	struct cros_ec_device *ec_dev = dev_get_drvdata(&rpdev->dev);

	if (len > ec_dev->din_size) {
		dev_warn(ec_dev->dev,
			 "ipi received length %d > din_size, truncating", len);
		len = ec_dev->din_size;
	}

	memcpy(ec_dev->din, data, len);

	return 0;
}

static int cros_ec_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct device *dev = &rpdev->dev;
	struct cros_ec_device *ec_dev;
	int ret;

	ec_dev = devm_kzalloc(dev, sizeof(*ec_dev), GFP_KERNEL);
	if (!ec_dev)
		return -ENOMEM;

	ec_dev->dev = dev;
	ec_dev->priv = rpdev;
	ec_dev->cmd_xfer = cros_ec_cmd_xfer_rpmsg;
	ec_dev->pkt_xfer = cros_ec_pkt_xfer_rpmsg;
	ec_dev->phys_name = dev_name(&rpdev->dev);
	ec_dev->din_size = sizeof(struct ec_host_response) +
			   sizeof(struct ec_response_get_protocol_info);
	ec_dev->dout_size = sizeof(struct ec_host_request);
	dev_set_drvdata(dev, ec_dev);

	ret = cros_ec_register(ec_dev);
	if (ret) {
		dev_err(dev, "cannot register EC\n");
		return ret;
	}

	return 0;
}

static const struct rpmsg_device_id cros_ec_rpmsg_device_id[] = {
	{ .name = "cros-ec-rpmsg", },
	{ }
};
MODULE_DEVICE_TABLE(rpmsg, cros_ec_rpmsg_device_id);

static struct rpmsg_driver cros_ec_driver_rpmsg = {
	.drv.name       = KBUILD_MODNAME,
	.id_table       = cros_ec_rpmsg_device_id,
	.probe		= cros_ec_rpmsg_probe,
	.callback	= cros_ec_rpmsg_callback,
};

module_rpmsg_driver(cros_ec_driver_rpmsg);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("ChromeOS EC multi function device (rpmsg)");
