/*
 * Copyright (C) STMicroelectronics SA 2015
 * Authors: Yannick Fertre <yannick.fertre@st.com>
 *          Hugues Fruchet <hugues.fruchet@st.com>
 * License terms:  GNU General Public License (GPL), version 2
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/time.h>

#include "hva.h"
#include "hva-hw.h"

/* HVA register offsets */
#define HVA_HIF_REG_RST                 0x0100U
#define HVA_HIF_REG_RST_ACK             0x0104U
#define HVA_HIF_REG_MIF_CFG             0x0108U
#define HVA_HIF_REG_HEC_MIF_CFG         0x010CU
#define HVA_HIF_REG_CFL                 0x0110U
#define HVA_HIF_FIFO_CMD                0x0114U
#define HVA_HIF_FIFO_STS                0x0118U
#define HVA_HIF_REG_SFL                 0x011CU
#define HVA_HIF_REG_IT_ACK              0x0120U
#define HVA_HIF_REG_ERR_IT_ACK          0x0124U
#define HVA_HIF_REG_LMI_ERR             0x0128U
#define HVA_HIF_REG_EMI_ERR             0x012CU
#define HVA_HIF_REG_HEC_MIF_ERR         0x0130U
#define HVA_HIF_REG_HEC_STS             0x0134U
#define HVA_HIF_REG_HVC_STS             0x0138U
#define HVA_HIF_REG_HJE_STS             0x013CU
#define HVA_HIF_REG_CNT                 0x0140U
#define HVA_HIF_REG_HEC_CHKSYN_DIS      0x0144U
#define HVA_HIF_REG_CLK_GATING          0x0148U
#define HVA_HIF_REG_VERSION             0x014CU
#define HVA_HIF_REG_BSM                 0x0150U

/* define value for version id register (HVA_HIF_REG_VERSION) */
#define VERSION_ID_MASK	0x0000FFFF

/* define values for BSM register (HVA_HIF_REG_BSM) */
#define BSM_CFG_VAL1	0x0003F000
#define BSM_CFG_VAL2	0x003F0000

/* define values for memory interface register (HVA_HIF_REG_MIF_CFG) */
#define MIF_CFG_VAL1	0x04460446
#define MIF_CFG_VAL2	0x04460806
#define MIF_CFG_VAL3	0x00000000

/* define value for HEC memory interface register (HVA_HIF_REG_MIF_CFG) */
#define HEC_MIF_CFG_VAL	0x000000C4

/*  Bits definition for clock gating register (HVA_HIF_REG_CLK_GATING) */
#define CLK_GATING_HVC	BIT(0)
#define CLK_GATING_HEC	BIT(1)
#define CLK_GATING_HJE	BIT(2)

/* fix hva clock rate */
#define CLK_RATE		300000000

/* fix delay for pmruntime */
#define AUTOSUSPEND_DELAY_MS	3

static irqreturn_t hva_hw_its_interrupt(int irq, void *data)
{
	struct hva_device *hva = data;

	/* read status registers */
	hva->sts_reg = readl_relaxed(hva->regs + HVA_HIF_FIFO_STS);
	hva->sfl_reg = readl_relaxed(hva->regs + HVA_HIF_REG_SFL);

	/* acknowledge interruption */
	writel_relaxed(0x1, hva->regs + HVA_HIF_REG_IT_ACK);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t hva_hw_its_irq_thread(int irq, void *arg)
{
	struct hva_device *hva = arg;
	struct device *dev = hva_to_dev(hva);
	u32 status = hva->sts_reg & 0xFF;
	u8 client_id = (hva->sts_reg & 0xFF00) >> 8;
	struct hva_ctx *ctx = NULL;

	dev_dbg(dev, "%s     %s: status :0x%02x fifo level :0x%02x\n",
		HVA_PREFIX, __func__, hva->sts_reg & 0xFF, hva->sfl_reg & 0xF);

	/* check client ID */
	if (client_id >= MAX_CONTEXT) {
		dev_err(dev, "%s     %s: bad client identifier: %d\n",
			ctx->name, __func__, client_id);
		ctx->encode_errors++;
		ctx->hw_err = true;
		goto out;
	}

	ctx = hva->contexts_list[client_id];

	switch (status) {
	case NO_ERROR:
		dev_dbg(dev, "%s     %s: no error\n",
			ctx->name, __func__);
		ctx->hw_err = false;
		break;
	case H264_SLICE_READY:
		dev_dbg(dev, "%s     %s: h264 slice ready\n",
			ctx->name, __func__);
		ctx->hw_err = false;
		break;
	case H264_FRAME_SKIPPED:
		dev_dbg(dev, "%s     %s: h264 frame skipped\n",
			ctx->name, __func__);
		ctx->hw_err = false;
		break;
	case JPEG_BITSTREAM_OVERSIZE:
		dev_err(dev, "%s     %s:jpeg bitstream oversize\n",
			ctx->name, __func__);
		ctx->encode_errors++;
		ctx->hw_err = true;
		break;
	case H264_BITSTREAM_OVERSIZE:
		dev_err(dev, "%s     %s:h264 bitstream oversize\n",
			ctx->name, __func__);
		ctx->encode_errors++;
		ctx->hw_err = true;
		break;
	case H264_SLICE_LIMIT_SIZE:
		dev_err(dev, "%s     %s: h264 slice limit size is reached\n",
			ctx->name, __func__);
		ctx->encode_errors++;
		ctx->hw_err = true;
		break;
	case H264_MAX_SLICE_NUMBER:
		dev_err(dev, "%s     %s: h264 max slice number is reached\n",
			ctx->name, __func__);
		ctx->encode_errors++;
		ctx->hw_err = true;
		break;
	case TASK_LIST_FULL:
		dev_err(dev, "%s     %s:task list full\n",
			ctx->name, __func__);
		ctx->encode_errors++;
		ctx->hw_err = true;
		break;
	case UNKNOWN_COMMAND:
		dev_err(dev, "%s     %s:command not known\n",
			ctx->name, __func__);
		ctx->encode_errors++;
		ctx->hw_err = true;
		break;
	case WRONG_CODEC_OR_RESOLUTION:
		dev_err(dev, "%s     %s:wrong codec or resolution\n",
			ctx->name, __func__);
		ctx->encode_errors++;
		ctx->hw_err = true;
		break;
	default:
		dev_err(dev, "%s     %s:status not recognized\n",
			ctx->name, __func__);
		ctx->encode_errors++;
		ctx->hw_err = true;
		break;
	}
out:
	complete(&hva->interrupt);

	return IRQ_HANDLED;
}

static irqreturn_t hva_hw_err_interrupt(int irq, void *data)
{
	struct hva_device *hva = data;

	/* read status registers */
	hva->sts_reg = readl_relaxed(hva->regs + HVA_HIF_FIFO_STS);
	hva->sfl_reg = readl_relaxed(hva->regs + HVA_HIF_REG_SFL);

	/* read error registers */
	hva->lmi_err_reg = readl_relaxed(hva->regs + HVA_HIF_REG_LMI_ERR);
	hva->emi_err_reg = readl_relaxed(hva->regs + HVA_HIF_REG_EMI_ERR);
	hva->hec_mif_err_reg = readl_relaxed(hva->regs +
					     HVA_HIF_REG_HEC_MIF_ERR);

	/* acknowledge interruption */
	writel_relaxed(0x1, hva->regs + HVA_HIF_REG_IT_ACK);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t hva_hw_err_irq_thread(int irq, void *arg)
{
	struct hva_device *hva = arg;
	struct device *dev = hva_to_dev(hva);
	u8 client_id = 0;
	struct hva_ctx *ctx;

	dev_dbg(dev, "%s     status :0x%02x fifo level :0x%02x\n",
		HVA_PREFIX, hva->sts_reg & 0xFF, hva->sfl_reg & 0xF);

	/* check client ID */
	client_id = (hva->sts_reg & 0xFF00) >> 8;
	if (client_id >= MAX_CONTEXT) {
		dev_err(dev, "%s     bad client identifier: %d\n", HVA_PREFIX,
			client_id);
		goto out;
	}

	ctx = hva->contexts_list[client_id];

	if (hva->lmi_err_reg) {
		dev_err(dev, "%s     local memory interface error :0x%08x\n",
			ctx->name, hva->lmi_err_reg);
		ctx->encode_errors++;
		ctx->hw_err = true;
	}

	if (hva->lmi_err_reg) {
		dev_err(dev, "%s     external memory iterface error :0x%08x\n",
			ctx->name, hva->emi_err_reg);
		ctx->encode_errors++;
		ctx->hw_err = true;
	}

	if (hva->hec_mif_err_reg) {
		dev_err(dev, "%s     hec memory interface error :0x%08x\n",
			ctx->name, hva->hec_mif_err_reg);
		ctx->encode_errors++;
		ctx->hw_err = true;
	}
out:
	complete(&hva->interrupt);

	return IRQ_HANDLED;
}

static unsigned long int hva_hw_get_chipset_id(struct hva_device *hva)
{
	struct device *dev = hva_to_dev(hva);
	unsigned long int id;

	mutex_lock(&hva->protect_mutex);

	if (pm_runtime_get_sync(dev) < 0) {
		dev_err(dev, "%s     get pm_runtime failed\n", HVA_PREFIX);
		mutex_unlock(&hva->protect_mutex);
		return -EFAULT;
	}

	id = readl_relaxed(hva->regs + HVA_HIF_REG_VERSION) &
			   VERSION_ID_MASK;

	pm_runtime_put_autosuspend(dev);

	mutex_unlock(&hva->protect_mutex);

	switch (id) {
	case HVA_VERSION_V400:
	case HVA_VERSION_V397:
		dev_info(dev, "%s     chipset identifier 0x%lx\n",
			 HVA_PREFIX, id);
		break;
	default:
		dev_err(dev, "%s     unknown chipset identifier 0x%lx\n",
			HVA_PREFIX, id);
		id = HVA_VERSION_UNKNOWN;
		break;
	}

	return id;
}

int hva_hw_probe(struct platform_device *pdev, struct hva_device *hva)
{
	struct device *dev = &pdev->dev;
	struct resource *regs;
	struct resource *esram;
	int irq = 0;
	int ret = 0;

	WARN_ON(!hva);
	hva->pdev = pdev;
	hva->dev = dev;

	/* get a memory region for mmio */
	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	hva->regs = devm_ioremap_resource(dev, regs);
	if (IS_ERR_OR_NULL(hva->regs)) {
		dev_err(dev, "%s     failed to get regs\n", HVA_PREFIX);
		return PTR_ERR(hva->regs);
	}

	/* get a memory region for esram from device tree */
	esram = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (IS_ERR_OR_NULL(esram)) {
		dev_err(dev, "%s     failed to get esram region\n", HVA_PREFIX);
		return PTR_ERR(esram);
	}

	hva->esram_addr = esram->start;
	hva->esram_size = esram->end - esram->start + 1;

	/* get clock resource */
	hva->clk = devm_clk_get(dev, "clk_hva");
	if (IS_ERR(hva->clk)) {
		dev_err(dev, "%s     failed to get clock\n", HVA_PREFIX);
		return PTR_ERR(hva->clk);
	}

	ret = clk_prepare(hva->clk);
	if (ret < 0) {
		dev_err(dev, "%s     failed to prepare clock\n", HVA_PREFIX);
		hva->clk = ERR_PTR(-EINVAL);
		return ret;
	}

	/* retrieve irq number from board resources */
	hva->irq_its = platform_get_irq(pdev, 0);
	if (!hva->irq_its) {
		dev_err(dev, "%s     failed to get IRQ resource\n", HVA_PREFIX);
		goto err_clk;
	}

	/* request irq */
	ret = devm_request_threaded_irq(dev, hva->irq_its, hva_hw_its_interrupt,
					hva_hw_its_irq_thread,
					IRQF_ONESHOT,
					"hva_its_irq", (void *)hva);
	if (ret) {
		dev_err(dev, "%s     failed to register its IRQ 0x%x\n",
			HVA_PREFIX, irq);
		goto err_clk;
	}
	disable_irq(hva->irq_its);

	/* retrieve irq number from board resources */
	hva->irq_err = platform_get_irq(pdev, 1);
	if (!hva->irq_err) {
		dev_err(dev, "%s     failed to get IRQ resource\n", HVA_PREFIX);
		goto err_clk;
	}

	/* request irq */
	ret = devm_request_threaded_irq(dev, hva->irq_err, hva_hw_err_interrupt,
					hva_hw_err_irq_thread,
					IRQF_ONESHOT,
					"hva_err_irq", (void *)hva);
	if (ret) {
		dev_err(dev, "%s     failed to register err IRQ 0x%x\n",
			HVA_PREFIX, irq);
		goto err_clk;
	}
	disable_irq(hva->irq_err);

	/* initialisation of the protection mutex */
	mutex_init(&hva->protect_mutex);

	/* initialisation of completion signal */
	init_completion(&hva->interrupt);

	/* init pm_runtime used for power management */
	pm_runtime_set_autosuspend_delay(dev, AUTOSUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_suspended(dev);
	pm_runtime_enable(dev);

	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err(dev, "%s     failed to set PM\n", HVA_PREFIX);
		goto err_pm;
	}

	/* check hardware ID */
	hva->chip_id = hva_hw_get_chipset_id(hva);

	if (hva->chip_id == HVA_VERSION_UNKNOWN) {
		ret = -EINVAL;
		goto err_pm;
	}

	dev_info(dev, "%s     found hva device (id=%lx)\n", HVA_PREFIX,
		 hva->chip_id);

	return 0;
err_pm:
	pm_runtime_put(dev);
err_clk:
	if (hva->clk)
		clk_unprepare(hva->clk);

	return ret;
}

void hva_hw_remove(struct hva_device *hva)
{
	struct device *dev = hva_to_dev(hva);

	/* disable interrupt */
	disable_irq(hva->irq_its);
	disable_irq(hva->irq_err);

	pm_runtime_put_autosuspend(dev);
	pm_runtime_disable(dev);
}

int hva_hw_runtime_suspend(struct device *dev)
{
	struct hva_device *hva = dev_get_drvdata(dev);

	clk_disable_unprepare(hva->clk);

	return 0;
}

int hva_hw_runtime_resume(struct device *dev)
{
	struct hva_device *hva = dev_get_drvdata(dev);

	if (clk_prepare_enable(hva->clk)) {
		dev_err(hva->dev, "%s     failed to prepare hva clk\n",
			HVA_PREFIX);
		return -EINVAL;
	}

	if (clk_set_rate(hva->clk, CLK_RATE)) {
		dev_err(dev, "%s     failed to set clock frequency\n",
			HVA_PREFIX);
		return -EINVAL;
	}

	return 0;
}

int hva_hw_execute_task(struct hva_ctx *ctx, enum hva_hw_cmd_type cmd,
			struct hva_buffer *task)
{
	struct hva_device *hva = ctx_to_hdev(ctx);
	struct device *dev = hva_to_dev(hva);
	unsigned long int version = 0;
	u8 client_id = ctx->client_id;
	u32 reg = 0;

	mutex_lock(&hva->protect_mutex);

	/* enable irqs */
	enable_irq(hva->irq_its);
	enable_irq(hva->irq_err);

	if (pm_runtime_get_sync(dev) < 0) {
		dev_err(dev, "%s     get pm_runtime failed\n", ctx->name);
		ctx->sys_errors++;
		goto out;
	}

	version = readl_relaxed(hva->regs + HVA_HIF_REG_VERSION) &
		  VERSION_ID_MASK;

	reg = readl_relaxed(hva->regs + HVA_HIF_REG_CLK_GATING);
	switch (cmd) {
	case JPEG_ENC:
		reg |= CLK_GATING_HJE;
		break;
	case H264_ENC:
	case VP8_ENC:
		reg |= CLK_GATING_HVC;
		break;
	default:
		dev_warn(dev, "%s     unknown command 0x%x\n", ctx->name, cmd);
		goto out;
	}
	writel_relaxed(reg, hva->regs + HVA_HIF_REG_CLK_GATING);

	dev_dbg(dev, "%s     %s: Write configuration registers\n", ctx->name,
		__func__);

	/* byte swap config */
	switch (version) {
	case HVA_VERSION_V397:
	case HVA_VERSION_V400:
		writel_relaxed(BSM_CFG_VAL1,
			       hva->regs + HVA_HIF_REG_BSM);
		break;
	default:
		dev_err(dev, "%s     unknown chipset identifier 0x%lx\n",
			ctx->name, version);
		ctx->sys_errors++;
		goto out;
	}

	/*
	 * define Max Opcode Size and Max Message Size
	 * for LMI and EMI
	 */
	switch (version) {
	case HVA_VERSION_V397:
	case HVA_VERSION_V400:
		writel_relaxed(MIF_CFG_VAL3,
			       hva->regs + HVA_HIF_REG_MIF_CFG);
		writel_relaxed(HEC_MIF_CFG_VAL,
			       hva->regs + HVA_HIF_REG_HEC_MIF_CFG);
		break;
	default:
		/* do nothing */
		break;
	}

	/* command FIFO: task_id[31:16] client_id[15:8] command_type[7:0] */
	dev_dbg(dev, "%s     %s: Send task ( cmd:%d, task_desc:0x%x)\n",
		ctx->name, __func__, cmd + (client_id << 8), task->paddr);
	writel_relaxed(cmd + (client_id << 8), hva->regs + HVA_HIF_FIFO_CMD);
	writel_relaxed(task->paddr, hva->regs + HVA_HIF_FIFO_CMD);

	if (!wait_for_completion_timeout(&hva->interrupt,
					 msecs_to_jiffies(2000))) {
		dev_err(dev, "%s     %s:Time out on completion\n", ctx->name,
			__func__);
		ctx->encode_errors++;
		ctx->hw_err = true;
	}
out:
	disable_irq(hva->irq_its);
	disable_irq(hva->irq_err);

	switch (cmd) {
	case JPEG_ENC:
		reg &= ~CLK_GATING_HJE;
		writel_relaxed(reg, hva->regs + HVA_HIF_REG_CLK_GATING);
		break;
	case H264_ENC:
	case VP8_ENC:
		reg &= ~CLK_GATING_HVC;
		writel_relaxed(reg, hva->regs + HVA_HIF_REG_CLK_GATING);

		break;
	default:
		dev_warn(dev, "%s     unknown command 0x%x\n", ctx->name, cmd);
	}

	pm_runtime_put_autosuspend(dev);
	mutex_unlock(&hva->protect_mutex);

	if (ctx->hw_err)
		return -EFAULT;

	return 0;
}
