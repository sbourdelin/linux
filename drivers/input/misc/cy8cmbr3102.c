/* Driver for Cypress CY8CMBR3102 CapSense Express Controller
 *
 * (C) 2017 by Gigatronik Technologies GmbH
 * Author: Patrick Vogelaar <patrick.vogelaar@gigatronik.com>
 * All rights reserved.
 *
 * This code is based on mma8450.c and atmel_captouch.c.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * NOTE: This implementation does not implement the full range of functions the
 * Cypress CY8CMBR3102 CapSense Express controller provides. It only implements
 * its use for connected touch buttons (yet).
 */

#define DRV_VERSION "0.1"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input-polldev.h>
#include <linux/of.h>

/* I2C Registers */
#define CY8CMBR3102_DEVICE_ID_REG		0x90
#define CY8CMBR3102_BUTTON_STAT			0xAA


#define CY8CMBR3102_MAX_NUM_OF_BUTTONS		0x02
#define CY8CMBR3102_DRV_NAME			"cy8cmbr3102"
#define CY8CMBR3102_POLL_INTERVAL		200
#define CY8CMBR3102_POLL_INTERVAL_MAX		300
#define CY8CMBR3102_DEVICE_ID			2561
#define CY8CMBR3102_MAX_RETRY			5

/*
 * @i2c_client: I2C slave device client pointer
 * @idev: Input (polled) device pointer
 * @num_btn: Number of buttons
 * @keycodes: map of button# to KeyCode
 * @cy8cmbr3102_lock: mutex lock
 */
struct cy8cmbr3102_device {
	struct i2c_client *client;
	struct input_polled_dev *idev;
	u32 num_btn;
	u32 keycodes[CY8CMBR3102_MAX_NUM_OF_BUTTONS];
	struct mutex cy8cmbr3102_lock;
};

static const struct i2c_device_id cy8cmbr3102_idtable[] = {
		{"cy8cmbr3102", 0},
		{}
};
MODULE_DEVICE_TABLE(i2c, cy8cmbr3102_idtable);

static void cy8cmbr3102_poll(struct input_polled_dev *idev)
{
	struct cy8cmbr3102_device *dev = idev->private;
	int i, ret, btn_state;

	mutex_lock(&dev->cy8cmbr3102_lock);

	ret = i2c_smbus_read_word_data(dev->client, CY8CMBR3102_BUTTON_STAT);
	if (ret < 0) {
		dev_err(&dev->client->dev, "i2c io error: %d\n", ret);
		mutex_unlock(&dev->cy8cmbr3102_lock);
		return;
	}

	for (i = 0; i < dev->num_btn; i++) {
		btn_state = ret & (0x1 << i);
		input_report_key(idev->input, dev->keycodes[i], btn_state);
		input_sync(idev->input);
	}

	mutex_unlock(&dev->cy8cmbr3102_lock);
}

static int cy8cmbr3102_remove(struct i2c_client *client)
{
	struct cy8cmbr3102_device *dev = i2c_get_clientdata(client);
	struct input_polled_dev *idev = dev->idev;

	dev_dbg(&client->dev, "%s\n", __func__);

	mutex_destroy(&dev->cy8cmbr3102_lock);
	input_unregister_polled_device(idev);

	return 0;
}

static int cy8cmbr3102_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct cy8cmbr3102_device *drvdata;
	struct device *dev = &client->dev;
	struct device_node *node;
	int i, err, ret;

	dev_dbg(&client->dev, "%s\n", __func__);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA |
						I2C_FUNC_SMBUS_WORD_DATA |
						I2C_FUNC_SMBUS_I2C_BLOCK)) {
		dev_err(dev, "needed i2c functionality is not supported\n");
		return -EINVAL;
	}

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->client = client;
	i2c_set_clientdata(client, drvdata);

	/* device is in low-power mode and needs to be waken up */
	for (i = 0; (i < CY8CMBR3102_MAX_RETRY) &&
					(ret != CY8CMBR3102_DEVICE_ID); i++) {
		ret = i2c_smbus_read_word_data(drvdata->client,
		CY8CMBR3102_DEVICE_ID_REG);
		dev_dbg(dev, "DEVICE_ID (%i): %i\n", i, ret);
	}

	if (ret < 0) {
		dev_err(&client->dev, "i2c io error: %d\n", ret);
		return -EIO;
	} else if (ret != CY8CMBR3102_DEVICE_ID) {
		dev_err(&client->dev, "read device ID %i is not equal to %i!\n",
				ret, CY8CMBR3102_DEVICE_ID);
		return -ENXIO;
	}
	dev_dbg(dev, "device identified by device ID\n");

	drvdata->idev = devm_input_allocate_polled_device(dev);
	if (!drvdata->idev) {
		dev_err(dev, "failed to allocate input device\n");
		return -ENOMEM;
	}

	node = dev->of_node;
	if (!node) {
		dev_err(dev, "failed to find matching node in device tree\n");
		return -EINVAL;
	}

	if (of_property_read_bool(node, "autorepeat"))
		__set_bit(EV_REP, drvdata->idev->input->evbit);

	drvdata->num_btn = of_property_count_u32_elems(node, "linux,keycodes");
	if (drvdata->num_btn > CY8CMBR3102_MAX_NUM_OF_BUTTONS)
		drvdata->num_btn = CY8CMBR3102_MAX_NUM_OF_BUTTONS;

	err = of_property_read_u32_array(node, "linux,keycodes",
			drvdata->keycodes, drvdata->num_btn);

	if (err) {
		dev_err(dev, "failed to read linux,keycodes property: %d\n",
				err);
		return err;
	}

	for (i = 0; i < drvdata->num_btn; i++)
		__set_bit(drvdata->keycodes[i], drvdata->idev->input->keybit);

	drvdata->idev->input->id.bustype = BUS_I2C;
	drvdata->idev->input->id.product = 0x3102;
	drvdata->idev->input->id.version = 0;
	drvdata->idev->input->name = CY8CMBR3102_DRV_NAME;
	drvdata->idev->poll = cy8cmbr3102_poll;
	drvdata->idev->poll_interval = CY8CMBR3102_POLL_INTERVAL;
	drvdata->idev->poll_interval_max = CY8CMBR3102_POLL_INTERVAL_MAX;
	drvdata->idev->private = drvdata;
	drvdata->idev->input->keycode = drvdata->keycodes;
	drvdata->idev->input->keycodemax = drvdata->num_btn;
	drvdata->idev->input->keycodesize = sizeof(drvdata->keycodes[0]);
	__set_bit(EV_KEY, drvdata->idev->input->evbit);

	mutex_init(&drvdata->cy8cmbr3102_lock);

	err = input_register_polled_device(drvdata->idev);
	if (err)
		return err;

	dev_info(&client->dev, "chip found, driver version " DRV_VERSION "\n");
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id of_cy8cmbr3102_match[] = {
		{.compatible = "cypress,cy8cmbr3102", },
		{}
};
MODULE_DEVICE_TABLE(of, of_cy8cmbr3102_match);
#endif

static struct i2c_driver cy8cmbr3102_driver = {
		.driver			= {
			.name		= "cy8cmbr3102",
			.owner		= THIS_MODULE,
			.of_match_table	= of_match_ptr(of_cy8cmbr3102_match),
		},
		.probe = cy8cmbr3102_probe,
		.remove = cy8cmbr3102_remove,
		.id_table = cy8cmbr3102_idtable,
};
module_i2c_driver(cy8cmbr3102_driver);

MODULE_AUTHOR("Patrick Vogelaar <patrick.vogelaar@gigatronik.com>");
MODULE_DESCRIPTION("Cypress CY8CMBR3102 CapSense Express controller");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
