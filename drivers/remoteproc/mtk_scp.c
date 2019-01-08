// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2018 MediaTek Inc.

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_data/mtk_scp.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>

#include "mtk_common.h"
#include "remoteproc_internal.h"

#define MAX_CODE_SIZE 0x500000

struct platform_device *scp_get_plat_device(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *scp_node;
	struct platform_device *scp_pdev;

	scp_node = of_parse_phandle(dev->of_node, "mediatek,scp", 0);
	if (!scp_node) {
		dev_err(dev, "can't get scp node\n");
		return NULL;
	}

	scp_pdev = of_find_device_by_node(scp_node);
	if (WARN_ON(!scp_pdev)) {
		dev_err(dev, "scp pdev failed\n");
		of_node_put(scp_node);
		return NULL;
	}

	return scp_pdev;
}
EXPORT_SYMBOL_GPL(scp_get_plat_device);

static void scp_wdt_handler(struct mtk_scp *scp)
{
	rproc_report_crash(scp->rproc, RPROC_WATCHDOG);
}

static void scp_init_ipi_handler(void *data, unsigned int len, void *priv)
{
	struct mtk_scp *scp = (struct mtk_scp *)priv;
	struct scp_run *run = (struct scp_run *)data;

	scp->run.signaled = run->signaled;
	strncpy(scp->run.fw_ver, run->fw_ver, SCP_FW_VER_LEN);
	scp->run.dec_capability = run->dec_capability;
	scp->run.enc_capability = run->enc_capability;
	wake_up_interruptible(&scp->run.wq);
}

static void scp_ipi_handler(struct mtk_scp *scp)
{
	struct share_obj *rcv_obj = scp->recv_buf;
	struct scp_ipi_desc *ipi_desc = scp->ipi_desc;
	u8 tmp_data[288];

	if (rcv_obj->id < SCP_IPI_MAX && ipi_desc[rcv_obj->id].handler) {
		memcpy_fromio(tmp_data, &rcv_obj->share_buf, rcv_obj->len);
		ipi_desc[rcv_obj->id].handler(&tmp_data[0],
					      rcv_obj->len,
					      ipi_desc[rcv_obj->id].priv);
		scp->ipi_id_ack[rcv_obj->id] = true;
		wake_up(&scp->ack_wq);
	} else {
		dev_err(scp->dev, "No such ipi id = %d\n", rcv_obj->id);
	}
}

static int scp_ipi_init(struct mtk_scp *scp)
{
	size_t send_offset = 0x800 - sizeof(struct share_obj);
	size_t recv_offset = send_offset - sizeof(struct share_obj);

	/* Disable SCP to host interrupt */
	writel(MT8183_SCP_IPC_INT_BIT, scp->reg_base + MT8183_SCP_TO_HOST);

	/* shared buffer initialization */
	scp->recv_buf = (__force struct share_obj *)(scp->sram_base +
						recv_offset);
	scp->send_buf = (__force struct share_obj *)(scp->sram_base +
						send_offset);
	memset_io(scp->recv_buf, 0, sizeof(scp->recv_buf));
	memset_io(scp->send_buf, 0, sizeof(scp->send_buf));

	return 0;
}

static void mtk_scp_reset_assert(const struct mtk_scp *scp)
{
	u32 val;

	val = readl(scp->reg_base + MT8183_SW_RSTN);
	val &= ~MT8183_SW_RSTN_BIT;
	writel(val, scp->reg_base + MT8183_SW_RSTN);
}

static void mtk_scp_reset_deassert(const struct mtk_scp *scp)
{
	u32 val;

	val = readl(scp->reg_base + MT8183_SW_RSTN);
	val |= MT8183_SW_RSTN_BIT;
	writel(val, scp->reg_base + MT8183_SW_RSTN);
}

static irqreturn_t scp_irq_handler(int irq, void *priv)
{
	struct mtk_scp *scp = priv;
	u32 scp_to_host;

	scp_to_host = readl(scp->reg_base + MT8183_SCP_TO_HOST);
	if (scp_to_host & MT8183_SCP_IPC_INT_BIT) {
		scp_ipi_handler(scp);
	} else {
		dev_err(scp->dev, "scp watchdog timeout! 0x%x", scp_to_host);
		scp_wdt_handler(scp);
	}

	/* SCP won't send another interrupt until we set SCP_TO_HOST to 0. */
	writel(MT8183_SCP_IPC_INT_BIT | MT8183_SCP_WDT_INT_BIT,
	       scp->reg_base + MT8183_SCP_TO_HOST);

	return IRQ_HANDLED;
}

static int mtk_scp_load(struct rproc *rproc, const struct firmware *fw)
{
	const struct mtk_scp *scp = rproc->priv;
	struct device *dev = scp->dev;
	int ret;

	/* Hold SCP in reset while loading FW. */
	mtk_scp_reset_assert(scp);

	ret = clk_prepare_enable(scp->clk);
	if (ret) {
		dev_err(dev, "failed to enable clocks\n");
		return ret;
	}

	writel(0x0, scp->reg_base + MT8183_SCP_SRAM_PDN);

	memcpy(scp->sram_base, fw->data, fw->size);
	return ret;
}

static int mtk_scp_start(struct rproc *rproc)
{
	struct mtk_scp *scp = (struct mtk_scp *)rproc->priv;
	struct device *dev = scp->dev;
	struct scp_run *run;
	int ret;

	ret = clk_prepare_enable(scp->clk);
	if (ret) {
		dev_err(dev, "failed to enable clocks\n");
		return ret;
	}

	mtk_scp_reset_deassert(scp);

	run = &scp->run;

	ret = wait_event_interruptible_timeout(
					run->wq,
					run->signaled,
					msecs_to_jiffies(2000));

	if (ret == 0) {
		dev_err(dev, "wait scp initialization timeout!\n");
		ret = -ETIME;
		goto stop;
	}
	if (ret == -ERESTARTSYS) {
		dev_err(dev, "wait scp interrupted by a signal!\n");
		goto stop;
	}
	dev_info(dev, "scp is ready. Fw version %s\n", run->fw_ver);

	return 0;

stop:
	mtk_scp_reset_assert(scp);
	clk_disable_unprepare(scp->clk);
	return ret;
}

static void *mtk_scp_da_to_va(struct rproc *rproc, u64 da, int len)
{
	struct mtk_scp *scp = (struct mtk_scp *)rproc->priv;
	int offset;

	if (da < scp->sram_size) {
		offset = da;
		if (offset >= 0 && ((offset + len) < scp->sram_size)) {
			return (__force void *)(scp->sram_base + offset);
		}
	} else if (da >= scp->sram_size && da < (scp->sram_size + MAX_CODE_SIZE)) {
		offset = da - scp->sram_size;
		if (offset >= 0 && (offset + len) < MAX_CODE_SIZE) {
			return scp->cpu_addr + offset;
		}
	} else {
		offset = da - scp->phys_addr;
		if (offset >= 0 && (offset + len) < (scp->dram_size - MAX_CODE_SIZE)) {
			return scp->cpu_addr + offset;
		}
	}

	return NULL;
}

static int mtk_scp_stop(struct rproc *rproc)
{
	struct mtk_scp *scp = (struct mtk_scp *)rproc->priv;

	mtk_scp_reset_assert(scp);
	clk_disable_unprepare(scp->clk);

	return 0;
}

static const struct rproc_ops mtk_scp_ops = {
	.start		= mtk_scp_start,
	.stop		= mtk_scp_stop,
	.load		= mtk_scp_load,
	.da_to_va	= mtk_scp_da_to_va,
};

unsigned int scp_get_vdec_hw_capa(struct platform_device *pdev)
{
	struct mtk_scp *scp = platform_get_drvdata(pdev);

	return scp->run.dec_capability;
}
EXPORT_SYMBOL_GPL(scp_get_vdec_hw_capa);

unsigned int scp_get_venc_hw_capa(struct platform_device *pdev)
{
	struct mtk_scp *scp = platform_get_drvdata(pdev);

	return scp->run.enc_capability;
}
EXPORT_SYMBOL_GPL(scp_get_venc_hw_capa);

void *scp_mapping_dm_addr(struct platform_device *pdev, u32 mem_addr)
{
	struct mtk_scp *scp = platform_get_drvdata(pdev);
	void *ptr = NULL;

	ptr = mtk_scp_da_to_va(scp->rproc, mem_addr, 0);

	if (ptr)
		return ptr;
	else
		return ERR_PTR(-EINVAL);
}
EXPORT_SYMBOL_GPL(scp_mapping_dm_addr);

static int scp_map_memory_region(struct mtk_scp *scp)
{
	struct device_node *node;
	struct resource r;
	int ret;

	node = of_parse_phandle(scp->dev->of_node, "memory-region", 0);
	if (!node) {
		dev_err(scp->dev, "no memory-region specified\n");
		return -EINVAL;
	}

	ret = of_address_to_resource(node, 0, &r);
	if (ret)
		return ret;

	scp->phys_addr = r.start;
	scp->dram_size = resource_size(&r);
	scp->cpu_addr = devm_ioremap_wc(scp->dev, scp->phys_addr, scp->dram_size);

	if (!scp->cpu_addr) {
		dev_err(scp->dev, "unable to map memory region: %pa+%zx\n",
			&r.start, scp->dram_size);
		return -EBUSY;
	}

	return 0;
}

static int mtk_scp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct mtk_scp *scp;
	struct rproc *rproc;
	struct resource *res;
	char *fw_name = "scp.img";
	int ret;

	rproc = rproc_alloc(dev,
			    np->name,
			    &mtk_scp_ops,
			    fw_name,
			    sizeof(*scp));
	if (!rproc) {
		dev_err(dev, "unable to allocate remoteproc\n");
		return -ENOMEM;
	}

	scp = (struct mtk_scp *)rproc->priv;
	scp->rproc = rproc;
	scp->dev = dev;
	platform_set_drvdata(pdev, scp);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sram");
	scp->sram_base = devm_ioremap_resource(dev, res);
	scp->sram_size = resource_size(res);
	if (IS_ERR((__force void *)scp->sram_base)) {
		dev_err(dev, "Failed to parse and map sram memory\n");
		ret = PTR_ERR((__force void *)scp->sram_base);
		goto free_rproc;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg");
	scp->reg_base = devm_ioremap_resource(dev, res);
	if (IS_ERR((__force void *)scp->reg_base)) {
		dev_err(dev, "Failed to parse and map cfg memory\n");
		ret = PTR_ERR((__force void *)scp->reg_base);
		goto free_rproc;
	}

	ret = scp_map_memory_region(scp);
	if (ret)
		goto free_rproc;

	scp->clk = devm_clk_get(dev, "main");
	if (IS_ERR(scp->clk)) {
		dev_err(dev, "Failed to get clock\n");
		ret = PTR_ERR(scp->clk);
		goto free_rproc;
	}

	ret = clk_prepare_enable(scp->clk);
	if (ret) {
		dev_err(dev, "failed to enable clocks\n");
		goto free_rproc;
	}

	ret = scp_ipi_init(scp);
	clk_disable_unprepare(scp->clk);
	if (ret) {
		dev_err(dev, "Failed to init ipi\n");
		goto free_rproc;
	}

	/* register scp initialization IPI */
	ret = scp_ipi_register(pdev,
			       SCP_IPI_INIT,
			       scp_init_ipi_handler,
			       "scp_init",
			       scp);
	if (ret) {
		dev_err(dev, "Failed to register IPI_SCP_INIT\n");
		goto free_rproc;
	}

	ret = devm_request_irq(dev,
			       platform_get_irq(pdev, 0),
			       scp_irq_handler,
			       0,
			       pdev->name,
			       scp);

	if (ret) {
		dev_err(dev, "failed to request irq\n");
		goto free_rproc;
	}

	mutex_init(&scp->scp_mutex);

	init_waitqueue_head(&scp->run.wq);
	init_waitqueue_head(&scp->ack_wq);

	ret = rproc_add(rproc);
	if (ret)
		goto destroy_mutex;

	return ret;

destroy_mutex:
	mutex_destroy(&scp->scp_mutex);
free_rproc:
	rproc_free(rproc);

	return ret;
}

static int mtk_scp_remove(struct platform_device *pdev)
{
	struct mtk_scp *scp = platform_get_drvdata(pdev);

	rproc_del(scp->rproc);
	rproc_free(scp->rproc);

	return 0;
}

static const struct of_device_id mtk_scp_of_match[] = {
	{ .compatible = "mediatek,mt8183-scp"},
	{},
};
MODULE_DEVICE_TABLE(of, mtk_scp_of_match);

static struct platform_driver mtk_scp_driver = {
	.probe = mtk_scp_probe,
	.remove = mtk_scp_remove,
	.driver = {
		.name = "mtk-scp",
		.of_match_table = of_match_ptr(mtk_scp_of_match),
	},
};

module_platform_driver(mtk_scp_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MediaTek scp control driver");
