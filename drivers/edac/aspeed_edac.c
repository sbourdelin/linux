// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2018 Cisco Systems
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/edac.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/stop_machine.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <asm/page.h>
#include "edac_module.h"


#define DRV_NAME "aspeed-edac"


/* registers */
#define ASPEED_MCR_PROT        0x00 /* protection key register */
#define ASPEED_MCR_CONF        0x04 /* configuration register */
#define ASPEED_MCR_INTR_CTRL   0x50 /* interrupt control/status register */
#define ASPEED_MCR_ADDR_UNREC  0x58 /* address of first un-recoverable error */
#define ASPEED_MCR_ADDR_REC    0x5c /* address of last recoverable error */
#define ASPEED_MCR_LAST        ASPEED_MCR_ADDR_REC


/* bits and masks */
#define ASPEED_MCR_PROT_PASSWD	            0xfc600309
#define ASPEED_MCR_CONF_DRAM_TYPE               BIT(4)
#define ASPEED_MCR_CONF_ECC                     BIT(7)
#define ASPEED_MCR_INTR_CTRL_CLEAR             BIT(31)
#define ASPEED_MCR_INTR_CTRL_CNT_REC   GENMASK(23, 16)
#define ASPEED_MCR_INTR_CTRL_CNT_UNREC GENMASK(15, 12)
#define ASPEED_MCR_INTR_CTRL_ENABLE  (BIT(0) | BIT(1))



static int aspeed_edac_regmap_reg_write(void *context, unsigned int reg,
					unsigned int val)
{
	void __iomem *regs = (void __iomem *)context;

	/* enable write to MCR register set */
	writel(ASPEED_MCR_PROT_PASSWD, regs + ASPEED_MCR_PROT);

	writel(val, regs + reg);

	/* disable write to MCR register set */
	writel(~ASPEED_MCR_PROT_PASSWD, regs + ASPEED_MCR_PROT);

	return 0;
}


static int aspeed_edac_regmap_reg_read(void *context, unsigned int reg,
				       unsigned int *val)
{
	void __iomem *regs = (void __iomem *)context;

	*val = readl(regs + reg);

	return 0;
}

static bool aspeed_edac_regmap_is_volatile(struct device *dev,
					   unsigned int reg)
{
	switch (reg) {
	case ASPEED_MCR_PROT:
	case ASPEED_MCR_INTR_CTRL:
	case ASPEED_MCR_ADDR_UNREC:
	case ASPEED_MCR_ADDR_REC:
		return true;
	default:
		return false;
	}
}


static const struct regmap_config aspeed_edac_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = ASPEED_MCR_LAST,
	.reg_write = aspeed_edac_regmap_reg_write,
	.reg_read = aspeed_edac_regmap_reg_read,
	.volatile_reg = aspeed_edac_regmap_is_volatile,
	.fast_io = true,
};


static struct regmap *aspeed_edac_regmap;


static void aspeed_edac_count_rec(struct mem_ctl_info *mci,
				  u8 rec_cnt,
				  u32 rec_addr)
{
	struct csrow_info *csrow = mci->csrows[0];
	u32 page, offset, syndrome;

	if (rec_cnt > 0) {
		/* report first few errors (if there are) */
		/* note: no addresses are recorded */
		if (rec_cnt > 1) {
			page = 0; /* not available */
			offset = 0;  /* not available */
			syndrome = 0; /* not available */
			edac_mc_handle_error(HW_EVENT_ERR_CORRECTED, mci,
					     rec_cnt-1, page, offset,
					     syndrome, 0, 0, -1,
					     "address(es) not available", "");
		}

		/* report last error */
		/* note: rec_addr is the last recoverable error addr */
		page = rec_addr >> PAGE_SHIFT;
		offset = rec_addr & ~PAGE_MASK;
		syndrome = 0; /* not available */
		edac_mc_handle_error(HW_EVENT_ERR_CORRECTED, mci, 1,
				     csrow->first_page + page, offset, syndrome,
				     0, 0, -1, "", "");
	}
}


static void aspeed_edac_count_un_rec(struct mem_ctl_info *mci,
				     u8 un_rec_cnt,
				     u32 un_rec_addr)
{
	struct csrow_info *csrow = mci->csrows[0];
	u32 page, offset, syndrome;

	if (un_rec_cnt > 0) {
		/* report 1. error */
		/* note: un_rec_addr is the first unrecoverable error addr */
		page = un_rec_addr >> PAGE_SHIFT;
		offset = un_rec_addr & ~PAGE_MASK;
		syndrome = 0; /* not available */
		edac_mc_handle_error(HW_EVENT_ERR_UNCORRECTED, mci, 1,
				     csrow->first_page + page, offset, syndrome,
				     0, 0, -1, "", "");

		/* report further errors (if there are) */
		/* note: no addresses are recorded */
		if (un_rec_cnt > 1) {
			page = 0;  /* not available */
			offset = 0;  /* not available */
			syndrome = 0; /* not available */
			edac_mc_handle_error(HW_EVENT_ERR_UNCORRECTED, mci,
					     un_rec_cnt-1, page, offset,
					     syndrome, 0, 0, -1,
					     "address(es) not available", "");
		}
	}
}


static void aspeed_edac_enable_interrupts(void)
{

	regmap_update_bits(aspeed_edac_regmap, ASPEED_MCR_INTR_CTRL,
			   ASPEED_MCR_INTR_CTRL_ENABLE,
			   ASPEED_MCR_INTR_CTRL_ENABLE);
}


static void aspeed_edac_disable_interrupts(void)
{
	regmap_update_bits(aspeed_edac_regmap, ASPEED_MCR_INTR_CTRL,
			   ASPEED_MCR_INTR_CTRL_ENABLE, 0);
}


static void aspeed_edac_clear_interrupts(void)
{
	regmap_update_bits(aspeed_edac_regmap, ASPEED_MCR_INTR_CTRL,
			   ASPEED_MCR_INTR_CTRL_CLEAR,
			   ASPEED_MCR_INTR_CTRL_CLEAR);

	regmap_update_bits(aspeed_edac_regmap, ASPEED_MCR_INTR_CTRL,
			   ASPEED_MCR_INTR_CTRL_CLEAR, 0);
}


static irqreturn_t aspeed_edac_isr(int irq, void *arg)
{
	u8  rec_cnt, un_rec_cnt;
	u32 rec_addr, un_rec_addr;
	struct mem_ctl_info *mci = arg;
	u32 reg50, reg5c, reg58;

	regmap_read(aspeed_edac_regmap, ASPEED_MCR_INTR_CTRL, &reg50);
	dev_dbg(mci->pdev, "received edac interrupt w/ mmc register 50: 0x%x\n",
		reg50);

	/* collect data about recoverable and unrecoverable errors */
	rec_cnt = (reg50 & ASPEED_MCR_INTR_CTRL_CNT_REC) >> 16;
	un_rec_cnt = (reg50 & ASPEED_MCR_INTR_CTRL_CNT_UNREC) >> 12;

	dev_dbg(mci->pdev, "%d recoverable interrupts and %d unrecoverable interrupts\n",
		rec_cnt, un_rec_cnt);

	regmap_read(aspeed_edac_regmap, ASPEED_MCR_ADDR_UNREC, &reg58);
	un_rec_addr = reg58 >> 4;

	regmap_read(aspeed_edac_regmap, ASPEED_MCR_ADDR_REC, &reg5c);
	rec_addr = reg5c >> 4;

	/* clear interrupt flags and error counters: */
	aspeed_edac_clear_interrupts();

	/* process recoverable and unrecoverable errors */
	if (rec_cnt > 0)
		aspeed_edac_count_rec(mci, rec_cnt, rec_addr);

	if (un_rec_cnt > 0)
		aspeed_edac_count_un_rec(mci, un_rec_cnt, un_rec_addr);

	if ((rec_cnt == 0) && (un_rec_cnt == 0))
		dev_dbg(mci->pdev, "received edac interrupt, but did not find any ecc counters\n");

	regmap_read(aspeed_edac_regmap, ASPEED_MCR_INTR_CTRL, &reg50);
	dev_dbg(mci->pdev, "edac interrupt handled. mmc reg 50 is now: 0x%x\n",
		reg50);

	return IRQ_HANDLED;
}


static int aspeed_edac_config_irq(void *ctx,
				  struct platform_device *pdev)
{
	int irq;
	int rc;

	/* register interrupt handler */

	irq = platform_get_irq(pdev, 0);
	dev_dbg(&pdev->dev, "got irq %d\n", irq);
	if (!irq)
		return -ENODEV;

	rc = devm_request_irq(&pdev->dev, irq, aspeed_edac_isr,
			      IRQF_TRIGGER_HIGH, DRV_NAME, ctx);
	if (rc) {
		dev_err(&pdev->dev, "unable to request irq %d\n", irq);
		return rc;
	}

	/* enable interrupts */
	aspeed_edac_enable_interrupts();

	return 0;
}


static int aspeed_edac_init_csrows(struct mem_ctl_info *mci)
{
	struct csrow_info *csrow = mci->csrows[0];
	struct dimm_info *dimm;
	struct device_node *np;
	u32 nr_pages, dram_type;
	struct resource r;
	u32 reg04;
	int rc;

	/* retrieve info about physical memory from device tree */
	np = of_find_node_by_path("/memory");

	if (!np) {
		dev_err(mci->pdev, "dt: missing /memory node\n");
		return -ENODEV;
	};

	rc = of_address_to_resource(np, 0, &r);

	of_node_put(np);

	if (rc) {
		dev_err(mci->pdev, "dt: failed requesting resource for /memory node\n");
		return rc;
	};

	dev_dbg(mci->pdev, "dt: /memory node resources: first page r.start=0x%x, resource_size=0x%x, PAGE_SHIFT macro=0x%x\n",
		r.start, resource_size(&r), PAGE_SHIFT);

	csrow->first_page = r.start >> PAGE_SHIFT;
	nr_pages = resource_size(&r) >> PAGE_SHIFT;
	csrow->last_page = csrow->first_page + nr_pages - 1;

	regmap_read(aspeed_edac_regmap, ASPEED_MCR_CONF, &reg04);
	dram_type = (reg04 & ASPEED_MCR_CONF_DRAM_TYPE) ? MEM_DDR4 : MEM_DDR3;

	dimm = csrow->channels[0]->dimm;
	dimm->mtype = dram_type;
	dimm->edac_mode = EDAC_SECDED;
	dimm->nr_pages = nr_pages / csrow->nr_channels;

	dev_dbg(mci->pdev, "initialized dimm with first_page=0x%lx and nr_pages=0x%x\n",
		csrow->first_page, nr_pages);

	return 0;
}


static int aspeed_edac_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	void __iomem *regs;
	struct resource *res;
	struct mem_ctl_info *mci;
	struct edac_mc_layer layers[2];
	struct device_node *np;
	u32 reg04;
	int rc;

	/* setup regmap */
	np = dev->of_node;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENOENT;

	regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	aspeed_edac_regmap = devm_regmap_init(dev, NULL, (__force void *)regs,
					       &aspeed_edac_regmap_config);
	if (IS_ERR(aspeed_edac_regmap))
		return PTR_ERR(aspeed_edac_regmap);

	/* bail out if ECC mode is not configured */
	regmap_read(aspeed_edac_regmap, ASPEED_MCR_CONF, &reg04);
	if (!(reg04 & ASPEED_MCR_CONF_ECC)) {
		dev_err(&pdev->dev, "ECC mode is not configured in u-boot\n");
		return -EPERM;
	}

	edac_op_state = EDAC_OPSTATE_INT;

	/* allocate & init EDAC MC data structure */
	layers[0].type = EDAC_MC_LAYER_CHIP_SELECT;
	layers[0].size = 1;
	layers[0].is_virt_csrow = true;
	layers[1].type = EDAC_MC_LAYER_CHANNEL;
	layers[1].size = 1;
	layers[1].is_virt_csrow = false;

	mci = edac_mc_alloc(0, ARRAY_SIZE(layers), layers, 0);
	if (mci == NULL)
		return -ENOMEM;

	mci->pdev = &pdev->dev;
	mci->mtype_cap = MEM_FLAG_DDR3 | MEM_FLAG_DDR4;
	mci->edac_ctl_cap = EDAC_FLAG_SECDED;
	mci->edac_cap = EDAC_FLAG_SECDED;
	mci->scrub_cap = SCRUB_FLAG_HW_SRC;
	mci->scrub_mode = SCRUB_HW_SRC;
	mci->mod_name = DRV_NAME;
	mci->ctl_name = "MIC";
	mci->dev_name = dev_name(&pdev->dev);

	rc = aspeed_edac_init_csrows(mci);
	if (rc) {
		dev_err(&pdev->dev, "failed to init csrows\n");
		goto probe_exit02;
	}

	platform_set_drvdata(pdev, mci);

	/* register with edac core */
	rc = edac_mc_add_mc(mci);
	if (rc) {
		dev_err(&pdev->dev, "failed to register with EDAC core\n");
		goto probe_exit02;
	}

	/* register interrupt handler and enable interrupts */
	rc = aspeed_edac_config_irq(mci, pdev);
	if (rc) {
		dev_err(&pdev->dev, "failed setting up irq\n");
		goto probe_exit01;
	}

	return 0;

probe_exit01:
	edac_mc_del_mc(&pdev->dev);
probe_exit02:
	edac_mc_free(mci);
	return rc;
}


static int aspeed_edac_remove(struct platform_device *pdev)
{
	struct mem_ctl_info *mci;

	/* disable interrupts */
	aspeed_edac_disable_interrupts();

	/* free resources */
	mci = edac_mc_del_mc(&pdev->dev);
	if (mci)
		edac_mc_free(mci);

	return 0;
}


static const struct of_device_id aspeed_edac_of_match[] = {
	{ .compatible = "aspeed,ast2500-sdram-edac" },
	{},
};


static struct platform_driver aspeed_edac_driver = {
	.driver		= {
		.name	= DRV_NAME,
		.of_match_table = aspeed_edac_of_match
	},
	.probe		= aspeed_edac_probe,
	.remove		= aspeed_edac_remove
};


static int __init aspeed_edac_init(void)
{
	return platform_driver_register(&aspeed_edac_driver);
}


static void __exit aspeed_edac_exit(void)
{
	platform_driver_unregister(&aspeed_edac_driver);
}


module_init(aspeed_edac_init);
module_exit(aspeed_edac_exit);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Stefan Schaeckeler <sschaeck@cisco.com>");
MODULE_DESCRIPTION("Aspeed AST2500 EDAC driver");
MODULE_VERSION("1.0");
