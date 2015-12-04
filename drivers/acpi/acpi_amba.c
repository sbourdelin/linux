/*
 * ACPI support for AMBA bus type.
 *
 * Copyright (C) 2015, Advanced Micro Devices, Inc.
 * Authors: Huang Rui <ray.huang@amd.com>
 *          Wang Hongcheng <annie.wang@amd.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/amba/bus.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/clk.h>

#include "internal.h"

ACPI_MODULE_NAME("amba");

/**
 * acpi_create_amba_device - Create AMBA device for ACPI device node.
 * @adev: ACPI device node to create an AMBA device.
 * @periphid: AMBA device periphid.
 * @fixed_rate: Clock frequency.
 * @pdata: Platform data specific to the device.
 *
 * Check if the given @adev can be represented as an AMBA device and, if
 * that's the case, create and register an AMBA device, populate its
 * common resources and returns a pointer to it.  Otherwise, return
 * %NULL or ERR_PTR() on error.
 *
 * Name of the AMBA device will be the same as @adev's.
 */
struct amba_device *acpi_create_amba_device(struct acpi_device *adev,
					    unsigned int periphid,
					    unsigned long fixed_rate,
					    void *pdata)
{
	struct amba_device *amba_dev = NULL;
	struct device *parent;
	struct acpi_device *acpi_parent;
	struct resource_entry *rentry;
	struct list_head resource_list;
	struct resource *resource = NULL;
	int count, ret = 0;
	unsigned int i;
	unsigned int irq[AMBA_NR_IRQS];
	struct clk *clk = ERR_PTR(-ENODEV);

	/*
	 * If the ACPI node already has a physical device attached,
	 * skip it.
	 */
	if (adev->physical_node_count)
		return NULL;

	INIT_LIST_HEAD(&resource_list);
	count = acpi_dev_get_resources(adev, &resource_list, NULL, NULL);
	if (count <= 0)
		return NULL;

	resource = kzalloc(sizeof(*resource), GFP_KERNEL);
	if (!resource)
		goto resource_alloc_err;

	count = 0;
	list_for_each_entry(rentry, &resource_list, node) {
		if (resource_type(rentry->res) == IORESOURCE_IRQ) {
			irq[count] = rentry->res->start;
			count++;
		}
		/*
		 * there is only one io memory resource entry
		 * at current AMBA device design
		 */
		if (resource_type(rentry->res) | IORESOURCE_MEM)
			memcpy(resource, rentry->res, sizeof(struct resource));
	}

	amba_dev = amba_device_alloc(dev_name(&adev->dev),
				     resource->start,
				     resource_size(resource));

	if (!amba_dev)
		goto amba_alloc_err;

	amba_dev->dev.coherent_dma_mask = acpi_dma_supported(adev) ? DMA_BIT_MASK(64) : 0;
	amba_dev->dev.platform_data = pdata;
	amba_dev->dev.fwnode = acpi_fwnode_handle(adev);

	/*
	 * If the ACPI node has a parent and that parent has a
	 * physical device attached to it, that physical device should
	 * be the parent of the AMBA device we are about to create.
	 */
	parent = NULL;
	acpi_parent = adev->parent;
	if (acpi_parent) {
		struct acpi_device_physical_node *entry;
		struct list_head *list;

		mutex_lock(&acpi_parent->physical_node_lock);
		list = &acpi_parent->physical_node_list;
		if (!list_empty(list)) {
			entry = list_first_entry(list, struct acpi_device_physical_node, node);
			parent = entry->dev;
		}
		mutex_unlock(&acpi_parent->physical_node_lock);
	}

	amba_dev->dev.parent = parent;
	amba_dev->periphid = periphid;

	WARN_ON_ONCE(count > AMBA_NR_IRQS);

	for (i = 0; i < count; i++)
		amba_dev->irq[i] = irq[i];

	clk = clk_register_fixed_rate(&amba_dev->dev,
				      dev_name(&amba_dev->dev),
				      NULL, CLK_IS_ROOT,
				      fixed_rate);
	if (IS_ERR_OR_NULL(clk))
		goto amba_register_err;

	ret = clk_register_clkdev(clk, "apb_pclk",
				  dev_name(&amba_dev->dev));
	if (ret)
		goto amba_register_err;

	ret = amba_device_add(amba_dev, resource);
	if (ret)
		goto amba_register_err;

	acpi_dev_free_resource_list(&resource_list);
	kfree(resource);
	return amba_dev;

amba_register_err:
	amba_device_put(amba_dev);

amba_alloc_err:
	kfree(resource);

resource_alloc_err:
	acpi_dev_free_resource_list(&resource_list);

	return ERR_PTR(ret);
}
