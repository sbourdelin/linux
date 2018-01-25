// Copyright (C) 2018 Intel Corporation
// SPDX-License-Identifier: GPL-2.0

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

#define DW9807_NAME		"dw9807"
#define DW9807_MAX_FOCUS_POS	1023
/*
 * This sets the minimum granularity for the focus positions.
 * A value of 1 gives maximum accuracy for a desired focus position
 */
#define DW9807_FOCUS_STEPS	1
/*
 * This acts as the minimum granularity of lens movement.
 * Keep this value power of 2, so the control steps can be
 * uniformly adjusted for gradual lens movement, with desired
 * number of control steps.
 */
#define DW9807_CTRL_STEPS	16
#define DW9807_CTRL_DELAY_US	1000

#define DW9807_CTL_ADDR		0x02
/*
 * DW9807 separates two registers to control the VCM position.
 * One for MSB value, another is LSB value.
 */
#define DW9807_MSB_ADDR		0x03
#define DW9807_LSB_ADDR		0x04
#define DW9807_STATUS_ADDR	0x05
#define DW9807_MODE_ADDR	0x06
#define DW9807_RESONANCE_ADDR	0x07

#define MAX_RETRY		10

struct dw9807_device {
	struct i2c_client *client;
	struct v4l2_ctrl_handler ctrls_vcm;
	struct v4l2_subdev sd;
	u16 current_val;
};

static inline struct dw9807_device *to_dw9807_vcm(struct v4l2_ctrl *ctrl)
{
	return container_of(ctrl->handler, struct dw9807_device, ctrls_vcm);
}

static inline struct dw9807_device *sd_to_dw9807_vcm(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct dw9807_device, sd);
}

static int dw9807_i2c_check(struct i2c_client *client)
{
	int ret;
	int status_addr = DW9807_STATUS_ADDR;
	u8 status_result = 0x1;

	ret = i2c_master_send(client, (const char *)&status_addr, sizeof(status_addr));
	if (ret != sizeof(status_addr)) {
		dev_err(&client->dev, "I2C write STATUS address fail ret = %d\n",
			ret);
		return -EIO;
	}

	ret = i2c_master_recv(client, (char *)&status_result, sizeof(status_result));
	if (ret != sizeof(status_result)) {
		dev_err(&client->dev, "I2C read STATUS value fail ret=%d\n",
			ret);
		return -EIO;
	}

	return status_result;
}

static int dw9807_i2c_write(struct i2c_client *client, u16 data)
{
	int ret;
	u8 tx_lsb[2];
	u8 tx_msb[2];
	int retry = 0;

	tx_lsb[0] = DW9807_LSB_ADDR;
	tx_lsb[1] = (u8)(data & 0xFF);

	tx_msb[0] = DW9807_MSB_ADDR;
	tx_msb[1] = (u8)((data >> 8) & 0x03);

	/* According to the datasheet, need to check the bus status before we
	 * write VCM position. This ensure that we really write the value
	 * into the register
	 */
	while (dw9807_i2c_check(client) != 0) {
		if (MAX_RETRY == ++retry) {
			dev_err(&client->dev, "Cannot do the write operation because VCM is busy\n");
			return -EIO;
		}
		usleep_range(DW9807_CTRL_DELAY_US, DW9807_CTRL_DELAY_US + 10);
	}

	/* Write MSB value to register */
	ret = i2c_master_send(client, (const char *)&tx_msb, sizeof(tx_msb));
	if (ret != sizeof(tx_msb)) {
		dev_err(&client->dev, "I2C write MSB fail\n");
		return -EIO;
	}

	retry = 0;
	while (dw9807_i2c_check(client) != 0) {
		if (MAX_RETRY == ++retry) {
			dev_err(&client->dev, "Cannot do the write operation because VCM is busy\n");
			return -EIO;
		}
		usleep_range(DW9807_CTRL_DELAY_US, DW9807_CTRL_DELAY_US + 10);
	}

	/* Write LSB value to register */
	ret = i2c_master_send(client, (const char *)&tx_lsb, sizeof(tx_lsb));
	if (ret != sizeof(tx_lsb)) {
		dev_err(&client->dev, "I2C write LSB fail\n");
		return -EIO;
	}

	return 0;
}

static int dw9807_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct dw9807_device *dev_vcm = to_dw9807_vcm(ctrl);

	if (ctrl->id == V4L2_CID_FOCUS_ABSOLUTE) {
		struct i2c_client *client = dev_vcm->client;

		dev_vcm->current_val = ctrl->val;
		return dw9807_i2c_write(client, ctrl->val);
	}

	return -EINVAL;
}

static const struct v4l2_ctrl_ops dw9807_vcm_ctrl_ops = {
	.s_ctrl = dw9807_set_ctrl,
};

static int dw9807_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct dw9807_device *dw9807_dev = sd_to_dw9807_vcm(sd);
	struct device *dev = &dw9807_dev->client->dev;
	int rval;

	rval = pm_runtime_get_sync(dev);
	if (rval < 0) {
		pm_runtime_put_noidle(dev);
		return rval;
	}

	return 0;
}

static int dw9807_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct dw9807_device *dw9807_dev = sd_to_dw9807_vcm(sd);
	struct device *dev = &dw9807_dev->client->dev;

	pm_runtime_put(dev);

	return 0;
}

static const struct v4l2_subdev_internal_ops dw9807_int_ops = {
	.open = dw9807_open,
	.close = dw9807_close,
};

static const struct v4l2_subdev_ops dw9807_ops = { };

static void dw9807_subdev_cleanup(struct dw9807_device *dw9807_dev)
{
	v4l2_async_unregister_subdev(&dw9807_dev->sd);
	v4l2_ctrl_handler_free(&dw9807_dev->ctrls_vcm);
	media_entity_cleanup(&dw9807_dev->sd.entity);
}

static int dw9807_init_controls(struct dw9807_device *dev_vcm)
{
	struct v4l2_ctrl_handler *hdl = &dev_vcm->ctrls_vcm;
	const struct v4l2_ctrl_ops *ops = &dw9807_vcm_ctrl_ops;
	struct i2c_client *client = dev_vcm->client;

	v4l2_ctrl_handler_init(hdl, 1);

	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FOCUS_ABSOLUTE,
			  0, DW9807_MAX_FOCUS_POS, DW9807_FOCUS_STEPS, 0);

	dev_vcm->sd.ctrl_handler = hdl;
	if (hdl->error) {
		dev_err(&client->dev, "%s fail error: 0x%x\n",
			__func__, hdl->error);
		return hdl->error;
	}

	return 0;
}

static int dw9807_probe(struct i2c_client *client)
{
	struct dw9807_device *dw9807_dev;
	int rval;

	dw9807_dev = devm_kzalloc(&client->dev, sizeof(*dw9807_dev),
				  GFP_KERNEL);
	if (dw9807_dev == NULL)
		return -ENOMEM;

	dw9807_dev->client = client;

	v4l2_i2c_subdev_init(&dw9807_dev->sd, client, &dw9807_ops);
	dw9807_dev->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	dw9807_dev->sd.internal_ops = &dw9807_int_ops;

	rval = dw9807_init_controls(dw9807_dev);
	if (rval)
		goto err_cleanup;

	rval = media_entity_pads_init(&dw9807_dev->sd.entity, 0, NULL);
	if (rval < 0)
		goto err_cleanup;

	dw9807_dev->sd.entity.function = MEDIA_ENT_F_LENS;

	rval = v4l2_async_register_subdev(&dw9807_dev->sd);
	if (rval < 0)
		goto err_cleanup;

	pm_runtime_set_active(&client->dev);
	pm_runtime_enable(&client->dev);
	pm_runtime_idle(&client->dev);

	return 0;

err_cleanup:
	dw9807_subdev_cleanup(dw9807_dev);
	dev_err(&client->dev, "Probe failed: %d\n", rval);
	return rval;
}

static int dw9807_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct dw9807_device *dw9807_dev = sd_to_dw9807_vcm(sd);

	pm_runtime_disable(&client->dev);
	pm_runtime_set_suspended(&client->dev);

	dw9807_subdev_cleanup(dw9807_dev);

	return 0;
}

/*
 * This function sets the vcm position, so it consumes least current
 * The lens position is gradually moved in units of DW9807_CTRL_STEPS,
 * to make the movements smoothly.
 */
static int __maybe_unused dw9807_vcm_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct dw9807_device *dw9807_dev = sd_to_dw9807_vcm(sd);
	int ret, val;
	u8 tx_data[2];

	for (val = dw9807_dev->current_val & ~(DW9807_CTRL_STEPS - 1);
	     val >= 0; val -= DW9807_CTRL_STEPS) {
		ret = dw9807_i2c_write(client, val);
		if (ret)
			dev_err_once(dev, "%s I2C failure: %d", __func__, ret);
		usleep_range(DW9807_CTRL_DELAY_US, DW9807_CTRL_DELAY_US + 10);
	}

	/* Power down */
	tx_data[0] = DW9807_CTL_ADDR;
	tx_data[1] = 0x01;

	ret = i2c_master_send(client, (const char *)&tx_data, sizeof(tx_data));

	if (ret != sizeof(tx_data)) {
		dev_err(&client->dev, "I2C write CTL fail\n");
		return -EIO;
	}

	return 0;
}

/*
 * This function sets the vcm position to the value set by the user
 * through v4l2_ctrl_ops s_ctrl handler
 * The lens position is gradually moved in units of DW9807_CTRL_STEPS,
 * to make the movements smoothly.
 */
static int  __maybe_unused dw9807_vcm_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct dw9807_device *dw9807_dev = sd_to_dw9807_vcm(sd);
	int ret, val;
	u8 tx_data[2];

	/* Power on */
	tx_data[0] = DW9807_CTL_ADDR;
	tx_data[1] = 0x00;

	ret = i2c_master_send(client, (const char *)&tx_data, sizeof(tx_data));
	if (ret != sizeof(tx_data)) {
		dev_err(&client->dev, "I2C write CTL fail\n");
		return -EIO;
	}

	for (val = dw9807_dev->current_val % DW9807_CTRL_STEPS;
	     val < dw9807_dev->current_val + DW9807_CTRL_STEPS - 1;
	     val += DW9807_CTRL_STEPS) {
		ret = dw9807_i2c_write(client, val);
		if (ret)
			dev_err_ratelimited(dev, "%s I2C failure: %d",
						__func__, ret);
		usleep_range(DW9807_CTRL_DELAY_US, DW9807_CTRL_DELAY_US + 10);
	}

	return 0;
}

static const struct i2c_device_id dw9807_id_table[] = {
	{ DW9807_NAME, 0},
	{ { 0 } }
};

MODULE_DEVICE_TABLE(i2c, dw9807_id_table);

static const struct of_device_id dw9807_of_table[] = {
	{ .compatible = "dongwoon,dw9807" },
	{ { 0 } }
};
MODULE_DEVICE_TABLE(of, dw9807_of_table);

static const struct dev_pm_ops dw9807_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dw9807_vcm_suspend, dw9807_vcm_resume)
	SET_RUNTIME_PM_OPS(dw9807_vcm_suspend, dw9807_vcm_resume, NULL)
};

static struct i2c_driver dw9807_i2c_driver = {
	.driver = {
		.name = DW9807_NAME,
		.pm = &dw9807_pm_ops,
		.of_match_table = dw9807_of_table,
	},
	.probe_new = dw9807_probe,
	.remove = dw9807_remove,
	.id_table = dw9807_id_table,
};

module_i2c_driver(dw9807_i2c_driver);

MODULE_DESCRIPTION("DW9807 VCM driver");
MODULE_LICENSE("GPL v2");
