/*
 * Atmel Atmegaxx Capacitive Touch Driver
 *
 * Copyright (C) 2016 Google, inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * For raw i2c access from userspace, use i2cset/i2cget
 * to poke at /dev/i2c-N devices.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/slab.h>

/* Maximum number of buttons supported */
#define MAX_NUM_OF_BUTTONS		8

/* Registers */
#define REG_KEY1_THRESHOLD		0x02
#define REG_KEY2_THRESHOLD		0x03
#define REG_KEY3_THRESHOLD		0x04
#define REG_KEY4_THRESHOLD		0x05

#define REG_KEY1_REF_H			0x20
#define REG_KEY1_REF_L			0x21
#define REG_KEY2_REF_H			0x22
#define REG_KEY2_REF_L			0x23
#define REG_KEY3_REF_H			0x24
#define REG_KEY3_REF_L			0x25
#define REG_KEY4_REF_H			0x26
#define REG_KEY4_REF_L			0x27

#define REG_KEY1_DLT_H			0x30
#define REG_KEY1_DLT_L			0x31
#define REG_KEY2_DLT_H			0x32
#define REG_KEY2_DLT_L			0x33
#define REG_KEY3_DLT_H			0x34
#define REG_KEY3_DLT_L			0x35
#define REG_KEY4_DLT_H			0x36
#define REG_KEY4_DLT_L			0x37

#define REG_KEY_STATE			0x3C

/*
 * @i2c_client: I2C slave device client pointer
 * @input: Input device pointer
 * @num_btn: Number of buttons
 * @keycodes: map of button# to KeyCode
 * @prev_btn: Previous key state to detect button "press" or "release"
 * @xfer_buf: I2C transfer buffer
 */
struct atmegaxx_captouch_device {
	struct i2c_client *client;
	struct input_dev *input;
	u32 num_btn;
	u32 keycodes[MAX_NUM_OF_BUTTONS];
	u8 prev_btn;
	u8 xfer_buf[8] ____cacheline_aligned;
};

/*
 * Read from I2C slave device
 * The protocol is that the client has to provide both the register address
 * and the length, and while reading back the device would prepend the data
 * with address and length for verification.
 */
static int atmegaxx_read(struct atmegaxx_captouch_device *atmegaxx,
			 u8 reg, u8 *data, size_t len)
{
	struct i2c_client *client = atmegaxx->client;
	struct device *dev = &client->dev;
	struct i2c_msg msg[2];
	int err;

	if (len > sizeof(atmegaxx->xfer_buf) - 2)
		return -EINVAL;

	atmegaxx->xfer_buf[0] = reg;
	atmegaxx->xfer_buf[1] = len;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = atmegaxx->xfer_buf;
	msg[0].len = 2;

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = atmegaxx->xfer_buf;
	msg[1].len = len + 2;

	err = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (err != ARRAY_SIZE(msg))
		return err < 0 ? err : -EIO;

	if (atmegaxx->xfer_buf[0] != reg) {
		dev_err(dev, "I2C read error: register address does not match\n");
		return -ECOMM;
	}

	memcpy(data, &atmegaxx->xfer_buf[2], len);

	return 0;
}

/*
 * Handle interrupt and report the key changes to the input system.
 * Multi-touch can be supported; however, it really depends on whether
 * the device can multi-touch.
 */
static irqreturn_t atmegaxx_captouch_isr(int irq, void *data)
{
	struct atmegaxx_captouch_device *atmegaxx = data;
	struct device *dev = &atmegaxx->client->dev;
	int error;
	int i;
	u8 new_btn;
	u8 changed_btn;

	error = atmegaxx_read(atmegaxx, REG_KEY_STATE, &new_btn, 1);
	if (error) {
		dev_err(dev, "failed to read button state: %d\n", error);
		goto out;
	}

	dev_dbg(dev, "%s: button state %#02x\n", __func__, new_btn);

	changed_btn = new_btn ^ atmegaxx->prev_btn;
	atmegaxx->prev_btn = new_btn;

	for (i = 0; i < atmegaxx->num_btn; i++) {
		if (changed_btn & BIT(i))
			input_report_key(atmegaxx->input,
					 atmegaxx->keycodes[i],
					 new_btn & BIT(i));
	}

	input_sync(atmegaxx->input);

out:
	return IRQ_HANDLED;
}

/*
 * Probe function to setup the device, input system and interrupt
 */
static int atmegaxx_captouch_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	struct atmegaxx_captouch_device *atmegaxx;
	struct device *dev = &client->dev;
	struct device_node *node;
	int i;
	int err;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA |
					I2C_FUNC_SMBUS_WORD_DATA |
					I2C_FUNC_SMBUS_I2C_BLOCK)) {
		dev_err(dev, "needed i2c functionality is not supported\n");
		return -EINVAL;
	}

	atmegaxx = devm_kzalloc(dev, sizeof(*atmegaxx), GFP_KERNEL);
	if (!atmegaxx)
		return -ENOMEM;

	atmegaxx->client = client;
	i2c_set_clientdata(client, atmegaxx);

	err = atmegaxx_read(atmegaxx, REG_KEY_STATE,
			    &atmegaxx->prev_btn, sizeof(atmegaxx->prev_btn));
	if (err) {
		dev_err(dev, "failed to read initial button state: %d\n", err);
		return err;
	}

	atmegaxx->input = devm_input_allocate_device(dev);
	if (!atmegaxx->input) {
		dev_err(dev, "failed to allocate input device\n");
		return -ENOMEM;
	}

	atmegaxx->input->id.bustype = BUS_I2C;
	atmegaxx->input->id.product = 0x880A;
	atmegaxx->input->id.version = 0;
	atmegaxx->input->name = "ATMegaXX Capacitive Button Controller";
	__set_bit(EV_KEY, atmegaxx->input->evbit);

	node = dev->of_node;
	if (!node) {
		dev_err(dev, "failed to find matching node in device tree\n");
		return -EINVAL;
	}

	if (of_property_read_bool(node, "autorepeat"))
		__set_bit(EV_REP, atmegaxx->input->evbit);

	atmegaxx->num_btn = of_property_count_u32_elems(node, "linux,keymap");
	if (atmegaxx->num_btn > MAX_NUM_OF_BUTTONS)
		atmegaxx->num_btn = MAX_NUM_OF_BUTTONS;

	err = of_property_read_u32_array(node, "linux,keycodes",
					 atmegaxx->keycodes,
					 atmegaxx->num_btn);
	if (err) {
		dev_err(dev,
			"failed to read linux,keycode property: %d\n", err);
		return err;
	}

	for (i = 0; i < atmegaxx->num_btn; i++)
		__set_bit(atmegaxx->keycodes[i], atmegaxx->input->keybit);

	atmegaxx->input->keycode = atmegaxx->keycodes;
	atmegaxx->input->keycodesize = sizeof(atmegaxx->keycodes[0]);
	atmegaxx->input->keycodemax = atmegaxx->num_btn;

	err = input_register_device(atmegaxx->input);
	if (err)
		return err;

	err = devm_request_threaded_irq(dev, client->irq, NULL,
				atmegaxx_captouch_isr, IRQF_ONESHOT,
				"atmegaxx_captouch", atmegaxx);
	if (err) {
		dev_err(dev, "failed to request irq %d: %d\n",
			client->irq, err);
		return err;
	}

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id atmegaxx_captouch_of_id[] = {
	{
		.compatible = "atmel,atmegaxx_captouch",
	},
	{ /* sentinel */ }
}
MODULE_DEVICE_TABLE(of, atmegaxx_captouch_of_id);
#endif

static const struct i2c_device_id atmegaxx_captouch_id[] = {
	{ "atmegaxx_captouch", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, atmegaxx_captouch_id);

static struct i2c_driver atmegaxx_captouch_driver = {
	.probe		= atmegaxx_captouch_probe,
	.id_table	= atmegaxx_captouch_id,
	.driver		= {
		.name	= "atmegaxx_captouch",
		.of_match_table = of_match_ptr(atmegaxx_captouch_of_id),
	},
};
module_i2c_driver(atmegaxx_captouch_driver);

/* Module information */
MODULE_AUTHOR("Hung-yu Wu <hywu@google.com>");
MODULE_DESCRIPTION("Atmel ATmegaXX Capacitance Touch Sensor I2C Driver");
MODULE_LICENSE("GPL v2");
