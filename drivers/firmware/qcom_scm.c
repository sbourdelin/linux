/* Copyright (c) 2010,2015, The Linux Foundation. All rights reserved.
 * Copyright (C) 2015 Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/cpumask.h>
#include <linux/export.h>
#include <linux/types.h>
#include <linux/qcom_scm.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>

#include "qcom_scm.h"

struct qcom_scm {
	struct device *dev;
	struct clk *core_clk;
	struct clk *iface_clk;
	struct clk *bus_clk;
};

static struct qcom_scm *__scm;

static int qcom_scm_clk_enable(void)
{
	int ret;

	ret = clk_prepare_enable(__scm->core_clk);
	if (ret)
		goto bail;

	if (__scm->iface_clk) {
		ret = clk_prepare_enable(__scm->iface_clk);
		if (ret)
			goto disable_core;
	}

	if (__scm->bus_clk) {
		ret = clk_prepare_enable(__scm->bus_clk);
		if (ret)
			goto disable_iface;
	}

	return 0;

disable_iface:
	if (__scm->iface_clk)
		clk_disable_unprepare(__scm->iface_clk);
disable_core:
	if (__scm->bus_clk)
		clk_disable_unprepare(__scm->core_clk);
bail:
	return ret;
}

static void qcom_scm_clk_disable(void)
{
	clk_disable_unprepare(__scm->core_clk);
	if (__scm->iface_clk)
		clk_disable_unprepare(__scm->iface_clk);
	if (__scm->bus_clk)
		clk_disable_unprepare(__scm->bus_clk);
}

/**
 * qcom_scm_set_cold_boot_addr() - Set the cold boot address for cpus
 * @entry: Entry point function for the cpus
 * @cpus: The cpumask of cpus that will use the entry point
 *
 * Set the cold boot address of the cpus. Any cpu outside the supported
 * range would be removed from the cpu present mask.
 */
int qcom_scm_set_cold_boot_addr(void *entry, const cpumask_t *cpus)
{
	return __qcom_scm_set_cold_boot_addr(entry, cpus);
}
EXPORT_SYMBOL(qcom_scm_set_cold_boot_addr);

/**
 * qcom_scm_set_warm_boot_addr() - Set the warm boot address for cpus
 * @entry: Entry point function for the cpus
 * @cpus: The cpumask of cpus that will use the entry point
 *
 * Set the Linux entry point for the SCM to transfer control to when coming
 * out of a power down. CPU power down may be executed on cpuidle or hotplug.
 */
int qcom_scm_set_warm_boot_addr(void *entry, const cpumask_t *cpus)
{
	return __qcom_scm_set_warm_boot_addr(entry, cpus);
}
EXPORT_SYMBOL(qcom_scm_set_warm_boot_addr);

/**
 * qcom_scm_cpu_power_down() - Power down the cpu
 * @flags - Flags to flush cache
 *
 * This is an end point to power down cpu. If there was a pending interrupt,
 * the control would return from this function, otherwise, the cpu jumps to the
 * warm boot entry point set for this cpu upon reset.
 */
void qcom_scm_cpu_power_down(u32 flags)
{
	__qcom_scm_cpu_power_down(flags);
}
EXPORT_SYMBOL(qcom_scm_cpu_power_down);

/**
 * qcom_scm_hdcp_available() - Check if secure environment supports HDCP.
 *
 * Return true if HDCP is supported, false if not.
 */
bool qcom_scm_hdcp_available(void)
{
	int ret = qcom_scm_clk_enable();

	if (ret)
		goto clk_err;

	ret = __qcom_scm_is_call_available(QCOM_SCM_SVC_HDCP,
						QCOM_SCM_CMD_HDCP);

	qcom_scm_clk_disable();

clk_err:
	return (ret > 0) ? true : false;
}
EXPORT_SYMBOL(qcom_scm_hdcp_available);

/**
 * qcom_scm_hdcp_req() - Send HDCP request.
 * @req: HDCP request array
 * @req_cnt: HDCP request array count
 * @resp: response buffer passed to SCM
 *
 * Write HDCP register(s) through SCM.
 */
int qcom_scm_hdcp_req(struct qcom_scm_hdcp_req *req, u32 req_cnt, u32 *resp)
{
	int ret = qcom_scm_clk_enable();

	if (ret)
		return ret;

	ret = __qcom_scm_hdcp_req(req, req_cnt, resp);
	qcom_scm_clk_disable();
	return ret;
}
EXPORT_SYMBOL(qcom_scm_hdcp_req);

/**
 * qcom_scm_is_available() - Checks if SCM is available
 */
bool qcom_scm_is_available(void)
{
	return !!__scm;
}
EXPORT_SYMBOL(qcom_scm_is_available);

static void qcom_scm_init(void)
{
	__qcom_scm_init();
}

void *qcom_scm_alloc_buffer(size_t size, dma_addr_t *dma_addr,
				      gfp_t gfp)
{
	if (__scm)
		return dma_alloc_writecombine(__scm->dev, size, dma_addr, gfp);
	else
		return ERR_PTR(-ENODEV);
}

void qcom_scm_free_buffer(size_t size, void *cpu_addr,
				 dma_addr_t dma_addr)
{
	if (__scm)
		dma_free_writecombine(__scm->dev, size, cpu_addr, dma_addr);
}

static int qcom_scm_probe(struct platform_device *pdev)
{
	struct qcom_scm *scm;
	long rate;
	int ret;

	scm = devm_kzalloc(&pdev->dev, sizeof(*scm), GFP_KERNEL);
	if (!scm)
		return -ENOMEM;

	scm->core_clk = devm_clk_get(&pdev->dev, "core");
	if (IS_ERR(scm->core_clk)) {
		if (PTR_ERR(scm->core_clk) != -EPROBE_DEFER)
			dev_err(&pdev->dev, "failed to acquire core clk\n");
		return PTR_ERR(scm->core_clk);
	}

	if (of_device_is_compatible(pdev->dev.of_node, "qcom,scm-apq8064")) {
		scm->iface_clk = devm_clk_get(&pdev->dev, "iface");
		if (IS_ERR(scm->iface_clk)) {
			if (PTR_ERR(scm->iface_clk) != -EPROBE_DEFER)
				dev_err(&pdev->dev, "failed to acquire iface clk\n");
			return PTR_ERR(scm->iface_clk);
		}

		scm->bus_clk = devm_clk_get(&pdev->dev, "bus");
		if (IS_ERR(scm->bus_clk)) {
			if (PTR_ERR(scm->bus_clk) != -EPROBE_DEFER)
				dev_err(&pdev->dev, "failed to acquire bus clk\n");
			return PTR_ERR(scm->bus_clk);
		}
	}

	/* vote for max clk rate for highest performance */
	rate = clk_round_rate(scm->core_clk, INT_MAX);
	ret = clk_set_rate(scm->core_clk, rate);
	if (ret)
		return ret;

	__scm = scm;
	__scm->dev = &pdev->dev;

	qcom_scm_init();

	return 0;
}

static const struct of_device_id qcom_scm_dt_match[] = {
	{ .compatible = "qcom,scm-apq8064",},
	{ .compatible = "qcom,scm-apq8084",},
	{ .compatible = "qcom,scm-msm8916",},
	{ .compatible = "qcom,scm-msm8974",},
	{},
};

MODULE_DEVICE_TABLE(of, qcom_scm_dt_match);

static struct platform_driver qcom_scm_driver = {
	.driver = {
		.name	= "qcom_scm",
		.of_match_table = qcom_scm_dt_match,
	},
	.probe = qcom_scm_probe,
};

builtin_platform_driver(qcom_scm_driver);
MODULE_DESCRIPTION("Qualcomm SCM driver");
MODULE_LICENSE("GPL v2");
