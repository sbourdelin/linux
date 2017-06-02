/*
 * Soundwire Intel Driver
 * Copyright (c) 2016-17, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/acpi.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/soundwire/soundwire.h>
#include <linux/soundwire/sdw_intel.h>
#include "sdw_intel_shim.h"

#define SDW_ISHIM_BASE				0x2C000
#define SDW_IALH_BASE				0x2C800
#define SDW_ILINK_BASE				0x30000
#define SDW_ILINK_SIZE				0x10000

/* Intel SHIM Registers Definition */
#define SDW_ISHIM_LCAP				0x0

/**
 * struct sdw_ishim: Intel Shim context structure
 *
 * @shim: shim registers
 * @alh: Audio Link Hub (ALH) registers
 * @irq: interrupt number
 * @parent: parent device
 * @count: link count
 * @link: link instances
 * @config_ops: shim config ops
 */
struct sdw_ishim {
	void __iomem *shim;
	void __iomem *alh;
	int irq;
	struct device *parent;
	int count;
	struct sdw_ilink_data *link[SDW_MAX_LINKS];
	const struct sdw_config_ops *config_ops;
};


/*
 * shim init routines
 */
static int intel_sdw_cleanup_pdev(struct sdw_ishim *shim)
{
	int i;

	for (i = 0; i < shim->count; i++) {
		if (shim->link[i]->pdev)
			platform_device_unregister(shim->link[i]->pdev);
	}
	return 0;
}

static struct sdw_ishim *intel_sdw_add_controller(struct intel_sdw_res *res)
{
	struct acpi_device *adev;
	struct platform_device *pdev;
	struct sdw_ishim *shim;
	struct sdw_ilink_res *link_res;
	struct platform_device_info pdevinfo;
	u8 count;
	u32 caps;
	int ret, i;

	if (acpi_bus_get_device(res->parent, &adev))
		return NULL;

	/* now we found the controller, so find the links supported */
	count = 0;
	ret = fwnode_property_read_u8_array(acpi_fwnode_handle(adev),
				  "mipi-sdw-master-count", &count, 1);
	if (ret) {
		dev_err(&adev->dev, "Failed to read mipi-sdw-master-count: %d\n", ret);
		return NULL;
	}

	shim = kzalloc(sizeof(*shim), GFP_KERNEL);
	if (!shim)
		return NULL;

	shim->shim = res->mmio_base + SDW_ISHIM_BASE;
	shim->alh = res->mmio_base + SDW_IALH_BASE;
	shim->irq = res->irq;
	shim->parent = res->parent;
	shim->config_ops = res->config_ops;

	/* Check the SNDWLCAP.LCOUNT */
	caps = ioread32(shim->shim + SDW_ISHIM_LCAP);

	/* check HW supports vs property value and use min of two */
	count = min_t(u8, caps, count);

	dev_info(&adev->dev, "Creating %d SDW Link devices\n", count);
	shim->count = count;

	/* create those devices */
	for (i = 0; i < count; i++) {

		/* SRK: This should be inside for loop for each master instance */
		link_res = kmalloc(sizeof(*link_res), GFP_KERNEL);
		if (!link_res)
			goto link_err;

		link_res->irq = res->irq;
		link_res->registers = res->mmio_base + SDW_ILINK_BASE
					+ (SDW_ILINK_SIZE * i);
		link_res->shim = shim;

		memset(&pdevinfo, 0, sizeof(pdevinfo));

		pdevinfo.parent = res->parent;
		pdevinfo.name = "int-sdw";
		pdevinfo.id = i;
		pdevinfo.fwnode = acpi_fwnode_handle(adev);
		pdevinfo.data = link_res;
		pdevinfo.size_data = sizeof(*link_res);

		pdev = platform_device_register_full(&pdevinfo);
		if (IS_ERR(pdev)) {
			dev_err(&adev->dev, "platform device creation failed: %ld\n",
				PTR_ERR(pdev));
			goto pdev_err;
		} else {
			dev_dbg(&adev->dev, "created platform device %s\n",
				dev_name(&pdev->dev));
		}

		shim->link[i]->pdev = pdev;
		shim->link[i]->shim = link_res->registers + SDW_ISHIM_BASE;
		shim->link[i]->alh = link_res->registers + SDW_ILINK_BASE;

		kfree(link_res);
	}

	return shim;

pdev_err:
	intel_sdw_cleanup_pdev(shim);
link_err:
	kfree(shim);
	return NULL;
}

static acpi_status intel_sdw_acpi_cb(acpi_handle handle, u32 level,
					void *cdata, void **return_value)
{
	struct acpi_device *adev;

	if (acpi_bus_get_device(handle, &adev))
		return AE_NOT_FOUND;

	dev_dbg(&adev->dev, "Found ACPI handle\n");

	return AE_OK;
}

void *intel_sdw_init(acpi_handle *parent_handle, struct intel_sdw_res *res)
{
	acpi_status status;

	status = acpi_walk_namespace(ACPI_TYPE_DEVICE, parent_handle, 1,
				     intel_sdw_acpi_cb, NULL,
				     NULL, NULL);
	if (ACPI_FAILURE(status)) {
		pr_err("Intel SDW: failed to find controller: %d\n", status);
		return NULL;
	}

	return intel_sdw_add_controller(res);
}
EXPORT_SYMBOL_GPL(intel_sdw_init);

void intel_sdw_exit(void *arg)
{
	struct sdw_ishim *shim = arg;

	intel_sdw_cleanup_pdev(shim);
	kfree(shim);
}
EXPORT_SYMBOL_GPL(intel_sdw_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Intel Soundwire Shim driver");
