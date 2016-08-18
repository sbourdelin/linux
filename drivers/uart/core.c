/*
 * Copyright (C) 2016 Linaro Ltd.
 * Author: Rob Herring <robh@kernel.org>
 *
 * Based on drivers/spmi/spmi.c:
 * Copyright (c) 2012-2015, The Linux Foundation. All rights reserved.
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
#define DEBUG

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/uart_device.h>
#include <linux/pm_runtime.h>
#include <linux/serial_core.h>

static bool is_registered;
static DEFINE_IDA(ctrl_ida);

static void uart_dev_release(struct device *dev)
{
	struct uart_device *udev = to_uart_device(dev);
	kfree(udev);
}

static const struct device_type uart_dev_type = {
	.release	= uart_dev_release,
};

static void uart_ctrl_release(struct device *dev)
{
	struct uart_controller *ctrl = to_uart_controller(dev);
	ida_simple_remove(&ctrl_ida, ctrl->nr);
	kfree(ctrl);
}

static const struct device_type uart_ctrl_type = {
	.release	= uart_ctrl_release,
};

static int uart_device_match(struct device *dev, struct device_driver *drv)
{
	if (of_driver_match_device(dev, drv))
		return 1;

	if (drv->name)
		return strncmp(dev_name(dev), drv->name,
			       SPMI_NAME_SIZE) == 0;

	return 0;
}

/**
 * uart_device_add() - add a device previously constructed via uart_device_alloc()
 * @udev:	uart_device to be added
 */
int uart_device_add(struct uart_device *udev)
{
	struct uart_controller *ctrl = udev->ctrl;
	int err;

	dev_set_name(&udev->dev, "uartdev-%d", ctrl->nr);

	err = device_add(&udev->dev);
	if (err < 0) {
		dev_err(&udev->dev, "Can't add %s, status %d\n",
			dev_name(&udev->dev), err);
		goto err_device_add;
	}

	dev_dbg(&udev->dev, "device %s registered\n", dev_name(&udev->dev));

err_device_add:
	return err;
}
EXPORT_SYMBOL_GPL(uart_device_add);

/**
 * uart_device_remove(): remove an SPMI device
 * @udev:	uart_device to be removed
 */
void uart_device_remove(struct uart_device *udev)
{
	device_unregister(&udev->dev);
}
EXPORT_SYMBOL_GPL(uart_device_remove);

int uart_dev_config(struct uart_device *udev, int baud, int parity, int bits, int flow)
{
	return uart_set_options(udev->ctrl->port, NULL, baud, parity, bits, flow);
}

int uart_dev_connect(struct uart_device *udev)
{
	unsigned long page;
	struct uart_port *port = udev->ctrl->port;
	struct uart_state *state = port->state;

	WARN_ON(!state);

	if (!state->xmit.buf) {
		/* This is protected by the per port mutex */
		page = get_zeroed_page(GFP_KERNEL);
		if (!page)
			return -ENOMEM;

		state->xmit.buf = (unsigned char *) page;
		uart_circ_clear(&state->xmit);
	}

	if (!udev->ctrl->recv.buf) {
		/* This is protected by the per port mutex */
		page = get_zeroed_page(GFP_KERNEL);
		if (!page)
			return -ENOMEM;

		udev->ctrl->recv.buf = (unsigned char *) page;
		uart_circ_clear(&udev->ctrl->recv);
	}

	if (port && port->ops->pm)
		port->ops->pm(port, UART_PM_STATE_ON, state->pm_state);
	state->pm_state = UART_PM_STATE_ON;

	port->ops->set_mctrl(port, TIOCM_RTS | TIOCM_DTR);

	return udev->ctrl->port->ops->startup(udev->ctrl->port);
}

int uart_dev_rx(struct uart_device *udev, u8 *buf, size_t count)
{
	struct uart_port *port = udev->ctrl->port;
	struct uart_state *state = port->state;
	struct circ_buf *circ = &udev->ctrl->recv;
	int c, ret = 0;

	if (!circ->buf)
		return 0;

	while (1) {
		c = CIRC_CNT_TO_END(circ->head, circ->tail, UART_XMIT_SIZE);
		if (count < c)
			c = count;
		if (c <= 0)
			break;
		memcpy(buf, circ->buf + circ->tail, c);
		circ->tail = (circ->tail + c) & (PAGE_SIZE - 1);

		buf += c;
		count -= c;
		ret += c;
	}

	return ret;
}

int uart_dev_tx(struct uart_device *udev, u8 *buf, size_t count)
{
	struct uart_port *port = udev->ctrl->port;
	struct uart_state *state = port->state;
	struct circ_buf *circ = &state->xmit;
	unsigned long flags;
	int c, ret = 0;

	if (!circ->buf)
		return 0;

//	port = uart_port_lock(state, flags);
	while (1) {
		c = CIRC_SPACE_TO_END(circ->head, circ->tail, UART_XMIT_SIZE);
		if (count < c)
			c = count;
		if (c <= 0)
			break;
		memcpy(circ->buf + circ->head, buf, c);
		circ->head = (circ->head + c) & (UART_XMIT_SIZE - 1);
		buf += c;
		count -= c;
		ret += c;
	}
	port->ops->start_tx(port);

//	uart_port_unlock(port, flags);

	return ret;
}


static int uart_drv_probe(struct device *dev)
{
	const struct uart_dev_driver *sdrv = to_uart_dev_driver(dev->driver);
	struct uart_device *udev = to_uart_device(dev);
	int err;

	pm_runtime_get_noresume(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	err = sdrv->probe(udev);
	if (err)
		goto fail_probe;

	return 0;

fail_probe:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	pm_runtime_put_noidle(dev);
	return err;
}

static int uart_drv_remove(struct device *dev)
{
	const struct uart_dev_driver *sdrv = to_uart_dev_driver(dev->driver);

	pm_runtime_get_sync(dev);
	sdrv->remove(to_uart_device(dev));
	pm_runtime_put_noidle(dev);

	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	pm_runtime_put_noidle(dev);
	return 0;
}

static struct bus_type uart_bus_type = {
	.name		= "uart",
	.match		= uart_device_match,
	.probe		= uart_drv_probe,
	.remove		= uart_drv_remove,
};

/**
 * uart_controller_alloc() - Allocate a new SPMI device
 * @ctrl:	associated controller
 *
 * Caller is responsible for either calling uart_device_add() to add the
 * newly allocated controller, or calling uart_device_put() to discard it.
 */
struct uart_device *uart_device_alloc(struct uart_controller *ctrl)
{
	struct uart_device *udev;

	udev = kzalloc(sizeof(*udev), GFP_KERNEL);
	if (!udev)
		return NULL;

	udev->ctrl = ctrl;
	device_initialize(&udev->dev);
	udev->dev.parent = &ctrl->dev;
	udev->dev.bus = &uart_bus_type;
	udev->dev.type = &uart_dev_type;
	return udev;
}
EXPORT_SYMBOL_GPL(uart_device_alloc);

/**
 * uart_controller_alloc() - Allocate a new SPMI controller
 * @parent:	parent device
 * @size:	size of private data
 *
 * Caller is responsible for either calling uart_controller_add() to add the
 * newly allocated controller, or calling uart_controller_put() to discard it.
 * The allocated private data region may be accessed via
 * uart_controller_get_drvdata()
 */
struct uart_controller *uart_controller_alloc(struct device *parent,
					      size_t size)
{
	struct uart_controller *ctrl;
	int id;

	if (WARN_ON(!parent))
		return NULL;

	ctrl = kzalloc(sizeof(*ctrl) + size, GFP_KERNEL);
	if (!ctrl)
		return NULL;

	device_initialize(&ctrl->dev);
	ctrl->dev.type = &uart_ctrl_type;
	ctrl->dev.bus = &uart_bus_type;
	ctrl->dev.parent = parent;
	ctrl->dev.of_node = parent->of_node;
	uart_controller_set_drvdata(ctrl, &ctrl[1]);

	id = ida_simple_get(&ctrl_ida, 0, 0, GFP_KERNEL);
	if (id < 0) {
		dev_err(parent,
			"unable to allocate SPMI controller identifier.\n");
		uart_controller_put(ctrl);
		return NULL;
	}

	ctrl->nr = id;
	dev_set_name(&ctrl->dev, "uart-%d", id);

	dev_dbg(&ctrl->dev, "allocated controller 0x%p id %d\n", ctrl, id);
	return ctrl;
}
EXPORT_SYMBOL_GPL(uart_controller_alloc);

static void of_uart_register_devices(struct uart_controller *ctrl)
{
	struct device_node *node;
	int err;

	if (!ctrl->dev.of_node)
		return;

	for_each_available_child_of_node(ctrl->dev.of_node, node) {
		struct uart_device *udev;
		u32 reg[2];

		dev_dbg(&ctrl->dev, "adding child %s\n", node->full_name);

		udev = uart_device_alloc(ctrl);
		if (!udev)
			continue;

		udev->dev.of_node = node;

		err = uart_device_add(udev);
		if (err) {
			dev_err(&udev->dev,
				"failure adding device. status %d\n", err);
			uart_device_put(udev);
		}
	}
}

/**
 * uart_controller_add() - Add an SPMI controller
 * @ctrl:	controller to be registered.
 *
 * Register a controller previously allocated via uart_controller_alloc() with
 * the SPMI core.
 */
int uart_controller_add(struct uart_controller *ctrl)
{
	int ret;

	/* Can't register until after driver model init */
	if (WARN_ON(!is_registered))
		return -EAGAIN;

	ret = device_add(&ctrl->dev);
	if (ret)
		return ret;

	if (IS_ENABLED(CONFIG_OF))
		of_uart_register_devices(ctrl);

	dev_dbg(&ctrl->dev, "uart-%d registered: dev:%p\n",
		ctrl->nr, &ctrl->dev);

	return 0;
};
EXPORT_SYMBOL_GPL(uart_controller_add);

int uart_controller_rx(struct uart_controller *ctrl, unsigned int ch)
{
	struct circ_buf *circ = &ctrl->recv;
	unsigned long flags;
	int c, ret = 0;

	if (!circ->buf)
		return -ENODEV;

	c = CIRC_SPACE_TO_END(circ->head, circ->tail, PAGE_SIZE);
	if (c <= 0)
		return 0;

	circ->buf[circ->head] = ch;
	circ->head = (circ->head + 1) & (PAGE_SIZE - 1);

	return 1;
}

/* Remove a device associated with a controller */
static int uart_ctrl_remove_device(struct device *dev, void *data)
{
	struct uart_device *spmidev = to_uart_device(dev);
	if (dev->type == &uart_dev_type)
		uart_device_remove(spmidev);
	return 0;
}

/**
 * uart_controller_remove(): remove an SPMI controller
 * @ctrl:	controller to remove
 *
 * Remove a SPMI controller.  Caller is responsible for calling
 * uart_controller_put() to discard the allocated controller.
 */
void uart_controller_remove(struct uart_controller *ctrl)
{
	int dummy;

	if (!ctrl)
		return;

	dummy = device_for_each_child(&ctrl->dev, NULL,
				      uart_ctrl_remove_device);
	device_del(&ctrl->dev);
}
EXPORT_SYMBOL_GPL(uart_controller_remove);

/**
 * uart_driver_register() - Register client driver with SPMI core
 * @sdrv:	client driver to be associated with client-device.
 *
 * This API will register the client driver with the SPMI framework.
 * It is typically called from the driver's module-init function.
 */
int __uart_dev_driver_register(struct uart_dev_driver *sdrv, struct module *owner)
{
	sdrv->driver.bus = &uart_bus_type;
	sdrv->driver.owner = owner;
	return driver_register(&sdrv->driver);
}
EXPORT_SYMBOL_GPL(__uart_driver_register);

static void __exit uart_exit(void)
{
	bus_unregister(&uart_bus_type);
}
module_exit(uart_exit);

static int __init uart_init(void)
{
	int ret;

	ret = bus_register(&uart_bus_type);
	if (ret)
		return ret;

	is_registered = true;
	return 0;
}
postcore_initcall(uart_init);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("UART module");
//MODULE_ALIAS("platform:spmi");
