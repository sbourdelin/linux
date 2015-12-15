/*
 *  linux/drivers/mtd/devices/lp8x4x_sram.c
 *
 *  MTD Driver for SRAM on ICPDAS LP-8x4x
 *  Copyright (C) 2013 Sergei Ianovich <ynvich@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation or any later version.
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mtd/map.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/string_helpers.h>
#include <linux/types.h>

struct lp8x4x_sram_info {
	void __iomem	*bank;
	void __iomem	*virt;
	struct mutex	lock;
	unsigned	active_bank;
	struct mtd_info	mtd;
};

static int
lp8x4x_sram_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	struct lp8x4x_sram_info *info = mtd->priv;
	unsigned bank = instr->addr >> 11;
	unsigned offset = (instr->addr & 0x7ff) << 1;
	loff_t i;

	mutex_lock(&info->lock);
	if (unlikely(bank != info->active_bank)) {
		info->active_bank = bank;
		iowrite8(bank, info->bank);
	}
	for (i = 0; i < instr->len; i++) {
		iowrite8(0xff, info->virt + offset);
		offset += 2;
		if (unlikely(offset == 0)) {
			info->active_bank++;
			iowrite8(info->active_bank, info->bank);
		}
	}
	mutex_unlock(&info->lock);
	instr->state = MTD_ERASE_DONE;
	mtd_erase_callback(instr);

	return 0;
}

static int
lp8x4x_sram_write(struct mtd_info *mtd, loff_t to, size_t len,
		size_t *retlen, const u_char *b)
{
	struct lp8x4x_sram_info *info = mtd->priv;
	unsigned bank = to >> 11;
	unsigned offset = (to & 0x7ff) << 1;
	loff_t i;

	mutex_lock(&info->lock);
	if (unlikely(bank != info->active_bank)) {
		info->active_bank = bank;
		iowrite8(bank, info->bank);
	}
	for (i = 0; i < len; i++) {
		iowrite8(b[i], info->virt + offset);
		offset += 2;
		if (unlikely(offset == 0)) {
			info->active_bank++;
			iowrite8(info->active_bank, info->bank);
		}
	}
	mutex_unlock(&info->lock);
	*retlen = len;
	return 0;
}

static int
lp8x4x_sram_read(struct mtd_info *mtd, loff_t from, size_t len,
		size_t *retlen, u_char *b)
{
	struct lp8x4x_sram_info *info = mtd->priv;
	unsigned bank = from >> 11;
	unsigned offset = (from & 0x7ff) << 1;
	loff_t i;

	mutex_lock(&info->lock);
	if (unlikely(bank != info->active_bank)) {
		info->active_bank = bank;
		iowrite8(bank, info->bank);
	}
	for (i = 0; i < len; i++) {
		b[i] = ioread8(info->virt + offset);
		offset += 2;
		if (unlikely(offset == 0)) {
			info->active_bank++;
			iowrite8(info->active_bank, info->bank);
		}
	}
	mutex_unlock(&info->lock);
	*retlen = len;
	return 0;
}

static const struct of_device_id of_flash_match[] = {
	{
		.compatible	= "icpdas,sram-lp8x4x",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, of_flash_match);

static int
lp8x4x_sram_probe(struct platform_device *pdev)
{
	struct lp8x4x_sram_info *info;
	struct resource *res_virt, *res_bank;
	char sz_str[16];
	struct mtd_part_parser_data ppdata;
	int err = 0;

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	res_virt = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	info->virt =  devm_ioremap_resource(&pdev->dev, res_virt);
	if (IS_ERR(info->virt))
		return PTR_ERR(info->virt);

	res_bank = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	info->bank = devm_ioremap_resource(&pdev->dev, res_bank);
	if (IS_ERR(info->bank))
		return PTR_ERR(info->bank);

	info->mtd.priv = info;
	info->mtd.name = "SRAM";
	info->mtd.type = MTD_RAM;
	info->mtd.flags = MTD_CAP_RAM;
	info->mtd.size = resource_size(res_virt) << 7;
	info->mtd.erasesize = 512;
	info->mtd.writesize = 4;
	info->mtd._erase = lp8x4x_sram_erase;
	info->mtd._write = lp8x4x_sram_write;
	info->mtd._read = lp8x4x_sram_read;
	info->mtd.owner = THIS_MODULE;

	mutex_init(&info->lock);
	iowrite8(info->active_bank, info->bank);
	platform_set_drvdata(pdev, info);

	ppdata.of_node = pdev->dev.of_node;
	err = mtd_device_parse_register(&info->mtd, NULL, &ppdata,
			NULL, 0);

	if (err < 0) {
		dev_err(&pdev->dev, "failed to register MTD\n");
		return err;
	}

	string_get_size(info->mtd.size, 1, STRING_UNITS_2, sz_str,
			sizeof(sz_str));
	dev_info(&pdev->dev, "using %s SRAM on LP-8X4X as %s\n", sz_str,
			dev_name(&info->mtd.dev));
	return 0;
}

static int
lp8x4x_sram_remove(struct platform_device *dev)
{
	struct lp8x4x_sram_info *info = platform_get_drvdata(dev);

	return mtd_device_unregister(&info->mtd);
}

static struct platform_driver lp8x4x_sram_driver = {
	.driver = {
		.name		= "sram-lp8x4x",
		.of_match_table = of_flash_match,
	},
	.probe		= lp8x4x_sram_probe,
	.remove		= lp8x4x_sram_remove,
};

module_platform_driver(lp8x4x_sram_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sergei Ianovich <ynvich@gmail.com>");
MODULE_DESCRIPTION("MTD driver for SRAM on ICPDAS LP-8x4x");
