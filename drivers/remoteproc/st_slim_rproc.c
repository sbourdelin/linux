/*
 * st_slim_rproc.c
 *
 * Copyright (C) 2016 STMicroelectronics
 * Author: Peter Griffin <peter.griffin@linaro.org>
 * License terms:  GNU General Public License (GPL), version 2
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/remoteproc/st_slim_rproc.h>
#include "remoteproc_internal.h"

/* slimcore registers */
#define SLIM_ID_OFST		0x0
#define SLIM_VER_OFST		0x4

#define SLIM_EN_OFST		0x8
#define SLIM_EN_RUN			BIT(0)

#define SLIM_CLK_GATE_OFST	0xC
#define SLIM_CLK_GATE_DIS		BIT(0)
#define SLIM_CLK_GATE_RESET		BIT(2)

#define SLIM_SLIM_PC_OFST	0x20

/* dmem registers */
#define SLIM_REV_ID_OFST	0x0
#define SLIM_REV_ID_MIN_MASK		GENMASK(15, 8)
#define SLIM_REV_ID_MIN(id)		((id & SLIM_REV_ID_MIN_MASK) >> 8)
#define SLIM_REV_ID_MAJ_MASK		GENMASK(23, 16)
#define SLIM_REV_ID_MAJ(id)		((id & SLIM_REV_ID_MAJ_MASK) >> 16)


/* peripherals registers */
#define SLIM_STBUS_SYNC_OFST	0xF88
#define SLIM_STBUS_SYNC_DIS		BIT(0)

#define SLIM_INT_SET_OFST	0xFD4
#define SLIM_INT_CLR_OFST	0xFD8
#define SLIM_INT_MASK_OFST	0xFDC

#define SLIM_CMD_CLR_OFST	0xFC8
#define SLIM_CMD_MASK_OFST	0xFCC

const char *mem_names[SLIM_MEM_MAX] = {
	[SLIM_DMEM]	= "dmem",
	[SLIM_IMEM]	= "imem",
};

static int slim_clk_get(struct st_slim_rproc *slim_rproc, struct device *dev)
{
	int clk, err;

	for (clk = 0; clk < SLIM_MAX_CLK; clk++) {
		slim_rproc->clks[clk] = of_clk_get(dev->of_node, clk);
		if (IS_ERR(slim_rproc->clks[clk])) {
			err = PTR_ERR(slim_rproc->clks[clk]);
			if (err == -EPROBE_DEFER)
				goto err_put_clks;
			slim_rproc->clks[clk] = NULL;
			break;
		}
	}

	return 0;

err_put_clks:
	while (--clk >= 0)
		clk_put(slim_rproc->clks[clk]);

	return err;
}

static void slim_clk_disable(struct st_slim_rproc *slim_rproc)
{
	int clk;

	for (clk = 0; clk < SLIM_MAX_CLK && slim_rproc->clks[clk]; clk++)
		clk_disable_unprepare(slim_rproc->clks[clk]);
}

static int slim_clk_enable(struct st_slim_rproc *slim_rproc)
{
	int clk, ret;

	for (clk = 0; clk < SLIM_MAX_CLK && slim_rproc->clks[clk]; clk++) {
		ret = clk_prepare_enable(slim_rproc->clks[clk]);
		if (ret)
			goto err_disable_clks;
	}

	return 0;

err_disable_clks:
	while (--clk >= 0)
		clk_disable_unprepare(slim_rproc->clks[clk]);

	return ret;
}

/**
 * Remoteproc slim specific device handlers
 */
static int slim_rproc_start(struct rproc *rproc)
{
	struct device *dev = &rproc->dev;
	struct st_slim_rproc *slim_rproc = rproc->priv;
	unsigned long hw_id, hw_ver, fw_rev;
	u32 val;
	int ret;

	ret = slim_clk_enable(slim_rproc);
	if (ret) {
		dev_err(dev, "Failed to enable clocks\n");
		goto err_clk;
	}

	/* disable CPU pipeline clock & reset cpu pipeline */
	val = SLIM_CLK_GATE_DIS | SLIM_CLK_GATE_RESET;
	writel(val, slim_rproc->slimcore + SLIM_CLK_GATE_OFST);

	/* disable SLIM core STBus sync */
	writel(SLIM_STBUS_SYNC_DIS, slim_rproc->peri + SLIM_STBUS_SYNC_OFST);

	/* enable cpu pipeline clock */
	writel(!SLIM_CLK_GATE_DIS,
		slim_rproc->slimcore + SLIM_CLK_GATE_OFST);

	/* clear int & cmd mailbox */
	writel(~0UL, slim_rproc->peri + SLIM_INT_CLR_OFST);
	writel(~0UL, slim_rproc->peri + SLIM_CMD_CLR_OFST);

	/* enable all channels cmd & int */
	writel(~0UL, slim_rproc->peri + SLIM_INT_MASK_OFST);
	writel(~0UL, slim_rproc->peri + SLIM_CMD_MASK_OFST);

	/* enable cpu */
	writel(SLIM_EN_RUN, slim_rproc->slimcore + SLIM_EN_OFST);

	hw_id = readl_relaxed(slim_rproc->slimcore + SLIM_ID_OFST);
	hw_ver = readl_relaxed(slim_rproc->slimcore + SLIM_VER_OFST);

	fw_rev = readl(slim_rproc->mem[SLIM_DMEM].cpu_addr +
			SLIM_REV_ID_OFST);

	dev_info(dev, "fw rev:%ld.%ld on SLIM %ld.%ld\n",
		 SLIM_REV_ID_MAJ(fw_rev), SLIM_REV_ID_MIN(fw_rev),
		 hw_id, hw_ver);

err_clk:
	return ret;
}

static int slim_rproc_stop(struct rproc *rproc)
{
	struct st_slim_rproc *slim_rproc = rproc->priv;
	u32 val;

	/* mask all (cmd & int) channels */
	writel(0UL, slim_rproc->peri + SLIM_INT_MASK_OFST);
	writel(0UL, slim_rproc->peri + SLIM_CMD_MASK_OFST);

	/* disable cpu pipeline clock */
	writel(SLIM_CLK_GATE_DIS
		, slim_rproc->slimcore + SLIM_CLK_GATE_OFST);

	writel(!SLIM_EN_RUN, slim_rproc->slimcore + SLIM_EN_OFST);

	val = readl(slim_rproc->slimcore + SLIM_EN_OFST);
	if (val & SLIM_EN_RUN)
		dev_warn(&rproc->dev, "Failed to disable SLIM");

	slim_clk_disable(slim_rproc);

	dev_dbg(&rproc->dev, "slim stopped\n");

	return 0;
}

static void *slim_rproc_da_to_va(struct rproc *rproc, u64 da, int len)
{
	struct st_slim_rproc *slim_rproc = rproc->priv;
	void *va = NULL;
	int i;

	for (i = 0; i < SLIM_MEM_MAX; i++) {
		if (da != slim_rproc->mem[i].bus_addr)
			continue;

		va = slim_rproc->mem[i].cpu_addr;
		break;
	}

	dev_dbg(&rproc->dev, "%s: da = 0x%llx len = 0x%x va = 0x%p\n",
		__func__, da, len, va);

	return va;
}

static struct rproc_ops slim_rproc_ops = {
	.start		= slim_rproc_start,
	.stop		= slim_rproc_stop,
	.da_to_va       = slim_rproc_da_to_va,
};

/**
 * Firmware handler operations: sanity, boot address, load ...
 */

/**
 * slim_rproc_alloc() - allocate and initialise slim rproc
 * @pdev: Pointer to the platform_device struct
 * @fw_name: Name of firmware for rproc to use
 *
 * Function for allocating and initialising a slim rproc for use by
 * device drivers whose IP is based around the slim slim core. It
 * obtains and enables any clocks required by the slim core and also
 * ioremaps the various IO.
 *
 * Returns st_slim_rproc pointer or PTR_ERR() on error.
 */

struct st_slim_rproc *slim_rproc_alloc(struct platform_device *pdev,
				char *fw_name)
{
	struct device *dev = &pdev->dev;
	struct st_slim_rproc *slim_rproc;
	struct device_node *np = dev->of_node;
	struct rproc *rproc;
	struct resource *res;
	int err, i;

	if (WARN_ON(!np || !fw_name))
		return ERR_PTR(-EINVAL);

	if (!of_device_is_compatible(np, "st,slim-rproc"))
		return ERR_PTR(-EINVAL);

	rproc = rproc_alloc(dev, np->name, &slim_rproc_ops,
			fw_name, sizeof(*slim_rproc));
	if (!rproc)
		return ERR_PTR(-ENOMEM);

	rproc->has_iommu = false;
	rproc->has_rsctable = false;

	slim_rproc = rproc->priv;
	slim_rproc->rproc = rproc;

	/* get imem and dmem */
	for (i = 0; i < ARRAY_SIZE(mem_names); i++) {

		res = platform_get_resource_byname
			(pdev, IORESOURCE_MEM, mem_names[i]);

		slim_rproc->mem[i].cpu_addr = devm_ioremap_resource(dev, res);
		if (IS_ERR(slim_rproc->mem[i].cpu_addr)) {
			dev_err(&pdev->dev, "devm_ioremap_resource failed\n");
			err = PTR_ERR(slim_rproc->mem[i].cpu_addr);
			goto err;
		}
		slim_rproc->mem[i].bus_addr = res->start;
		slim_rproc->mem[i].size = resource_size(res);
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "slimcore");

	slim_rproc->slimcore = devm_ioremap_resource(dev, res);
	if (IS_ERR(slim_rproc->slimcore)) {
		dev_err(&pdev->dev,
			"devm_ioremap_resource failed for slimcore\n");
		err = PTR_ERR(slim_rproc->slimcore);
		goto err;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "peripherals");

	slim_rproc->peri = devm_ioremap_resource(dev, res);
	if (IS_ERR(slim_rproc->peri)) {
		dev_err(&pdev->dev, "devm_ioremap_resource failed for peri\n");
		err = PTR_ERR(slim_rproc->peri);
		goto err;
	}

	err = slim_clk_get(slim_rproc, dev);
	if (err)
		goto err;

	/* Register as a remoteproc device */
	err = rproc_add(rproc);
	if (err) {
		dev_err(dev, "registration of slim remoteproc failed\n");
		goto err_clk;
	}

	return slim_rproc;

err_clk:
	for (i = 0; i < SLIM_MAX_CLK && slim_rproc->clks[i]; i++)
		clk_put(slim_rproc->clks[i]);
err:
	rproc_put(rproc);
	return ERR_PTR(err);
}
EXPORT_SYMBOL(slim_rproc_alloc);

/**
  * slim_rproc_put() - put slim rproc resources
  * @slim_rproc: Pointer to the st_slim_rproc struct
  *
  * Function for calling respective _put() functions on
  * slim_rproc resources.
  *
  */
void slim_rproc_put(struct st_slim_rproc *slim_rproc)
{
	int clk;

	if (!slim_rproc)
		return;

	for (clk = 0; clk < SLIM_MAX_CLK && slim_rproc->clks[clk]; clk++)
		clk_put(slim_rproc->clks[clk]);

	rproc_put(slim_rproc->rproc);
}
EXPORT_SYMBOL(slim_rproc_put);

MODULE_AUTHOR("Peter Griffin");
MODULE_DESCRIPTION("STMicroelectronics SLIM rproc driver");
MODULE_LICENSE("GPL v2");
