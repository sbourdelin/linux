/*
 * File:	portdrv_pci.c
 * Purpose:	PCI Express Port Bus Driver
 *
 * Copyright (C) 2004 Intel
 * Copyright (C) Tom Long Nguyen (tom.l.nguyen@intel.com)
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/init.h>
#include <linux/pcieport_if.h>
#include <linux/aer.h>
#include <linux/dmi.h>
#include <linux/pci-aspm.h>

#include "portdrv.h"
#include "aer/aerdrv.h"
#include "../pci.h"

/*
 * Version Information
 */
#define DRIVER_VERSION "v1.0"
#define DRIVER_AUTHOR "tom.l.nguyen@intel.com"
#define DRIVER_DESC "PCIe Port Bus Driver"
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

/* If this switch is set, PCIe port native services should not be enabled. */
bool pcie_ports_disabled;

/*
 * If this switch is set, ACPI _OSC will be used to determine whether or not to
 * enable PCIe port native services.
 */
bool pcie_ports_auto = true;

static int __init pcie_port_setup(char *str)
{
	if (!strncmp(str, "compat", 6)) {
		pcie_ports_disabled = true;
	} else if (!strncmp(str, "native", 6)) {
		pcie_ports_disabled = false;
		pcie_ports_auto = false;
	} else if (!strncmp(str, "auto", 4)) {
		pcie_ports_disabled = false;
		pcie_ports_auto = true;
	}

	return 1;
}
__setup("pcie_ports=", pcie_port_setup);

/* global data */

/**
 * pcie_clear_root_pme_status - Clear root port PME interrupt status.
 * @dev: PCIe root port or event collector.
 */
void pcie_clear_root_pme_status(struct pci_dev *dev)
{
	pcie_capability_set_dword(dev, PCI_EXP_RTSTA, PCI_EXP_RTSTA_PME);
}

static int pcie_portdrv_restore_config(struct pci_dev *dev)
{
	int retval;

	retval = pci_enable_device(dev);
	if (retval)
		return retval;
	pci_set_master(dev);
	return 0;
}

enum pcie_port_type {
	PCIE_PORT_DEFAULT,
	PCIE_PORT_SPT,
};

struct pcie_port_config {
	bool suspend_allowed;
	bool runtime_suspend_allowed;
};

static const struct pcie_port_config pcie_port_configs[] = {
	[PCIE_PORT_DEFAULT] = {
		.suspend_allowed = true,
	},
	[PCIE_PORT_SPT] = {
		.suspend_allowed = true,
		.runtime_suspend_allowed = true,
	},
};

#ifdef CONFIG_PM
static const struct pcie_port_config *pcie_port_get_config(struct pci_dev *pdev)
{
	const struct pci_device_id *id = pci_match_id(pdev->driver->id_table,
						      pdev);
	return &pcie_port_configs[id->driver_data];
}

static int pcie_port_check_d3cold(struct pci_dev *pdev, void *data)
{
	bool *d3cold_ok = data;

	if (pdev->no_d3cold || !pdev->d3cold_allowed)
		*d3cold_ok = false;
	if (device_may_wakeup(&pdev->dev) && !pci_pme_capable(pdev, PCI_D3cold))
		*d3cold_ok = false;

	return !*d3cold_ok;
}

static bool pcie_port_can_suspend(struct pci_dev *pdev)
{
	bool d3cold_ok = true;

	/*
	 * When the port is put to D3hot the devices behind the port are
	 * effectively in D3cold as their config space cannot be accessed
	 * anymore and the link may be powered down.
	 *
	 * We only allow the port to go to D3hot the devices:
	 *  - Are allowed to go to D3cold
	 *  - Can wake up from D3cold if they are wake capable
	 */
	pci_walk_bus(pdev->subordinate, pcie_port_check_d3cold, &d3cold_ok);
	return d3cold_ok;
}

static bool pcie_port_suspend_allowed(struct pci_dev *pdev)
{
	const struct pcie_port_config *config = pcie_port_get_config(pdev);

	/*
	 * Older hardware is not capable of moving PCIe ports to D3 so
	 * anything earlier than 2015 is assumed not to support this.
	 */
	if (dmi_available) {
		unsigned year;

		if (!dmi_get_date(DMI_BIOS_DATE, &year, NULL, NULL) ||
		    year < 2015) {
			return false;
		}
	}

	/* Per port configuration can forbid it as well */
	if (!config->suspend_allowed)
		return false;

	return pcie_port_can_suspend(pdev);
}

static bool pcie_port_runtime_suspend_allowed(struct pci_dev *pdev)
{
	return pcie_port_get_config(pdev)->runtime_suspend_allowed;
}

static int pcie_port_suspend_noirq(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	if (pcie_port_suspend_allowed(pdev)) {
		pci_save_state(pdev);
		pci_set_power_state(pdev, PCI_D3hot);
		/*
		 * All devices behind the port are assumed to be in D3cold
		 * so update their state now.
		 */
		__pci_bus_set_current_state(pdev->subordinate, PCI_D3cold);
	}

	return 0;
}

static int pcie_port_resume_noirq(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	pci_set_power_state(pdev, PCI_D0);
	pci_restore_state(pdev);

	/*
	 * Some BIOSes forget to clear Root PME Status bits after system wakeup
	 * which breaks ACPI-based runtime wakeup on PCI Express, so clear those
	 * bits now just in case (shouldn't hurt).
	 */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT)
		pcie_clear_root_pme_status(pdev);
	return 0;
}

static int pcie_port_runtime_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	/*
	 * All devices behind the port are assumed to be in D3cold so
	 * update their state now.
	 */
	__pci_bus_set_current_state(pdev->subordinate, PCI_D3cold);
	return 0;
}

static int pcie_port_runtime_resume(struct device *dev)
{
	return 0;
}

static int pcie_port_runtime_idle(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	if (pcie_port_can_suspend(pdev)) {
		pm_schedule_suspend(dev, 10);
		return 0;
	}
	return -EBUSY;
}

static const struct dev_pm_ops pcie_portdrv_pm_ops = {
	.suspend	= pcie_port_device_suspend,
	.resume		= pcie_port_device_resume,
	.freeze		= pcie_port_device_suspend,
	.thaw		= pcie_port_device_resume,
	.poweroff	= pcie_port_device_suspend,
	.restore	= pcie_port_device_resume,
	.suspend_noirq	= pcie_port_suspend_noirq,
	.resume_noirq	= pcie_port_resume_noirq,
	.runtime_suspend = pcie_port_runtime_suspend,
	.runtime_resume	= pcie_port_runtime_resume,
	.runtime_idle	= pcie_port_runtime_idle,
};

#define PCIE_PORTDRV_PM_OPS	(&pcie_portdrv_pm_ops)

#else /* !PM */

static inline bool pcie_port_runtime_suspend_allowed(struct pci_dev *pdev)
{
	return false;
}

#define PCIE_PORTDRV_PM_OPS	NULL
#endif /* !PM */

/*
 * pcie_portdrv_probe - Probe PCI-Express port devices
 * @dev: PCI-Express port device being probed
 *
 * If detected invokes the pcie_port_device_register() method for
 * this port device.
 *
 */
static int pcie_portdrv_probe(struct pci_dev *dev,
					const struct pci_device_id *id)
{
	int status;

	if (!pci_is_pcie(dev) ||
	    ((pci_pcie_type(dev) != PCI_EXP_TYPE_ROOT_PORT) &&
	     (pci_pcie_type(dev) != PCI_EXP_TYPE_UPSTREAM) &&
	     (pci_pcie_type(dev) != PCI_EXP_TYPE_DOWNSTREAM)))
		return -ENODEV;

	status = pcie_port_device_register(dev);
	if (status)
		return status;

	pci_save_state(dev);

	if (pcie_port_runtime_suspend_allowed(dev))
		pm_runtime_put_noidle(&dev->dev);

	return 0;
}

static void pcie_portdrv_remove(struct pci_dev *dev)
{
	if (pcie_port_runtime_suspend_allowed(dev))
		pm_runtime_get_noresume(&dev->dev);

	pcie_port_device_remove(dev);
}

static int error_detected_iter(struct device *device, void *data)
{
	struct pcie_device *pcie_device;
	struct pcie_port_service_driver *driver;
	struct aer_broadcast_data *result_data;
	pci_ers_result_t status;

	result_data = (struct aer_broadcast_data *) data;

	if (device->bus == &pcie_port_bus_type && device->driver) {
		driver = to_service_driver(device->driver);
		if (!driver ||
			!driver->err_handler ||
			!driver->err_handler->error_detected)
			return 0;

		pcie_device = to_pcie_device(device);

		/* Forward error detected message to service drivers */
		status = driver->err_handler->error_detected(
			pcie_device->port,
			result_data->state);
		result_data->result =
			merge_result(result_data->result, status);
	}

	return 0;
}

static pci_ers_result_t pcie_portdrv_error_detected(struct pci_dev *dev,
					enum pci_channel_state error)
{
	struct aer_broadcast_data data = {error, PCI_ERS_RESULT_CAN_RECOVER};

	/* get true return value from &data */
	device_for_each_child(&dev->dev, &data, error_detected_iter);
	return data.result;
}

static int mmio_enabled_iter(struct device *device, void *data)
{
	struct pcie_device *pcie_device;
	struct pcie_port_service_driver *driver;
	pci_ers_result_t status, *result;

	result = (pci_ers_result_t *) data;

	if (device->bus == &pcie_port_bus_type && device->driver) {
		driver = to_service_driver(device->driver);
		if (driver &&
			driver->err_handler &&
			driver->err_handler->mmio_enabled) {
			pcie_device = to_pcie_device(device);

			/* Forward error message to service drivers */
			status = driver->err_handler->mmio_enabled(
					pcie_device->port);
			*result = merge_result(*result, status);
		}
	}

	return 0;
}

static pci_ers_result_t pcie_portdrv_mmio_enabled(struct pci_dev *dev)
{
	pci_ers_result_t status = PCI_ERS_RESULT_RECOVERED;

	/* get true return value from &status */
	device_for_each_child(&dev->dev, &status, mmio_enabled_iter);
	return status;
}

static int slot_reset_iter(struct device *device, void *data)
{
	struct pcie_device *pcie_device;
	struct pcie_port_service_driver *driver;
	pci_ers_result_t status, *result;

	result = (pci_ers_result_t *) data;

	if (device->bus == &pcie_port_bus_type && device->driver) {
		driver = to_service_driver(device->driver);
		if (driver &&
			driver->err_handler &&
			driver->err_handler->slot_reset) {
			pcie_device = to_pcie_device(device);

			/* Forward error message to service drivers */
			status = driver->err_handler->slot_reset(
					pcie_device->port);
			*result = merge_result(*result, status);
		}
	}

	return 0;
}

static pci_ers_result_t pcie_portdrv_slot_reset(struct pci_dev *dev)
{
	pci_ers_result_t status = PCI_ERS_RESULT_RECOVERED;

	/* If fatal, restore cfg space for possible link reset at upstream */
	if (dev->error_state == pci_channel_io_frozen) {
		dev->state_saved = true;
		pci_restore_state(dev);
		pcie_portdrv_restore_config(dev);
		pci_enable_pcie_error_reporting(dev);
	}

	/* get true return value from &status */
	device_for_each_child(&dev->dev, &status, slot_reset_iter);
	return status;
}

static int resume_iter(struct device *device, void *data)
{
	struct pcie_device *pcie_device;
	struct pcie_port_service_driver *driver;

	if (device->bus == &pcie_port_bus_type && device->driver) {
		driver = to_service_driver(device->driver);
		if (driver &&
			driver->err_handler &&
			driver->err_handler->resume) {
			pcie_device = to_pcie_device(device);

			/* Forward error message to service drivers */
			driver->err_handler->resume(pcie_device->port);
		}
	}

	return 0;
}

static void pcie_portdrv_err_resume(struct pci_dev *dev)
{
	device_for_each_child(&dev->dev, NULL, resume_iter);
}

/*
 * LINUX Device Driver Model
 */
static const struct pci_device_id port_pci_ids[] = {
	/* Intel Sunrisepoint */
	{ PCI_VDEVICE(INTEL, 0x9d14), .driver_data = PCIE_PORT_SPT },
	{ PCI_VDEVICE(INTEL, 0x9d15), .driver_data = PCIE_PORT_SPT },
	/* handle any PCI-Express port */
	{ PCI_DEVICE_CLASS(((PCI_CLASS_BRIDGE_PCI << 8) | 0x00), ~0),
	  .driver_data = PCIE_PORT_DEFAULT },
	{ /* end: all zeroes */ }
};
MODULE_DEVICE_TABLE(pci, port_pci_ids);

static const struct pci_error_handlers pcie_portdrv_err_handler = {
	.error_detected = pcie_portdrv_error_detected,
	.mmio_enabled = pcie_portdrv_mmio_enabled,
	.slot_reset = pcie_portdrv_slot_reset,
	.resume = pcie_portdrv_err_resume,
};

static struct pci_driver pcie_portdriver = {
	.name		= "pcieport",
	.id_table	= &port_pci_ids[0],

	.probe		= pcie_portdrv_probe,
	.remove		= pcie_portdrv_remove,

	.err_handler	= &pcie_portdrv_err_handler,

	.driver.pm	= PCIE_PORTDRV_PM_OPS,
};

static int __init dmi_pcie_pme_disable_msi(const struct dmi_system_id *d)
{
	pr_notice("%s detected: will not use MSI for PCIe PME signaling\n",
		  d->ident);
	pcie_pme_disable_msi();
	return 0;
}

static struct dmi_system_id __initdata pcie_portdrv_dmi_table[] = {
	/*
	 * Boxes that should not use MSI for PCIe PME signaling.
	 */
	{
	 .callback = dmi_pcie_pme_disable_msi,
	 .ident = "MSI Wind U-100",
	 .matches = {
		     DMI_MATCH(DMI_SYS_VENDOR,
				"MICRO-STAR INTERNATIONAL CO., LTD"),
		     DMI_MATCH(DMI_PRODUCT_NAME, "U-100"),
		     },
	 },
	 {}
};

static int __init pcie_portdrv_init(void)
{
	int retval;

	if (pcie_ports_disabled)
		return pci_register_driver(&pcie_portdriver);

	dmi_check_system(pcie_portdrv_dmi_table);

	retval = pcie_port_bus_register();
	if (retval) {
		printk(KERN_WARNING "PCIE: bus_register error: %d\n", retval);
		goto out;
	}
	retval = pci_register_driver(&pcie_portdriver);
	if (retval)
		pcie_port_bus_unregister();
 out:
	return retval;
}

module_init(pcie_portdrv_init);
