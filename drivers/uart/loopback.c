/*
 * Copyright (C) 2016 Linaro Ltd.
 * Author: Rob Herring <robh@kernel.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/uart_device.h>
#include <linux/workqueue.h>

struct loopback_data {
	struct uart_device *udev;
	struct delayed_work work;
};

static void loopback_work(struct work_struct *work)
{
	struct loopback_data *data = container_of((struct delayed_work *)work, struct loopback_data, work);
	struct uart_device *udev = data->udev;
	int cnt;
	char buf[64];

	cnt = uart_dev_rx(udev, buf, sizeof(buf));
	uart_dev_tx(udev, buf, cnt);

	schedule_delayed_work(&data->work, 5);
}

static int loopback_probe(struct uart_device *udev)
{
	struct loopback_data *data;

	data = devm_kzalloc(&udev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->udev = udev;

	dev_info(&udev->dev, "loopback probe!!!\n");

	uart_dev_connect(udev);
	uart_dev_config(udev, 115200, 'n', 8, 0);


	INIT_DELAYED_WORK(&data->work, loopback_work);
	schedule_delayed_work(&data->work, 100);

	return 0;
}

static const struct of_device_id loopback_of_match[] = {
	{ .compatible = "loopback-uart", },
	{},
};
MODULE_DEVICE_TABLE(of, loopback_of_match);

static struct uart_dev_driver loopback_driver = {
	.probe = loopback_probe,
	.driver	= {
		.name	= "loopback-uart",
		.of_match_table = of_match_ptr(loopback_of_match),
	},
};
module_uart_dev_driver(loopback_driver);
