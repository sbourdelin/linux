/*
 * ACPI support for indirect-IO bus.
 *
 * Copyright (C) 2017 Hisilicon Limited, All Rights Reserved.
 * Author: Zhichang Yuan <yuanzhichang@hisilicon.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/acpi.h>
#include <linux/platform_device.h>
#include <linux/logic_pio.h>

#include "internal.h"

ACPI_MODULE_NAME("indirect IO");

#define INDIRECT_IO_INFO(desc) ((unsigned long)&desc)

struct lpc_private_data {
	resource_size_t io_size;
	resource_size_t io_start;
};

struct indirectio_device_desc {
	void *pdata; /* device relevant info data */
	int (*pre_setup)(struct acpi_device *adev, void *pdata);
};

static struct lpc_private_data lpc_data = {
	.io_size = LPC_BUS_IO_SIZE,
	.io_start = LPC_MIN_BUS_RANGE,
};

static inline bool acpi_logicio_supported_resource(struct acpi_resource *res)
{
	switch (res->type) {
	case ACPI_RESOURCE_TYPE_ADDRESS32:
	case ACPI_RESOURCE_TYPE_ADDRESS64:
		return true;
	}
	return false;
}

static acpi_status acpi_count_logiciores(struct acpi_resource *res,
					   void *data)
{
	int *res_cnt = data;

	if (acpi_logicio_supported_resource(res) &&
		!acpi_dev_filter_resource_type(res, IORESOURCE_IO))
		(*res_cnt)++;

	return AE_OK;
}

static acpi_status acpi_read_one_logiciores(struct acpi_resource *res,
		void *data)
{
	struct acpi_resource **resource = data;

	if (acpi_logicio_supported_resource(res) &&
		!acpi_dev_filter_resource_type(res, IORESOURCE_IO)) {
		memcpy((*resource), res, sizeof(struct acpi_resource));
		(*resource)->length = sizeof(struct acpi_resource);
		(*resource)->type = res->type;
		(*resource)++;
	}

	return AE_OK;
}

static acpi_status
acpi_build_logiciores_template(struct acpi_device *adev,
			struct acpi_buffer *buffer)
{
	acpi_handle handle = adev->handle;
	struct acpi_resource *resource;
	acpi_status status;
	int res_cnt = 0;

	status = acpi_walk_resources(handle, METHOD_NAME__CRS,
				     acpi_count_logiciores, &res_cnt);
	if (ACPI_FAILURE(status) || !res_cnt) {
		dev_err(&adev->dev, "can't evaluate _CRS: %d\n", status);
		return -EINVAL;
	}

	buffer->length = sizeof(struct acpi_resource) * (res_cnt + 1) + 1;
	buffer->pointer = kzalloc(buffer->length - 1, GFP_KERNEL);
	if (!buffer->pointer)
		return -ENOMEM;

	resource = (struct acpi_resource *)buffer->pointer;
	status = acpi_walk_resources(handle, METHOD_NAME__CRS,
				     acpi_read_one_logiciores, &resource);
	if (ACPI_FAILURE(status)) {
		kfree(buffer->pointer);
		dev_err(&adev->dev, "can't evaluate _CRS: %d\n", status);
		return -EINVAL;
	}

	resource->type = ACPI_RESOURCE_TYPE_END_TAG;
	resource->length = sizeof(struct acpi_resource);

	return 0;
}

static int acpi_translate_logiciores(struct acpi_device *adev,
		struct acpi_device *host, struct acpi_buffer *buffer)
{
	int res_cnt = (buffer->length - 1) / sizeof(struct acpi_resource) - 1;
	struct acpi_resource *resource = buffer->pointer;
	struct acpi_resource_address64 addr;
	unsigned long sys_port;
	struct device *dev = &adev->dev;

	/* only one I/O resource now */
	if (res_cnt != 1) {
		dev_err(dev, "encode %d resources whose type is(%d)!\n",
			res_cnt, resource->type);
		return -EINVAL;
	}

	if (ACPI_FAILURE(acpi_resource_to_address64(resource, &addr))) {
		dev_err(dev, "convert acpi resource(%d) as addr64 FAIL!\n",
			resource->type);
		return -EFAULT;
	}

	/* For indirect-IO, addr length must be fixed. (>0, 0/1, 0/1)(0,0,0) */
	if (addr.min_address_fixed != addr.max_address_fixed) {
		dev_warn(dev, "variable I/O resource is invalid!\n");
		return -EINVAL;
	}

	dev_info(dev, "CRS IO: len=0x%llx [0x%llx - 0x%llx]\n",
			addr.address.address_length, addr.address.minimum,
			addr.address.maximum);
	sys_port = logic_pio_trans_hwaddr(&host->fwnode, addr.address.minimum);
	if (sys_port == -1) {
		dev_err(dev, "translate bus-addr(0x%llx) fail!\n",
			addr.address.minimum);
		return -EFAULT;
	}

	switch (resource->type) {
	case ACPI_RESOURCE_TYPE_ADDRESS32:
	{
		struct acpi_resource_address32 *out_res;

		out_res = &resource->data.address32;
		if (!addr.address.address_length)
			addr.address.address_length = out_res->address.maximum -
				out_res->address.minimum + 1;
		out_res->address.minimum = sys_port;
		out_res->address.maximum = sys_port +
				addr.address.address_length - 1;
		out_res->address.address_length = addr.address.address_length;

		dev_info(dev, "_SRS 32IO: [0x%x - 0x%x] len = 0x%x\n",
			out_res->address.minimum,
			out_res->address.maximum,
			out_res->address.address_length);

		break;
	}

	case ACPI_RESOURCE_TYPE_ADDRESS64:
	{
		struct acpi_resource_address64 *out_res;

		out_res = &resource->data.address64;
		if (!addr.address.address_length)
			addr.address.address_length = out_res->address.maximum -
				out_res->address.minimum + 1;
		out_res->address.minimum = sys_port;
		out_res->address.maximum = sys_port +
				addr.address.address_length - 1;
		out_res->address.address_length = addr.address.address_length;

		dev_info(dev, "_SRS 64IO: [0x%llx - 0x%llx] len = 0x%llx\n",
			out_res->address.minimum,
			out_res->address.maximum,
			out_res->address.address_length);

		break;
	}

	default:
		return -EINVAL;

	}

	return 0;
}

/*
 * update/set the current I/O resource of the designated device node.
 * after this calling, the enumeration can be started as the I/O resource
 * had been translated to logicial I/O from bus-local I/O.
 *
 * @adev: the device node to be updated the I/O resource;
 * @host: the device node where 'adev' is attached, which can be not
 *	the parent of 'adev';
 *
 * return 0 when successful, negative is for failure.
 */
static int acpi_set_logicio_resource(struct device *child,
		struct device *hostdev)
{
	struct acpi_device *adev;
	struct acpi_device *host;
	struct acpi_buffer buffer;
	acpi_status status;
	int ret;

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

	/* read the _CRS and convert as acpi_buffer */
	status = acpi_build_logiciores_template(adev, &buffer);
	if (ACPI_FAILURE(status)) {
		dev_warn(child, "Failure evaluating %s\n", METHOD_NAME__CRS);
		return -ENODEV;
	}

	/* translate the I/O resources */
	ret = acpi_translate_logiciores(adev, host, &buffer);
	if (ret) {
		kfree(buffer.pointer);
		dev_err(child, "Translate I/O range FAIL!\n");
		return ret;
	}

	/* set current resource... */
	status = acpi_set_current_resources(adev->handle, &buffer);
	kfree(buffer.pointer);
	if (ACPI_FAILURE(status)) {
		dev_err(child, "Error evaluating _SRS (0x%x)\n", status);
		ret = -EIO;
	}

	return ret;
}

static int lpc_host_io_setup(struct acpi_device *adev, void *pdata)
{
	struct logic_pio_hwaddr *range, *tmprange;
	struct lpc_private_data *lpc_private;
	struct acpi_device *child;

	lpc_private = (struct lpc_private_data *)pdata;
	range = kzalloc(sizeof(*range), GFP_KERNEL);
	if (!range)
		return -ENOMEM;
	range->fwnode = &adev->fwnode;
	range->flags = PIO_INDIRECT;
	range->size = lpc_private->io_size;
	range->hw_start = lpc_private->io_start;

	tmprange = logic_pio_register_range(range, 1);
	if (tmprange != range) {
		kfree(range);
		if (IS_ERR(tmprange))
			return -EFAULT;
	}

	/* For hisilpc, only care about the sons of host. */
	list_for_each_entry(child, &adev->children, node) {
		int ret;

		ret = acpi_set_logicio_resource(&child->dev, &adev->dev);
		if (ret) {
			dev_err(&child->dev, "set resource failed..\n");
			return ret;
		}
	}

	return 0;
}

static const struct indirectio_device_desc lpc_host_desc = {
	.pdata = &lpc_data,
	.pre_setup = lpc_host_io_setup,
};

/* All the host devices which apply indirect-IO can be listed here. */
static const struct acpi_device_id acpi_indirect_host_id[] = {
	{"HISI0191", INDIRECT_IO_INFO(lpc_host_desc)},
	{""},
};

static int acpi_indirectio_attach(struct acpi_device *adev,
				const struct acpi_device_id *id)
{
	struct indirectio_device_desc *hostdata;
	struct platform_device *pdev;
	int ret;

	hostdata = (struct indirectio_device_desc *)id->driver_data;
	if (!hostdata || !hostdata->pre_setup)
		return -EINVAL;

	ret = hostdata->pre_setup(adev, hostdata->pdata);
	if (!ret) {
		pdev = acpi_create_platform_device(adev, NULL);
		if (IS_ERR_OR_NULL(pdev)) {
			dev_err(&adev->dev, "Create platform device for host FAIL!\n");
			return -EFAULT;
		}
		acpi_device_set_enumerated(adev);
		ret = 1;
	}

	return ret;
}


static struct acpi_scan_handler acpi_indirect_handler = {
	.ids = acpi_indirect_host_id,
	.attach = acpi_indirectio_attach,
};

void __init acpi_indirectio_scan_init(void)
{
	acpi_scan_add_handler(&acpi_indirect_handler);
}
