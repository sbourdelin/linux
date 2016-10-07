/*
 * Juniper Networks PTX1K RCB I2CS Boot FPGA MTD driver
 * FPGA upgrades of the Spartan3AN/XC3S700 based I2CS.
 *
 * Copyright (C) 2015 Juniper Networks. All rights reserved.
 * Author:	JawaharBalaji Thirumalaisamy <jawaharb@juniper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mfd/ptxpmb_cpld.h>

struct nvram_mtd {
	void __iomem *base;
	struct device *dev;
	struct mtd_info mtd;
	struct mutex lock;
};


static int ram_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	struct nvram_mtd *nvram = container_of(mtd, struct nvram_mtd, mtd);

	memset((char *)nvram->base + instr->addr, 0xff, instr->len);
	instr->state = MTD_ERASE_DONE;
	mtd_erase_callback(instr);
	return 0;
}

static int ram_point(struct mtd_info *mtd, loff_t from, size_t len,
		     size_t *retlen, void **virt, resource_size_t *phys)
{
	struct nvram_mtd *nvram = container_of(mtd, struct nvram_mtd, mtd);

	virt = nvram->base + from;
	*retlen = len;
	return 0;
}

static int ram_unpoint(struct mtd_info *mtd, loff_t from, size_t len)
{
	return 0;
}

static int ram_read(struct mtd_info *mtd, loff_t from, size_t len,
		    size_t *retlen, u_char *buf)
{
	struct nvram_mtd *nvram = container_of(mtd, struct nvram_mtd, mtd);

	memcpy(buf, nvram->base + from, len);
	*retlen = len;
	return 0;
}

static int ram_write(struct mtd_info *mtd, loff_t to, size_t len,
		     size_t *retlen, const u_char *buf)
{
	struct nvram_mtd *nvram = container_of(mtd, struct nvram_mtd, mtd);

	memcpy((char *)nvram->base + to, buf, len);
	*retlen = len;
	return 0;
}

int nvram_init_mtd_parse(struct platform_device *pdev, struct mtd_info *mtd)
{
	struct mtd_part_parser_data ppdata = {};
	struct device *dev = &pdev->dev;
	int ret;

	mtd->name = dev_name(dev);
	mtd->type = MTD_RAM;
	mtd->flags = MTD_CAP_RAM;
	mtd->size = 0xFF00;
	mtd->writesize = 1;
	mtd->writebufsize = 64; /* Mimic CFI NOR flashes */
	mtd->erasesize = 0x1000;
	mtd->owner = THIS_MODULE;
	mtd->_erase = ram_erase;
	mtd->_point = ram_point;
	mtd->_unpoint = ram_unpoint;
	mtd->_read = ram_read;
	mtd->_write = ram_write;
	mtd->_panic_write = ram_write;

	ret = mtd_device_parse_register(mtd, NULL, &ppdata, NULL, 0);
	if (ret) {
		dev_err(dev, "mtd_device_parse_register returned %d\n", ret);
		return ret;
	}
	return ret;
}

static int nvram_probe(struct platform_device *pdev)
{
	struct pmb_boot_cpld __iomem *cpld;
	struct nvram_mtd *nvram;
	struct device *dev = &pdev->dev;
	struct resource *res;
	int ret;

	nvram = devm_kzalloc(dev, sizeof(*nvram), GFP_KERNEL);
	if (!nvram)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "Failed to get nvram mmio resource\n");
		return -ENOENT;
	}
	nvram->base = devm_ioremap_nocache(dev, res->start, resource_size(res));
	if (!nvram->base) {
		dev_err(dev, "Cannot map nvram\n");
		return -EADDRNOTAVAIL;
	}
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res) {
		/* Always assume that we need cpld control */
		dev_err(dev, "Failed to get cpld mmio resource\n");
		return -ENOENT;
	}
	cpld = devm_ioremap_nocache(dev, res->start, resource_size(res));
	if (!cpld) {
		dev_err(dev, "Cannot map cpld\n");
		return -EADDRNOTAVAIL;
	}
	nvram->dev = dev;
	platform_set_drvdata(pdev, nvram);
	ret = nvram_init_mtd_parse(pdev, &nvram->mtd);
	if (ret)
		return ret;

	if (READ_ONCE(cpld->cpld_rev) < 0xC6)
		dev_info(dev, "NVRAM requires atleast cpld_rev 0XC6\n");

	/* Initialize the window register in the cpld*/
	WRITE_ONCE(cpld->board.nvram.nv_win, 0x0);
	dev_info(dev, "Initialized window:0x%x\n",
			READ_ONCE(cpld->board.nvram.nv_win));
	return ret;
}

static int nvram_remove(struct platform_device *pdev)
{
	struct nvram_mtd *nvram;

	nvram = platform_get_drvdata(pdev);
	mtd_device_unregister(&nvram->mtd);
	return 0;
}

static const struct of_device_id ngpmb_mtd_ids[] = {
	{ .compatible = "jnx,ngpmb-nvram", },
	{ },
};
MODULE_DEVICE_TABLE(of, ngpmb_mtd_ids);

static struct platform_driver nvram_driver = {
	.probe  = nvram_probe,
	.remove = nvram_remove,
	.driver = {
		.name = "ngpmb-nvram",
		.owner = THIS_MODULE,
		.of_match_table = ngpmb_mtd_ids,
	},
};

module_platform_driver(nvram_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("JawaharBalaji Thirumalaisamy <jawaharb@juniper.net>");
MODULE_DESCRIPTION("EVO PTXPMB CPLD NVRAM Driver");
MODULE_ALIAS("platform:ngpmb-nvram");
