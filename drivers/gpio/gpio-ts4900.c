/*
 * Digital I/O driver for Technologic Systems I2C FPGA Core
 *
 * Copyright (C) 2015 Technologic Systems
 * Copyright (C) 2016 Savoir-Faire Linux
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether expressed or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License version 2 for more details.
 */

#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/of_device.h>
#include <linux/module.h>

#define DEFAULT_PIN_NUMBER	32
/*
 * Register bits used by the GPIO device
 * Some boards, such as TS-7970 do not have a separate input bit
 */
#define TS4900_GPIO_OE		0x01
#define TS4900_GPIO_OD		0x02
#define TS4900_GPIO_ID		0x04
#define TS7970_GPIO_ID		0x02

struct ts4900_gpio_priv {
	struct i2c_client *client;
	struct gpio_chip gpio_chip;
	unsigned int input_bit;
};

static int ts4900_gpio_write(struct i2c_client *client, u16 addr, u8 data)
{
	struct i2c_msg msg;
	u8 buf[3];
	int ret;

	buf[0] = addr >> 8;
	buf[1] = addr & 0xFF;
	buf[2] = data;

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = 3;
	msg.buf = buf;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret != 1) {
		dev_err(&client->dev, "%s: write error, ret=%d\n",
			__func__, ret);
		return -EIO;
	}

	return 0;
}

static int ts4900_gpio_read(struct i2c_client *client, u16 addr)
{
	struct i2c_msg msgs[2];
	u8 buf[2];
	int ret;

	buf[0] = addr >> 8;
	buf[1] = addr & 0xFF;

	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 2;
	msgs[0].buf = buf;

	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = 1;
	msgs[1].buf = buf;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs)) {
		dev_err(&client->dev, "%s: read error, ret=%d\n",
			__func__, ret);
		return -EIO;
	}

	return buf[0];
}

static int __ts4900_gpio_direction_output(struct i2c_client *client, int gpio,
				     int value)
{
	u8 reg = 0;

	if (value)
		reg = TS4900_GPIO_OD | TS4900_GPIO_OE;
	else
		reg = TS4900_GPIO_OE;

	return ts4900_gpio_write(client, gpio, reg);
}

static int ts4900_gpio_direction_input(struct gpio_chip *chip,
				       unsigned int offset)
{
	struct ts4900_gpio_priv *priv = gpiochip_get_data(chip);

	/*
	 * This will clear the output enable bit, the other bits are
	 * dontcare when this is cleared
	 */
	return ts4900_gpio_write(priv->client, offset, 0);
}

static int ts4900_gpio_direction_output(struct gpio_chip *chip,
					unsigned int offset, int value)
{
	struct ts4900_gpio_priv *priv = gpiochip_get_data(chip);

	return __ts4900_gpio_direction_output(priv->client, offset, value);
}

static int ts4900_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct ts4900_gpio_priv *priv = gpiochip_get_data(chip);
	int ret;
	u8 reg;

	reg = ts4900_gpio_read(priv->client, offset);

	ret = (reg & priv->input_bit) ? 1 : 0;

	return ret;
}

static void ts4900_gpio_set(struct gpio_chip *chip, unsigned int offset,
			    int value)
{
	struct ts4900_gpio_priv *priv = gpiochip_get_data(chip);

	__ts4900_gpio_direction_output(priv->client, offset, value);
}

static struct gpio_chip template_chip = {
	.label			= "ts4900-gpio",
	.owner			= THIS_MODULE,
	.direction_input	= ts4900_gpio_direction_input,
	.direction_output	= ts4900_gpio_direction_output,
	.get			= ts4900_gpio_get,
	.set			= ts4900_gpio_set,
	.base			= -1,
	.can_sleep		= true,
};

static const struct of_device_id ts4900_gpio_of_match_table[] = {
	{
		.compatible = "technologic,ts4900-gpio",
		.data = (void *)TS4900_GPIO_ID,
	}, {
		.compatible = "technologic,ts7970-gpio",
		.data = (void *)TS7970_GPIO_ID,
	},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, ts4900_gpio_of_match_table);

static int ts4900_gpio_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	const struct of_device_id *match;
	struct ts4900_gpio_priv *priv;
	u32 ngpio;
	int ret;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA))
		return -EIO;

	match = of_match_device(ts4900_gpio_of_match_table, &client->dev);
	if (!match)
		return -EINVAL;

	if (of_property_read_u32(client->dev.of_node, "ngpios", &ngpio))
		ngpio = DEFAULT_PIN_NUMBER;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	i2c_set_clientdata(client, priv);

	priv->client = client;
	priv->gpio_chip = template_chip;
	priv->gpio_chip.label = "ts4900-gpio";
	priv->gpio_chip.ngpio = ngpio;
	priv->gpio_chip.parent = &client->dev;
	priv->input_bit = (uintptr_t)match->data;

	ret = gpiochip_add_data(&priv->gpio_chip, priv);
	if (ret < 0) {
		dev_err(&client->dev, "Unable to register gpiochip\n");
		return ret;
	}

	return 0;
}

static int ts4900_gpio_remove(struct i2c_client *client)
{
	struct ts4900_gpio_priv *priv = i2c_get_clientdata(client);

	gpiochip_remove(&priv->gpio_chip);

	return 0;
}

static const struct i2c_device_id ts4900_gpio_id_table[] = {
	{ "ts4900-gpio", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, ts4900_gpio_id_table);

static struct i2c_driver ts4900_gpio_driver = {
	.driver = {
		.name = "ts4900-gpio",
		.of_match_table = ts4900_gpio_of_match_table,
	},
	.probe = ts4900_gpio_probe,
	.remove = ts4900_gpio_remove,
	.id_table = ts4900_gpio_id_table,
};
module_i2c_driver(ts4900_gpio_driver);

MODULE_AUTHOR("Technologic Systems");
MODULE_DESCRIPTION("GPIO interface for Technologic Systems I2C-FPGA core");
MODULE_LICENSE("GPL");
