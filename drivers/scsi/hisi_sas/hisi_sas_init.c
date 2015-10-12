/*
 * Copyright (c) 2015 Linaro Ltd.
 * Copyright (c) 2015 Hisilicon Limited.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include "hisi_sas.h"

static const struct of_device_id sas_of_match[] = {
	{ .compatible = "hisilicon,sas-controller-v1",},
	{},
};
MODULE_DEVICE_TABLE(of, sas_of_match);
static int hisi_sas_probe(struct platform_device *pdev)
{

	return 0;
}

static int hisi_sas_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver hisi_sas_driver = {
	.probe = hisi_sas_probe,
	.remove = hisi_sas_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = sas_of_match,
	},
};

static __init int hisi_sas_init(void)
{
	pr_info("hisi_sas: driver version %s\n", DRV_VERSION);

	return platform_driver_register(&hisi_sas_driver);
}

static __exit void hisi_sas_exit(void)
{
	platform_driver_unregister(&hisi_sas_driver);
}

module_init(hisi_sas_init);
module_exit(hisi_sas_exit);

MODULE_VERSION(DRV_VERSION);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("John Garry <john.garry@huawei.com>");
MODULE_DESCRIPTION("HISILICON SAS controller driver");
MODULE_ALIAS("platform:" DRV_NAME);
