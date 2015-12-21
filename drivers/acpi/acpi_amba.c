
/*
 * ACPI support for platform bus type.
 *
 * Copyright (C) 2015, Linaro Ltd
 * Authors: Graeme Gregory <graeme.gregory@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/acpi.h>
#include <linux/amba/bus.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "internal.h"

static const struct acpi_device_id amba_id_list[] = {
	{"ARMH0011", 0}, /* PL011 SBSA Uart */
	{"ARMH0061", 0}, /* PL061 GPIO Device */
	{"", 0},
};

static struct clk *amba_dummy_clk;

static void amba_register_dummy_clk(void)
{
	struct clk *clk;

	/* If clock already registered */
	if (amba_dummy_clk)
		return;

	clk = clk_register_fixed_rate(NULL, "apb_pclk", NULL, CLK_IS_ROOT, 0);
	clk_register_clkdev(clk, "apb_pclk", NULL);

	amba_dummy_clk = clk;
}

static int amba_handler_attach(struct acpi_device *adev,
				const struct acpi_device_id *id)
{
	struct amba_device *dev = NULL;
	struct acpi_device *acpi_parent;
	struct resource_entry *rentry;
	struct list_head resource_list;
	struct resource *resources = NULL;
	bool address_found = false;
	int ret, count, irq_no = 0;

	/* If the ACPI node already has a physical device attached, skip it. */
	if (adev->physical_node_count)
		return 0;

	amba_register_dummy_clk();

	dev = amba_device_alloc(NULL, 0, 0);
	if (!dev) {
		pr_err("%s(): amba_device_alloc() failed for %s\n",
		       __func__, dev_name(&adev->dev));
		return 0;
	}

	INIT_LIST_HEAD(&resource_list);
	count = acpi_dev_get_resources(adev, &resource_list, NULL, NULL);
	if (count < 0) {
		return 0;
	} else if (count > 0) {
		resources = kmalloc_array(count, sizeof(struct resource),
				    GFP_KERNEL);
		if (!resources) {
			acpi_dev_free_resource_list(&resource_list);
			return 0;
		}
		count = 0;
		list_for_each_entry(rentry, &resource_list, node) {
			switch (resource_type(rentry->res)) {
			case IORESOURCE_MEM:
				if (!address_found) {
					dev->res = *rentry->res;
					address_found = true;
				}
				break;
			case IORESOURCE_IRQ:
				if (irq_no < AMBA_NR_IRQS)
					dev->irq[irq_no++] = rentry->res->start;
				break;
			default:
				dev_warn(&adev->dev, "Invalid resource\n");
			}
		}
		acpi_dev_free_resource_list(&resource_list);
	}

	/*
	 * If the ACPI node has a parent and that parent has a physical device
	 * attached to it, that physical device should be the parent of the
	 * platform device we are about to create.
	 */
	dev->dev.parent = NULL;
	acpi_parent = adev->parent;
	if (acpi_parent) {
		struct acpi_device_physical_node *entry;
		struct list_head *list;

		mutex_lock(&acpi_parent->physical_node_lock);
		list = &acpi_parent->physical_node_list;
		if (!list_empty(list)) {
			entry = list_first_entry(list,
					struct acpi_device_physical_node,
					node);
			dev->dev.parent = entry->dev;
		}
		mutex_unlock(&acpi_parent->physical_node_lock);
	}

	dev_set_name(&dev->dev, "%s", dev_name(&adev->dev));
	ACPI_COMPANION_SET(&dev->dev, adev);

	ret = amba_device_add(dev, &iomem_resource);
	if (ret) {
		pr_err("%s(): amba_device_add() failed (%d) for %s\n",
		       __func__, ret, dev_name(&adev->dev));
		goto err_free;
	}

	return 1;

err_free:
	amba_device_put(dev);
	return 0;
}

static struct acpi_scan_handler amba_handler = {
	.ids = amba_id_list,
	.attach = amba_handler_attach,
};

void __init acpi_amba_init(void)
{
	acpi_scan_add_handler(&amba_handler);
}
