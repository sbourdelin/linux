// SPDX-License-Identifier: GPL-2.0
/*
 * This file implements the error recovery as a core part of PCIe error
 * reporting. When a PCIe error is delivered, an error message will be
 * collected and printed to console, then, an error recovery procedure
 * will be executed by following the PCI error recovery rules.
 *
 * Copyright (C) 2006 Intel Corp.
 *	Tom Long Nguyen (tom.l.nguyen@intel.com)
 *	Zhang Yanmin (yanmin.zhang@intel.com)
 */

#include <linux/pci.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/aer.h>
#include "portdrv.h"
#include "../pci.h"

struct aer_broadcast_data {
	enum pci_channel_state state;
	enum pci_ers_result result;
};

static pci_ers_result_t merge_result(enum pci_ers_result orig,
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

static int report_error_detected(struct pci_dev *dev, void *data)
{
	pci_ers_result_t vote;
	const struct pci_error_handlers *err_handler;
	struct aer_broadcast_data *result_data;

	result_data = (struct aer_broadcast_data *) data;

	device_lock(&dev->dev);
	dev->error_state = result_data->state;

	if (!dev->driver ||
		!dev->driver->err_handler ||
		!dev->driver->err_handler->error_detected) {
		/*
		 * If any device in the subtree does not have an error_detected
		 * callback, PCI_ERS_RESULT_NO_AER_DRIVER prevents subsequent
		 * erronr callbacks of "any" device in the subtree, and will
		 * exit in the disconnected error state.
		 */
		if (dev->hdr_type != PCI_HEADER_TYPE_BRIDGE)
			vote = PCI_ERS_RESULT_NO_AER_DRIVER;
		else
			vote = PCI_ERS_RESULT_NONE;
	} else {
		err_handler = dev->driver->err_handler;
		vote = err_handler->error_detected(dev, result_data->state);
		pci_uevent_ers(dev, PCI_ERS_RESULT_NONE);
	}

	result_data->result = merge_result(result_data->result, vote);
	device_unlock(&dev->dev);
	return 0;
}

static int report_mmio_enabled(struct pci_dev *dev, void *data)
{
	pci_ers_result_t vote;
	const struct pci_error_handlers *err_handler;
	struct aer_broadcast_data *result_data;

	result_data = (struct aer_broadcast_data *) data;

	device_lock(&dev->dev);
	if (!dev->driver ||
		!dev->driver->err_handler ||
		!dev->driver->err_handler->mmio_enabled)
		goto out;

	err_handler = dev->driver->err_handler;
	vote = err_handler->mmio_enabled(dev);
	result_data->result = merge_result(result_data->result, vote);
out:
	device_unlock(&dev->dev);
	return 0;
}

static int report_slot_reset(struct pci_dev *dev, void *data)
{
	pci_ers_result_t vote;
	const struct pci_error_handlers *err_handler;
	struct aer_broadcast_data *result_data;

	result_data = (struct aer_broadcast_data *) data;

	device_lock(&dev->dev);
	if (!dev->driver ||
		!dev->driver->err_handler ||
		!dev->driver->err_handler->slot_reset)
		goto out;

	err_handler = dev->driver->err_handler;
	vote = err_handler->slot_reset(dev);
	result_data->result = merge_result(result_data->result, vote);
out:
	device_unlock(&dev->dev);
	return 0;
}

static int report_resume(struct pci_dev *dev, void *data)
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
	pci_uevent_ers(dev, PCI_ERS_RESULT_RECOVERED);
out:
	device_unlock(&dev->dev);
	return 0;
}

static int report_disconnect(struct pci_dev *dev, void *data)
{
	device_lock(&dev->dev);
	pci_dev_set_disconnected(dev, NULL);
	pci_uevent_ers(dev, PCI_ERS_RESULT_DISCONNECT);
	device_unlock(&dev->dev);
	return 0;
}

/**
 * default_reset_link - default reset function
 * @dev: pointer to pci_dev data structure
 *
 * Invoked when performing link reset on a Downstream Port or a
 * Root Port with no aer driver.
 */
static pci_ers_result_t default_reset_link(struct pci_dev *dev)
{
	int rc;

	rc = pci_bus_error_reset(dev);
	pci_printk(KERN_DEBUG, dev, "downstream link has been reset\n");
	return rc ? PCI_ERS_RESULT_DISCONNECT : PCI_ERS_RESULT_RECOVERED;
}

static pci_ers_result_t reset_link(struct pci_dev *dev, u32 service)
{
	pci_ers_result_t status;
	struct pcie_port_service_driver *driver = NULL;

	driver = pcie_port_find_service(dev, service);
	if (driver && driver->reset_link) {
		status = driver->reset_link(dev);
	} else if (dev->has_secondary_link) {
		status = default_reset_link(dev);
	} else {
		pci_printk(KERN_DEBUG, dev, "no link-reset support at upstream device %s\n",
			pci_name(dev));
		return PCI_ERS_RESULT_DISCONNECT;
	}

	if (status != PCI_ERS_RESULT_RECOVERED) {
		pci_printk(KERN_DEBUG, dev, "link reset at upstream device %s failed\n",
			pci_name(dev));
		return PCI_ERS_RESULT_DISCONNECT;
	}

	return status;
}

/**
 * broadcast_error_message - handle message broadcast to downstream drivers
 * @dev: pointer to from where in a hierarchy message is broadcasted down
 * @state: error state
 * @error_mesg: message to print
 * @cb: callback to be broadcasted
 *
 * Invoked during error recovery process. Once being invoked, the content
 * of error severity will be broadcasted to all downstream drivers in a
 * hierarchy in question.
 */
static pci_ers_result_t broadcast_error_message(struct pci_dev *dev,
	enum pci_channel_state state,
	char *error_mesg,
	int (*cb)(struct pci_dev *, void *))
{
	struct aer_broadcast_data result_data;

	pci_printk(KERN_DEBUG, dev, "broadcast %s message\n", error_mesg);
	result_data.state = state;
	if (cb == report_error_detected)
		result_data.result = PCI_ERS_RESULT_CAN_RECOVER;
	else
		result_data.result = PCI_ERS_RESULT_RECOVERED;

	pci_walk_bus(dev->subordinate, cb, &result_data);
	return result_data.result;
}

/**
 * pcie_disconnect_device - Called when error handling ends with
 * 			    PCI_ERS_RESULT_DISCONNECT status.
 *
 * Reaching here means error handling has irrevocably failed. This function
 * will ungracefully disconnect all the devices below the bus that has
 * experienced the unrecoverable error.
 *
 * If the link is active after the removing all devices on the bus, this will
 * attempt to re-enumerate the bus from scratch.
 */
static void pcie_disconnect_device(struct pci_dev *dev)
{
	struct pci_bus *bus = dev->subordinate;
	struct pci_dev *child, *tmp;

	broadcast_error_message(dev, PCI_ERS_RESULT_DISCONNECT,
				"disconnect", report_disconnect);
	pci_lock_rescan_remove();
	list_for_each_entry_safe(child, tmp, &bus->devices, bus_list)
		pci_stop_and_remove_bus_device(child);

	pci_bridge_secondary_bus_reset(dev);
	if (pcie_wait_for_link(dev, true))
		pci_rescan_bus(bus);
	pci_unlock_rescan_remove();
}

static void pcie_do_recovery(struct pci_dev *dev, enum pci_channel_state state,
			     u32 service)
{
	pci_ers_result_t status;

	/*
	 * Error recovery runs on all subordinates of the first downstream port.
	 * If the downstream port detected the error, it is cleared at the end.
	 */
	if (!(pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT ||
	      pci_pcie_type(dev) == PCI_EXP_TYPE_DOWNSTREAM))
		dev = dev->bus->self;

	status = broadcast_error_message(dev,
			state,
			"error_detected",
			report_error_detected);

	if (state == pci_channel_io_frozen &&
	    reset_link(dev, service) != PCI_ERS_RESULT_RECOVERED)
		goto failed;

	if (status == PCI_ERS_RESULT_CAN_RECOVER)
		status = broadcast_error_message(dev,
				state,
				"mmio_enabled",
				report_mmio_enabled);

	if (status == PCI_ERS_RESULT_NEED_RESET) {
		/*
		 * TODO: Should call platform-specific
		 * functions to reset slot before calling
		 * drivers' slot_reset callbacks?
		 */
		status = broadcast_error_message(dev,
				state,
				"slot_reset",
				report_slot_reset);
	}

	if (status != PCI_ERS_RESULT_RECOVERED)
		goto failed;

	broadcast_error_message(dev,
				state,
				"resume",
				report_resume);

	pci_aer_clear_device_status(dev);
	pci_cleanup_aer_uncorrect_error_status(dev);
	pci_info(dev, "AER: Device recovery successful\n");
	return;
failed:
	pci_info(dev, "AER: Device recovery failed\n");
	pcie_disconnect_device(dev);
}

void pcie_do_fatal_recovery(struct pci_dev *dev, u32 service)
{
	pcie_do_recovery(dev, pci_channel_io_frozen, service);
}

void pcie_do_nonfatal_recovery(struct pci_dev *dev)
{
	pcie_do_recovery(dev, pci_channel_io_normal, PCIE_PORT_SERVICE_AER);
}
