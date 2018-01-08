/*
 * Copyright (c) 2017, The Linux Foundation. All rights reserved.

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/pci.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/aer.h>
#include <linux/pcieport_if.h>
#include "portdrv.h"

static DEFINE_MUTEX(pci_err_recovery_lock);

pci_ers_result_t pci_merge_result(enum pci_ers_result orig,
				  enum pci_ers_result new)
{
	if (new == PCI_ERS_RESULT_NO_AER_DRIVER)
		return PCI_ERS_RESULT_NO_AER_DRIVER;

	if (new == PCI_ERS_RESULT_NONE)
		return orig;

	switch (orig) {
	case PCI_ERS_RESULT_CAN_RECOVER:
	case PCI_ERS_RESULT_RECOVERED:
		orig = new;
		break;
	case PCI_ERS_RESULT_DISCONNECT:
		if (new == PCI_ERS_RESULT_NEED_RESET)
			orig = PCI_ERS_RESULT_NEED_RESET;
		break;
	default:
		break;
	}

	return orig;
}

int pci_report_mmio_enabled(struct pci_dev *dev, void *data)
{
	pci_ers_result_t vote;
	const struct pci_error_handlers *err_handler;
	struct pci_err_broadcast_data *result_data;

	result_data = (struct pci_err_broadcast_data *) data;

	device_lock(&dev->dev);
	if (!dev->driver ||
		!dev->driver->err_handler ||
		!dev->driver->err_handler->mmio_enabled)
		goto out;

	err_handler = dev->driver->err_handler;
	vote = err_handler->mmio_enabled(dev);
	result_data->result = pci_merge_result(result_data->result, vote);
out:
	device_unlock(&dev->dev);
	return 0;
}

int pci_report_slot_reset(struct pci_dev *dev, void *data)
{
	pci_ers_result_t vote;
	const struct pci_error_handlers *err_handler;
	struct pci_err_broadcast_data *result_data;

	result_data = (struct pci_err_broadcast_data *) data;

	device_lock(&dev->dev);
	if (!dev->driver ||
		!dev->driver->err_handler ||
		!dev->driver->err_handler->slot_reset)
		goto out;

	err_handler = dev->driver->err_handler;
	vote = err_handler->slot_reset(dev);
	result_data->result = pci_merge_result(result_data->result, vote);
out:
	device_unlock(&dev->dev);
	return 0;
}

int pci_report_resume(struct pci_dev *dev, void *data)
{
	const struct pci_error_handlers *err_handler;

	device_lock(&dev->dev);
	dev->error_state = pci_channel_io_normal;

	if (!dev->driver ||
		!dev->driver->err_handler ||
		!dev->driver->err_handler->resume)
		goto out;

	err_handler = dev->driver->err_handler;
	err_handler->resume(dev);
out:
	device_unlock(&dev->dev);
	return 0;
}

int pci_report_error_detected(struct pci_dev *dev, void *data)
{
	pci_ers_result_t vote;
	const struct pci_error_handlers *err_handler;
	struct pci_err_broadcast_data *result_data;

	result_data = (struct pci_err_broadcast_data *) data;

	device_lock(&dev->dev);
	dev->error_state = result_data->state;

	if (!dev->driver ||
		!dev->driver->err_handler ||
		!dev->driver->err_handler->error_detected) {
		if (result_data->state == pci_channel_io_frozen &&
			dev->hdr_type != PCI_HEADER_TYPE_BRIDGE) {
			/*
			 * In case of fatal recovery, if one of down-
			 * stream device has no driver. We might be
			 * unable to recover because a later insmod
			 * of a driver for this device is unaware of
			 * its hw state.
			 */
			dev_printk(KERN_DEBUG, &dev->dev, "device has %s\n",
				   dev->driver ?
				   "no error-aware driver" : "no driver");
		}

		/*
		 * If there's any device in the subtree that does not
		 * have an error_detected callback, returning
		 * PCI_ERS_RESULT_NO_AER_DRIVER prevents calling of
		 * the subsequent mmio_enabled/slot_reset/resume
		 * callbacks of "any" device in the subtree. All the
		 * devices in the subtree are left in the error state
		 * without recovery.
		 */

		if (dev->hdr_type != PCI_HEADER_TYPE_BRIDGE)
			vote = PCI_ERS_RESULT_NO_AER_DRIVER;
		else
			vote = PCI_ERS_RESULT_NONE;
	} else {
		err_handler = dev->driver->err_handler;
		vote = err_handler->error_detected(dev, result_data->state);
	}

	result_data->result = pci_merge_result(result_data->result, vote);
	device_unlock(&dev->dev);
	return 0;
}

/**
 * pci_default_reset_link - default reset function
 * @dev: pointer to pci_dev data structure
 *
 * Invoked when performing link reset on a Downstream Port or a
 * Root Port with no aer driver.
 */
static pci_ers_result_t pci_default_reset_link(struct pci_dev *dev)
{
	pci_reset_bridge_secondary_bus(dev);
	dev_printk(KERN_DEBUG, &dev->dev, "downstream link has been reset\n");
	return PCI_ERS_RESULT_RECOVERED;
}

pci_ers_result_t pci_reset_link(struct pci_dev *dev, int severity)
{
	struct pci_dev *udev;
	pci_ers_result_t status;
	struct pcie_port_service_driver *driver = NULL;

	if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE) {
		/* Reset this port for all subordinates */
		udev = dev;
	} else {
		/* Reset the upstream component (likely downstream port) */
		udev = dev->bus->self;
	}


	/* Use the service driver of the component firstly */
#if IS_ENABLED(CONFIG_PCIEDPC)
	if (severity == PCI_ERR_DPC_FATAL)
		driver = pci_find_dpc_service(udev);
#endif
#if IS_ENABLED(CONFIG_PCIEAER)
	if ((severity == PCI_ERR_AER_FATAL) ||
	    (severity == PCI_ERR_AER_NONFATAL) ||
	    (severity == PCI_ERR_AER_CORRECTABLE))
		driver = pci_find_aer_service(udev);
#endif

	if (driver && driver->reset_link) {
		status = driver->reset_link(udev);
	} else if (udev->has_secondary_link) {
		status = pci_default_reset_link(udev);
	} else {
		dev_printk(KERN_DEBUG, &dev->dev,
			"no link-reset support at upstream device %s\n",
			pci_name(udev));
		return PCI_ERS_RESULT_DISCONNECT;
	}

	if (status != PCI_ERS_RESULT_RECOVERED) {
		dev_printk(KERN_DEBUG, &dev->dev,
			"link reset at upstream device %s failed\n",
			pci_name(udev));
		return PCI_ERS_RESULT_DISCONNECT;
	}

	return status;
}

/**
 * pci_broadcast_error_message - handle message broadcast to downstream drivers
 * @dev: pointer to from where in a hierarchy message is broadcasted down
 * @state: error state
 * @error_mesg: message to print
 * @cb: callback to be broadcasted
 *
 * Invoked during error recovery process. Once being invoked, the content
 * of error severity will be broadcasted to all downstream drivers in a
 * hierarchy in question.
 */
pci_ers_result_t pci_broadcast_error_message(struct pci_dev *dev,
	enum pci_channel_state state,
	char *error_mesg,
	int (*cb)(struct pci_dev *, void *),
	int severity)
{
	struct pci_err_broadcast_data result_data;

	dev_printk(KERN_DEBUG, &dev->dev, "broadcast %s message\n", error_mesg);
	result_data.state = state;
	if (cb == pci_report_error_detected)
		result_data.result = PCI_ERS_RESULT_CAN_RECOVER;
	else
		result_data.result = PCI_ERS_RESULT_RECOVERED;

	if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE) {
		/* If DPC is triggered, call resume error hanlder
		 * because, at this point we can safely assume that
		 * link recovery has happened.
		 */
		if ((severity == PCI_ERR_DPC_FATAL) &&
			(cb == pci_report_resume)) {
			cb(dev, NULL);
			return PCI_ERS_RESULT_RECOVERED;
		}
		/*
		 * If the error is reported by a bridge, we think this error
		 * is related to the downstream link of the bridge, so we
		 * do error recovery on all subordinates of the bridge instead
		 * of the bridge and clear the error status of the bridge.
		 */
		if (cb == pci_report_error_detected)
			dev->error_state = state;
		pci_walk_bus(dev->subordinate, cb, &result_data);
		if (cb == pci_report_resume) {
			pci_cleanup_aer_uncorrect_error_status(dev);
			dev->error_state = pci_channel_io_normal;
		}
	} else {
		/*
		 * If the error is reported by an end point, we think this
		 * error is related to the upstream link of the end point.
		 */
		pci_walk_bus(dev->bus, cb, &result_data);
	}

	return result_data.result;
}

/**
 * pci_do_recovery - handle nonfatal/fatal error recovery process
 * @dev: pointer to a pci_dev data structure of agent detecting an error
 * @severity: error severity type
 *
 * Invoked when an error is nonfatal/fatal. Once being invoked, broadcast
 * error detected message to all downstream drivers within a hierarchy in
 * question and return the returned code.
 */
void pci_do_recovery(struct pci_dev *dev, int severity)
{
	pci_ers_result_t status, result = PCI_ERS_RESULT_RECOVERED;
	enum pci_channel_state state;

	mutex_lock(&pci_err_recovery_lock);

	if ((severity == PCI_ERR_AER_FATAL) ||
	    (severity == PCI_ERR_DPC_FATAL))
		state = pci_channel_io_frozen;
	else
		state = pci_channel_io_normal;

	status = pci_broadcast_error_message(dev,
			state,
			"error_detected",
			pci_report_error_detected,
			severity);

	if ((severity == PCI_ERR_AER_FATAL) ||
	    (severity == PCI_ERR_DPC_FATAL)) {
		result = pci_reset_link(dev, severity);
		if (result != PCI_ERS_RESULT_RECOVERED)
			goto failed;
	}

	if (severity == PCI_ERR_DPC_FATAL)
		goto resume;

	if (status == PCI_ERS_RESULT_CAN_RECOVER)
		status = pci_broadcast_error_message(dev,
				state,
				"mmio_enabled",
				pci_report_mmio_enabled,
				severity);

	if (status == PCI_ERS_RESULT_NEED_RESET) {
		/*
		 * TODO: Should call platform-specific
		 * functions to reset slot before calling
		 * drivers' slot_reset callbacks?
		 */
		status = pci_broadcast_error_message(dev,
				state,
				"slot_reset",
				pci_report_slot_reset,
				severity);
	}

	if (status != PCI_ERS_RESULT_RECOVERED)
		goto failed;

resume:
	pci_broadcast_error_message(dev,
				state,
				"resume",
				pci_report_resume,
				severity);

	dev_info(&dev->dev, "Device recovery successful\n");
	mutex_unlock(&pci_err_recovery_lock);
	return;

failed:
	/* TODO: Should kernel panic here? */
	mutex_unlock(&pci_err_recovery_lock);
	dev_info(&dev->dev, "Device recovery failed\n");
}
