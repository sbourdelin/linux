/*
 * Oxford Semiconductor OXNAS NAND driver
 *
 * Heavily based on plat_nand.c :
 * Author: Vitaly Wool <vitalywool@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/partitions.h>

/* nand commands */
#define NAND_CMD_ALE		BIT(18)
#define NAND_CMD_CLE		BIT(19)
#define NAND_CMD_CS		0
#define NAND_CMD_RESET		0xff
#define NAND_CMD		(NAND_CMD_CS | NAND_CMD_CLE)
#define NAND_ADDR		(NAND_CMD_CS | NAND_CMD_ALE)
#define NAND_DATA		(NAND_CMD_CS)

struct oxnas_nand_data {
	struct nand_chip	chip;
	void __iomem		*io_base;
	struct clk		*clk;
};

static void oxnas_nand_cmd_ctrl(struct mtd_info *mtd, int cmd,
				unsigned int ctrl)
{
	struct nand_chip *this = mtd->priv;
	unsigned long nandaddr = (unsigned long) this->IO_ADDR_W;

	if (ctrl & NAND_CTRL_CHANGE) {
		nandaddr &= ~(NAND_CMD | NAND_ADDR);
		if (ctrl & NAND_CLE)
			nandaddr |= NAND_CMD;
		else if (ctrl & NAND_ALE)
			nandaddr |= NAND_ADDR;
		this->IO_ADDR_W = (void __iomem *) nandaddr;
	}

	if (cmd != NAND_CMD_NONE)
		writeb(cmd, (void __iomem *) nandaddr);
}

/*
 * Probe for the NAND device.
 */
static int oxnas_nand_probe(struct platform_device *pdev)
{
	struct oxnas_nand_data *data;
	struct mtd_info *mtd;
	struct resource *res;
	int err = 0;

	/* Allocate memory for the device structure (and zero it) */
	data = devm_kzalloc(&pdev->dev, sizeof(struct oxnas_nand_data),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	data->io_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(data->io_base))
		return PTR_ERR(data->io_base);

	data->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(data->clk))
		data->clk = NULL;

	nand_set_flash_node(&data->chip, pdev->dev.of_node);
	mtd = nand_to_mtd(&data->chip);
	mtd->dev.parent = &pdev->dev;
	mtd->priv = &data->chip;

	data->chip.IO_ADDR_R = data->io_base;
	data->chip.IO_ADDR_W = data->io_base;
	data->chip.cmd_ctrl = oxnas_nand_cmd_ctrl;
	data->chip.chip_delay = 30;
	data->chip.ecc.mode = NAND_ECC_SOFT;
	data->chip.ecc.algo = NAND_ECC_HAMMING;

	platform_set_drvdata(pdev, data);

	clk_prepare_enable(data->clk);
	device_reset_optional(&pdev->dev);

	/* Scan to find existence of the device */
	if (nand_scan(mtd, 1)) {
		err = -ENXIO;
		goto out;
	}

	err = mtd_device_parse_register(mtd, NULL, NULL, NULL, 0);
	if (!err)
		return err;

	nand_release(mtd);
out:
	return err;
}

static int oxnas_nand_remove(struct platform_device *pdev)
{
	struct oxnas_nand_data *data = platform_get_drvdata(pdev);

	nand_release(nand_to_mtd(&data->chip));

	return 0;
}

static const struct of_device_id oxnas_nand_match[] = {
	{ .compatible = "oxsemi,ox820-nand" },
	{},
};
MODULE_DEVICE_TABLE(of, oxnas_nand_match);

static struct platform_driver oxnas_nand_driver = {
	.probe	= oxnas_nand_probe,
	.remove	= oxnas_nand_remove,
	.driver	= {
		.name		= "oxnas_nand",
		.of_match_table = oxnas_nand_match,
	},
};

module_platform_driver(oxnas_nand_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vitaly Wool");
MODULE_DESCRIPTION("Oxnas NAND driver");
MODULE_ALIAS("platform:oxnas_nand");
