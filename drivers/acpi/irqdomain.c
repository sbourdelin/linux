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
 * @rcirq: IRQ number
 * @trigger: trigger type of the IRQ number to be mapped
 * @polarity: polarity of the IRQ to be mapped
 *
 * Returns: a valid linux IRQ number on success
 *          -ENODEV if the given acpi_resource_source cannot be found
 *          -EPROBE_DEFER if the IRQ domain has not been registered
 *          -EINVAL for all other errors
 */
int acpi_irq_domain_register_irq(struct acpi_resource_source *source, u32 rcirq,
				 int trigger, int polarity)
{
	struct irq_domain *domain;
	struct acpi_device *device;
	acpi_handle handle;
	acpi_status status;
	unsigned int type;
	int ret;

	status = acpi_get_handle(NULL, source->string_ptr, &handle);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	device = acpi_bus_get_acpi_device(handle);
	if (!device)
		return -ENODEV;

	domain = irq_find_matching_fwnode(&device->fwnode, DOMAIN_BUS_ANY);
	if (!domain) {
		ret = -EPROBE_DEFER;
		goto out_put_device;
	}

	type = acpi_dev_get_irq_type(trigger, polarity);
	ret = irq_create_mapping(domain, rcirq);
	if (ret)
		irq_set_irq_type(ret, type);

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
 * @rcirq: IRQ number
 *
 * Returns: 0 on success
 *          -ENODEV if the given acpi_resource_source cannot be found
 *          -EINVAL for all other errors
 */
int acpi_irq_domain_unregister_irq(struct acpi_resource_source *source,
				   u32 rcirq)
{
	struct irq_domain *domain;
	struct acpi_device *device;
	acpi_handle handle;
	acpi_status status;
	int ret = 0;

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

	irq_dispose_mapping(irq_find_mapping(domain, rcirq));

out_put_device:
	acpi_bus_put_acpi_device(device);
	return ret;
}
EXPORT_SYMBOL_GPL(acpi_irq_domain_unregister_irq);
