/*
 * Copyright (C) 2016 Linaro Ltd.
 * Author: Rob Herring <robh@kernel.org>
 *
 * Based on include/linux/spmi.h:
 * Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
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
#ifndef _LINUX_UART_DEVICE_H
#define _LINUX_UART_DEVICE_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/circ_buf.h>
#include <linux/mod_devicetable.h>

/**
 * struct uart_device - Basic representation of an SPMI device
 * @dev:	Driver model representation of the device.
 * @ctrl:	SPMI controller managing the bus hosting this device.
 * @usid:	This devices' Unique Slave IDentifier.
 */
struct uart_device {
	struct device		dev;
	struct uart_controller	*ctrl;
};

static inline struct uart_device *to_uart_device(struct device *d)
{
	return container_of(d, struct uart_device, dev);
}

static inline void *uart_device_get_drvdata(const struct uart_device *sdev)
{
	return dev_get_drvdata(&sdev->dev);
}

static inline void uart_device_set_drvdata(struct uart_device *sdev, void *data)
{
	dev_set_drvdata(&sdev->dev, data);
}

struct uart_device *uart_device_alloc(struct uart_controller *ctrl);

static inline void uart_device_put(struct uart_device *sdev)
{
	if (sdev)
		put_device(&sdev->dev);
}

int uart_device_add(struct uart_device *sdev);

void uart_device_remove(struct uart_device *sdev);

struct uart_port;

/**
 * struct uart_controller - interface to the SPMI master controller
 * @dev:	Driver model representation of the device.
 * @nr:		board-specific number identifier for this controller/bus
 * @cmd:	sends a non-data command sequence on the SPMI bus.
 * @read_cmd:	sends a register read command sequence on the SPMI bus.
 * @write_cmd:	sends a register write command sequence on the SPMI bus.
 */
struct uart_controller {
	struct device		dev;
	struct uart_port	*port;
	unsigned int		nr;
	struct circ_buf		recv;
};

static inline struct uart_controller *to_uart_controller(struct device *d)
{
	return container_of(d, struct uart_controller, dev);
}

static inline
void *uart_controller_get_drvdata(const struct uart_controller *ctrl)
{
	return dev_get_drvdata(&ctrl->dev);
}

static inline void uart_controller_set_drvdata(struct uart_controller *ctrl,
					       void *data)
{
	dev_set_drvdata(&ctrl->dev, data);
}

struct uart_controller *uart_controller_alloc(struct device *parent,
					      size_t size);

/**
 * uart_controller_put() - decrement controller refcount
 * @ctrl	SPMI controller.
 */
static inline void uart_controller_put(struct uart_controller *ctrl)
{
	if (ctrl)
		put_device(&ctrl->dev);
}

int uart_controller_add(struct uart_controller *ctrl);
void uart_controller_remove(struct uart_controller *ctrl);

int uart_controller_rx(struct uart_controller *ctrl, unsigned int ch);

/**
 * struct spmi_driver - SPMI slave device driver
 * @driver:	SPMI device drivers should initialize name and owner field of
 *		this structure.
 * @probe:	binds this driver to a SPMI device.
 * @remove:	unbinds this driver from the SPMI device.
 *
 * If PM runtime support is desired for a slave, a device driver can call
 * pm_runtime_put() from their probe() routine (and a balancing
 * pm_runtime_get() in remove()).  PM runtime support for a slave is
 * implemented by issuing a SLEEP command to the slave on runtime_suspend(),
 * transitioning the slave into the SLEEP state.  On runtime_resume(), a WAKEUP
 * command is sent to the slave to bring it back to ACTIVE.
 */
struct uart_dev_driver {
	struct device_driver driver;
	int	(*probe)(struct uart_device *sdev);
	void	(*remove)(struct uart_device *sdev);
};

static inline struct uart_dev_driver *to_uart_dev_driver(struct device_driver *d)
{
	return container_of(d, struct uart_dev_driver, driver);
}

#define uart_dev_driver_register(sdrv) \
	__uart_dev_driver_register(sdrv, THIS_MODULE)
int __uart_dev_driver_register(struct uart_dev_driver *sdrv, struct module *owner);

/**
 * spmi_driver_unregister() - unregister an SPMI client driver
 * @sdrv:	the driver to unregister
 */
static inline void uart_dev_driver_unregister(struct uart_dev_driver *sdrv)
{
	if (sdrv)
		driver_unregister(&sdrv->driver);
}

#define module_uart_dev_driver(__uart_dev_driver) \
	module_driver(__uart_dev_driver, uart_dev_driver_register, \
			uart_dev_driver_unregister)

int uart_dev_config(struct uart_device *udev, int baud, int parity, int bits, int flow);
int uart_dev_connect(struct uart_device *udev);
int uart_dev_tx(struct uart_device *udev, u8 *buf, size_t count);
int uart_dev_rx(struct uart_device *udev, u8 *buf, size_t count);

#endif
