/*
 *  Driver for Ntrig/Microsoft Touchscreens over SPI
 *
 *  Copyright (c) 2016 Red Hat Inc.
 *
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; version 2 of the License.
 */

#include <linux/kernel.h>

#include <asm/unaligned.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/acpi.h>

#define NTRIG_PACKET_SIZE	264

struct ntrig {
	struct spi_device *spi;
	struct gpio_desc *gpiod_int;
	struct gpio_desc *gpiod_rst[2];
	struct input_dev *input_dev;
};

struct ntrig_finger {
	u8 status;
	u16 tracking_id;
	u16 x;
	u16 cx;
	u16 y;
	u16 cy;
	u16 width;
	u16 height;
	u32 padding;
} __packed;

static ssize_t ntrig_spi_read(struct spi_device *spi, char *rxbuf, size_t len)
{
	struct spi_message msg;
	struct spi_transfer xfer;
	u8 rd_buf[NTRIG_PACKET_SIZE];
	int err = 0;

	if (!spi)
		return 0;

	memset(&xfer, 0, sizeof(xfer));
	memset(rd_buf, 0, sizeof(rd_buf));
	spi_message_init(&msg);

	xfer.tx_buf = NULL;
	xfer.len = len;
	xfer.rx_buf = rd_buf;
	spi_message_add_tail(&xfer, &msg);

	err = spi_sync(spi, &msg);
	memcpy(rxbuf, rd_buf, xfer.len);

	return err;
}

static void ntrig_spi_report_touch(struct ntrig *ntr,
				   struct ntrig_finger *finger)
{
	int st = finger->status & 0x01;
	int id = get_unaligned_le16(&finger->tracking_id);
	int  x = get_unaligned_le16(&finger->x);
	int  y = get_unaligned_le16(&finger->y);
	int  w = get_unaligned_le16(&finger->width);
	int  h = get_unaligned_le16(&finger->height);
	int slot;

	slot = input_mt_get_slot_by_key(ntr->input_dev, id);
	if (slot < 0)
		return;

	input_mt_slot(ntr->input_dev, slot);
	input_mt_report_slot_state(ntr->input_dev, MT_TOOL_FINGER, st);
	if (st) {
		input_report_abs(ntr->input_dev, ABS_MT_POSITION_X, x);
		input_report_abs(ntr->input_dev, ABS_MT_POSITION_Y, y);
		input_report_abs(ntr->input_dev, ABS_MT_TOUCH_MAJOR, w);
		input_report_abs(ntr->input_dev, ABS_MT_WIDTH_MAJOR, w);
		input_report_abs(ntr->input_dev, ABS_MT_TOUCH_MINOR, h);
		input_report_abs(ntr->input_dev, ABS_MT_WIDTH_MINOR, h);
	}
}

static int ntrig_spi_process(struct ntrig *ntr, const char *data)
{
	const char header[] = {0xff, 0xff, 0xff, 0xff, 0xa5, 0x5a, 0xe7, 0x7e,
			       0x01, 0xd2, 0x00, 0x80, 0x01, 0x03, 0x03};
	u16 timestamp;
	unsigned int i;

	if (memcmp(header, data, sizeof(header)))
		dev_err(&ntr->spi->dev, "%s header error: %15ph, ignoring... %s:%d\n",
			__func__, data, __FILE__, __LINE__);

	timestamp = get_unaligned_le16(&data[15]);

	for (i = 0; i < 13; i++) {
		struct ntrig_finger *finger;

		finger = (struct ntrig_finger *)&data[17 +
					       i * sizeof(struct ntrig_finger)];

		if (finger->status & 0x10)
			break;

		ntrig_spi_report_touch(ntr, finger);
	}

	input_mt_sync_frame(ntr->input_dev);
	input_sync(ntr->input_dev);
	return 0;
}

static irqreturn_t ntrig_spi_irq_handler(int irq, void *dev_id)
{
	struct ntrig *data = dev_id;
	char spiRxbuf[NTRIG_PACKET_SIZE];

	while (gpiod_get_value(data->gpiod_int)) {
		memset(spiRxbuf, 0x00, NTRIG_PACKET_SIZE);
		ntrig_spi_read(data->spi, spiRxbuf, NTRIG_PACKET_SIZE);
		dev_dbg(&data->spi->dev, "%s received -> %*ph %s:%d\n",
			__func__, NTRIG_PACKET_SIZE, spiRxbuf,
			__FILE__, __LINE__);
		ntrig_spi_process(data, spiRxbuf);
	}

	return IRQ_HANDLED;
}

static void ntrig_spi_power(struct ntrig *data, unsigned int value)
{
	gpiod_set_value(data->gpiod_rst[0], value);
	gpiod_set_value(data->gpiod_rst[1], value);
}

/**
 * ntrig_spi_get_gpio_config - Get GPIO config from ACPI/DT
 *
 * @ts: ntrig_spi_ts_data pointer
 */
static int ntrig_spi_get_gpio_config(struct ntrig *data)
{
	int error;
	struct device *dev;
	struct gpio_desc *gpiod;
	int i;

	if (!data->spi)
		return -EINVAL;

	dev = &data->spi->dev;

	/* Get the interrupt GPIO pin number */
	gpiod = devm_gpiod_get_index(dev, NULL, 2, GPIOD_IN);
	if (IS_ERR(gpiod)) {
		error = PTR_ERR(gpiod);
		if (error != -EPROBE_DEFER)
			dev_err(dev, "Failed to get int GPIO: %d\n", error);
		return error;
	}

	data->gpiod_int = gpiod;

	/* Get the reset lines GPIO pin number */
	for (i = 0; i < 2; i++) {
		gpiod = devm_gpiod_get_index(dev, NULL, i, GPIOD_OUT_LOW);
		if (IS_ERR(gpiod)) {
			error = PTR_ERR(gpiod);
			if (error != -EPROBE_DEFER)
				dev_err(dev,
					"Failed to get power GPIO %d: %d\n",
					i,
					error);
			return error;
		}

		data->gpiod_rst[i] = gpiod;
	}

	return 0;
}

static int ntrig_spi_create_input(struct ntrig *data)
{
	struct input_dev *input;
	int error;

	input = devm_input_allocate_device(&data->spi->dev);
	if (IS_ERR(input))
		return PTR_ERR(input);

	data->input_dev = input;

	input_set_abs_params(input, ABS_MT_POSITION_X, 0, 9600, 0, 0);
	input_abs_set_res(input, ABS_MT_POSITION_X, 40);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, 7200, 0, 0);
	input_abs_set_res(input, ABS_MT_POSITION_Y, 48);
	input_set_abs_params(input, ABS_MT_WIDTH_MAJOR, 0, 1024, 0, 0);
	input_set_abs_params(input, ABS_MT_WIDTH_MINOR, 0, 1024, 0, 0);
	input_mt_init_slots(input, 10, INPUT_MT_DIRECT);

	input->name = "Ntrig Capacitive TouchScreen";
	input->phys = "input/ts";
	input->id.bustype = BUS_SPI;
	input->id.vendor = 0x1b96;
	input->id.product = 0x0000;
	input->id.version = 0x0000;

	error = input_register_device(input);
	if (error) {
		dev_err(&data->spi->dev,
			"Failed to register input device: %d", error);
		return error;
	}

	return 0;
}

static void ntrig_spi_free_irq(struct ntrig *data)
{
	devm_free_irq(&data->spi->dev, data->spi->irq, data);
}

static int ntrig_spi_request_irq(struct ntrig *data)
{
	struct spi_device *spi = data->spi;

	return devm_request_threaded_irq(&spi->dev, spi->irq,
					  NULL, ntrig_spi_irq_handler,
					  IRQ_TYPE_EDGE_RISING | IRQF_ONESHOT,
					  "Ntrig-irq", data);
}

static int ntrig_spi_probe(struct spi_device *spi)
{
	struct ntrig *data = NULL;
	int error;

	/* Set up SPI*/
	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_0;
	error = spi_setup(spi);
	if (error)
		return error;

	data = devm_kzalloc(&spi->dev, sizeof(*data), GFP_KERNEL);

	data->spi = spi;
	spi_set_drvdata(spi, data);

	error = ntrig_spi_get_gpio_config(data);
	if (error)
		return error;

	ntrig_spi_power(data, 1);
	msleep(20);
	ntrig_spi_power(data, 0);
	msleep(20);
	ntrig_spi_power(data, 1);

	error = ntrig_spi_create_input(data);
	if (error)
		return error;

	error = ntrig_spi_request_irq(data);
	if (error)
		return error;

	return PTR_ERR_OR_ZERO(data);
}

static int __maybe_unused ntrig_spi_suspend(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct ntrig *data = spi_get_drvdata(spi);

	ntrig_spi_free_irq(data);

	ntrig_spi_power(data, 0);

	return 0;
}

static int __maybe_unused ntrig_spi_resume(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct ntrig *data = spi_get_drvdata(spi);
	int error;

	ntrig_spi_power(data, 1);

	error = ntrig_spi_request_irq(data);
	if (error)
		return error;

	return 0;
}

static SIMPLE_DEV_PM_OPS(ntrig_spi_pm_ops, ntrig_spi_suspend, ntrig_spi_resume);

#ifdef CONFIG_ACPI
static const struct acpi_device_id ntrig_spi_acpi_match[] = {
	{ "MSHW0037", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, ntrig_spi_acpi_match);
#endif

static struct spi_driver ntrig_spi_driver = {
	.driver = {
		.name	= "Ntrig-spi",
		.acpi_match_table = ACPI_PTR(ntrig_spi_acpi_match),
		.pm = &ntrig_spi_pm_ops,
	},
	.probe = ntrig_spi_probe,
};

module_spi_driver(ntrig_spi_driver);

MODULE_AUTHOR("Benjamin Tissoires <benjamin.tissoires@gmail.com>");
MODULE_DESCRIPTION("Ntrig SPI touchscreen driver");
MODULE_LICENSE("GPL v2");
