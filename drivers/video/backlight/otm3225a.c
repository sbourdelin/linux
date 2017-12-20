/*
 * Driver for ORISE Technology OTM3225A SOC for TFT LCD
 *
 * Copyright (C) 2014-2017, EETS GmbH, Felix Brack <fb@ltec.ch>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * This driver implements a lcd device for the ORISE OTM3225A display
 * controller. The control interface to the display is SPI and the display's
 * memory is updated over the 16-bit RGB interface.
 * The main source of information for writing this driver was provided by the
 * OTM3225A datasheet from ORISE Technology. Some information arise from the
 * ILI9328 datasheet from ILITEK as well as from the datasheets and sample code
 * provided by Crystalfontz America Inc. who sells the CFAF240320A-032T, a 3.2"
 * TFT LC display using the OTM3225A controller.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/lcd.h>
#include <linux/spi/spi.h>

#define OTM3225A_INDEX_REG	0x70
#define OTM3225A_DATA_REG	0x72

struct otm3225a_data {
	struct spi_device *spi;
	struct lcd_device *ld;
	int power;
};

struct otm3225a_spi_instruction {
	unsigned char reg;	/* register to write */
	unsigned short value;	/* data to write to 'reg' */
	unsigned short delay;	/* delay in ms after write */
};

static struct otm3225a_spi_instruction display_init[] = {
	{ 0x01, 0x0000, 0 }, { 0x02, 0x0700, 0 }, { 0x03, 0x50A0, 0 },
	{ 0x04, 0x0000, 0 }, { 0x08, 0x0606, 0 }, { 0x09, 0x0000, 0 },
	{ 0x0A, 0x0000, 0 }, { 0x0C, 0x0000, 0 }, { 0x0D, 0x0000, 0 },
	{ 0x0F, 0x0002, 0 }, { 0x11, 0x0007, 0 }, { 0x12, 0x0000, 0 },
	{ 0x13, 0x0000, 200 }, { 0x07, 0x0101, 0 }, { 0x10, 0x12B0, 0 },
	{ 0x11, 0x0007, 0 }, { 0x12, 0x01BB, 50 }, { 0xB1, 0x0000, 0 },
	{ 0xB3, 0x0000, 0 }, { 0xB5, 0x0000, 0 }, { 0xBE, 0x0000, 0 },
	{ 0x13, 0x0013, 0 }, { 0x29, 0x0010, 50 }, { 0x30, 0x000A, 0 },
	{ 0x31, 0x1326, 0 }, { 0x32, 0x0A29, 0 }, { 0x35, 0x0A0A, 0 },
	{ 0x36, 0x1E03, 0 }, { 0x37, 0x031E, 0 }, { 0x38, 0x0706, 0 },
	{ 0x39, 0x0303, 0 }, { 0x3C, 0x010E, 0 }, { 0x3D, 0x040E, 0 },
	{ 0x50, 0x0000, 0 }, { 0x51, 0x00EF, 0 }, { 0x52, 0x0000, 0 },
	{ 0x53, 0x013F, 0 }, { 0x60, 0x2700, 0 }, { 0x61, 0x0001, 0 },
	{ 0x6A, 0x0000, 0 }, { 0x80, 0x0000, 0 }, { 0x81, 0x0000, 0 },
	{ 0x82, 0x0000, 0 }, { 0x83, 0x0000, 0 }, { 0x84, 0x0000, 0 },
	{ 0x85, 0x0000, 0 }, { 0x90, 0x0010, 0 }, { 0x92, 0x0000, 0 },
	{ 0x93, 0x0103, 0 }, { 0x95, 0x0210, 0 }, { 0x97, 0x0000, 0 },
	{ 0x98, 0x0000, 0 }, { 0x07, 0x0133, 0 },
};

static struct otm3225a_spi_instruction display_enable_rgb_interface[] = {
	{ 0x03, 0x1080, 0 },
	{ 0x20, 0x0000, 0 },
	{ 0x21, 0x0000, 0 },
	{ 0x0C, 0x0111, 500 },
};

static struct otm3225a_spi_instruction display_off[] = {
	{ 0x07, 0x0131, 100 },
	{ 0x07, 0x0130, 100 },
	{ 0x07, 0x0100, 0 },
	{ 0x10, 0x0280, 0 },
	{ 0x12, 0x018B, 0 },
};

static struct otm3225a_spi_instruction display_on[] = {
	{ 0x10, 0x1280, 0 },
	{ 0x07, 0x0101, 100 },
	{ 0x07, 0x0121, 0 },
	{ 0x07, 0x0123, 100 },
	{ 0x07, 0x0133, 10 },
};

static void otm3225a_write(struct spi_device *spi,
			   struct otm3225a_spi_instruction *instruction,
			   unsigned int count)
{
	unsigned char buf[3];

	while (count--) {
		/* address register using index register */
		buf[0] = OTM3225A_INDEX_REG;
		buf[1] = 0x00;
		buf[2] = instruction->reg;
		spi_write(spi, buf, 3);

		/* write data to addressed register */
		buf[0] = OTM3225A_DATA_REG;
		buf[1] = (instruction->value >> 8) & 0xff;
		buf[2] = instruction->value & 0xff;
		spi_write(spi, buf, 3);

		/* execute delay if any */
		mdelay(instruction->delay);
		instruction++;
	}
}

static int otm3225a_set_power(struct lcd_device *ld, int power)
{
	struct otm3225a_data *dd = lcd_get_data(ld);

	if (power == dd->power)
		return 0;

	if (power > FB_BLANK_UNBLANK)
		otm3225a_write(dd->spi, display_off, ARRAY_SIZE(display_off));
	else
		otm3225a_write(dd->spi, display_on, ARRAY_SIZE(display_on));
	dd->power = power;

	return 0;
}

static int otm3225a_get_power(struct lcd_device *ld)
{
	struct otm3225a_data *dd = lcd_get_data(ld);

	return dd->power;
}

struct lcd_ops otm3225a_ops = {
	.set_power = otm3225a_set_power,
	.get_power = otm3225a_get_power,
};

static int otm3225a_probe(struct spi_device *spi)
{
	struct otm3225a_data *dd;
	struct lcd_device *ld;
	int ret = 0;

	dd = kzalloc(sizeof(struct otm3225a_data), GFP_KERNEL);
	if (dd == NULL) {
		ret = -ENOMEM;
		goto err;
	}

	ld = lcd_device_register("otm3225a", &spi->dev, dd, &otm3225a_ops);
	if (IS_ERR(ld)) {
		ret = PTR_ERR(ld);
		goto err;
	}
	dd->spi = spi;
	dd->ld = ld;
	dev_set_drvdata(&spi->dev, dd);

	dev_info(&spi->dev, "Initializing and switching to RGB interface");
	otm3225a_write(spi, display_init, ARRAY_SIZE(display_init));
	otm3225a_write(spi, display_enable_rgb_interface,
		       ARRAY_SIZE(display_enable_rgb_interface));

err:
	return ret;
}

static int otm3225a_remove(struct spi_device *spi)
{
	struct otm3225a_data *dd;

	dd = dev_get_drvdata(&spi->dev);
	lcd_device_unregister(dd->ld);
	kfree(dd);
	return 0;
}

static struct spi_driver otm3225a_driver = {
	.driver = {
		.name = "otm3225a",
		.owner = THIS_MODULE,
	},
	.probe = otm3225a_probe,
	.remove = otm3225a_remove,
};

static __init int otm3225a_init(void)
{
	return spi_register_driver(&otm3225a_driver);
}

static __exit void otm3225a_exit(void)
{
	spi_unregister_driver(&otm3225a_driver);
}

module_init(otm3225a_init);
module_exit(otm3225a_exit);

MODULE_AUTHOR("Felix Brack <fb@ltec.ch>");
MODULE_DESCRIPTION("OTM3225A TFT LCD driver");
MODULE_VERSION("1.0.0");
MODULE_LICENSE("GPL v2");
