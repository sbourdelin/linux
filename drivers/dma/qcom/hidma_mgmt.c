/*
 * Qualcomm Technologies HIDMA DMA engine Management interface
 *
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/dmaengine.h>
#include <linux/acpi.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>

#include "hidma_mgmt.h"

#define QOS_N_OFFSET			0x300
#define CFG_OFFSET			0x400
#define MAX_BUS_REQ_LEN_OFFSET		0x41C
#define MAX_XACTIONS_OFFSET		0x420
#define HW_VERSION_OFFSET		0x424
#define CHRESET_TIMEOUT_OFFSET		0x418

#define MAX_WR_XACTIONS_MASK		0x1F
#define MAX_RD_XACTIONS_MASK		0x1F
#define WEIGHT_MASK			0x7F
#define MAX_BUS_REQ_LEN_MASK		0xFFFF
#define CHRESET_TIMEOUUT_MASK		0xFFFFF

#define MAX_WR_XACTIONS_BIT_POS	16
#define MAX_BUS_WR_REQ_BIT_POS		16
#define WRR_BIT_POS			8
#define PRIORITY_BIT_POS		15

#define AUTOSUSPEND_TIMEOUT		2000
#define MAX_CHANNEL_WEIGHT		15

int hidma_mgmt_setup(struct hidma_mgmt_dev *mgmtdev)
{
	unsigned int i;
	u32 val;

	if (!is_power_of_2(mgmtdev->max_write_request) ||
		(mgmtdev->max_write_request < 128) ||
		(mgmtdev->max_write_request > 1024)) {
		dev_err(&mgmtdev->pdev->dev, "invalid write request %d\n",
			mgmtdev->max_write_request);
		return -EINVAL;
	}

	if (!is_power_of_2(mgmtdev->max_read_request) ||
		(mgmtdev->max_read_request < 128) ||
		(mgmtdev->max_read_request > 1024)) {
		dev_err(&mgmtdev->pdev->dev, "invalid read request %d\n",
			mgmtdev->max_read_request);
		return  -EINVAL;
	}

	if (mgmtdev->max_wr_xactions > MAX_WR_XACTIONS_MASK) {
		dev_err(&mgmtdev->pdev->dev,
			"max_wr_xactions cannot be bigger than %d\n",
			MAX_WR_XACTIONS_MASK);
		return -EINVAL;
	}

	if (mgmtdev->max_rd_xactions > MAX_RD_XACTIONS_MASK) {
		dev_err(&mgmtdev->pdev->dev,
			"max_rd_xactions cannot be bigger than %d\n",
			MAX_RD_XACTIONS_MASK);
		return -EINVAL;
	}

	for (i = 0; i < mgmtdev->dma_channels; i++) {
		if (mgmtdev->priority[i] > 1) {
			dev_err(&mgmtdev->pdev->dev, "priority can be 0 or 1\n");
			return -EINVAL;
		}

		if (mgmtdev->weight[i] > MAX_CHANNEL_WEIGHT) {
			dev_err(&mgmtdev->pdev->dev,
				"max value of weight can be %d.\n",
				MAX_CHANNEL_WEIGHT);
			return -EINVAL;
		}

		/* weight needs to be at least one */
		if (mgmtdev->weight[i] == 0)
			mgmtdev->weight[i] = 1;
	}

	pm_runtime_get_sync(&mgmtdev->pdev->dev);
	val = readl(mgmtdev->virtaddr + MAX_BUS_REQ_LEN_OFFSET);
	val &= ~(MAX_BUS_REQ_LEN_MASK << MAX_BUS_WR_REQ_BIT_POS);
	val |= (mgmtdev->max_write_request << MAX_BUS_WR_REQ_BIT_POS);
	val &= ~(MAX_BUS_REQ_LEN_MASK);
	val |= (mgmtdev->max_read_request);
	writel(val, mgmtdev->virtaddr + MAX_BUS_REQ_LEN_OFFSET);

	val = readl(mgmtdev->virtaddr + MAX_XACTIONS_OFFSET);
	val &= ~(MAX_WR_XACTIONS_MASK << MAX_WR_XACTIONS_BIT_POS);
	val |= (mgmtdev->max_wr_xactions << MAX_WR_XACTIONS_BIT_POS);
	val &= ~(MAX_RD_XACTIONS_MASK);
	val |= (mgmtdev->max_rd_xactions);
	writel(val, mgmtdev->virtaddr + MAX_XACTIONS_OFFSET);

	mgmtdev->hw_version = readl(mgmtdev->virtaddr + HW_VERSION_OFFSET);
	mgmtdev->hw_version_major = (mgmtdev->hw_version >> 28) & 0xF;
	mgmtdev->hw_version_minor = (mgmtdev->hw_version >> 16) & 0xF;

	for (i = 0; i < mgmtdev->dma_channels; i++) {
		val = readl(mgmtdev->virtaddr + QOS_N_OFFSET + (4 * i));
		val &= ~(1 << PRIORITY_BIT_POS);
		val |= ((mgmtdev->priority[i] & 0x1) << PRIORITY_BIT_POS);
		val &= ~(WEIGHT_MASK << WRR_BIT_POS);
		val |= ((mgmtdev->weight[i] & WEIGHT_MASK) << WRR_BIT_POS);
		writel(val, mgmtdev->virtaddr + QOS_N_OFFSET + (4 * i));
	}

	val = readl(mgmtdev->virtaddr + CHRESET_TIMEOUT_OFFSET);
	val &= ~CHRESET_TIMEOUUT_MASK;
	val |= (mgmtdev->chreset_timeout_cycles & CHRESET_TIMEOUUT_MASK);
	writel(val, mgmtdev->virtaddr + CHRESET_TIMEOUT_OFFSET);

	pm_runtime_mark_last_busy(&mgmtdev->pdev->dev);
	pm_runtime_put_autosuspend(&mgmtdev->pdev->dev);
	return 0;
}
EXPORT_SYMBOL_GPL(hidma_mgmt_setup);

static int hidma_mgmt_probe(struct platform_device *pdev)
{
	struct hidma_mgmt_dev *mgmtdev;
	struct resource *res;
	void *virtaddr;
	int irq;
	int rc;
	u32 val;

	pm_runtime_set_autosuspend_delay(&pdev->dev, AUTOSUSPEND_TIMEOUT);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_sync(&pdev->dev);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	virtaddr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(virtaddr)) {
		rc = -ENOMEM;
		goto out;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "irq resources not found\n");
		rc = irq;
		goto out;
	}

	mgmtdev = devm_kzalloc(&pdev->dev, sizeof(*mgmtdev), GFP_KERNEL);
	if (!mgmtdev) {
		rc = -ENOMEM;
		goto out;
	}

	mgmtdev->pdev = pdev;
	mgmtdev->addrsize = resource_size(res);
	mgmtdev->virtaddr = virtaddr;

	rc = device_property_read_u32(&pdev->dev, "dma-channels",
				&mgmtdev->dma_channels);
	if (rc) {
		dev_err(&pdev->dev, "number of channels missing\n");
		goto out;
	}

	rc = device_property_read_u32(&pdev->dev,
				"channel-reset-timeout-cycles",
				&mgmtdev->chreset_timeout_cycles);
	if (rc) {
		dev_err(&pdev->dev, "channel reset timeout missing\n");
		goto out;
	}

	rc = device_property_read_u32(&pdev->dev, "max-write-burst-bytes",
				&mgmtdev->max_write_request);
	if (rc) {
		dev_err(&pdev->dev, "max-write-burst-bytes missing\n");
		goto out;
	}

	rc = device_property_read_u32(&pdev->dev, "max-read-burst-bytes",
				&mgmtdev->max_read_request);
	if (rc) {
		dev_err(&pdev->dev, "max-read-burst-bytes missing\n");
		goto out;
	}

	rc = device_property_read_u32(&pdev->dev, "max-write-transactions",
				&mgmtdev->max_wr_xactions);
	if (rc) {
		dev_err(&pdev->dev, "max-write-transactions missing\n");
		goto out;
	}

	rc = device_property_read_u32(&pdev->dev, "max-read-transactions",
				&mgmtdev->max_rd_xactions);
	if (rc) {
		dev_err(&pdev->dev, "max-read-transactions missing\n");
		rc = -EINVAL;
		goto out;
	}

	mgmtdev->priority = devm_kcalloc(&pdev->dev,
		mgmtdev->dma_channels, sizeof(*mgmtdev->priority), GFP_KERNEL);
	if (!mgmtdev->priority) {
		rc = -ENOMEM;
		goto out;
	}

	mgmtdev->weight = devm_kcalloc(&pdev->dev,
		mgmtdev->dma_channels, sizeof(*mgmtdev->weight), GFP_KERNEL);
	if (!mgmtdev->weight) {
		rc = -ENOMEM;
		goto out;
	}

	rc = device_property_read_u32_array(&pdev->dev, "channel-priority",
				mgmtdev->priority, mgmtdev->dma_channels);
	if (rc) {
		dev_err(&pdev->dev, "channel-priority missing\n");
		goto out;
	}

	rc = device_property_read_u32_array(&pdev->dev, "channel-weight",
				mgmtdev->weight, mgmtdev->dma_channels);
	if (rc) {
		dev_err(&pdev->dev, "channel-weight missing\n");
		goto out;
	}

	rc = hidma_mgmt_setup(mgmtdev);
	if (rc) {
		dev_err(&pdev->dev, "setup failed\n");
		goto out;
	}

	/* start the HW */
	val = readl(mgmtdev->virtaddr + CFG_OFFSET);
	val |= 1;
	writel(val, mgmtdev->virtaddr + CFG_OFFSET);

	rc = hidma_mgmt_init_sys(mgmtdev);
	if (rc) {
		dev_err(&pdev->dev, "sysfs setup failed\n");
		goto out;
	}

	dev_info(&pdev->dev,
		 "HW rev: %d.%d @ %pa with %d physical channels\n",
		 mgmtdev->hw_version_major, mgmtdev->hw_version_minor,
		 &res->start, mgmtdev->dma_channels);

	platform_set_drvdata(pdev, mgmtdev);
	pm_runtime_mark_last_busy(&pdev->dev);
	pm_runtime_put_autosuspend(&pdev->dev);
	return 0;
out:
	pm_runtime_disable(&pdev->dev);
	pm_runtime_put_sync_suspend(&pdev->dev);
	return rc;
}

#if IS_ENABLED(CONFIG_ACPI)
static const struct acpi_device_id hidma_mgmt_acpi_ids[] = {
	{"QCOM8060"},
	{},
};
#endif

static const struct of_device_id hidma_mgmt_match[] = {
	{ .compatible = "qcom,hidma-mgmt-1.0", },
	{},
};
MODULE_DEVICE_TABLE(of, hidma_mgmt_match);

static struct platform_driver hidma_mgmt_driver = {
	.probe = hidma_mgmt_probe,
	.driver = {
		.name = "hidma-mgmt",
		.of_match_table = hidma_mgmt_match,
		.acpi_match_table = ACPI_PTR(hidma_mgmt_acpi_ids),
	},
};
module_platform_driver(hidma_mgmt_driver);
MODULE_LICENSE("GPL v2");
