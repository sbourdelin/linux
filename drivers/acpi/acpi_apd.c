/*
 * AMD ACPI support for ACPI2platform device.
 *
 * Copyright (c) 2014,2015 AMD Corporation.
 * Authors: Ken Xue <Ken.Xue@amd.com>
 *          Jeff Wu <15618388108@163.com>
 *	    Wang Hongcheng <Annie.Wang@amd.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/clkdev.h>
#include <linux/acpi.h>
#include <linux/err.h>
#include <linux/pm.h>
#include <linux/amba/bus.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/amba/pl330.h>
#include <linux/interrupt.h>
#include <linux/serial_8250.h>

#include "internal.h"

ACPI_MODULE_NAME("acpi_apd");
struct apd_private_data;

/**
 * ACPI_APD_SYSFS : add device attributes in sysfs
 * ACPI_APD_PM : attach power domain to device
 */
#define ACPI_APD_SYSFS	BIT(0)
#define ACPI_APD_PM	BIT(1)

static u8 peri_id[2] = { 0, 1 };

static int apd_acpi_xlate_filter(int slave_id, struct device *dev);

static struct dma_pl330_platdata amd_pl330 = {
	.nr_valid_peri = 2,
	.peri_id = peri_id,
	.has_no_cap_mask = true,
	.mcbuf_sz = 0,
	.flags = IRQF_SHARED,
	.acpi_xlate_filter = apd_acpi_xlate_filter,
};

static struct plat_dw8250_data amd_dw8250 = {
	.has_pl330_dma = 1,
};

/**
 * struct apd_device_desc - a descriptor for apd device.
 * @flags: device flags like %ACPI_APD_SYSFS, %ACPI_APD_PM;
 * @fixed_clk_rate: fixed rate input clock source for acpi device;
 *0 means no fixed rate input clock source;
 * @clk_con_id: name of input clock source;
 * @setup: a hook routine to set device resource during create platform device.
 *
 * Device description defined as acpi_device_id.driver_data.
*/
struct apd_device_desc {
	unsigned int flags;
	unsigned int fixed_clk_rate;
	int (*setup)(struct apd_private_data *pdata);
};

struct apd_private_data {
	struct clk *clk;
	struct acpi_device *adev;
	const struct apd_device_desc *dev_desc;
};

int apd_acpi_xlate_filter(int slave_id, struct device *dev)
{
	if (((slave_id == 1) && (!strcmp(dev_name(dev), "AMD0020:00DMA")))
	    || ((slave_id == 2) && (!strcmp(dev_name(dev), "AMD0020:01DMA"))))
		return 0;

	return 1;
}

#ifdef CONFIG_X86_AMD_PLATFORM_DEVICE
#define APD_ADDR(desc)	((unsigned long)&desc)

static int acpi_apd_setup(struct apd_private_data *pdata)
{
	const struct apd_device_desc *dev_desc = pdata->dev_desc;
	struct clk *clk = ERR_PTR(-ENODEV);

	if (dev_desc->fixed_clk_rate) {
		clk = clk_register_fixed_rate(&pdata->adev->dev,
					dev_name(&pdata->adev->dev),
					NULL, CLK_IS_ROOT,
					dev_desc->fixed_clk_rate);
		clk_register_clkdev(clk, NULL, dev_name(&pdata->adev->dev));
		pdata->clk = clk;
	}

	return 0;
}

static void setup_quirks(struct platform_device *pdev,
			 struct acpi_amba_quirk *quirk)
{
	if (!strncmp(pdev->name, "AMD0020", 7)) {
		quirk->quirk |= MULTI_ATTACHED_QUIRK | BASE_OFFSET_QUIRK;
		quirk->base_offset = SZ_4K;
	}
}

static struct apd_device_desc cz_i2c_desc = {
	.setup = acpi_apd_setup,
	.fixed_clk_rate = 133000000,
};

static struct apd_device_desc cz_uart_desc = {
	.setup = acpi_apd_setup,
	.fixed_clk_rate = 48000000,
};

#else

#define APD_ADDR(desc) (0UL)

#endif /* CONFIG_X86_AMD_PLATFORM_DEVICE */

/**
 * Create platform device during acpi scan attach handle.
 * Return value > 0 on success of creating device.
 */
static int acpi_apd_create_device(struct acpi_device *adev,
				  const struct acpi_device_id *id)
{
	const struct apd_device_desc *dev_desc = (void *)id->driver_data;
	struct apd_private_data *pdata;
	struct platform_device *pdev;
	struct amba_device *amba_dev;
	struct acpi_amba_quirk amba_quirks;
	int ret;

	if (!dev_desc) {
		pdev = acpi_create_platform_device(adev);
		return IS_ERR_OR_NULL(pdev) ? PTR_ERR(pdev) : 1;
	}

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	pdata->adev = adev;
	pdata->dev_desc = dev_desc;

	if (dev_desc->setup) {
		ret = dev_desc->setup(pdata);
		if (ret)
			goto err_out;
	}

	adev->driver_data = pdata;

	pdev = acpi_create_platform_device(adev);
	if (IS_ERR_OR_NULL(pdev))
		goto err_out;

	if (!strncmp(pdev->name, "AMD0020", 7)) {
		ret = platform_device_add_data(pdev, &amd_dw8250,
					       sizeof(amd_dw8250));
		if (ret)
			goto err_out;

		memset(&amba_quirks, 0, sizeof(amba_quirks));
		setup_quirks(pdev, &amba_quirks);

		amba_dev = acpi_create_amba_device(pdata->adev, 0x00041330,
						   48000000,
						   &amd_pl330,
						   &amba_quirks);
		if (IS_ERR_OR_NULL(amba_dev))
			goto err_out;
	}

	ret = PTR_ERR(pdev);
	adev->driver_data = NULL;

 err_out:
	kfree(pdata);
	return ret;
}

static const struct acpi_device_id acpi_apd_device_ids[] = {
	/* Generic apd devices */
	{ "AMD0010", APD_ADDR(cz_i2c_desc) },
	{ "AMD0020", APD_ADDR(cz_uart_desc) },
	{ "AMD0030", },
	{ }
};

static struct acpi_scan_handler apd_handler = {
	.ids = acpi_apd_device_ids,
	.attach = acpi_apd_create_device,
};

void __init acpi_apd_init(void)
{
	acpi_scan_add_handler(&apd_handler);
}
