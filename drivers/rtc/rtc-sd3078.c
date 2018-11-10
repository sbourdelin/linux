// SPDX-License-Identifier: GPL-2.0
/*
 * Real Time Clock (RTC) Driver for sd3078
 * Copyright (C) 2018 Zoro Li
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/bcd.h>
#include <linux/rtc.h>
#include <linux/slab.h>

#define SD3078_REG_SC			0x00
#define SD3078_REG_MN			0x01
#define SD3078_REG_HR			0x02
#define SD3078_REG_DW			0x03
#define SD3078_REG_DM			0x04
#define SD3078_REG_MO			0x05
#define SD3078_REG_YR			0x06

#define SD3078_REG_CTRL1		0x0f
#define SD3078_REG_CTRL2		0x10
#define SD3078_REG_CTRL3		0x11

#define KEY_WRITE1		0x80
#define KEY_WRITE2		0x04
#define KEY_WRITE3		0x80

struct sd3078 {
	struct rtc_device *rtc;
};

struct i2c_driver sd3078_driver;


static int sd3078_i2c_read_regs(struct i2c_client *client,
	unsigned char reg, unsigned char buf[], unsigned int len)
{
	struct i2c_adapter *adap = client->adapter;
	unsigned char reg_buf = reg;
	int ret;
	struct i2c_msg msgs[] = {
		{/* setup read ptr */
			.addr = client->addr,
			.len = 1,
			.buf = &reg_buf
		},
		{/* read status + date */
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = len,
			.buf = buf
		},
	};

	ret = i2c_transfer(adap, msgs, 2);
	return (ret == 2) ? len : ret;
}

static int sd3078_i2c_write_reg(struct i2c_client *client,
	unsigned char reg, unsigned char value)
{
	unsigned char data[2];
	int ret;

	data[0] = reg;
	data[1] = value;
	ret = i2c_master_send(client, data, 2);
	return ret;
}

/*
 * In order to prevent arbitrary modification of the time register,
 * when modification of the register,
 * the "write" bit needs to be written in a certain order.
 * 1. set WRITE1 bit
 * 2. set WRITE2 bit
 * 1. set WRITE3 bit
 */
static void sd3078_enable_reg_write(struct i2c_client *client)
{
	unsigned char reg1, reg2;

	sd3078_i2c_read_regs(client, SD3078_REG_CTRL1, &reg1, 1);
	sd3078_i2c_read_regs(client, SD3078_REG_CTRL2, &reg2, 1);

	reg2 |= KEY_WRITE1;
	sd3078_i2c_write_reg(client, SD3078_REG_CTRL2, reg2);

	reg1 |= KEY_WRITE2;
	sd3078_i2c_write_reg(client, SD3078_REG_CTRL1, reg1);

	reg1 |= KEY_WRITE3;
	sd3078_i2c_write_reg(client, SD3078_REG_CTRL1, reg1);

	sd3078_i2c_read_regs(client, SD3078_REG_CTRL1, &reg1, 1);
	sd3078_i2c_read_regs(client, SD3078_REG_CTRL2, &reg2, 1);
}

/*
 * In order to prevent arbitrary modification of the time register,
 * we should disable the write function.
 * when disable write,
 * the "write" bit needs to be clear in a certain order.
 * 1. clear WRITE2 bit
 * 2. clear WRITE3 bit
 * 3. clear WRITE1 bit
 */
static void sd3078_disable_reg_write(struct i2c_client *client)
{
	unsigned char reg1, reg2;

	sd3078_i2c_read_regs(client, SD3078_REG_CTRL1, &reg1, 1);
	sd3078_i2c_read_regs(client, SD3078_REG_CTRL2, &reg2, 1);

	reg1 &= ~KEY_WRITE2;
	sd3078_i2c_write_reg(client, SD3078_REG_CTRL1, reg1);

	reg1 &= ~KEY_WRITE3;
	sd3078_i2c_write_reg(client, SD3078_REG_CTRL1, reg1);

	reg2 &= ~KEY_WRITE1;
	sd3078_i2c_write_reg(client, SD3078_REG_CTRL2, reg2);

	sd3078_i2c_read_regs(client, SD3078_REG_CTRL1, &reg1, 1);
	sd3078_i2c_read_regs(client, SD3078_REG_CTRL2, &reg2, 1);
}

static int sd3078_get_datetime(struct i2c_client *client, struct rtc_time *tm)
{
	unsigned char buf[7] = {0};
	unsigned char hour;

	sd3078_i2c_read_regs(client, SD3078_REG_SC, buf, 7);

	tm->tm_sec	= bcd2bin(buf[SD3078_REG_SC] & 0x7F);
	tm->tm_min	= bcd2bin(buf[SD3078_REG_MN] & 0x7F);
	hour = buf[SD3078_REG_HR];

	if (hour & 0x80) /* 24H PM */
		tm->tm_hour = bcd2bin(buf[SD3078_REG_HR] & 0x3F);
	else if (hour & 0x20) /* 12H PM */
		tm->tm_hour = bcd2bin(buf[SD3078_REG_HR] & 0x1F) + 12;
	else /* 12H AM */
		tm->tm_hour = bcd2bin(buf[SD3078_REG_HR] & 0x1F);

	tm->tm_mday = bcd2bin(buf[SD3078_REG_DM] & 0x3F);
	tm->tm_wday = buf[SD3078_REG_DW] & 0x07;
	/* rtc mn 1-12 */
	tm->tm_mon	= bcd2bin(buf[SD3078_REG_MO] & 0x1F) - 1;
	tm->tm_year = bcd2bin(buf[SD3078_REG_YR])+100;

	if (rtc_valid_tm(tm) < 0)
		dev_err(&client->dev, "retrieved date/time is not valid.\n");

	return 0;
}

static int sd3078_set_datetime(struct i2c_client *client, struct rtc_time *tm)
{
	int i, err;
	unsigned char buf[9];

	dev_dbg(&client->dev,
		"%s: secs=%d, mins=%d, hours=%d, mday=%d, mon=%d, year=%d, wday=%d\n",
		__func__,
		tm->tm_sec, tm->tm_min, tm->tm_hour,
		tm->tm_mday, tm->tm_mon, tm->tm_year, tm->tm_wday);

    /* hours, minutes and seconds */
	buf[SD3078_REG_SC] = bin2bcd(tm->tm_sec);
	buf[SD3078_REG_MN] = bin2bcd(tm->tm_min);
	/* set 24H mode */
	buf[SD3078_REG_HR] = bin2bcd(tm->tm_hour) | 0x80;

	buf[SD3078_REG_DM] = bin2bcd(tm->tm_mday);

    /* month, 1 - 12 */
	buf[SD3078_REG_MO] = bin2bcd(tm->tm_mon) + 1;

    /* year and century */
	buf[SD3078_REG_YR] = bin2bcd(tm->tm_year % 100);
	buf[SD3078_REG_DW] = tm->tm_wday & 0x07;

	sd3078_enable_reg_write(client);

    /* write register's data */
	for (i = 0; i < 7; i++) {
		err = sd3078_i2c_write_reg(client,
			SD3078_REG_SC + i, buf[SD3078_REG_SC + i]);
		if (err != 2)
			return -EIO;
	}

	sd3078_disable_reg_write(client);

	return 0;
}


#ifdef CONFIG_RTC_INTF_DEV
static int sd3078_rtc_ioctl(struct device *dev,
		unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	default:
		return -ENOIOCTLCMD;
	}
}
#else
#define sd3078_rtc_ioctl NULL
#endif

static int sd3078_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	return sd3078_get_datetime(to_i2c_client(dev), tm);
}

static int sd3078_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	return sd3078_set_datetime(to_i2c_client(dev), tm);
}

static const struct rtc_class_ops sd3078_rtc_ops = {
	.ioctl		= sd3078_rtc_ioctl,
	.read_time	= sd3078_rtc_read_time,
	.set_time	= sd3078_rtc_set_time,
};

static int sd3078_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	struct sd3078 *sd3078;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;

	sd3078 = devm_kzalloc(&client->dev, sizeof(*sd3078), GFP_KERNEL);
	if (!sd3078)
		return -ENOMEM;

	i2c_set_clientdata(client, sd3078);

	sd3078->rtc = devm_rtc_device_register(&client->dev,
			sd3078_driver.driver.name,
			&sd3078_rtc_ops, THIS_MODULE);

	if (IS_ERR(sd3078->rtc))
		return PTR_ERR(sd3078->rtc);

	return 0;
}

static int sd3078_remove(struct i2c_client *client)
{
	return 0;
}

static const struct i2c_device_id sd3078_id[] = {
	{"sd3078", 0},
	{ }
};
MODULE_DEVICE_TABLE(i2c, sd3078_id);

#ifdef CONFIG_OF
static const struct of_device_id rtc_dt_ids[] = {
	{ .compatible = "whwave,sd3078" },
	{},
};
#endif

struct i2c_driver sd3078_driver = {
	.driver     = {
		.name   = "sd3078",
		.owner  = THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = of_match_ptr(rtc_dt_ids),
#endif
	},
	.probe      = sd3078_probe,
	.remove     = sd3078_remove,
	.id_table   = sd3078_id,
};

static int __init sd3078_init(void)
{
	return i2c_add_driver(&sd3078_driver);
}

static void __exit sd3078_exit(void)
{
	i2c_del_driver(&sd3078_driver);
}

module_init(sd3078_init);
module_exit(sd3078_exit);

MODULE_AUTHOR("Zoro Li <long17.cool@163.com>");
MODULE_DESCRIPTION("SD3078 RTC driver");
MODULE_LICENSE("GPL");
