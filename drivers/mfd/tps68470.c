/*
 * TPS68470 chip family multi-function driver
 *
 * Copyright (C) 2017 Intel Corporation
 * Authors:
 * Rajmohan Mani <rajmohan.mani@intel.com>
 * Tianshu Qiu <tian.shu.qiu@intel.com>
 * Jian Xu Zheng <jian.xu.zheng@intel.com>
 * Yuning Pu <yuning.pu@intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/mfd/core.h>
#include <linux/mfd/tps68470.h>
#include <linux/init.h>
#include <linux/regmap.h>

static const struct mfd_cell tps68470s[] = {
	{
		.name = "tps68470-gpio",
	},
	{
		.name = "tps68470_pmic_opregion",
	},
};

/*
 * tps68470_reg_read: Read a single tps68470 register.
 *
 * @tps: Device to read from.
 * @reg: Register to read.
 * @val: Contains the value
 */
int tps68470_reg_read(struct tps68470 *tps, unsigned int reg,
			unsigned int *val)
{
	int ret;

	mutex_lock(&tps->lock);
	ret = regmap_read(tps->regmap, reg, val);
	mutex_unlock(&tps->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(tps68470_reg_read);

/*
 * tps68470_reg_write: Write a single tps68470 register.
 *
 * @tps68470: Device to write to.
 * @reg: Register to write to.
 * @val: Value to write.
 */
int tps68470_reg_write(struct tps68470 *tps, unsigned int reg,
			unsigned int val)
{
	int ret;

	mutex_lock(&tps->lock);
	ret = regmap_write(tps->regmap, reg, val);
	mutex_unlock(&tps->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(tps68470_reg_write);

/*
 * tps68470_update_bits: Modify bits w.r.t mask and val.
 *
 * @tps68470: Device to write to.
 * @reg: Register to read-write to.
 * @mask: Mask.
 * @val: Value to write.
 */
int tps68470_update_bits(struct tps68470 *tps, unsigned int reg,
				unsigned int mask, unsigned int val)
{
	int ret;

	mutex_lock(&tps->lock);
	ret = regmap_update_bits(tps->regmap, reg, mask, val);
	mutex_unlock(&tps->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(tps68470_update_bits);

static const struct regmap_config tps68470_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = TPS68470_REG_MAX,
};

static int tps68470_chip_init(struct tps68470 *tps)
{
	unsigned int version;
	int ret;

	ret = tps68470_reg_read(tps, TPS68470_REG_REVID, &version);
	if (ret < 0) {
		dev_err(tps->dev,
			"Failed to read revision register: %d\n", ret);
		return ret;
	}

	dev_info(tps->dev, "TPS68470 REVID: 0x%x\n", version);

	ret = tps68470_reg_write(tps, TPS68470_REG_RESET, 0xff);
	if (ret < 0)
		return ret;

	/* FIXME: configure these dynamically */
	/* Enable Daisy Chain LDO and configure relevant GPIOs as output */
	ret = tps68470_reg_write(tps, TPS68470_REG_S_I2C_CTL, 2);
	if (ret < 0)
		return ret;

	ret = tps68470_reg_write(tps, TPS68470_REG_GPCTL4A, 2);
	if (ret < 0)
		return ret;

	ret = tps68470_reg_write(tps, TPS68470_REG_GPCTL5A, 2);
	if (ret < 0)
		return ret;

	ret = tps68470_reg_write(tps, TPS68470_REG_GPCTL6A, 2);
	if (ret < 0)
		return ret;

	/*
	 * When SDA and SCL are routed to GPIO1 and GPIO2, the mode
	 * for these GPIOs must be configured using their respective
	 * GPCTLxA registers as inputs with no pull-ups.
	 */
	ret = tps68470_reg_write(tps, TPS68470_REG_GPCTL1A, 0);
	if (ret < 0)
		return ret;

	ret = tps68470_reg_write(tps, TPS68470_REG_GPCTL2A, 0);
	if (ret < 0)
		return ret;

	/* Enable daisy chain */
	ret = tps68470_update_bits(tps, TPS68470_REG_S_I2C_CTL, 1, 1);
	if (ret < 0)
		return ret;

	usleep_range(TPS68470_DAISY_CHAIN_DELAY_US,
			TPS68470_DAISY_CHAIN_DELAY_US + 10);
	return 0;
}

static int tps68470_probe(struct i2c_client *client)
{
	struct tps68470 *tps;
	int ret;

	tps = devm_kzalloc(&client->dev, sizeof(*tps), GFP_KERNEL);
	if (!tps)
		return -ENOMEM;

	mutex_init(&tps->lock);
	i2c_set_clientdata(client, tps);
	tps->dev = &client->dev;

	tps->regmap = devm_regmap_init_i2c(client, &tps68470_regmap_config);
	if (IS_ERR(tps->regmap)) {
		dev_err(tps->dev, "devm_regmap_init_i2c Error %d\n", ret);
		return PTR_ERR(tps->regmap);
	}

	ret = mfd_add_devices(tps->dev, -1, tps68470s,
			      ARRAY_SIZE(tps68470s), NULL, 0, NULL);
	if (ret < 0) {
		dev_err(tps->dev, "mfd_add_devices failed: %d\n", ret);
		return ret;
	}

	ret = tps68470_chip_init(tps);
	if (ret < 0) {
		dev_err(tps->dev, "TPS68470 Init Error %d\n", ret);
		goto fail;
	}

	return 0;
fail:
	mutex_lock(&tps->lock);
	mfd_remove_devices(tps->dev);
	mutex_unlock(&tps->lock);

	return ret;
}

static int tps68470_remove(struct i2c_client *client)
{
	struct tps68470 *tps = i2c_get_clientdata(client);

	mutex_lock(&tps->lock);
	mfd_remove_devices(tps->dev);
	mutex_unlock(&tps->lock);

	return 0;
}

static const struct acpi_device_id tps68470_acpi_ids[] = {
	{"INT3472"},
	{},
};

MODULE_DEVICE_TABLE(acpi, tps68470_acpi_ids);

static struct i2c_driver tps68470_driver = {
	.driver = {
		   .name = "tps68470",
		   .acpi_match_table = ACPI_PTR(tps68470_acpi_ids),
	},
	.probe_new = tps68470_probe,
	.remove = tps68470_remove,
};
builtin_i2c_driver(tps68470_driver);
