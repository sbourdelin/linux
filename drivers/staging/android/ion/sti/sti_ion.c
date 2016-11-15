/*
 * STIH4xx ION Driver
 *
 * Copyright (c) 2016 STMicroelectronics
 *
 * Author: Benjamin Gaignard <benjamin.gaignard@st.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/err.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "../ion_priv.h"
#include "../ion.h"
#include "../ion_of.h"

struct sti_ion_dev {
	struct ion_heap	**heaps;
	struct ion_device *idev;
	struct ion_platform_data *data;
};

static struct ion_of_heap sti_heaps[] = {
	PLATFORM_HEAP("linux,ion-heap-dma", 0, ION_HEAP_TYPE_DMA, "cma"),
	{}
};

static int sti_ion_probe(struct platform_device *pdev)
{
	struct sti_ion_dev *ipdev;
	int i;

	ipdev = devm_kzalloc(&pdev->dev, sizeof(*ipdev), GFP_KERNEL);
	if (!ipdev)
		return -ENOMEM;

	platform_set_drvdata(pdev, ipdev);

	ipdev->idev = ion_device_create(NULL);
	if (IS_ERR(ipdev->idev))
		return PTR_ERR(ipdev->idev);

	ipdev->data = ion_parse_dt(pdev, sti_heaps);
	if (IS_ERR(ipdev->data))
		return PTR_ERR(ipdev->data);

	ipdev->heaps = devm_kzalloc(&pdev->dev,
				sizeof(struct ion_heap) * ipdev->data->nr,
				GFP_KERNEL);
	if (!ipdev->heaps) {
		ion_destroy_platform_data(ipdev->data);
		return -ENOMEM;
	}

	for (i = 0; i < ipdev->data->nr; i++) {
		ipdev->heaps[i] = ion_heap_create(&ipdev->data->heaps[i]);
		if (!ipdev->heaps) {
			ion_destroy_platform_data(ipdev->data);
			return -ENOMEM;
		}
		ion_device_add_heap(ipdev->idev, ipdev->heaps[i]);
	}
	return 0;
}

static int sti_ion_remove(struct platform_device *pdev)
{
	struct sti_ion_dev *ipdev;
	int i;

	ipdev = platform_get_drvdata(pdev);

	for (i = 0; i < ipdev->data->nr; i++)
		ion_heap_destroy(ipdev->heaps[i]);

	ion_destroy_platform_data(ipdev->data);
	ion_device_destroy(ipdev->idev);

	return 0;
}

static const struct of_device_id sti_ion_match_table[] = {
	{.compatible = "st,sti-ion"},
	{},
};

static struct platform_driver sti_ion_driver = {
	.probe = sti_ion_probe,
	.remove = sti_ion_remove,
	.driver = {
		.name = "ion-sti",
		.of_match_table = sti_ion_match_table,
	},
};

module_platform_driver(sti_ion_driver);
