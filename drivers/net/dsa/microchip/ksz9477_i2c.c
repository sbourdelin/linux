// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip KSZ9477 series register access through I2C
 *
 * Copyright (C) 2018 Microchip Technology Inc.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/i2c.h>

#include "ksz_priv.h"
#include "ksz_i2c.h"

/* Enough to read all switch port registers. */
#define I2C_TX_BUF_LEN			0x100

static int ksz9477_i2c_read_reg(struct i2c_client *i2c, u32 reg, u8 *val,
				unsigned int len)
{
	struct i2c_msg msg[2];
	int ret = 0;

	val[0] = (u8)(reg >> 8);
	val[1] = (u8)reg;

	msg[0].addr = i2c->addr;
	msg[0].flags = 0;
	msg[0].len = 2;
	msg[0].buf = val;

	msg[1].addr = i2c->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = len;
	msg[1].buf = &val[2];

	if (i2c_transfer(i2c->adapter, msg, 2) != 2)
		ret = -ENODEV;
	return ret;
}

static int ksz9477_i2c_write_reg(struct i2c_client *i2c, u32 reg, u8 *val,
				 unsigned int len)
{
	struct i2c_msg msg;
	int ret = 0;

	val[0] = (u8)(reg >> 8);
	val[1] = (u8)reg;

	msg.addr = i2c->addr;
	msg.flags = 0;
	msg.len = 2 + len;
	msg.buf = val;

	if (i2c_transfer(i2c->adapter, &msg, 1) != 1)
		ret = -ENODEV;
	return ret;
}

static int ksz_i2c_read(struct ksz_device *dev, u32 reg, u8 *data,
			unsigned int len)
{
	struct i2c_client *i2c = dev->priv;
	int ret;

	ret = ksz9477_i2c_read_reg(i2c, reg, dev->txbuf, len);
	if (!ret)
		memcpy(data, &dev->txbuf[2], len);
	return ret;
}

static int ksz_i2c_write(struct ksz_device *dev, u32 reg, void *data,
			 unsigned int len)
{
	struct i2c_client *i2c = dev->priv;

	if (len > I2C_TX_BUF_LEN)
		len = I2C_TX_BUF_LEN;
	memcpy(&dev->txbuf[2], data, len);
	return ksz9477_i2c_write_reg(i2c, reg, dev->txbuf, len);
}

static int ksz_i2c_read24(struct ksz_device *dev, u32 reg, u32 *val)
{
	int ret;

	*val = 0;
	ret = ksz_i2c_read(dev, reg, (u8 *)val, 3);
	if (!ret) {
		*val = be32_to_cpu(*val);
		/* convert to 24bit */
		*val >>= 8;
	}

	return ret;
}

static int ksz_i2c_write24(struct ksz_device *dev, u32 reg, u32 value)
{
	/* make it to big endian 24bit from MSB */
	value <<= 8;
	value = cpu_to_be32(value);
	return ksz_i2c_write(dev, reg, &value, 3);
}

static const struct ksz_io_ops ksz9477_i2c_ops = {
	.read8 = ksz_i2c_read8,
	.read16 = ksz_i2c_read16,
	.read24 = ksz_i2c_read24,
	.read32 = ksz_i2c_read32,
	.write8 = ksz_i2c_write8,
	.write16 = ksz_i2c_write16,
	.write24 = ksz_i2c_write24,
	.write32 = ksz_i2c_write32,
	.get = ksz_i2c_get,
	.set = ksz_i2c_set,
};

static int ksz9477_i2c_probe(struct i2c_client *i2c,
			     const struct i2c_device_id *i2c_id)
{
	struct ksz_device *dev;
	int ret;

	dev = ksz_switch_alloc(&i2c->dev, &ksz9477_i2c_ops, i2c);
	if (!dev)
		return -ENOMEM;

	if (i2c->dev.platform_data)
		dev->pdata = i2c->dev.platform_data;

	dev->txbuf = devm_kzalloc(dev->dev, 2 + I2C_TX_BUF_LEN, GFP_KERNEL);

	ret = ksz9477_switch_register(dev);

	/* Main DSA driver may not be started yet. */
	if (ret)
		return ret;

	i2c_set_clientdata(i2c, dev);

	return 0;
}

static int ksz9477_i2c_remove(struct i2c_client *i2c)
{
	struct ksz_device *dev = i2c_get_clientdata(i2c);

	if (dev)
		ksz_switch_remove(dev);

	return 0;
}

static void ksz9477_i2c_shutdown(struct i2c_client *i2c)
{
	struct ksz_device *dev = i2c_get_clientdata(i2c);

	if (dev && dev->dev_ops->shutdown)
		dev->dev_ops->shutdown(dev);
}

static const struct i2c_device_id ksz9477_i2c_id[] = {
	{ "ksz9477-switch", 0 },
	{},
};

MODULE_DEVICE_TABLE(i2c, ksz9477_i2c_id);

static const struct of_device_id ksz9477_dt_ids[] = {
	{ .compatible = "microchip,ksz9477" },
	{ .compatible = "microchip,ksz9897" },
	{},
};
MODULE_DEVICE_TABLE(of, ksz9477_dt_ids);

static struct i2c_driver ksz9477_i2c_driver = {
	.driver = {
		.name	= "ksz9477-switch",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(ksz9477_dt_ids),
	},
	.probe	= ksz9477_i2c_probe,
	.remove	= ksz9477_i2c_remove,
	.shutdown = ksz9477_i2c_shutdown,
	.id_table = ksz9477_i2c_id,
};

module_i2c_driver(ksz9477_i2c_driver);

MODULE_AUTHOR("Woojung Huh <Woojung.Huh@microchip.com>");
MODULE_DESCRIPTION("Microchip KSZ9477 Series Switch I2C access Driver");
MODULE_LICENSE("GPL");
