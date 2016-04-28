/*
 * Copyright (c) 2016 Hisilicon Limited.
 *
 * Authors: Wei Hu <xavier.huwei@huawei.com>
 * Authors: Nenglong Zhao <zhaonenglong@hisilicon.com>
 * Authors: Lijun Ou <oulijun@huawei.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include "hns_roce_device.h"
#include "hns_roce_hw_v1.h"

/**
 * hns_roce_v1_reset - reset roce
 * @hr_dev: roce device struct pointer
 * @val: 1 -- drop reset, 0 -- reset
 * return 0 - success , negative --fail
 */
int hns_roce_v1_reset(struct hns_roce_dev *hr_dev, u32 val)
{
	struct device_node *dsaf_node;
	struct device *dev = &hr_dev->pdev->dev;
	struct device_node *np = dev->of_node;
	int ret;

	dsaf_node = of_parse_phandle(np, "dsaf-handle", 0);

	if (!val) {
		ret = hns_dsaf_roce_reset(&dsaf_node->fwnode, 0);
	} else {
		ret = hns_dsaf_roce_reset(&dsaf_node->fwnode, 0);
		if (ret)
			return ret;

		msleep(SLEEP_TIME_INTERVAL);
		ret = hns_dsaf_roce_reset(&dsaf_node->fwnode, 1);
	}

		return ret;
}

struct hns_roce_hw hns_roce_hw_v1 = {
	.reset = hns_roce_v1_reset,
};
