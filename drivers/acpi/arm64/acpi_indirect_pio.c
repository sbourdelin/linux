/*
 * ACPI support for indirect-PIO bus.
 *
 * Copyright (C) 2017 Hisilicon Limited, All Rights Reserved.
 * Author: Gabriele Paoloni <gabriele.paoloni@huawei.com>
 * Author: Zhichang Yuan <yuanzhichang@hisilicon.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/acpi.h>
#include <linux/platform_device.h>
#include <linux/logic_pio.h>

#include <acpi/acpi_indirect_pio.h>

ACPI_MODULE_NAME("indirect PIO");

#define INDIRECT_PIO_INFO(desc) ((unsigned long)&desc)

static acpi_status acpi_count_logic_iores(struct acpi_resource *res,
					   void *data)
{
	int *res_cnt = data;

	if (!acpi_dev_filter_resource_type(res, IORESOURCE_IO))
		(*res_cnt)++;

	return AE_OK;
}

static acpi_status acpi_read_one_logicpiores(struct acpi_resource *res,
		void *data)
{
	struct acpi_resource **resource = data;

	if (!acpi_dev_filter_resource_type(res, IORESOURCE_IO)) {
		memcpy((*resource), res, sizeof(struct acpi_resource));
		(*resource)->length = sizeof(struct acpi_resource);
		(*resource)->type = res->type;
		(*resource)++;
	}

	return AE_OK;
}

static acpi_status
acpi_build_logicpiores_template(struct acpi_device *adev,
			struct acpi_buffer *buffer)
{
	acpi_handle handle = adev->handle;
	struct acpi_resource *resource;
	acpi_status status;
	int res_cnt = 0;

	status = acpi_walk_resources(handle, METHOD_NAME__CRS,
				     acpi_count_logic_iores, &res_cnt);
	if (ACPI_FAILURE(status)) {
		dev_err(&adev->dev, "can't evaluate _CRS: %d\n", status);
		return -EINVAL;
	}

	if (!res_cnt) {
		dev_err(&adev->dev, "no logic IO resources\n");
		return -ENODEV;
	}

	buffer->length = sizeof(struct acpi_resource) * (res_cnt + 1);
	buffer->pointer = kzalloc(buffer->length, GFP_KERNEL);
	if (!buffer->pointer)
		return -ENOMEM;

	resource = (struct acpi_resource *)buffer->pointer;
	status = acpi_walk_resources(handle, METHOD_NAME__CRS,
				     acpi_read_one_logicpiores, &resource);
	if (ACPI_FAILURE(status)) {
		kfree(buffer->pointer);
		dev_err(&adev->dev, "can't evaluate _CRS: %d\n", status);
		return -EINVAL;
	}

	resource->type = ACPI_RESOURCE_TYPE_END_TAG;
	resource->length = sizeof(struct acpi_resource);

	return 0;
}

static int acpi_translate_logicpiores(struct acpi_device *adev,
		struct acpi_device *host, struct acpi_buffer *buffer)
{
	struct acpi_resource *resource = buffer->pointer;
	unsigned long sys_port;
	struct device *dev = &adev->dev;
	union acpi_resource_data *trans_data = &resource->data;
	resource_size_t bus_addr;
	resource_size_t max_pio;
	resource_size_t length;

	switch (resource->type) {
	case ACPI_RESOURCE_TYPE_ADDRESS16:
		if (trans_data->address16.min_address_fixed !=
				trans_data->address16.max_address_fixed) {
			dev_warn(dev, "variable I/O resource is invalid!\n");
			return -EINVAL;
		}
		bus_addr = trans_data->address16.address.minimum;
		length = trans_data->address16.address.address_length;
		max_pio = U16_MAX;
		break;
	case ACPI_RESOURCE_TYPE_ADDRESS32:
		if (trans_data->address32.min_address_fixed !=
				trans_data->address32.max_address_fixed) {
			dev_warn(dev, "variable I/O resource is invalid!\n");
			return -EINVAL;
		}
		bus_addr = trans_data->address32.address.minimum;
		length = trans_data->address32.address.address_length;
		max_pio = U32_MAX;
		break;
	case ACPI_RESOURCE_TYPE_ADDRESS64:
		if (trans_data->address64.min_address_fixed !=
				trans_data->address64.max_address_fixed) {
			dev_warn(dev, "variable I/O resource is invalid!\n");
			return -EINVAL;
		}
		bus_addr = trans_data->address64.address.minimum;
		length = trans_data->address64.address.address_length;
		max_pio = U64_MAX;
		break;
	case ACPI_RESOURCE_TYPE_EXTENDED_ADDRESS64:
		if (trans_data->ext_address64.min_address_fixed !=
				trans_data->ext_address64.max_address_fixed) {
			dev_warn(dev, "variable I/O resource is invalid!\n");
			return -EINVAL;
		}
		bus_addr = trans_data->ext_address64.address.minimum;
		length = trans_data->ext_address64.address.address_length;
		max_pio = U64_MAX;
		break;
	case ACPI_RESOURCE_TYPE_IO:
		bus_addr = trans_data->io.minimum;
		length = trans_data->io.address_length;
		max_pio = U16_MAX;
		break;
	case ACPI_RESOURCE_TYPE_FIXED_IO:
		bus_addr = trans_data->fixed_io.address;
		length = trans_data->fixed_io.address_length;
		max_pio = U16_MAX;
		break;
	default:
		return -EINVAL;
	}

	sys_port = logic_pio_trans_hwaddr(&host->fwnode, bus_addr);
	if (sys_port == -1) {
		dev_err(dev, "translate bus-addr(0x%llx) fail!\n", bus_addr);
		return -EFAULT;
	}

	/*
	 * we need to check if the resource address can contain the
	 * translated IO token
	 */
	if ((sys_port + length) > max_pio) {
		dev_err(dev, "sys_port exceeds the max resource address\n");
		return -ENOSPC;
	}

	switch (resource->type) {
	case ACPI_RESOURCE_TYPE_ADDRESS16:
		trans_data->address16.address.minimum = sys_port;
		trans_data->address16.address.maximum = sys_port + length;
		break;
	case ACPI_RESOURCE_TYPE_ADDRESS32:
		trans_data->address32.address.minimum = sys_port;
		trans_data->address32.address.maximum = sys_port + length;
		break;
	case ACPI_RESOURCE_TYPE_ADDRESS64:
		trans_data->address64.address.minimum = sys_port;
		trans_data->address64.address.maximum = sys_port + length;
		break;
	case ACPI_RESOURCE_TYPE_EXTENDED_ADDRESS64:
		trans_data->ext_address64.address.minimum = sys_port;
		trans_data->ext_address64.address.maximum = sys_port + length;
		break;
	case ACPI_RESOURCE_TYPE_IO:
		trans_data->io.minimum = sys_port;
		trans_data->io.maximum = sys_port + length;
		break;
	case ACPI_RESOURCE_TYPE_FIXED_IO:
		trans_data->fixed_io.address = sys_port;
		break;
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
int acpi_set_logic_pio_resource(struct device *child,
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
	status = acpi_build_logicpiores_template(adev, &buffer);
	if (ACPI_FAILURE(status)) {
		dev_warn(child, "Failure evaluating %s\n", METHOD_NAME__CRS);
		return -ENODEV;
	}

	/* translate the I/O resources */
	ret = acpi_translate_logicpiores(adev, host, &buffer);
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

/* All the host devices which apply indirect-PIO can be listed here. */
static const struct acpi_device_id acpi_indirect_host_id[] = {
	{""},
};

static int acpi_indirectpio_attach(struct acpi_device *adev,
				const struct acpi_device_id *id)
{
	struct indirect_pio_device_desc *hostdata;
	struct platform_device *pdev;
	int ret;

	hostdata = (struct indirect_pio_device_desc *)id->driver_data;
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
	.attach = acpi_indirectpio_attach,
};

void __init acpi_indirectio_scan_init(void)
{
	acpi_scan_add_handler(&acpi_indirect_handler);
}
