/*
 * Copyright (C) 2015 RC Module
 * Andrew Andrianov <andrew@ncrmnt.org>
 * Also based on work from Google, The Linux Foundation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include "ion.h"
#include "ion_priv.h"
#include "ion_of.h"

struct sample_ion_dev {
        struct ion_heap         **heaps;
        struct ion_device       *idev;
};

static struct ion_of_heap heaps[] = {
	PLATFORM_HEAP("sample-system", 0, ION_HEAP_TYPE_SYSTEM, "system"),
	PLATFORM_HEAP("sample-camera", 1, ION_HEAP_TYPE_DMA, "camera"),
	PLATFORM_HEAP("sample-fb", 2, ION_HEAP_TYPE_DMA, "fb"),
	{}
};

static int ion_sample_probe(struct platform_device *pdev)
{
	struct ion_platform_data *data;
	struct sample_ion_dev *ipdev;
	int i;

	ipdev = devm_kzalloc(&pdev->dev, sizeof(*ipdev), GFP_KERNEL);
	if (!ipdev)
		return -ENOMEM;

	platform_set_drvdata(pdev, ipdev);

	ipdev->idev = ion_device_create(NULL);
	if (!ipdev->idev)
		return -ENOMEM;

	data = ion_parse_dt(pdev, heaps);
	if (IS_ERR(data))
		return PTR_ERR(data);

	ipdev->heaps = devm_kzalloc(&pdev->dev,
				sizeof(struct ion_heap)*data->nr, GFP_KERNEL);

	if (!ipdev->heaps)
		return -ENOMEM;

	for (i = 0; i < data->nr; i++) {
		ipdev->heaps[i] = ion_heap_create(&data->heaps[i]);
		if (!ipdev->heaps[i])
			return -ENOMEM;
		ion_device_add_heap(ipdev->idev, ipdev->heaps[i]);
	}
	return 0;
}

static int ion_sample_remove(struct platform_device *pdev)
{
	/* Everything should be devm */
	return 0;
}

static const struct of_device_id of_match_table[] = {
	{ .compatible = "sample-ion", },
	{ /* end of list */ }
};

static struct platform_driver ion_sample_driver = {
	.probe		= ion_sample_probe,
	.remove		= ion_sample_remove,
	.driver		= {
		.name	= "ion-of",
		.of_match_table = of_match_ptr(of_match_table),
	},
};

static int __init ion_sample_init(void)
{
	return platform_driver_register(&ion_sample_driver);
}
device_initcall(ion_sample_init);
