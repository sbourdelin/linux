/*
 * ACPI ResourceSource/IRQ domain mapping support
 *
 * Copyright (c) 2016, The Linux Foundation. All rights reserved.
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
#include <linux/acpi.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>

/**
 * acpi_irq_domain_register_irq() - Register the mapping for an IRQ produced
 *                                  by the given acpi_resource_source to a
 *                                  Linux IRQ number
 * @source: IRQ source
 * @hwirq: Hardware IRQ number
 * @trigger: trigger type of the IRQ number to be mapped
 * @polarity: polarity of the IRQ to be mapped
 *
 * Returns: a valid linux IRQ number on success
 *          -ENODEV if the given acpi_resource_source cannot be found
 *          -EPROBE_DEFER if the IRQ domain has not been registered
 *          -EINVAL for all other errors
 */
int acpi_irq_domain_register_irq(const struct acpi_resource_source *source,
				 u32 hwirq, int trigger, int polarity)
{
	struct irq_fwspec fwspec;
	struct acpi_device *device;
	acpi_handle handle;
	acpi_status status;
	int ret;

	/* An empty acpi_resource_source means it is a GSI */
	if (!source->string_length)
		return acpi_register_gsi(NULL, hwirq, trigger, polarity);

	status = acpi_get_handle(NULL, source->string_ptr, &handle);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	device = acpi_bus_get_acpi_device(handle);
	if (!device)
		return -ENODEV;

	if (irq_find_matching_fwnode(&device->fwnode, DOMAIN_BUS_ANY) == NULL) {
		ret = -EPROBE_DEFER;
		goto out_put_device;
	}

	fwspec.fwnode = &device->fwnode;
	fwspec.param[0] = hwirq;
	fwspec.param[1] = acpi_dev_get_irq_type(trigger, polarity);
	fwspec.param_count = 2;

	ret = irq_create_fwspec_mapping(&fwspec);

out_put_device:
	acpi_bus_put_acpi_device(device);

	return ret;
}
EXPORT_SYMBOL_GPL(acpi_irq_domain_register_irq);

/**
 * acpi_irq_domain_unregister_irq() - Delete the mapping for an IRQ produced
 *                                    by the given acpi_resource_source to a
 *                                    Linux IRQ number
 * @source: IRQ source
 * @hwirq: Hardware IRQ number
 *
 * Returns: 0 on success
 *          -ENODEV if the given acpi_resource_source cannot be found
 *          -EINVAL for all other errors
 */
int acpi_irq_domain_unregister_irq(const struct acpi_resource_source *source,
				   u32 hwirq)
{
	struct irq_domain *domain;
	struct acpi_device *device;
	acpi_handle handle;
	acpi_status status;
	int ret = 0;

	if (!source->string_length) {
		acpi_unregister_gsi(hwirq);
		return 0;
	}

	status = acpi_get_handle(NULL, source->string_ptr, &handle);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	device = acpi_bus_get_acpi_device(handle);
	if (!device)
		return -ENODEV;

	domain = irq_find_matching_fwnode(&device->fwnode, DOMAIN_BUS_ANY);
	if (!domain) {
		ret = -EINVAL;
		goto out_put_device;
	}

	irq_dispose_mapping(irq_find_mapping(domain, hwirq));

out_put_device:
	acpi_bus_put_acpi_device(device);

	return ret;
}
EXPORT_SYMBOL_GPL(acpi_irq_domain_unregister_irq);
