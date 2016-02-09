/*
 * UCSI ACPI driver
 *
 * Copyright (C) 2016, Intel Corporation
 * Author: Heikki Krogerus <heikki.krogerus@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/acpi.h>

#include "ucsi.h"

struct ucsi_acpi {
	struct device *dev;
	struct ucsi *ucsi;
	struct ucsi_ppm ppm;
};

static const u8 ucsi_uuid[] = {
	0xc2, 0x98, 0x83, 0x6f,	0xa4, 0x7c, 0xe4, 0x11,
	0xad, 0x36, 0x63, 0x10, 0x42, 0xb5, 0x00, 0x8f,
};

static int ucsi_acpi_cmd(struct ucsi_ppm *ppm)
{
	struct ucsi_acpi *ua = container_of(ppm, struct ucsi_acpi, ppm);
	union acpi_object *obj;

	obj = acpi_evaluate_dsm(ACPI_HANDLE(ua->dev), ucsi_uuid, 1, 1, NULL);
	if (!obj) {
		dev_err(ua->dev, "%s: failed to evaluate _DSM\n", __func__);
		return -EIO;
	}

	ACPI_FREE(obj);
	return 0;
}

static void ucsi_acpi_notify(acpi_handle handle, u32 event, void *data)
{
	struct ucsi_acpi *ua = data;

	if (!ucsi_interrupt(ua->ucsi))
		dev_err(ua->dev, "spurious ACPI notification\n");
}

static int ucsi_acpi_probe(struct platform_device *pdev)
{
	struct ucsi_acpi *ua;
	struct resource *res;
	acpi_status status;
	int ret;

	ua = devm_kzalloc(&pdev->dev, sizeof(*ua), GFP_KERNEL);
	if (!ua)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "missing memory resource\n");
		return -ENODEV;
	}

	ua->ppm.data = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!ua->ppm.data)
		return -ENOMEM;

	ua->ppm.cmd = ucsi_acpi_cmd;
	ua->dev = &pdev->dev;

	ua->ucsi = ucsi_register_ppm(&pdev->dev, &ua->ppm);
	if (IS_ERR(ua->ucsi))
		return PTR_ERR(ua->ucsi);

	status = acpi_install_notify_handler(ACPI_HANDLE(&pdev->dev),
					     ACPI_DEVICE_NOTIFY,
					     ucsi_acpi_notify, ua);
	if (ACPI_FAILURE(status)) {
		ret = -ENODEV;
		goto err;
	}

	ret = ucsi_init(ua->ucsi);
	if (ret) {
		acpi_remove_notify_handler(ACPI_HANDLE(&pdev->dev),
					   ACPI_DEVICE_NOTIFY,
					   ucsi_acpi_notify);
		goto err;
	}

	platform_set_drvdata(pdev, ua);
	return 0;
err:
	ucsi_unregister_ppm(ua->ucsi);
	return ret;
}

static int ucsi_acpi_remove(struct platform_device *pdev)
{
	struct ucsi_acpi *ua = platform_get_drvdata(pdev);

	acpi_remove_notify_handler(ACPI_HANDLE(&pdev->dev),
				   ACPI_DEVICE_NOTIFY, ucsi_acpi_notify);
	ucsi_unregister_ppm(ua->ucsi);
	return 0;
}

static const struct acpi_device_id ucsi_acpi_match[] = {
	{ "PNP0CA0", 0 },
	{ },
};
MODULE_DEVICE_TABLE(acpi, ucsi_acpi_match);

static struct platform_driver ucsi_acpi_platform_driver = {
	.driver = {
		.name = "ucsi_acpi",
		.acpi_match_table = ACPI_PTR(ucsi_acpi_match),
	},
	.probe = ucsi_acpi_probe,
	.remove = ucsi_acpi_remove,
};

module_platform_driver(ucsi_acpi_platform_driver);

MODULE_AUTHOR("Heikki Krogerus <heikki.krogerus@linux.intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("UCSI ACPI driver");
