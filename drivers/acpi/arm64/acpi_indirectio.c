/*
 * ACPI support for indirect-IO bus.
 *
 * Copyright (C) 2017 HiSilicon Limited, All Rights Reserved.
 * Author: Gabriele Paoloni <gabriele.paoloni@huawei.com>
 * Author: Zhichang Yuan <yuanzhichang@hisilicon.com>
 * Author: John Garry <john.garry@huawei.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/acpi.h>
#include <linux/logic_pio.h>
#include <linux/mfd/core.h>
#include <linux/platform_device.h>

ACPI_MODULE_NAME("indirect IO");

#define ACPI_INDIRECTIO_NAME_LENGTH 255

#define INDIRECT_IO_INFO(desc) ((unsigned long)&desc)

struct acpi_indirectio_mfd_cell {
	struct mfd_cell_acpi_match acpi_match;
	char name[ACPI_INDIRECTIO_NAME_LENGTH];
	char pnpid[ACPI_INDIRECTIO_NAME_LENGTH];
};

struct acpi_indirectio_host_data {
	resource_size_t io_size;
	resource_size_t io_start;
};

struct acpi_indirectio_device_desc {
	struct acpi_indirectio_host_data pdata; /* device relevant info data */
	int (*pre_setup)(struct acpi_device *adev,
			 struct acpi_indirectio_host_data *pdata);
};

static int acpi_translate_logicio_res(struct acpi_device *adev,
		struct acpi_device *host, struct resource *resource)
{
	unsigned long sys_port;
	struct device *dev = &adev->dev;
	resource_size_t length = resource->end - resource->start;

	sys_port = logic_pio_trans_hwaddr(&host->fwnode, resource->start,
					length);

	if (sys_port == -1) {
		dev_err(dev, "translate bus-addr(0x%llx) fail!\n",
			resource->start);
		return -EFAULT;
	}

	resource->start = sys_port;
	resource->end = sys_port + length;

	return 0;
}

/*
 * update/set the current I/O resource of the designated device node.
 * after this calling, the enumeration can be started as the I/O resource
 * had been translated to logicial I/O from bus-local I/O.
 *
 * @child: the device node to be updated the I/O resource;
 * @hostdev: the device node where 'adev' is attached, which can be not
 *  the parent of 'adev';
 * @res: double pointer to be set to the address of the updated resources
 * @num_res: address of the variable to contain the number of updated resources
 *
 * return 0 when successful, negative is for failure.
 */
int acpi_indirectio_set_logicio_res(struct device *child,
					 struct device *hostdev,
					 const struct resource **res,
					 int *num_res)
{
	struct acpi_device *adev;
	struct acpi_device *host;
	struct resource_entry *rentry;
	LIST_HEAD(resource_list);
	struct resource *resources = NULL;
	int count;
	int i;
	int ret = -EIO;

	if (!child || !hostdev)
		return -EINVAL;

	host = to_acpi_device(hostdev);
	adev = to_acpi_device(child);

	/* check the device state */
	if (!adev->status.present) {
		dev_info(child, "ACPI: device is not present!\n");
		return 0;
	}
	/* whether the child had been enumerated? */
	if (acpi_device_enumerated(adev)) {
		dev_info(child, "ACPI: had been enumerated!\n");
		return 0;
	}

	count = acpi_dev_get_resources(adev, &resource_list, NULL, NULL);
	if (count <= 0) {
		dev_err(&adev->dev, "failed to get ACPI resources\n");
		return count ? count : -EIO;
	}

	resources = kcalloc(count, sizeof(struct resource), GFP_KERNEL);
	if (!resources) {
		acpi_dev_free_resource_list(&resource_list);
		return -ENOMEM;
	}
	count = 0;
	list_for_each_entry(rentry, &resource_list, node)
		resources[count++] = *rentry->res;

	acpi_dev_free_resource_list(&resource_list);

	/* translate the I/O resources */
	for (i = 0; i < count; i++) {
		if (resources[i].flags & IORESOURCE_IO) {
			ret = acpi_translate_logicio_res(adev, host,
							&resources[i]);
			if (ret) {
				kfree(resources);
				dev_err(child,
					"Translate I/O range failed (%d)!\n",
					ret);
				return ret;
			}
		}
	}
	*res = resources;
	*num_res = count;

	return ret;
}

int
acpi_indirectio_pre_setup(struct acpi_device *adev,
			  struct acpi_indirectio_host_data *pdata)
{
	struct platform_device *pdev;
	struct mfd_cell *mfd_cells;
	struct logic_pio_hwaddr *range;
	struct acpi_device *child;
	struct acpi_indirectio_mfd_cell *acpi_indirectio_mfd_cells;
	int size, ret, count = 0, cell_num = 0;

	range = kzalloc(sizeof(*range), GFP_KERNEL);
	if (!range)
		return -ENOMEM;
	range->fwnode = &adev->fwnode;
	range->flags = PIO_INDIRECT;
	range->size = pdata->io_size;
	range->hw_start = pdata->io_start;

	ret = logic_pio_register_range(range);
	if (ret)
		goto free_range;

	list_for_each_entry(child, &adev->children, node)
		cell_num++;

	/* allocate the mfd cells */
	size = sizeof(*mfd_cells) + sizeof(*acpi_indirectio_mfd_cells);
	mfd_cells = kcalloc(cell_num, size, GFP_KERNEL);
	if (!mfd_cells) {
		ret = -ENOMEM;
		goto free_range;
	}

	acpi_indirectio_mfd_cells = (struct acpi_indirectio_mfd_cell *)
					&mfd_cells[cell_num];
	/* Only consider the children of the host */
	list_for_each_entry(child, &adev->children, node) {
		struct mfd_cell *mfd_cell = &mfd_cells[count];
		struct acpi_indirectio_mfd_cell *acpi_indirectio_mfd_cell =
					&acpi_indirectio_mfd_cells[count];
		const struct mfd_cell_acpi_match *acpi_match =
					&acpi_indirectio_mfd_cell->acpi_match;
		char *name = &acpi_indirectio_mfd_cell[count].name[0];
		char *pnpid = &acpi_indirectio_mfd_cell[count].pnpid[0];
		struct mfd_cell_acpi_match match = {
				.pnpid = pnpid,
		};

		snprintf(name, ACPI_INDIRECTIO_NAME_LENGTH, "indirect-io-%s",
			 acpi_device_hid(child));
		snprintf(pnpid, ACPI_INDIRECTIO_NAME_LENGTH, "%s",
			 acpi_device_hid(child));

		memcpy((void *)acpi_match, (void *)&match, sizeof(*acpi_match));
		mfd_cell->name = name;
		mfd_cell->acpi_match = acpi_match;

		ret =
		acpi_indirectio_set_logicio_res(&child->dev,
						&adev->dev,
						&mfd_cell->resources,
						&mfd_cell->num_resources);
		if (ret) {
			dev_err(&child->dev, "set resource failed (%d)\n", ret);
			goto free_mfd_res;
		}
		count++;
	}

	pdev = acpi_create_platform_device(adev, NULL);
	if (IS_ERR_OR_NULL(pdev)) {
		dev_err(&adev->dev, "Create platform device for host failed!\n");
		ret = PTR_ERR(pdev);
		goto free_mfd_res;
	}
	acpi_device_set_enumerated(adev);

	ret = mfd_add_devices(&pdev->dev, PLATFORM_DEVID_NONE,
			mfd_cells, cell_num, NULL, 0, NULL);
	if (ret) {
		dev_err(&pdev->dev, "failed to add mfd cells (%d)\n", ret);
		goto free_mfd_res;
	}

	return ret;

free_mfd_res:
	while (cell_num--)
		kfree(mfd_cells[cell_num].resources);
	kfree(mfd_cells);
free_range:
	kfree(range);

	return ret;
}

/* All the host devices which apply indirect-IO can be listed here. */
static const struct acpi_device_id acpi_indirect_host_id[] = {
	{""},
};

static int acpi_indirectio_attach(struct acpi_device *adev,
				const struct acpi_device_id *id)
{
	struct acpi_indirectio_device_desc *hostdata;
	int ret;

	hostdata = (struct acpi_indirectio_device_desc *)id->driver_data;
	if (!hostdata || !hostdata->pre_setup)
		return -EINVAL;

	ret = hostdata->pre_setup(adev, &hostdata->pdata);

	if (ret < 0)
		return ret;

	return 1;
}

static struct acpi_scan_handler acpi_indirect_handler = {
	.ids = acpi_indirect_host_id,
	.attach = acpi_indirectio_attach,
};

void __init acpi_indirectio_scan_init(void)
{
	acpi_scan_add_handler(&acpi_indirect_handler);
}
