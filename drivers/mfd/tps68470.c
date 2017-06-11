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
#include <linux/gpio.h>
#include <linux/gpio/machine.h>
#include <linux/init.h>
#include <linux/mfd/core.h>
#include <linux/mfd/tps68470.h>
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
 * This lookup table for the TPS68470 GPIOs, lists
 * the 7 GPIOs (that can be configured as input or output
 * as appropriate) and 3 special purpose GPIOs that are
 * "output only". Exporting these GPIOs in a system mounted
 * with the TPS68470, in conjunction with the gpio-tps68470
 * driver, allows the platform firmware to configure these
 * GPIOs appropriately, through the ACPI operation region.
 * These 7 configurable GPIOs can be connected to power rails,
 * sensor control (e.g sensor reset), while the 3 GPIOs can
 * be used for sensor control.
 */
struct gpiod_lookup_table gpios_table = {
	.dev_id = NULL,
	.table = {
		  GPIO_LOOKUP("tps68470-gpio", 0, "gpio.0", GPIO_ACTIVE_HIGH),
		  GPIO_LOOKUP("tps68470-gpio", 1, "gpio.1", GPIO_ACTIVE_HIGH),
		  GPIO_LOOKUP("tps68470-gpio", 2, "gpio.2", GPIO_ACTIVE_HIGH),
		  GPIO_LOOKUP("tps68470-gpio", 3, "gpio.3", GPIO_ACTIVE_HIGH),
		  GPIO_LOOKUP("tps68470-gpio", 4, "gpio.4", GPIO_ACTIVE_HIGH),
		  GPIO_LOOKUP("tps68470-gpio", 5, "gpio.5", GPIO_ACTIVE_HIGH),
		  GPIO_LOOKUP("tps68470-gpio", 6, "gpio.6", GPIO_ACTIVE_HIGH),
		  GPIO_LOOKUP("tps68470-gpio", 7, "s_enable", GPIO_ACTIVE_HIGH),
		  GPIO_LOOKUP("tps68470-gpio", 8, "s_idle", GPIO_ACTIVE_HIGH),
		  GPIO_LOOKUP("tps68470-gpio", 9, "s_resetn", GPIO_ACTIVE_HIGH),
		  {},
	},
};

static const struct regmap_config tps68470_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = TPS68470_REG_MAX,
};

static int tps68470_chip_init(struct device *dev, struct regmap *regmap)
{
	unsigned int version;
	int ret;

	ret = regmap_read(regmap, TPS68470_REG_REVID, &version);
	if (ret < 0) {
		dev_err(dev, "Failed to read revision register: %d\n", ret);
		return ret;
	}

	dev_info(dev, "TPS68470 REVID: 0x%x\n", version);

	ret = regmap_write(regmap, TPS68470_REG_RESET, 0xff);
	if (ret < 0)
		return ret;

	/* FIXME: configure these dynamically */
	/* Enable Daisy Chain LDO and configure relevant GPIOs as output */
	ret = regmap_write(regmap, TPS68470_REG_S_I2C_CTL, 2);
	if (ret < 0)
		return ret;

	ret = regmap_write(regmap, TPS68470_REG_GPCTL4A, 2);
	if (ret < 0)
		return ret;

	ret = regmap_write(regmap, TPS68470_REG_GPCTL5A, 2);
	if (ret < 0)
		return ret;

	ret = regmap_write(regmap, TPS68470_REG_GPCTL6A, 2);
	if (ret < 0)
		return ret;

	/*
	 * When SDA and SCL are routed to GPIO1 and GPIO2, the mode
	 * for these GPIOs must be configured using their respective
	 * GPCTLxA registers as inputs with no pull-ups.
	 */
	ret = regmap_write(regmap, TPS68470_REG_GPCTL1A, 0);
	if (ret < 0)
		return ret;

	ret = regmap_write(regmap, TPS68470_REG_GPCTL2A, 0);
	if (ret < 0)
		return ret;

	/* Enable daisy chain */
	ret = regmap_update_bits(regmap, TPS68470_REG_S_I2C_CTL, 1, 1);
	if (ret < 0)
		return ret;

	usleep_range(TPS68470_DAISY_CHAIN_DELAY_US,
			TPS68470_DAISY_CHAIN_DELAY_US + 10);
	return 0;
}

static int tps68470_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regmap *regmap;
	int ret;

	regmap = devm_regmap_init_i2c(client, &tps68470_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(dev, "devm_regmap_init_i2c Error %ld\n",
		PTR_ERR(regmap));
		return PTR_ERR(regmap);
	}

	i2c_set_clientdata(client, regmap);

	gpiod_add_lookup_table(&gpios_table);

	ret = devm_mfd_add_devices(dev, -1, tps68470s,
			      ARRAY_SIZE(tps68470s), NULL, 0, NULL);
	if (ret < 0) {
		dev_err(dev, "mfd_add_devices failed: %d\n", ret);
		return ret;
	}

	ret = tps68470_chip_init(dev, regmap);
	if (ret < 0) {
		dev_err(dev, "TPS68470 Init Error %d\n", ret);
		return ret;
	}

	return 0;
}

static const struct i2c_device_id tps68470_id_table[] = {
	{},
};

MODULE_DEVICE_TABLE(i2c, tps68470_id_table);

static const struct acpi_device_id tps68470_acpi_ids[] = {
	{"INT3472"},
	{},
};

MODULE_DEVICE_TABLE(acpi, tps68470_acpi_ids);

static void tps68470_shutdown(struct i2c_client *client)
{
	gpiod_remove_lookup_table(&gpios_table);
}

static struct i2c_driver tps68470_driver = {
	.driver = {
		   .name = "tps68470",
		   .acpi_match_table = ACPI_PTR(tps68470_acpi_ids),
	},
	.id_table = tps68470_id_table,
	.probe_new = tps68470_probe,
	.shutdown = tps68470_shutdown,
};
builtin_i2c_driver(tps68470_driver);
