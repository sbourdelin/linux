/*
 * Qualcomm Peripheral Image Loader
 *
 * Copyright (C) 2016 Linaro Ltd.
 * Copyright (C) 2014 Sony Mobile Communications AB
 * Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>
#include <linux/soc/qcom/smem.h>
#include <linux/soc/qcom/smem_state.h>
#include <linux/mutex.h>
#include <linux/of_device.h>

#include "remoteproc_internal.h"
#include "qcom_mdt_loader.h"

#include <linux/qcom_scm.h>

#define MPSS_FIRMWARE_NAME		"modem.mdt"

#define MPSS_CRASH_REASON_SMEM		421

/* RMB Status Register Values */
#define RMB_PBL_SUCCESS			0x1

#define RMB_MBA_XPU_UNLOCKED		0x1
#define RMB_MBA_XPU_UNLOCKED_SCRIBBLED	0x2
#define RMB_MBA_META_DATA_AUTH_SUCCESS	0x3
#define RMB_MBA_AUTH_COMPLETE		0x4

/* PBL/MBA interface registers */
#define RMB_MBA_IMAGE_REG		0x00
#define RMB_PBL_STATUS_REG		0x04
#define RMB_MBA_COMMAND_REG		0x08
#define RMB_MBA_STATUS_REG		0x0C
#define RMB_PMI_META_DATA_REG		0x10
#define RMB_PMI_CODE_START_REG		0x14
#define RMB_PMI_CODE_LENGTH_REG		0x18

#define RMB_CMD_META_DATA_READY		0x1
#define RMB_CMD_LOAD_READY		0x2

/* QDSP6SS Register Offsets */
#define QDSP6SS_RESET_REG		0x014
#define QDSP6SS_GFMUX_CTL_REG		0x020
#define QDSP6SS_PWR_CTL_REG		0x030
#define QDSP6SS_MEM_PWR_CTL		0x0B0
#define QDSP6SS_STRAP_ACC		0x110

/* AXI Halt Register Offsets */
#define AXI_HALTREQ_REG			0x0
#define AXI_HALTACK_REG			0x4
#define AXI_IDLE_REG			0x8

#define HALT_ACK_TIMEOUT_MS		100

/* QDSP6SS_RESET */
#define Q6SS_STOP_CORE			BIT(0)
#define Q6SS_CORE_ARES			BIT(1)
#define Q6SS_BUS_ARES_ENABLE		BIT(2)

/* QDSP6SS_GFMUX_CTL */
#define Q6SS_CLK_ENABLE			BIT(1)

/* QDSP6SS_PWR_CTL */
#define Q6SS_L2DATA_SLP_NRET_N_0	BIT(0)
#define Q6SS_L2DATA_SLP_NRET_N_1	BIT(1)
#define Q6SS_L2DATA_SLP_NRET_N_2	BIT(2)
#define Q6SS_L2TAG_SLP_NRET_N		BIT(16)
#define Q6SS_ETB_SLP_NRET_N		BIT(17)
#define Q6SS_L2DATA_STBY_N		BIT(18)
#define Q6SS_SLP_RET_N			BIT(19)
#define Q6SS_CLAMP_IO			BIT(20)
#define QDSS_BHS_ON			BIT(21)
#define QDSS_LDO_BYP			BIT(22)

/* QDSP6v56 parameters */
#define QDSP6v56_LDO_BYP                BIT(25)
#define QDSP6v56_BHS_ON                 BIT(24)
#define QDSP6v56_CLAMP_WL               BIT(21)
#define QDSP6v56_CLAMP_QMC_MEM          BIT(22)
#define HALT_CHECK_MAX_LOOPS            (200)
#define QDSP6SS_XO_CBCR                 (0x0038)
#define QDSP6SS_ACC_OVERRIDE_VAL	0x20
struct q6_rproc_res {
	char **proxy_clks;
	int proxy_clk_cnt;
	char **active_clks;
	int active_clk_cnt;
	char **proxy_regs;
	int proxy_reg_cnt;
	char **active_regs;
	int active_reg_cnt;
	int **proxy_reg_action;
	int **active_reg_action;
	int *proxy_reg_load;
	int *active_reg_load;
	int *proxy_reg_voltage;
	int *active_reg_voltage;
	char *q6_version;
	char *q6_mba_image;
	int (*q6_reset_init)(void *q, void *p);
};
struct q6v5 {
	struct device *dev;
	struct rproc *rproc;

	void __iomem *reg_base;
	void __iomem *rmb_base;
	void __iomem *restart_reg;
	struct regmap *halt_map;
	u32 halt_q6;
	u32 halt_modem;
	u32 halt_nc;

	struct reset_control *mss_restart;

	struct qcom_smem_state *state;
	unsigned stop_bit;

	const struct q6_rproc_res *q6_rproc_res;
	struct clk **active_clks;
	struct clk **proxy_clks;
	struct regulator **proxy_regs;
	struct regulator **active_regs;

	struct completion start_done;
	struct completion stop_done;
	bool running;

	phys_addr_t mba_phys;
	void *mba_region;
	size_t mba_size;

	phys_addr_t mpss_phys;
	phys_addr_t mpss_reloc;
	void *mpss_region;
	size_t mpss_size;
	struct mutex q6_lock;
	bool proxy_unvote_reg;
	bool proxy_unvote_clk;
};

static int q6_regulator_init(struct q6v5 *qproc)
{
	struct regulator **reg_arr;
	int i;

	if (qproc->q6_rproc_res->proxy_reg_cnt) {
		reg_arr = devm_kzalloc(qproc->dev,
		sizeof(reg_arr) * qproc->q6_rproc_res->proxy_reg_cnt,
		GFP_KERNEL);

		for (i = 0; i < qproc->q6_rproc_res->proxy_reg_cnt; i++) {
			reg_arr[i] = devm_regulator_get(qproc->dev,
			qproc->q6_rproc_res->proxy_regs[i]);
			if (IS_ERR(reg_arr[i]))
				return PTR_ERR(reg_arr[i]);
			qproc->proxy_regs = reg_arr;
		}
	}

	if (qproc->q6_rproc_res->active_reg_cnt) {
		reg_arr = devm_kzalloc(qproc->dev,
		sizeof(reg_arr) * qproc->q6_rproc_res->active_reg_cnt,
		GFP_KERNEL);

		for (i = 0; i < qproc->q6_rproc_res->active_reg_cnt; i++) {
			reg_arr[i] = devm_regulator_get(qproc->dev,
			qproc->q6_rproc_res->active_regs[i]);

			if (IS_ERR(reg_arr[i]))
				return PTR_ERR(reg_arr[i]);
			qproc->active_regs = reg_arr;
		}
	}

	return 0;
}

static int q6_proxy_regulator_enable(struct q6v5 *qproc)
{
	int i, j, ret = 0;
	int **reg_loadnvoltsetflag;
	int *reg_load;
	int *reg_voltage;

	reg_loadnvoltsetflag = qproc->q6_rproc_res->proxy_reg_action;
	reg_load = qproc->q6_rproc_res->proxy_reg_load;
	reg_voltage = qproc->q6_rproc_res->proxy_reg_voltage;

	for (i = 0; i < qproc->q6_rproc_res->proxy_reg_cnt; i++) {
		for (j = 0; j <= 1; j++) {
			if (j == 0 && *(reg_loadnvoltsetflag + i*j + j))
				regulator_set_load(qproc->proxy_regs[i],
				reg_load[i]);
			if (j == 1 && *(reg_loadnvoltsetflag + i*j + j))
				regulator_set_voltage(qproc->proxy_regs[i],
				reg_voltage[i], INT_MAX);
		}
	}

	for (i = 0; i < qproc->q6_rproc_res->proxy_reg_cnt; i++) {
		ret = regulator_enable(qproc->proxy_regs[i]);
		if (ret) {
			for (; i > 0; --i) {
				regulator_disable(qproc->proxy_regs[i]);
				return ret;
			}
		}
	}

	qproc->proxy_unvote_reg = true;

	return 0;
}

static int q6_active_regulator_enable(struct q6v5 *qproc)
{
	int i, j, ret = 0;
	int **reg_loadnvoltsetflag;
	int *reg_load;
	int *reg_voltage;

	reg_loadnvoltsetflag = qproc->q6_rproc_res->active_reg_action;
	reg_load = qproc->q6_rproc_res->active_reg_load;
	reg_voltage = qproc->q6_rproc_res->active_reg_voltage;

	for (i = 0; i < qproc->q6_rproc_res->active_reg_cnt; i++) {
		for (j = 0; j <= 1; j++) {
			if (j == 0 && *(reg_loadnvoltsetflag + i*j + j))
				regulator_set_load(qproc->active_regs[i],
				reg_load[i]);
			if (j == 1 && *(reg_loadnvoltsetflag + i*j + j))
				regulator_set_voltage(qproc->active_regs[i],
				reg_voltage[i], INT_MAX);
		}
	}

	for (i = 0; i < qproc->q6_rproc_res->active_reg_cnt; i++) {
		ret = regulator_enable(qproc->active_regs[i]);
		if (ret) {
			for (; i > 0; --i) {
				regulator_disable(qproc->active_regs[i]);
				return ret;
			}
		}
	}

	return 0;
}

static int q6_regulator_enable(struct q6v5 *qproc)
{
	int ret;

	if (qproc->q6_rproc_res->proxy_reg_cnt)
		ret = q6_proxy_regulator_enable(qproc);

	if (qproc->q6_rproc_res->active_reg_cnt)
		ret = q6_active_regulator_enable(qproc);

	return ret;
}

static int q6_proxy_regulator_disable(struct q6v5 *qproc)
{
	int i, j;
	int **reg_loadnvoltsetflag;

	reg_loadnvoltsetflag = qproc->q6_rproc_res->proxy_reg_action;
	if (!qproc->proxy_unvote_reg)
		return 0;
	for (i = qproc->q6_rproc_res->proxy_reg_cnt-1; i >= 0; i--) {
		for (j = 0; j <= 1; j++) {
			if (j == 0 && *(reg_loadnvoltsetflag + i*j + j))
				regulator_set_load(qproc->proxy_regs[i], 0);
			if (j == 1 && *(reg_loadnvoltsetflag + i*j + j))
				regulator_set_voltage(qproc->proxy_regs[i],
				0, INT_MAX);
		}
	}
	for (i = qproc->q6_rproc_res->proxy_reg_cnt-1; i >= 0; i--)
		regulator_disable(qproc->proxy_regs[i]);
	qproc->proxy_unvote_reg = false;
	return 0;
}

static int q6_active_regulator_disable(struct q6v5 *qproc)
{
	int i, j, ret = 0;
	int **reg_loadnvoltsetflag;

	reg_loadnvoltsetflag = qproc->q6_rproc_res->active_reg_action;

	for (i = qproc->q6_rproc_res->active_reg_cnt-1; i > 0; i--) {
		for (j = 0; j <= 1; j++) {
			if (j == 0 && *(reg_loadnvoltsetflag + i*j + j))
				regulator_set_load(qproc->active_regs[i], 0);
			if (j == 1 && *(reg_loadnvoltsetflag + i*j + j))
				regulator_set_voltage(qproc->active_regs[i],
				0, INT_MAX);
		}
	}
	for (i = qproc->q6_rproc_res->active_reg_cnt-1; i >= 0; i--)
		ret = regulator_disable(qproc->proxy_regs[i]);
	return 0;
}

static void q6_regulator_disable(struct q6v5 *qproc)
{
	if (qproc->q6_rproc_res->proxy_reg_cnt)
		q6_proxy_regulator_disable(qproc);

	if (qproc->q6_rproc_res->active_reg_cnt)
		q6_active_regulator_disable(qproc);
}

static int q6_proxy_clk_enable(struct q6v5 *qproc)
{
	int i, ret = 0;

	for (i = 0; i < qproc->q6_rproc_res->proxy_clk_cnt; i++) {
		ret = clk_prepare_enable(qproc->proxy_clks[i]);
		if (ret) {
			for (; i > 0; --i) {
				clk_disable_unprepare(qproc->proxy_clks[i]);
				return ret;
			}
		}
	}
	qproc->proxy_unvote_clk = true;
	return 0;
}

static void q6_proxy_clk_disable(struct q6v5 *qproc)
{
	int i;

	if (!qproc->proxy_unvote_clk)
		return;
	for (i = qproc->q6_rproc_res->proxy_clk_cnt-1; i >= 0; i--)
		clk_disable_unprepare(qproc->proxy_clks[i]);
	qproc->proxy_unvote_clk = false;
}

static int q6_active_clk_enable(struct q6v5 *qproc)
{
	int i, ret = 0;

	for (i = 0; i < qproc->q6_rproc_res->active_clk_cnt; i++) {
		ret = clk_prepare_enable(qproc->active_clks[i]);
		if (ret) {
			for (; i > 0; i--) {
				clk_disable_unprepare(qproc->active_clks[i]);
				return ret;
			}
		}
	}
	return 0;
}

static void q6_active_clk_disable(struct q6v5 *qproc)
{
	int i;

	for (i = qproc->q6_rproc_res->active_clk_cnt-1; i >= 0; i--)
		clk_disable_unprepare(qproc->active_clks[i]);
}

static void pil_mss_restart_reg(struct q6v5 *qproc, u32 mss_restart)
{
	if (qproc->restart_reg) {
		writel_relaxed(mss_restart, qproc->restart_reg);
		udelay(2);
	}
}

static int q6_load(struct rproc *rproc, const struct firmware *fw)
{
	struct q6v5 *qproc = rproc->priv;

	memcpy(qproc->mba_region, fw->data, fw->size);

	return 0;
}

static const struct rproc_fw_ops q6_fw_ops = {
	.find_rsc_table = qcom_mdt_find_rsc_table,
	.load = q6_load,
};

static int q6_rmb_pbl_wait(struct q6v5 *qproc, int ms)
{
	unsigned long timeout;
	s32 val;

	timeout = jiffies + msecs_to_jiffies(ms);
	for (;;) {
		val = readl(qproc->rmb_base + RMB_PBL_STATUS_REG);
		if (val)
			break;

		if (time_after(jiffies, timeout))
			return -ETIMEDOUT;

		msleep(1);
	}

	return val;
}

static int q6_rmb_mba_wait(struct q6v5 *qproc, u32 status, int ms)
{

	unsigned long timeout;
	s32 val;

	timeout = jiffies + msecs_to_jiffies(ms);
	for (;;) {
		val = readl(qproc->rmb_base + RMB_MBA_STATUS_REG);
		if (val < 0)
			break;

		if (!status && val)
			break;
		else if (status && val == status)
			break;

		if (time_after(jiffies, timeout))
			return -ETIMEDOUT;

		msleep(1);
	}

	return val;
}

static int q6proc_reset(struct q6v5 *qproc)
{
	int ret, i, count;
	u64 val;

	/* Override the ACC value if required */
	if (!strcmp(qproc->q6_rproc_res->q6_version, "v56"))
		writel_relaxed(QDSP6SS_ACC_OVERRIDE_VAL,
				qproc->reg_base + QDSP6SS_STRAP_ACC);

	/* Assert resets, stop core */
	val = readl_relaxed(qproc->reg_base + QDSP6SS_RESET_REG);
	val |= (Q6SS_CORE_ARES | Q6SS_BUS_ARES_ENABLE | Q6SS_STOP_CORE);
	writel_relaxed(val, qproc->reg_base + QDSP6SS_RESET_REG);

	/* BHS require xo cbcr to be enabled */
	if (!strcmp(qproc->q6_rproc_res->q6_version, "v56")) {
		val = readl_relaxed(qproc->reg_base + QDSP6SS_XO_CBCR);
		val |= 0x1;
		writel_relaxed(val, qproc->reg_base + QDSP6SS_XO_CBCR);
		for (count = HALT_CHECK_MAX_LOOPS; count > 0; count--) {
			val = readl_relaxed(qproc->reg_base + QDSP6SS_XO_CBCR);
			if (!(val & BIT(31)))
				break;
			udelay(1);
		}

		val = readl_relaxed(qproc->reg_base + QDSP6SS_XO_CBCR);
		if ((val & BIT(31)))
			dev_err(qproc->dev, "Failed to enable xo branch clock.\n");
	}
	/* Enable power block headswitch, and wait for it to stabilize */
	val = readl_relaxed(qproc->reg_base + QDSP6SS_PWR_CTL_REG);
	val |= QDSP6v56_BHS_ON;
	writel_relaxed(val, qproc->reg_base + QDSP6SS_PWR_CTL_REG);
	udelay(1);

	/* Put LDO in bypass mode */
	val |= QDSP6v56_LDO_BYP;
	writel_relaxed(val, qproc->reg_base + QDSP6SS_PWR_CTL_REG);

	if (!strcmp(qproc->q6_rproc_res->q6_version, "v56")) {
		/*
		 * Deassert QDSP6 compiler memory clamp
		 */
		val = readl_relaxed(qproc->reg_base + QDSP6SS_PWR_CTL_REG);
		val &= ~QDSP6v56_CLAMP_QMC_MEM;
		writel_relaxed(val, qproc->reg_base + QDSP6SS_PWR_CTL_REG);

		/* Deassert memory peripheral sleep and L2 memory standby */
		val |= (Q6SS_L2DATA_STBY_N | Q6SS_SLP_RET_N);
		writel_relaxed(val, qproc->reg_base + QDSP6SS_PWR_CTL_REG);

		/* Turn on L1, L2, ETB and JU memories 1 at a time */
		val = readl_relaxed(qproc->reg_base + QDSP6SS_MEM_PWR_CTL);
		for (i = 19; i >= 0; i--) {
			val |= BIT(i);
			writel_relaxed(val, qproc->reg_base +
						QDSP6SS_MEM_PWR_CTL);
			/*
			 * Wait for 1us for both memory peripheral and
			 * data array to turn on.
			 */
			 mb();
			udelay(1);
		}
		/* Remove word line clamp */
		val = readl_relaxed(qproc->reg_base + QDSP6SS_PWR_CTL_REG);
		val &= ~QDSP6v56_CLAMP_WL;
		writel_relaxed(val, qproc->reg_base + QDSP6SS_PWR_CTL_REG);
	} else {
		/*
		 * Turn on memories. L2 banks should be done individually
		 * to minimize inrush current.
		 */
		val = readl(qproc->reg_base + QDSP6SS_PWR_CTL_REG);
		val |= Q6SS_SLP_RET_N | Q6SS_L2TAG_SLP_NRET_N |
			Q6SS_ETB_SLP_NRET_N | Q6SS_L2DATA_STBY_N;
		writel(val, qproc->reg_base + QDSP6SS_PWR_CTL_REG);
		val |= Q6SS_L2DATA_SLP_NRET_N_2;
		writel(val, qproc->reg_base + QDSP6SS_PWR_CTL_REG);
		val |= Q6SS_L2DATA_SLP_NRET_N_1;
		writel(val, qproc->reg_base + QDSP6SS_PWR_CTL_REG);
		val |= Q6SS_L2DATA_SLP_NRET_N_0;
		writel(val, qproc->reg_base + QDSP6SS_PWR_CTL_REG);
	}
	/* Remove IO clamp */
	val &= ~Q6SS_CLAMP_IO;
	writel_relaxed(val, qproc->reg_base + QDSP6SS_PWR_CTL_REG);

	/* Bring core out of reset */
	val = readl(qproc->reg_base + QDSP6SS_RESET_REG);
	val &= ~Q6SS_CORE_ARES;
	writel(val, qproc->reg_base + QDSP6SS_RESET_REG);

	/* Turn on core clock */
	val = readl_relaxed(qproc->reg_base + QDSP6SS_GFMUX_CTL_REG);
	val |= Q6SS_CLK_ENABLE;
	writel_relaxed(val, qproc->reg_base + QDSP6SS_GFMUX_CTL_REG);

	/* Start core execution */
	val = readl(qproc->reg_base + QDSP6SS_RESET_REG);
	val &= ~Q6SS_STOP_CORE;
	writel(val, qproc->reg_base + QDSP6SS_RESET_REG);

	/* Wait for PBL status */
	ret = q6_rmb_pbl_wait(qproc, 1000);
	if (ret == -ETIMEDOUT) {
		dev_err(qproc->dev, "PBL boot timed out\n");
	} else if (ret != RMB_PBL_SUCCESS) {
		dev_err(qproc->dev, "PBL returned unexpected status %d\n", ret);
		ret = -EINVAL;
	} else {
		ret = 0;
	}

	return ret;
}

static void q6v5proc_halt_axi_port(struct q6v5 *qproc,
				   struct regmap *halt_map,
				   u32 offset)
{
	unsigned long timeout;
	unsigned int val;
	int ret;

	/* Assert halt request */
	regmap_write(halt_map, offset + AXI_HALTREQ_REG, 1);

	/* Wait for halt */
	timeout = jiffies + msecs_to_jiffies(HALT_ACK_TIMEOUT_MS);
	for (;;) {
		ret = regmap_read(halt_map, offset + AXI_HALTACK_REG, &val);
		if (ret || val || time_after(jiffies, timeout))
			break;

		msleep(1);
	}

	ret = regmap_read(halt_map, offset + AXI_IDLE_REG, &val);
	if (ret || !val)
		dev_err(qproc->dev, "port failed halt\n");

	/* Clear halt request (port will remain halted until reset) */
	regmap_write(halt_map, offset + AXI_HALTREQ_REG, 0);
}

static int q6_mpss_init_image(struct q6v5 *qproc, const struct firmware *fw)
{
	unsigned long dma_attrs = DMA_ATTR_FORCE_CONTIGUOUS;
	dma_addr_t phys;
	void *ptr;
	int ret;

	ptr = dma_alloc_attrs(qproc->dev, fw->size, &phys, GFP_KERNEL, dma_attrs);
	if (!ptr) {
		dev_err(qproc->dev, "failed to allocate mdt buffer\n");
		return -ENOMEM;
	}

	memcpy(ptr, fw->data, fw->size);

	writel(phys, qproc->rmb_base + RMB_PMI_META_DATA_REG);
	writel(RMB_CMD_META_DATA_READY, qproc->rmb_base + RMB_MBA_COMMAND_REG);

	ret = q6_rmb_mba_wait(qproc, RMB_MBA_META_DATA_AUTH_SUCCESS, 1000);
	if (ret == -ETIMEDOUT)
		dev_err(qproc->dev, "MPSS header authentication timed out\n");
	else if (ret < 0)
		dev_err(qproc->dev, "MPSS header authentication failed: %d\n", ret);

	dma_free_attrs(qproc->dev, fw->size, ptr, phys, dma_attrs);

	return ret < 0 ? ret : 0;
}

static int q6_mpss_validate(struct q6v5 *qproc, const struct firmware *fw)
{
	const struct elf32_phdr *phdrs;
	const struct elf32_phdr *phdr;
	struct elf32_hdr *ehdr;
	phys_addr_t boot_addr;
	phys_addr_t fw_addr;
	bool relocate;
	size_t size;
	int ret;
	int i;

	ret = qcom_mdt_parse(fw, &fw_addr, NULL, &relocate);
	if (ret) {
		dev_err(qproc->dev, "failed to parse mdt header\n");
		return ret;
	}

	if (relocate)
		boot_addr = qproc->mpss_phys;
	else
		boot_addr = fw_addr;

	ehdr = (struct elf32_hdr *)fw->data;
	phdrs = (struct elf32_phdr *)(ehdr + 1);
	for (i = 0; i < ehdr->e_phnum; i++, phdr++) {
		phdr = &phdrs[i];

		if (phdr->p_type != PT_LOAD)
			continue;

		if ((phdr->p_flags & QCOM_MDT_TYPE_MASK) == QCOM_MDT_TYPE_HASH)
			continue;

		if (!phdr->p_memsz)
			continue;

		size = readl(qproc->rmb_base + RMB_PMI_CODE_LENGTH_REG);
		if (!size) {
			writel(boot_addr, qproc->rmb_base + RMB_PMI_CODE_START_REG);
			writel(RMB_CMD_LOAD_READY, qproc->rmb_base + RMB_MBA_COMMAND_REG);
		}

		size += phdr->p_memsz;
		writel(size, qproc->rmb_base + RMB_PMI_CODE_LENGTH_REG);
	}

	ret = q6_rmb_mba_wait(qproc, RMB_MBA_AUTH_COMPLETE, 10000);
	if (ret == -ETIMEDOUT)
		dev_err(qproc->dev, "MPSS authentication timed out\n");
	else if (ret < 0)
		dev_err(qproc->dev, "MPSS authentication failed: %d\n", ret);

	return ret < 0 ? ret : 0;
}

static int q6_mpss_load(struct q6v5 *qproc)
{
	const struct firmware *fw;
	phys_addr_t fw_addr;
	bool relocate;
	int ret;

	ret = request_firmware(&fw, MPSS_FIRMWARE_NAME, qproc->dev);
	if (ret < 0) {
		dev_err(qproc->dev, "unable to load " MPSS_FIRMWARE_NAME "\n");
		return ret;
	}

	ret = qcom_mdt_parse(fw, &fw_addr, NULL, &relocate);
	if (ret) {
		dev_err(qproc->dev, "failed to parse mdt header\n");
		goto release_firmware;
	}

	if (relocate)
		qproc->mpss_reloc = fw_addr;

	/* Initialize the RMB validator */
	writel(0, qproc->rmb_base + RMB_PMI_CODE_LENGTH_REG);

	ret = q6_mpss_init_image(qproc, fw);
	if (ret)
		goto release_firmware;

	ret = qcom_mdt_load(qproc->rproc, fw, MPSS_FIRMWARE_NAME);
	if (ret)
		goto release_firmware;

	ret = q6_mpss_validate(qproc, fw);

release_firmware:
	release_firmware(fw);

	return ret < 0 ? ret : 0;
}

static int q6_start(struct rproc *rproc)
{
	struct q6v5 *qproc = (struct q6v5 *)rproc->priv;
	int ret;

	mutex_lock(&qproc->q6_lock);
	ret = q6_regulator_enable(qproc);
	if (ret) {
		dev_err(qproc->dev, "failed to enable reg supplies\n");
		return ret;
	}

	ret = q6_proxy_clk_enable(qproc);
	if (ret) {
		dev_err(qproc->dev, "failed to enable proxy_clk\n");
		goto err_proxy_clk;
	}

	ret = q6_active_clk_enable(qproc);
	if (ret) {
		dev_err(qproc->dev, "failed to enable active clocks\n");
		goto err_active_clks;
	}

	if (!strcmp(qproc->q6_rproc_res->q6_version, "v56"))
		pil_mss_restart_reg(qproc, 0);
	else {
		ret = reset_control_deassert(qproc->mss_restart);
		if (ret) {
			dev_err(qproc->dev, "failed to deassert mss restart\n");
			goto err_deassert;
		}
	}

	writel_relaxed(qproc->mba_phys, qproc->rmb_base + RMB_MBA_IMAGE_REG);

	ret = q6proc_reset(qproc);
	if (ret)
		goto halt_axi_ports;

	ret = q6_rmb_mba_wait(qproc, 0, 5000);
	if (ret == -ETIMEDOUT) {
		dev_err(qproc->dev, "MBA boot timed out\n");
		goto halt_axi_ports;
	} else if (ret != RMB_MBA_XPU_UNLOCKED &&
		   ret != RMB_MBA_XPU_UNLOCKED_SCRIBBLED) {
		dev_err(qproc->dev, "MBA returned unexpected status %d\n", ret);
		ret = -EINVAL;
		goto halt_axi_ports;
	}

	dev_info(qproc->dev, "MBA booted, loading mpss\n");
	ret = q6_mpss_load(qproc);
	if (ret)
		goto halt_axi_ports;
	ret = wait_for_completion_timeout(&qproc->start_done,
					  msecs_to_jiffies(10000));
	if (ret == 0) {
		dev_err(qproc->dev, "start timed out\n");
		ret = -ETIMEDOUT;
		goto halt_axi_ports;
	}

	qproc->running = true;
	/* TODO: All done, release the handover resources */
	q6_proxy_clk_disable(qproc);
	q6_proxy_regulator_disable(qproc);
	mutex_unlock(&qproc->q6_lock);
	return 0;

halt_axi_ports:
	q6v5proc_halt_axi_port(qproc, qproc->halt_map, qproc->halt_q6);
	q6v5proc_halt_axi_port(qproc, qproc->halt_map, qproc->halt_modem);
	q6v5proc_halt_axi_port(qproc, qproc->halt_map, qproc->halt_nc);
err_deassert:
	q6_active_clk_disable(qproc);
err_active_clks:
	q6_proxy_clk_disable(qproc);
err_proxy_clk:
	q6_regulator_disable(qproc);
	mutex_unlock(&qproc->q6_lock);
	return ret;
}

static int q6_stop(struct rproc *rproc)
{
	struct q6v5 *qproc = (struct q6v5 *)rproc->priv;
	int ret;
	u64 val;

	mutex_lock(&qproc->q6_lock);
	qcom_smem_state_update_bits(qproc->state,
				    BIT(qproc->stop_bit), BIT(qproc->stop_bit));

	ret = wait_for_completion_timeout(&qproc->stop_done,
					  msecs_to_jiffies(5000));
	if (ret == 0)
		dev_err(qproc->dev, "timed out on wait\n");

	qcom_smem_state_update_bits(qproc->state, BIT(qproc->stop_bit), 0);

	q6v5proc_halt_axi_port(qproc, qproc->halt_map, qproc->halt_q6);
	q6v5proc_halt_axi_port(qproc, qproc->halt_map, qproc->halt_modem);
	q6v5proc_halt_axi_port(qproc, qproc->halt_map, qproc->halt_nc);

	if (!strcmp(qproc->q6_rproc_res->q6_version, "v56")) {
		/*
		 * Assert QDSP6 I/O clamp, memory wordline clamp, and compiler
		 * memory clamp as a software workaround to avoid high MX
		 * current during LPASS/MSS restart.
		 */

		val = readl_relaxed(qproc->reg_base + QDSP6SS_PWR_CTL_REG);
		val |= (Q6SS_CLAMP_IO | QDSP6v56_CLAMP_WL |
				QDSP6v56_CLAMP_QMC_MEM);
		writel_relaxed(val, qproc->reg_base + QDSP6SS_PWR_CTL_REG);
		pil_mss_restart_reg(qproc, 1);
	} else
		reset_control_assert(qproc->mss_restart);
	q6_active_clk_disable(qproc);
	q6_proxy_clk_disable(qproc);
	q6_proxy_regulator_disable(qproc);
	q6_active_regulator_disable(qproc);
	qproc->running = false;
	mutex_unlock(&qproc->q6_lock);
	return 0;
}

static void *q6_da_to_va(struct rproc *rproc, u64 da, int len)
{
	struct q6v5 *qproc = rproc->priv;
	int offset;

	offset = da - qproc->mpss_reloc;
	if (offset < 0 || offset + len > qproc->mpss_size)
		return NULL;

	return qproc->mpss_region + offset;
}

static const struct rproc_ops q6_ops = {
	.start = q6_start,
	.stop = q6_stop,
	.da_to_va = q6_da_to_va,
};

static irqreturn_t q6_wdog_interrupt(int irq, void *dev)
{
	struct q6v5 *qproc = dev;
	size_t len;
	char *msg;

	/* Sometimes the stop triggers a watchdog rather than a stop-ack */
	if (!qproc->running) {
		complete(&qproc->stop_done);
		return IRQ_HANDLED;
	}

	msg = qcom_smem_get(QCOM_SMEM_HOST_ANY, MPSS_CRASH_REASON_SMEM, &len);
	if (!IS_ERR(msg) && len > 0 && msg[0])
		dev_err(qproc->dev, "watchdog received: %s\n", msg);
	else
		dev_err(qproc->dev, "watchdog without message\n");

	rproc_report_crash(qproc->rproc, RPROC_WATCHDOG);

	if (!IS_ERR(msg))
		msg[0] = '\0';

	return IRQ_HANDLED;
}

static irqreturn_t q6_fatal_interrupt(int irq, void *dev)
{
	struct q6v5 *qproc = dev;
	size_t len;
	char *msg;

	msg = qcom_smem_get(QCOM_SMEM_HOST_ANY, MPSS_CRASH_REASON_SMEM, &len);
	if (!IS_ERR(msg) && len > 0 && msg[0])
		dev_err(qproc->dev, "fatal error received: %s\n", msg);
	else
		dev_err(qproc->dev, "fatal error without message\n");

	rproc_report_crash(qproc->rproc, RPROC_FATAL_ERROR);

	if (!IS_ERR(msg))
		msg[0] = '\0';

	return IRQ_HANDLED;
}

static irqreturn_t q6_handover_interrupt(int irq, void *dev)
{
	struct q6v5 *qproc = dev;

	complete(&qproc->start_done);
	return IRQ_HANDLED;
}

static irqreturn_t q6_stop_ack_interrupt(int irq, void *dev)
{
	struct q6v5 *qproc = dev;

	complete(&qproc->stop_done);
	return IRQ_HANDLED;
}

static int q6_init_mem(struct q6v5 *qproc, struct platform_device *pdev)
{
	struct of_phandle_args args;
	struct resource *res;
	int ret;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "qdsp6");
	qproc->reg_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(qproc->reg_base))
		return PTR_ERR(qproc->reg_base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "rmb");
	qproc->rmb_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(qproc->rmb_base))
		return PTR_ERR(qproc->rmb_base);

	ret = of_parse_phandle_with_fixed_args(pdev->dev.of_node,
					       "qcom,halt-regs", 3, 0, &args);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to parse qcom,halt-regs\n");
		return -EINVAL;
	}

	qproc->halt_map = syscon_node_to_regmap(args.np);
	of_node_put(args.np);
	if (IS_ERR(qproc->halt_map))
		return PTR_ERR(qproc->halt_map);

	qproc->halt_q6 = args.args[0];
	qproc->halt_modem = args.args[1];
	qproc->halt_nc = args.args[2];

	return 0;
}

static int q6_init_clocks(struct q6v5 *qproc)
{
	struct clk **clk_arr;
	int i;

	if (qproc->q6_rproc_res->proxy_clk_cnt) {
		clk_arr = devm_kzalloc(qproc->dev,
		sizeof(clk_arr) * qproc->q6_rproc_res->proxy_clk_cnt,
		GFP_KERNEL);

		for (i = 0; i < qproc->q6_rproc_res->proxy_clk_cnt; i++) {
			clk_arr[i] = devm_clk_get(qproc->dev,
			qproc->q6_rproc_res->proxy_clks[i]);

			if (IS_ERR(clk_arr[i])) {
				dev_err(qproc->dev, "failed to get %s clock\n",
				qproc->q6_rproc_res->proxy_clks[i]);
				return PTR_ERR(clk_arr[i]);
			}
			qproc->proxy_clks = clk_arr;
		}
	}

	if (qproc->q6_rproc_res->active_clk_cnt) {
		clk_arr = devm_kzalloc(qproc->dev,
		sizeof(clk_arr) * qproc->q6_rproc_res->proxy_clk_cnt,
		GFP_KERNEL);

		for (i = 0; i < qproc->q6_rproc_res->active_clk_cnt; i++) {
			clk_arr[i] = devm_clk_get(qproc->dev,
			qproc->q6_rproc_res->active_clks[i]);
			if (IS_ERR(clk_arr[i])) {
				dev_err(qproc->dev, "failed to get %s clock\n",
				qproc->q6_rproc_res->active_clks[i]);
				return PTR_ERR(clk_arr[i]);
			}

			qproc->active_clks = clk_arr;
		}
	}

	return 0;
}

static int q6v5_init_reset(void *q, void *p)
{
	struct q6v5 *qproc = q;
	struct platform_device *pdev = p;

	qproc->mss_restart = devm_reset_control_get(&pdev->dev, NULL);
	if (IS_ERR(qproc->mss_restart)) {
		dev_err(&pdev->dev, "failed to acquire mss restart\n");
		return PTR_ERR(qproc->mss_restart);
	}

	return 0;
}

static int q6v56_init_reset(void *q, void *p)
{
	struct resource *res;
	struct q6v5 *qproc = q;
	struct platform_device *pdev = p;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "restart_reg");
	qproc->restart_reg = devm_ioremap(qproc->dev, res->start,
							resource_size(res));
	if (IS_ERR(qproc->restart_reg)) {
		dev_err(qproc->dev, "failed to get restart_reg\n");
		return PTR_ERR(qproc->restart_reg);
	}

	return 0;
}

static int q6_request_irq(struct q6v5 *qproc,
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

static int q6_alloc_memory_region(struct q6v5 *qproc)
{
	struct device_node *child;
	struct device_node *node;
	struct resource r;
	int ret;

	child = of_get_child_by_name(qproc->dev->of_node, "mba");
	node = of_parse_phandle(child, "memory-region", 0);
	ret = of_address_to_resource(node, 0, &r);
	if (ret) {
		dev_err(qproc->dev, "unable to resolve mba region\n");
		return ret;
	}

	qproc->mba_phys = r.start;
	qproc->mba_size = resource_size(&r);
	qproc->mba_region = devm_ioremap_wc(qproc->dev, qproc->mba_phys, qproc->mba_size);
	if (!qproc->mba_region) {
		dev_err(qproc->dev, "unable to map memory region: %pa+%zx\n",
			&r.start, qproc->mba_size);
		return -EBUSY;
	}

	child = of_get_child_by_name(qproc->dev->of_node, "mpss");
	node = of_parse_phandle(child, "memory-region", 0);
	ret = of_address_to_resource(node, 0, &r);
	if (ret) {
		dev_err(qproc->dev, "unable to resolve mpss region\n");
		return ret;
	}

	qproc->mpss_phys = qproc->mpss_reloc = r.start;
	qproc->mpss_size = resource_size(&r);
	qproc->mpss_region = devm_ioremap_wc(qproc->dev, qproc->mpss_phys, qproc->mpss_size);
	if (!qproc->mpss_region) {
		dev_err(qproc->dev, "unable to map memory region: %pa+%zx\n",
			&r.start, qproc->mpss_size);
		return -EBUSY;
	}

	return 0;
}

static int q6_probe(struct platform_device *pdev)
{
	struct q6v5 *qproc;
	struct rproc *rproc;
	const struct q6_rproc_res *desc;
	int ret;

	desc = of_device_get_match_data(&pdev->dev);
	if (!desc)
		return -EINVAL;

	rproc = rproc_alloc(&pdev->dev, pdev->name, &q6_ops,
			    desc->q6_mba_image, sizeof(*qproc));
	if (!rproc) {
		dev_err(&pdev->dev, "failed to allocate rproc\n");
		return -ENOMEM;
	}

	rproc->fw_ops = &q6_fw_ops;

	qproc = (struct q6v5 *)rproc->priv;
	qproc->dev = &pdev->dev;
	qproc->rproc = rproc;
	platform_set_drvdata(pdev, qproc);

	init_completion(&qproc->start_done);
	init_completion(&qproc->stop_done);

	qproc->q6_rproc_res = desc;

	ret = q6_init_mem(qproc, pdev);
	if (ret)
		goto free_rproc;

	ret = q6_alloc_memory_region(qproc);
	if (ret)
		goto free_rproc;

	ret = q6_init_clocks(qproc);
	if (ret)
		goto free_rproc;

	ret = qproc->q6_rproc_res->q6_reset_init(qproc, pdev);
	if (ret)
		goto free_rproc;

	ret = q6_regulator_init(qproc);
	if (ret)
		goto free_rproc;

	mutex_init(&qproc->q6_lock);

	ret = q6_request_irq(qproc, pdev, "wdog", q6_wdog_interrupt);
	if (ret < 0)
		goto free_rproc;

	ret = q6_request_irq(qproc, pdev, "fatal", q6_fatal_interrupt);
	if (ret < 0)
		goto free_rproc;

	ret = q6_request_irq(qproc, pdev, "handover", q6_handover_interrupt);
	if (ret < 0)
		goto free_rproc;

	ret = q6_request_irq(qproc, pdev, "stop-ack", q6_stop_ack_interrupt);
	if (ret < 0)
		goto free_rproc;

	qproc->state = qcom_smem_state_get(&pdev->dev, "stop", &qproc->stop_bit);
	if (IS_ERR(qproc->state)) {
		ret = PTR_ERR(qproc->state);
		goto free_rproc;
	}

	ret = rproc_add(rproc);
	if (ret)
		goto free_rproc;

	return 0;

free_rproc:
	rproc_free(rproc);

	return ret;
}

static int q6_remove(struct platform_device *pdev)
{
	struct q6v5 *qproc = platform_get_drvdata(pdev);

	rproc_del(qproc->rproc);
	rproc_free(qproc->rproc);

	return 0;
}

char *proxy_8x96_reg_str[] = {"mx", "cx", "vdd_pll"};
int  proxy_8x96_reg_action[3][2] = { {0, 1}, {1, 1}, {1, 0} };
int  proxy_8x96_reg_load[] = {0, 100000, 100000};
int  proxy_8x96_reg_min_voltage[] = {1050000, 1250000, 0};
char *proxy_8x96_clk_str[] = {"xo", "pnoc", "qdss"};
char *active_8x96_clk_str[] = {"iface", "bus", "mem", "gpll0_mss_clk",
		"snoc_axi_clk", "mnoc_axi_clk"};

static const struct q6_rproc_res msm_8996_res = {
	.proxy_clks = proxy_8x96_clk_str,
	.proxy_clk_cnt = 3,
	.active_clks = active_8x96_clk_str,
	.active_clk_cnt = 6,
	.proxy_regs = proxy_8x96_reg_str,
	.active_regs = NULL,
	.proxy_reg_action = (int **)proxy_8x96_reg_action,
	.proxy_reg_load = (int *)proxy_8x96_reg_load,
	.active_reg_action = NULL,
	.active_reg_load = NULL,
	.proxy_reg_voltage = (int *)proxy_8x96_reg_min_voltage,
	.active_reg_voltage = NULL,
	.proxy_reg_cnt = 3,
	.active_reg_cnt = 0,
	.q6_reset_init = q6v56_init_reset,
	.q6_version = "v56",
	.q6_mba_image = "mba.mbn",
};

char *proxy_8x16_reg_str[] = {"mx", "cx", "pll"};
char *active_8x16_reg_str[] = {"mss"};
int  proxy_8x16_reg_action[4][2] = { {0, 1}, {1, 0}, {1, 0} };
int  active_8x16_reg_action[1][2] = { {1, 1} };
int  proxy_8x16_reg_load[] = {100000, 0, 100000, 100000};
int  active_8x16_reg_load[] = {100000};
int  proxy_8x16_reg_min_voltage[] = {1050000, 0, 0};
int  active_8x16_reg_min_voltage[] = {1000000};
char *proxy_8x16_clk_str[] = {"xo"};
char *active_8x16_clk_str[] = {"iface", "bus", "mem"};

static const struct q6_rproc_res msm_8916_res = {
	.proxy_clks = proxy_8x16_clk_str,
	.proxy_clk_cnt = 1,
	.active_clks = active_8x16_clk_str,
	.active_clk_cnt = 3,
	.proxy_regs = proxy_8x16_reg_str,
	.active_regs = active_8x16_reg_str,
	.proxy_reg_action = (int **)proxy_8x16_reg_action,
	.proxy_reg_load = (int *)proxy_8x16_reg_load,
	.active_reg_action = (int **)active_8x16_reg_action,
	.active_reg_load = (int *)active_8x16_reg_load,
	.proxy_reg_voltage = (int *)proxy_8x16_reg_min_voltage,
	.active_reg_voltage = active_8x16_reg_min_voltage,
	.proxy_reg_cnt = 3,
	.active_reg_cnt = 1,
	.q6_reset_init = q6v5_init_reset,
	.q6_version = "v5",
	.q6_mba_image = "mba.b00",
};

static const struct of_device_id q6_of_match[] = {
	{ .compatible = "qcom,q6v5-pil", .data = &msm_8916_res},
	{ .compatible = "qcom,q6v56-pil", .data = &msm_8996_res},
	{ },
};

static struct platform_driver q6_driver = {
	.probe = q6_probe,
	.remove = q6_remove,
	.driver = {
		.name = "qcom-q6v5-pil",
		.of_match_table = q6_of_match,
	},
};
module_platform_driver(q6_driver);

MODULE_DESCRIPTION("Peripheral Image Loader for Hexagon");
MODULE_LICENSE("GPL v2");
