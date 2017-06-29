/*
 * Qualcomm q6v5-wcss Peripheral Image Loader
 *
 * Copyright (c) 2017, The Linux Foundation. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/elf.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/qcom_scm.h>
#include <linux/regmap.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>
#include <linux/soc/qcom/mdt_loader.h>
#include <linux/soc/qcom/smem.h>
#include <linux/soc/qcom/smem_state.h>

#include "qcom_common.h"
#include "remoteproc_internal.h"

#define WCSS_CRASH_REASON_SMEM 421
#define WCNSS_PAS_ID		6
#define STOP_ACK_TIMEOUT_MS 10000

#define QDSP6SS_RST_EVB 0x10
#define QDSP6SS_RESET 0x14
#define QDSP6SS_DBG_CFG 0x18
#define QDSP6SS_XO_CBCR 0x38
#define QDSP6SS_MEM_PWR_CTL 0xb0
#define QDSP6SS_BHS_STATUS 0x78
#define TCSR_GLOBAL_CFG0 0x0
#define TCSR_GLOBAL_CFG1 0x4

#define QDSP6SS_GFMUX_CTL 0x20
#define QDSP6SS_PWR_CTL 0x30
#define TCSR_HALTREQ 0x0
#define TCSR_HALTACK 0x4
#define TCSR_Q6_HALTREQ 0x0
#define TCSR_Q6_HALTACK 0x4
#define SSCAON_CONFIG 0x8
#define SSCAON_STATUS 0xc
#define HALTACK BIT(0)
#define BHS_EN_REST_ACK BIT(0)

struct q6v5 {
	struct device *dev;
	struct qcom_rproc_subdev smd_subdev;
	phys_addr_t mem_phys;
	size_t	mem_size;
	void *mem_region;
	void __iomem *q6_base;
	void __iomem *mpm_base;
	struct regmap *tcsr;
	unsigned int halt_gbl;
	unsigned int halt_q6;
	unsigned int halt_wcss;
	struct rproc *rproc;
	struct completion start_done;
	struct completion stop_done;
	struct qcom_smem_state *state;
	unsigned int stop_bit;
	unsigned int shutdown_bit;
	bool running;
	struct clk **clks;
	int clk_cnt;
	struct reset_control *wcss_aon_reset;
	struct reset_control *wcss_reset;
	struct reset_control *wcss_q6_reset;
};

static struct resource_table *q6v5_find_rsc_table(struct rproc *rproc,
						  const struct firmware *fw,
						  int *tablesz)
{
	static struct resource_table table = { .ver = 1, };

	*tablesz = sizeof(table);
	return &table;
}

static int q6v5_init_clocks(struct device *dev, struct q6v5 *qproc)
{
	int i;
	const char *cname;
	struct property *prop;

	qproc->clk_cnt = of_property_count_strings(dev->of_node,
						   "clock-names");

	if (!qproc->clk_cnt)
		return 0;

	qproc->clks = devm_kzalloc(dev, sizeof(*qproc->clks) * qproc->clk_cnt,
				   GFP_KERNEL);

	of_property_for_each_string(dev->of_node, "clock-names", prop, cname) {
		struct clk *c = devm_clk_get(dev, cname);

		if (IS_ERR_OR_NULL(c)) {
			if (PTR_ERR(c) != -EPROBE_DEFER)
				dev_err(dev, "Failed to get %s clock\n", cname);

			return PTR_ERR(c);
		}

		qproc->clks[i++] = c;
	}

	return 0;
}

static int q6v5_clk_enable(struct q6v5 *qproc)
{
	int rc;
	int i;

	for (i = 0; i < qproc->clk_cnt; i++) {
		rc = clk_prepare_enable(qproc->clks[i]);
		if (rc)
			goto err;
	}

	return 0;
err:
	for (i--; i >= 0; i--)
		clk_disable_unprepare(qproc->clks[i]);

	return rc;
}

static int wcss_powerdown(struct q6v5 *qproc)
{
	unsigned int nretry = 0;
	unsigned int val = 0;
	int ret;

	/* Assert WCSS/Q6 HALTREQ - 1 */
	nretry = 0;

	ret = regmap_update_bits(qproc->tcsr, qproc->halt_wcss + TCSR_HALTREQ,
				 1, 1);
	if (ret)
		return ret;

	while (1) {
		regmap_read(qproc->tcsr, qproc->halt_wcss + TCSR_HALTACK,
			    &val);
		if (val & HALTACK)
			break;
		mdelay(1);
		nretry++;
		if (nretry >= 10) {
			pr_warn("can't get TCSR haltACK\n");
			break;
		}
	}

	/* Check HALTACK */
	/* Set MPM_SSCAON_CONFIG 13 - 2 */
	val = readl(qproc->mpm_base + SSCAON_CONFIG);
	val |= BIT(13);
	writel(val, qproc->mpm_base + SSCAON_CONFIG);

	/* Set MPM_SSCAON_CONFIG 15 - 3 */
	val = readl(qproc->mpm_base + SSCAON_CONFIG);
	val |= BIT(15);
	val &= ~(BIT(16));
	val &= ~(BIT(17));
	val &= ~(BIT(18));
	writel(val, qproc->mpm_base + SSCAON_CONFIG);

	/* Set MPM_SSCAON_CONFIG 1 - 4 */
	val = readl(qproc->mpm_base + SSCAON_CONFIG);
	val |= BIT(1);
	writel(val, qproc->mpm_base + SSCAON_CONFIG);

	/* wait for SSCAON_STATUS to be 0x400 - 5 */
	nretry = 0;
	while (1) {
		val = readl(qproc->mpm_base + SSCAON_STATUS);
		/* ignore bits 16 to 31 */
		val &= 0xffff;
		if (val == BIT(10))
			break;
		nretry++;
		mdelay(1);
		if (nretry == 10) {
			pr_warn("can't get SSCAON_STATUS\n");
			break;
		}
	}

	/* Enable Q6/WCSS BLOCK ARES - 6 */
	reset_control_assert(qproc->wcss_aon_reset);

	/* Enable MPM_WCSSAON_CONFIG 13 - 7 */
	val = readl(qproc->mpm_base + SSCAON_CONFIG);
	val &= (~(BIT(13)));
	writel(val, qproc->mpm_base + SSCAON_CONFIG);

	/* Enable A2AB/ACMT/ECHAB ARES - 8 */
	/* De-assert WCSS/Q6 HALTREQ - 8 */
	reset_control_assert(qproc->wcss_reset);

	ret = regmap_update_bits(qproc->tcsr, qproc->halt_wcss + TCSR_HALTREQ,
				 1, 0);

	return ret;
}

static int q6_powerdown(struct q6v5 *qproc)
{
	int i = 0, ret;
	unsigned int nretry = 0;
	unsigned int val = 0;

	/* Halt Q6 bus interface - 9*/
	ret = regmap_update_bits(qproc->tcsr, qproc->halt_q6 + TCSR_Q6_HALTREQ,
				 1, 1);
	if (ret)
		return ret;

	nretry = 0;
	while (1) {
		regmap_read(qproc->tcsr, qproc->halt_q6 + TCSR_Q6_HALTACK,
			    &val);
		if (val & HALTACK)
			break;
		mdelay(1);
		nretry++;
		if (nretry >= 10) {
			pr_err("can't get TCSR Q6 haltACK\n");
			break;
		}
	}

	/* Disable Q6 Core clock - 10 */
	val = readl(qproc->q6_base + QDSP6SS_GFMUX_CTL);
	val &= (~(BIT(1)));
	writel(val, qproc->q6_base + QDSP6SS_GFMUX_CTL);

	/* Clamp I/O - 11 */
	val = readl(qproc->q6_base + QDSP6SS_PWR_CTL);
	val |= BIT(20);
	writel(val, qproc->q6_base + QDSP6SS_PWR_CTL);

	/* Clamp WL - 12 */
	val = readl(qproc->q6_base + QDSP6SS_PWR_CTL);
	val |= BIT(21);
	writel(val, qproc->q6_base + QDSP6SS_PWR_CTL);

	/* Clear Erase standby - 13 */
	val = readl(qproc->q6_base + QDSP6SS_PWR_CTL);
	val &= (~(BIT(18)));
	writel(val, qproc->q6_base + QDSP6SS_PWR_CTL);

	/* Clear Sleep RTN - 14 */
	val = readl(qproc->q6_base + QDSP6SS_PWR_CTL);
	val &= (~(BIT(19)));
	writel(val, qproc->q6_base + QDSP6SS_PWR_CTL);

	/* turn off QDSP6 memory foot/head switch one bank at a time - 15*/
	for (i = 0; i < 20; i++) {
		val = readl(qproc->q6_base + QDSP6SS_MEM_PWR_CTL);
		val &= (~(BIT(i)));
		writel(val, qproc->q6_base + QDSP6SS_MEM_PWR_CTL);
		mdelay(1);
	}

	/* Assert QMC memory RTN - 16 */
	val = readl(qproc->q6_base + QDSP6SS_PWR_CTL);
	val |= BIT(22);
	writel(val, qproc->q6_base + QDSP6SS_PWR_CTL);

	/* Turn off BHS - 17 */
	val = readl(qproc->q6_base + QDSP6SS_PWR_CTL);
	val &= (~(BIT(24)));
	writel(val, qproc->q6_base + QDSP6SS_PWR_CTL);
	udelay(1);
	/* Wait till BHS Reset is done */
	nretry = 0;
	while (1) {
		val = readl(qproc->q6_base + QDSP6SS_BHS_STATUS);
		if (!(val & BHS_EN_REST_ACK))
			break;
		mdelay(1);
		nretry++;
		if (nretry >= 10) {
			pr_err("BHS_STATUS not OFF\n");
			break;
		}
	}

	/* HALT CLEAR - 18 */
	ret = regmap_update_bits(qproc->tcsr, qproc->halt_q6 + TCSR_Q6_HALTREQ,
				 1, 0);
	if (ret)
		return ret;

	/* Enable Q6 Block reset - 19 */
	reset_control_assert(qproc->wcss_q6_reset);

	return 0;
}

static int q6_rproc_stop(struct rproc *rproc)
{
	struct q6v5 *qproc = rproc->priv;
	int ret = 0;

	qproc->running = false;

	/* WCSS powerdown */
	qcom_smem_state_update_bits(qproc->state, BIT(qproc->stop_bit),
				    BIT(qproc->stop_bit));

	ret = wait_for_completion_timeout(&qproc->stop_done,
					  msecs_to_jiffies(5000));
	if (ret == 0) {
		dev_err(qproc->dev, "timed out on wait\n");
		return -ETIMEDOUT;
	}

	qcom_smem_state_update_bits(qproc->state, BIT(qproc->stop_bit), 0);

	ret = wcss_powerdown(qproc);
	if (ret)
		return ret;

	/* Q6 Power down */
	ret = q6_powerdown(qproc);
	if (ret)
		return ret;

	return 0;
}

static int q6_rproc_start(struct rproc *rproc)
{
	struct q6v5 *qproc = rproc->priv;
	int temp = 19;
	unsigned long val = 0;
	unsigned int nretry = 0;
	int ret = 0;

	ret = q6v5_clk_enable(qproc);
	if (ret) {
		dev_err(qproc->dev, "failed to enable clocks\n");
		return ret;
	}

	/* Release Q6 and WCSS reset */
	reset_control_deassert(qproc->wcss_reset);
	reset_control_deassert(qproc->wcss_q6_reset);

	/* Lithium configuration - clock gating and bus arbitration */
	ret = regmap_update_bits(qproc->tcsr,
				 qproc->halt_gbl + TCSR_GLOBAL_CFG0,
				 0x1F, 0x14);
	if (ret)
		return ret;

	ret = regmap_update_bits(qproc->tcsr,
				 qproc->halt_gbl + TCSR_GLOBAL_CFG1,
				 1, 0);
	if (ret)
		return ret;

	/* Write bootaddr to EVB so that Q6WCSS will jump there after reset */
	writel(rproc->bootaddr >> 4, qproc->q6_base + QDSP6SS_RST_EVB);
	/* Turn on XO clock. It is required for BHS and memory operation */
	writel(0x1, qproc->q6_base + QDSP6SS_XO_CBCR);
	/* Turn on BHS */
	writel(0x1700000, qproc->q6_base + QDSP6SS_PWR_CTL);
	udelay(1);

	/* Wait till BHS Reset is done */
	while (1) {
		val = readl(qproc->q6_base + QDSP6SS_BHS_STATUS);
		if (val & BHS_EN_REST_ACK)
			break;
		mdelay(1);
		nretry++;
		if (nretry >= 10) {
			pr_err("BHS_STATUS not ON\n");
			break;
		}
	}

	/* Put LDO in bypass mode */
	writel(0x3700000, qproc->q6_base + QDSP6SS_PWR_CTL);
	/* De-assert QDSP6 complier memory clamp */
	writel(0x3300000, qproc->q6_base + QDSP6SS_PWR_CTL);
	/* De-assert memory peripheral sleep and L2 memory standby */
	writel(0x33c0000, qproc->q6_base + QDSP6SS_PWR_CTL);

	/* turn on QDSP6 memory foot/head switch one bank at a time */
	while  (temp >= 0) {
		val = readl(qproc->q6_base + QDSP6SS_MEM_PWR_CTL);
		val = val | 1 << temp;
		writel(val, qproc->q6_base + QDSP6SS_MEM_PWR_CTL);
		val = readl(qproc->q6_base + QDSP6SS_MEM_PWR_CTL);
		mdelay(10);
		temp -= 1;
	}
	/* Remove the QDSP6 core memory word line clamp */
	writel(0x31FFFFF, qproc->q6_base + QDSP6SS_PWR_CTL);
	/* Remove QDSP6 I/O clamp */
	writel(0x30FFFFF, qproc->q6_base + QDSP6SS_PWR_CTL);

	/* Bring Q6 out of reset and stop the core */
	writel(0x5, qproc->q6_base + QDSP6SS_RESET);

	/* Retain debugger state during next QDSP6 reset */
	writel(0x0, qproc->q6_base + QDSP6SS_DBG_CFG);
	/* Turn on the QDSP6 core clock */
	writel(0x102, qproc->q6_base + QDSP6SS_GFMUX_CTL);
	/* Enable the core to run */
	writel(0x4, qproc->q6_base + QDSP6SS_RESET);

	ret = wait_for_completion_timeout(&qproc->start_done,
					  msecs_to_jiffies(5000));
	if (ret == 0) {
		dev_err(qproc->dev, "start timed out\n");
		return -ETIMEDOUT;
	}

	qproc->running = true;

	return 0;
}

static struct rproc_ops q6v5_rproc_ops = {
	.start		= q6_rproc_start,
	.stop		= q6_rproc_stop,
};

static struct rproc_fw_ops q6_fw_ops;

static int q6v5_request_irq(struct q6v5 *qproc,
			    struct platform_device *pdev,
			    const char *name,
			    irq_handler_t thread_fn)
{
	int ret;

	ret = platform_get_irq_byname(pdev, name);
	if (ret < 0) {
		dev_err(&pdev->dev, "no %s IRQ defined\n", name);
		return ret;
	}

	ret = devm_request_threaded_irq(&pdev->dev, ret,
					NULL, thread_fn,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"q6v5", qproc);
	if (ret)
		dev_err(&pdev->dev, "request %s IRQ failed\n", name);

	return ret;
}

static irqreturn_t q6v5_fatal_interrupt(int irq, void *dev)
{
	struct q6v5 *qproc = dev;
	char *msg;
	size_t len;

	if (!qproc->running)
		return IRQ_HANDLED;

	msg = qcom_smem_get(QCOM_SMEM_HOST_ANY, WCSS_CRASH_REASON_SMEM, &len);
	if (!IS_ERR(msg) && len > 0 && msg[0])
		dev_err(qproc->dev, "Fatal error from wcss: %s\n", msg);
	else
		dev_err(qproc->dev, "Fatal error received no message!\n");

	rproc_report_crash(qproc->rproc, RPROC_FATAL_ERROR);

	if (!IS_ERR(msg))
		msg[0] = '\0';

	return IRQ_HANDLED;
}

static irqreturn_t q6v5_handover_interrupt(int irq, void *dev)
{
	struct q6v5 *qproc = dev;

	complete(&qproc->start_done);
	return IRQ_HANDLED;
}

static irqreturn_t q6v5_stop_ack_interrupt(int irq, void *dev)
{
	struct q6v5 *qproc = dev;

	complete(&qproc->stop_done);
	return IRQ_HANDLED;
}

static irqreturn_t q6v5_wdog_interrupt(int irq, void *dev)
{
	struct q6v5 *qproc = dev;
	char *msg;
	size_t len;

	if (!qproc->running) {
		complete(&qproc->stop_done);
		return IRQ_HANDLED;
	}

	msg = qcom_smem_get(QCOM_SMEM_HOST_ANY, WCSS_CRASH_REASON_SMEM, &len);
	if (!IS_ERR(msg) && len > 0 && msg[0])
		dev_err(qproc->dev, "Watchdog bite from wcss %s\n", msg);
	else
		dev_err(qproc->dev, "Watchdog bit received no message!\n");

	rproc_report_crash(qproc->rproc, RPROC_WATCHDOG);

	if (!IS_ERR(msg))
		msg[0] = '\0';

	return IRQ_HANDLED;
}

static int q6v5_load(struct rproc *rproc, const struct firmware *fw)
{
	struct q6v5 *qproc = (struct q6v5 *)rproc->priv;

	return qcom_mdt_load(qproc->dev, fw, rproc->firmware, WCNSS_PAS_ID,
			     qproc->mem_region, qproc->mem_phys,
			     qproc->mem_size, false);
}

static int q6_alloc_memory_region(struct q6v5 *qproc)
{
	struct device_node *node;
	struct resource r;
	int ret;

	node = of_parse_phandle(qproc->dev->of_node, "memory-region", 0);
	if (!node) {
		dev_err(qproc->dev, "no memory-region specified\n");
		return -EINVAL;
	}

	ret = of_address_to_resource(node, 0, &r);
	if (ret)
		return ret;

	qproc->mem_phys = r.start;
	qproc->mem_size = resource_size(&r);
	qproc->mem_region = devm_ioremap_wc(qproc->dev, qproc->mem_phys,
					    qproc->mem_size);
	if (!qproc->mem_region) {
		dev_err(qproc->dev, "unable to map memory region: %pa+%zx\n",
			&r.start, qproc->mem_size);
		return -EBUSY;
	}

	return 0;
}

static int q6v5_init_mem(struct q6v5 *qproc, struct platform_device *pdev)
{
	struct of_phandle_args args;
	struct resource *res;
	int ret;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "mpm");
	if (IS_ERR_OR_NULL(res))
		return -ENODEV;

	qproc->mpm_base = ioremap(res->start, resource_size(res));
	if (IS_ERR_OR_NULL(qproc->mpm_base))
		return PTR_ERR(qproc->mpm_base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "q6");
	if (IS_ERR_OR_NULL(res)) {
		ret = -ENODEV;
		goto free_mpm;
	}

	qproc->q6_base = ioremap(res->start, resource_size(res));
	if (IS_ERR_OR_NULL(qproc->q6_base)) {
		ret = PTR_ERR(qproc->q6_base);
		goto free_mpm;
	}

	ret = of_parse_phandle_with_fixed_args(pdev->dev.of_node,
					       "qcom,halt-regs", 3,
					       0, &args);
	if (ret < 0)
		goto free_q6;

	qproc->tcsr = syscon_node_to_regmap(args.np);
	of_node_put(args.np);
	if (IS_ERR_OR_NULL(qproc->tcsr)) {
		ret = PTR_ERR(qproc->tcsr);
		goto free_q6;
	}

	qproc->halt_gbl = args.args[0];
	qproc->halt_q6 = args.args[1];
	qproc->halt_wcss = args.args[2];

	return 0;

free_q6:
	iounmap(qproc->q6_base);

free_mpm:
	iounmap(qproc->mpm_base);

	return ret;
}

static int q6_rproc_probe(struct platform_device *pdev)
{
	struct q6v5 *qproc;
	struct rproc *rproc;
	int ret;
	struct qcom_smem_state *state;
	unsigned int stop_bit;
	const char *firmware_name = of_device_get_match_data(&pdev->dev);

	state = qcom_smem_state_get(&pdev->dev, "stop",
				    &stop_bit);
	if (IS_ERR(state))
		/* Wait till SMP2P is registered and up */
		return -EPROBE_DEFER;

	rproc = rproc_alloc(&pdev->dev, pdev->name, &q6v5_rproc_ops,
			    firmware_name,
			    sizeof(*qproc));
	if (unlikely(!rproc))
		return -ENOMEM;

	qproc = rproc->priv;
	qproc->dev = &pdev->dev;
	qproc->rproc = rproc;
	rproc->has_iommu = false;

	q6_fw_ops = *rproc->fw_ops;
	q6_fw_ops.find_rsc_table = q6v5_find_rsc_table;
	q6_fw_ops.load = q6v5_load;

	q6v5_init_mem(qproc, pdev);

	qproc->wcss_aon_reset = devm_reset_control_get(&pdev->dev,
						       "wcss_aon_reset");
	if (IS_ERR(qproc->wcss_aon_reset))
		return PTR_ERR(qproc->wcss_aon_reset);

	qproc->wcss_reset = devm_reset_control_get(&pdev->dev,
						   "wcss_reset");
	if (IS_ERR(qproc->wcss_reset))
		return PTR_ERR(qproc->wcss_reset);

	qproc->wcss_q6_reset = devm_reset_control_get(&pdev->dev,
						      "wcss_q6_reset");
	if (IS_ERR(qproc->wcss_q6_reset))
		return PTR_ERR(qproc->wcss_q6_reset);

	platform_set_drvdata(pdev, qproc);

	rproc->fw_ops = &q6_fw_ops;

	qproc->state = qcom_smem_state_get(&pdev->dev, "stop",
					   &qproc->stop_bit);
	if (IS_ERR(qproc->state)) {
		pr_err("Can't get stop bit status fro SMP2P\n");
		goto free_rproc;
	}

	qproc->state = qcom_smem_state_get(&pdev->dev, "shutdown",
			&qproc->shutdown_bit);
	if (IS_ERR(qproc->state)) {
		pr_err("Can't get shutdown bit status fro SMP2P\n");
		goto free_rproc;
	}

	ret = q6v5_init_clocks(&pdev->dev, qproc);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to get active clocks.\n");
		goto free_rproc;
	}

	ret = q6v5_request_irq(qproc, pdev, "wdog", q6v5_wdog_interrupt);
	if (ret < 0)
		goto free_rproc;

	ret = q6v5_request_irq(qproc, pdev, "fatal", q6v5_fatal_interrupt);
	if (ret < 0)
		goto free_rproc;

	ret = q6v5_request_irq(qproc, pdev, "handover",
			       q6v5_handover_interrupt);
	if (ret < 0)
		goto free_rproc;

	ret = q6v5_request_irq(qproc, pdev, "stop-ack",
			       q6v5_stop_ack_interrupt);
	if (ret < 0)
		goto free_rproc;

	init_completion(&qproc->start_done);
	init_completion(&qproc->stop_done);

	ret = q6_alloc_memory_region(qproc);
	if (ret < 0)
		goto free_rproc;

	qcom_add_smd_subdev(rproc, &qproc->smd_subdev);

	ret = rproc_add(rproc);
	if (ret)
		goto free_rproc;

	qproc->running = false;

	return 0;

free_rproc:
	rproc_put(rproc);
	return -EIO;
}

static int q6_rproc_remove(struct platform_device *pdev)
{
	struct q6v5 *qproc;
	struct rproc *rproc;

	qproc = platform_get_drvdata(pdev);
	rproc = qproc->rproc;

	rproc_del(rproc);
	rproc_put(rproc);

	return 0;
}

static const struct of_device_id q6_match_table[] = {
	{ .compatible = "q6v5-wcss-pil", .data = "IPQ8074/q6_fw.mdt" },
	{}
};
MODULE_DEVICE_TABLE(of, q6_match_table);

static struct platform_driver q6_rproc_driver = {
	.probe = q6_rproc_probe,
	.remove = q6_rproc_remove,
	.driver = {
		.name = "q6v5-wcss",
		.of_match_table = q6_match_table,
		.owner = THIS_MODULE,
	},
};
module_platform_driver(q6_rproc_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Qualcomm q6v5-wcss remote proc control driver");
