// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm Technology Inc. Non PAS ADSP Peripheral Image Loader for SDM845.
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 */

#include <linux/clk.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/reset.h>
#include <linux/iopoll.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/remoteproc.h>
#include <linux/soc/qcom/mdt_loader.h>
#include <linux/soc/qcom/smem.h>
#include <linux/soc/qcom/smem_state.h>

#include "qcom_common.h"
#include "qcom_q6v5.h"
#include "remoteproc_internal.h"

/* time out value */
#define ACK_TIMEOUT			1000
#define BOOT_FSM_TIMEOUT		10000
/* mask values */
#define EVB_MASK			GENMASK(27, 4)
/*QDSP6SS register offsets*/
#define RST_EVB_REG			0x10
#define CORE_START_REG			0x400
#define BOOT_CMD_REG			0x404
#define BOOT_STATUS_REG			0x408
#define RET_CFG_REG			0x1C
/*TCSR register offsets*/
#define LPASS_MASTER_IDLE_REG		0x8
#define LPASS_HALTACK_REG		0x4
#define LPASS_PWR_ON_REG		0x10
#define LPASS_HALTREQ_REG		0x0

struct non_pas_adsp_data {
	int crash_reason_smem;
	const char *firmware_name;
	bool has_aggre2_clk;

	const char *ssr_name;
	const char *sysmon_name;
	int ssctl_id;
};

struct qcom_adsp {
	struct device *dev;
	struct rproc *rproc;

	struct qcom_q6v5 q6v5;

	struct clk *xo;
	struct clk *aggre2_clk;
	struct clk *gcc_sway_cbcr;
	struct clk *lpass_audio_aon_clk;
	struct clk *lpass_ahbs_aon_cbcr;
	struct clk *lpass_ahbm_aon_cbcr;
	struct clk *qdsp6ss_xo_cbcr;
	struct clk *qdsp6ss_sleep_cbcr;
	struct clk *qdsp6ss_core_cbcr;

	struct regulator *cx_supply;
	struct regulator *px_supply;

	void __iomem *qdsp6ss_base;

	struct reset_control *pdc_sync_reset;
	struct reset_control *cc_lpass_restart;

	struct regmap *halt_map;
	unsigned int  halt_lpass;

	int crash_reason_smem;
	bool has_aggre2_clk;

	struct completion start_done;
	struct completion stop_done;

	phys_addr_t mem_phys;
	phys_addr_t mem_reloc;
	void *mem_region;
	size_t mem_size;

	struct qcom_rproc_glink glink_subdev;
	struct qcom_rproc_subdev smd_subdev;
	struct qcom_rproc_ssr ssr_subdev;
	struct qcom_sysmon *sysmon;
};

static int adsp_clk_enable(struct qcom_adsp *adsp)
{
	int ret;

	/* Enable SWAY clock */
	ret = clk_prepare_enable(adsp->gcc_sway_cbcr);
	if (ret)
		return ret;

	/* Enable LPASS AHB AON Bus */
	ret = clk_prepare_enable(adsp->lpass_audio_aon_clk);
	if (ret)
		return ret;

	/* Enable the QDSP6SS AHBM and AHBS clocks */
	ret = clk_prepare_enable(adsp->lpass_ahbs_aon_cbcr);
	if (ret)
		return ret;

	ret = clk_prepare_enable(adsp->lpass_ahbm_aon_cbcr);
	if (ret)
		return ret;

	/* Turn on the XO clock, required to boot FSM */
	ret = clk_prepare_enable(adsp->qdsp6ss_xo_cbcr);
	if (ret)
		return ret;

	/* Enable the QDSP6SS sleep clock for the QDSP6 watchdog enablement */
	ret = clk_prepare_enable(adsp->qdsp6ss_sleep_cbcr);
	if (ret)
		return ret;

	/* Configure QDSP6 core CBC to enable clock */
	ret = clk_prepare_enable(adsp->qdsp6ss_core_cbcr);
	if (ret)
		return ret;

	return ret;
}

static int adsp_reset(struct qcom_adsp *adsp)
{
	unsigned int val;
	int ret = 0;

	/* De-assert QDSP6 stop core. QDSP6 will execute after out of reset */
	writel(0x1, adsp->qdsp6ss_base + CORE_START_REG);

	/* Trigger boot FSM to start QDSP6 */
	writel(0x1, adsp->qdsp6ss_base + BOOT_CMD_REG);

	/* Wait for core to come out of reset */
	ret = readl_poll_timeout(adsp->qdsp6ss_base + BOOT_STATUS_REG,
			val, (val & BIT(0)) != 0, 10, BOOT_FSM_TIMEOUT);
	if (ret)
		dev_err(adsp->dev, "Boot FSM failed to complete.\n");

	return ret;
}

static int qcom_adsp_start(struct qcom_adsp *adsp)
{
	int ret;

	ret = adsp_clk_enable(adsp);
	if (ret) {
		dev_err(adsp->dev, "adsp clk_enable failed\n");
		return ret;
	}

	/* Program boot address */
	writel(adsp->mem_phys >> 4, adsp->qdsp6ss_base + RST_EVB_REG);

	ret = adsp_reset(adsp);
	if (ret)
		dev_err(adsp->dev, "De-assert QDSP6 out of reset failed\n");

	return ret;
}

static int qcom_adsp_shutdown(struct qcom_adsp *adsp)
{
	unsigned long timeout;
	unsigned int val;
	int ret;

	/* Reset the retention logic */
	val = readl(adsp->qdsp6ss_base + RET_CFG_REG);
	val |= 0x1;
	writel(val, adsp->qdsp6ss_base + RET_CFG_REG);

	/* Disable QDSP6 core CBCR clock */
	clk_disable_unprepare(adsp->qdsp6ss_core_cbcr);

	/* Disable the QDSP6SS sleep clock */
	clk_disable_unprepare(adsp->qdsp6ss_sleep_cbcr);

	/* Turn off the XO clock */
	clk_disable_unprepare(adsp->qdsp6ss_xo_cbcr);

	/* Disable the QDSP6SS AHBM and AHBS clocks */
	clk_disable_unprepare(adsp->lpass_ahbs_aon_cbcr);

	clk_disable_unprepare(adsp->lpass_ahbm_aon_cbcr);

	/* Disable LPASS AHB AON Bus */
	clk_disable_unprepare(adsp->lpass_audio_aon_clk);

	/* Disable the slave way clock to LPASS */
	clk_disable_unprepare(adsp->gcc_sway_cbcr);

	/* QDSP6 master port needs to be explicitly halted */
	ret = regmap_read(adsp->halt_map,
			adsp->halt_lpass + LPASS_PWR_ON_REG, &val);
	if (ret || !val)
		goto reset;

	ret = regmap_read(adsp->halt_map,
			adsp->halt_lpass + LPASS_MASTER_IDLE_REG,
			&val);
	if (ret || val)
		goto reset;

	regmap_write(adsp->halt_map,
			adsp->halt_lpass + LPASS_HALTREQ_REG, 1);

	/*  Wait for halt ACK from QDSP6 */
	timeout = jiffies + msecs_to_jiffies(ACK_TIMEOUT);
	for (;;) {
		ret = regmap_read(adsp->halt_map,
			adsp->halt_lpass + LPASS_HALTACK_REG, &val);
		if (ret || val || time_after(jiffies, timeout))
			break;

		udelay(1000);
	}

	ret = regmap_read(adsp->halt_map,
			adsp->halt_lpass + LPASS_MASTER_IDLE_REG, &val);
	if (ret || !val)
		dev_err(adsp->dev, "port failed halt\n");

reset:
	/* Assert the LPASS PDC Reset */
	reset_control_assert(adsp->pdc_sync_reset);
	/* Place the LPASS processor into reset */
	reset_control_assert(adsp->cc_lpass_restart);
	/* wait after asserting subsystem restart from AOSS */
	udelay(200);

	/* Clear the halt request for the AXIM and AHBM for Q6 */
	regmap_write(adsp->halt_map, adsp->halt_lpass + LPASS_HALTREQ_REG, 0);

	/* De-assert the LPASS PDC Reset */
	reset_control_deassert(adsp->pdc_sync_reset);
	/* Remove the LPASS reset */
	reset_control_deassert(adsp->cc_lpass_restart);
	/* wait after de-asserting subsystem restart from AOSS */
	udelay(200);

	return 0;
}

static int adsp_load(struct rproc *rproc, const struct firmware *fw)
{
	struct qcom_adsp *adsp = (struct qcom_adsp *)rproc->priv;

	return qcom_mdt_load_no_init(adsp->dev, fw, rproc->firmware, 0,
			     adsp->mem_region, adsp->mem_phys, adsp->mem_size,
			     &adsp->mem_reloc);
}

static int adsp_start(struct rproc *rproc)
{
	struct qcom_adsp *adsp = (struct qcom_adsp *)rproc->priv;
	int ret;

	qcom_q6v5_prepare(&adsp->q6v5);

	ret = clk_prepare_enable(adsp->xo);
	if (ret)
		return ret;

	ret = clk_prepare_enable(adsp->aggre2_clk);
	if (ret)
		goto disable_xo_clk;

	ret = regulator_enable(adsp->cx_supply);
	if (ret)
		goto disable_aggre2_clk;

	ret = regulator_enable(adsp->px_supply);
	if (ret)
		goto disable_cx_supply;

	ret = qcom_adsp_start(adsp);
	if (ret) {
		dev_err(adsp->dev,
			"failed to bootup adsp\n");
		goto disable_px_supply;
	}

	ret = qcom_q6v5_wait_for_start(&adsp->q6v5, msecs_to_jiffies(5000));
	if (ret == -ETIMEDOUT) {
		dev_err(adsp->dev, "start timed out\n");
		qcom_adsp_shutdown(adsp);
		goto disable_px_supply;
	}

	return 0;

disable_px_supply:
	regulator_disable(adsp->px_supply);
disable_cx_supply:
	regulator_disable(adsp->cx_supply);
disable_aggre2_clk:
	clk_disable_unprepare(adsp->aggre2_clk);
disable_xo_clk:
	clk_disable_unprepare(adsp->xo);

	return ret;
}

static void qcom_adsp_pil_handover(struct qcom_q6v5 *q6v5)
{
	struct qcom_adsp *adsp = container_of(q6v5, struct qcom_adsp, q6v5);

	regulator_disable(adsp->px_supply);
	regulator_disable(adsp->cx_supply);
	clk_disable_unprepare(adsp->aggre2_clk);
	clk_disable_unprepare(adsp->xo);
}

static int adsp_stop(struct rproc *rproc)
{
	struct qcom_adsp *adsp = (struct qcom_adsp *)rproc->priv;
	int handover;
	int ret;

	ret = qcom_q6v5_request_stop(&adsp->q6v5);
	if (ret == -ETIMEDOUT)
		dev_err(adsp->dev, "timed out on wait\n");

	ret = qcom_adsp_shutdown(adsp);
	if (ret)
		dev_err(adsp->dev, "failed to shutdown: %d\n", ret);

	handover = qcom_q6v5_unprepare(&adsp->q6v5);
	if (handover)
		qcom_adsp_pil_handover(&adsp->q6v5);

	return ret;
}

static void *adsp_da_to_va(struct rproc *rproc, u64 da, int len)
{
	struct qcom_adsp *adsp = (struct qcom_adsp *)rproc->priv;
	int offset;

	offset = da - adsp->mem_reloc;
	if (offset < 0 || offset + len > adsp->mem_size)
		return NULL;

	return adsp->mem_region + offset;
}

static const struct rproc_ops adsp_ops = {
	.start = adsp_start,
	.stop = adsp_stop,
	.da_to_va = adsp_da_to_va,
	.parse_fw = qcom_register_dump_segments,
	.load = adsp_load,
};

static int adsp_init_clock(struct qcom_adsp *adsp)
{
	int ret;

	adsp->xo = devm_clk_get(adsp->dev, "xo");
	if (IS_ERR(adsp->xo)) {
		ret = PTR_ERR(adsp->xo);
		if (ret != -EPROBE_DEFER)
			dev_err(adsp->dev, "failed to get xo clock");
		return ret;
	}

	if (adsp->has_aggre2_clk) {
		adsp->aggre2_clk = devm_clk_get(adsp->dev, "aggre2");
		if (IS_ERR(adsp->aggre2_clk)) {
			ret = PTR_ERR(adsp->aggre2_clk);
			if (ret != -EPROBE_DEFER)
				dev_err(adsp->dev,
						"failed to get aggre2 clock");
			return ret;
		}
	}

	adsp->gcc_sway_cbcr = devm_clk_get(adsp->dev, "sway_cbcr");
	if (IS_ERR(adsp->gcc_sway_cbcr)) {
		ret = PTR_ERR(adsp->xo);
		if (ret != -EPROBE_DEFER)
			dev_err(adsp->dev, "failed to get gcc_sway clock\n");
		return PTR_ERR(adsp->gcc_sway_cbcr);
	}

	adsp->lpass_audio_aon_clk = devm_clk_get(adsp->dev, "lpass_aon");
	if (IS_ERR(adsp->lpass_audio_aon_clk)) {
		ret = PTR_ERR(adsp->xo);
		if (ret != -EPROBE_DEFER)
			dev_err(adsp->dev, "failed to get lpass aon clk\n");
		return PTR_ERR(adsp->lpass_audio_aon_clk);
	}

	adsp->lpass_ahbs_aon_cbcr = devm_clk_get(adsp->dev,
						 "lpass_ahbs_aon_cbcr");
	if (IS_ERR(adsp->lpass_ahbs_aon_cbcr)) {
		ret = PTR_ERR(adsp->xo);
		if (ret != -EPROBE_DEFER)
			dev_err(adsp->dev, "failed to get ahb_aon clk\n");
		return PTR_ERR(adsp->lpass_ahbs_aon_cbcr);
	}

	adsp->lpass_ahbm_aon_cbcr = devm_clk_get(adsp->dev,
						 "lpass_ahbm_aon_cbcr");
	if (IS_ERR(adsp->lpass_ahbm_aon_cbcr)) {
		ret = PTR_ERR(adsp->xo);
		if (ret != -EPROBE_DEFER)
			dev_err(adsp->dev, "failed to get ahbm_aon clk\n");
		return PTR_ERR(adsp->lpass_ahbm_aon_cbcr);
	}

	adsp->qdsp6ss_xo_cbcr = devm_clk_get(adsp->dev, "qdsp6ss_xo");
	if (IS_ERR(adsp->qdsp6ss_xo_cbcr)) {
		ret = PTR_ERR(adsp->xo);
		if (ret != -EPROBE_DEFER)
			dev_err(adsp->dev, "failed to get qdsp6ss_xo clk\n");
		return PTR_ERR(adsp->qdsp6ss_xo_cbcr);
	}

	adsp->qdsp6ss_sleep_cbcr = devm_clk_get(adsp->dev, "qdsp6ss_sleep");
	if (IS_ERR(adsp->qdsp6ss_sleep_cbcr)) {
		ret = PTR_ERR(adsp->xo);
		if (ret != -EPROBE_DEFER)
			dev_err(adsp->dev, "failed to get qdsp6ss_sleep clk\n");
		return PTR_ERR(adsp->qdsp6ss_sleep_cbcr);
	}

	adsp->qdsp6ss_core_cbcr = devm_clk_get(adsp->dev, "qdsp6ss_core");
	if (IS_ERR(adsp->qdsp6ss_core_cbcr)) {
		ret = PTR_ERR(adsp->xo);
		if (ret != -EPROBE_DEFER)
			dev_err(adsp->dev, "failed to get qdsp6ss_core clk\n");
		return PTR_ERR(adsp->qdsp6ss_core_cbcr);
	}

	return 0;
}

static int adsp_init_regulator(struct qcom_adsp *adsp)
{
	adsp->cx_supply = devm_regulator_get(adsp->dev, "cx");
	if (IS_ERR(adsp->cx_supply))
		return PTR_ERR(adsp->cx_supply);

	regulator_set_load(adsp->cx_supply, 100000);

	adsp->px_supply = devm_regulator_get(adsp->dev, "px");
	return PTR_ERR_OR_ZERO(adsp->px_supply);
}

static int adsp_init_reset(struct qcom_adsp *adsp)
{
	adsp->pdc_sync_reset = devm_reset_control_get_exclusive(adsp->dev,
			"pdc_sync");
	if (IS_ERR(adsp->pdc_sync_reset)) {
		dev_err(adsp->dev, "failed to acquire pdc_sync reset\n");
		return PTR_ERR(adsp->pdc_sync_reset);
	}

	adsp->cc_lpass_restart = devm_reset_control_get_exclusive(adsp->dev,
			"cc_lpass");
	if (IS_ERR(adsp->cc_lpass_restart)) {
		dev_err(adsp->dev, "failed to acquire cc_lpass restart\n");
		return PTR_ERR(adsp->cc_lpass_restart);
	}

	return 0;
}

static int adsp_init_mmio(struct qcom_adsp *adsp,
				struct platform_device *pdev)
{
	struct device_node *syscon;
	struct resource *res;
	int ret;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "qdsp6ss");
	adsp->qdsp6ss_base = devm_ioremap(&pdev->dev, res->start,
					  resource_size(res));
	if (IS_ERR(adsp->qdsp6ss_base)) {
		dev_err(adsp->dev, "failed to map QDSP6SS registers\n");
		return PTR_ERR(adsp->qdsp6ss_base);
	}

	syscon = of_parse_phandle(pdev->dev.of_node, "qcom,halt-regs", 0);
	if (!syscon) {
		dev_err(&pdev->dev, "failed to parse qcom,halt-regs\n");
		return -EINVAL;
	}

	adsp->halt_map = syscon_node_to_regmap(syscon);
	of_node_put(syscon);
	if (IS_ERR(adsp->halt_map))
		return PTR_ERR(adsp->halt_map);

	ret = of_property_read_u32_index(pdev->dev.of_node, "qcom,halt-regs",
					 1, &adsp->halt_lpass);
	if (ret < 0) {
		dev_err(&pdev->dev, "no offset in syscon\n");
		return ret;
	}

	return 0;
}

static int adsp_alloc_memory_region(struct qcom_adsp *adsp)
{
	struct device_node *node;
	struct resource r;
	int ret;

	node = of_parse_phandle(adsp->dev->of_node, "memory-region", 0);
	if (!node) {
		dev_err(adsp->dev, "no memory-region specified\n");
		return -EINVAL;
	}

	ret = of_address_to_resource(node, 0, &r);
	if (ret)
		return ret;

	adsp->mem_phys = adsp->mem_reloc = r.start;
	adsp->mem_size = resource_size(&r);
	adsp->mem_region = devm_ioremap_wc(adsp->dev,
				adsp->mem_phys, adsp->mem_size);
	if (!adsp->mem_region) {
		dev_err(adsp->dev, "unable to map memory region: %pa+%zx\n",
			&r.start, adsp->mem_size);
		return -EBUSY;
	}

	return 0;
}

static int adsp_probe(struct platform_device *pdev)
{
	const struct non_pas_adsp_data *desc;
	struct qcom_adsp *adsp;
	struct rproc *rproc;
	int ret;

	desc = of_device_get_match_data(&pdev->dev);
	if (!desc)
		return -EINVAL;

	rproc = rproc_alloc(&pdev->dev, pdev->name, &adsp_ops,
			    desc->firmware_name, sizeof(*adsp));
	if (!rproc) {
		dev_err(&pdev->dev, "unable to allocate remoteproc\n");
		return -ENOMEM;
	}

	adsp = (struct qcom_adsp *)rproc->priv;
	adsp->dev = &pdev->dev;
	adsp->rproc = rproc;
	adsp->has_aggre2_clk = desc->has_aggre2_clk;
	platform_set_drvdata(pdev, adsp);

	ret = adsp_alloc_memory_region(adsp);
	if (ret)
		goto free_rproc;

	ret = adsp_init_clock(adsp);
	if (ret)
		goto free_rproc;

	ret = adsp_init_regulator(adsp);
	if (ret)
		goto free_rproc;

	ret = adsp_init_reset(adsp);
	if (ret)
		goto free_rproc;

	ret = adsp_init_mmio(adsp, pdev);
	if (ret)
		goto free_rproc;

	ret = qcom_q6v5_init(&adsp->q6v5, pdev, rproc, desc->crash_reason_smem,
			     qcom_adsp_pil_handover);
	if (ret)
		goto free_rproc;

	qcom_add_glink_subdev(rproc, &adsp->glink_subdev);
	qcom_add_smd_subdev(rproc, &adsp->smd_subdev);
	qcom_add_ssr_subdev(rproc, &adsp->ssr_subdev, desc->ssr_name);
	adsp->sysmon = qcom_add_sysmon_subdev(rproc,
					      desc->sysmon_name,
					      desc->ssctl_id);

	ret = rproc_add(rproc);
	if (ret)
		goto free_rproc;

	return 0;

free_rproc:
	rproc_free(rproc);

	return ret;
}

static int adsp_remove(struct platform_device *pdev)
{
	struct qcom_adsp *adsp = platform_get_drvdata(pdev);

	rproc_del(adsp->rproc);

	qcom_remove_glink_subdev(adsp->rproc, &adsp->glink_subdev);
	qcom_remove_sysmon_subdev(adsp->sysmon);
	qcom_remove_smd_subdev(adsp->rproc, &adsp->smd_subdev);
	qcom_remove_ssr_subdev(adsp->rproc, &adsp->ssr_subdev);
	rproc_free(adsp->rproc);

	return 0;
}

static const struct non_pas_adsp_data adsp_resource_init = {
		.crash_reason_smem = 423,
		.firmware_name = "adsp.mdt",
		.has_aggre2_clk = false,
		.ssr_name = "lpass",
		.sysmon_name = "adsp",
		.ssctl_id = 0x14,
};

static const struct of_device_id adsp_of_match[] = {
	{ .compatible = "qcom,sdm845-apss-adsp-pil",
	  .data = &adsp_resource_init},
	{ },
};
MODULE_DEVICE_TABLE(of, adsp_of_match);

static struct platform_driver non_pas_adsp_driver = {
	.probe = adsp_probe,
	.remove = adsp_remove,
	.driver = {
		.name = "qcom_non_pas_adsp_pil",
		.of_match_table = adsp_of_match,
	},
};

module_platform_driver(non_pas_adsp_driver);
MODULE_DESCRIPTION("QTi SDM845 NON-PAS ADSP Peripherial Image Loader");
MODULE_LICENSE("GPL v2");
