/*
 * Copyright (c) 2016, National Instruments Corp.
 *
 * Generic NVMEM support for OTP regions in MTD devices
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/mtd/mtd.h>

struct mtd_nvmem {
	struct mtd_info *info;
	struct nvmem_device *dev;
	struct regmap *regmap;
};

static int mtd_otp_regmap_read(void *context, const void *reg, size_t reg_size,
			       void *val, size_t val_size)
{
	struct mtd_info *info = context;
	const u8 *offset = reg;
	int err;
	size_t retlen;

	if (reg_size != 1)
		return -EINVAL;

	err = mtd_read_user_prot_reg(info, *offset, val_size > info->size
				     ? info->size : val_size, &retlen,
				     (u_char *)val);

	return 0;
}

static int mtd_otp_regmap_write(void *context, const void *data, size_t count)
{
	/* Not implemented */
	return 0;
}

static const struct regmap_bus mtd_otp_bus = {
	.read = mtd_otp_regmap_read,
	.write = mtd_otp_regmap_write,
	.reg_format_endian_default = REGMAP_ENDIAN_NATIVE,
	.val_format_endian_default = REGMAP_ENDIAN_NATIVE,
};

static bool mtd_otp_nvmem_writeable_reg(struct device *dev, unsigned int reg)
{
	return false;
}

static struct regmap_config mtd_otp_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.reg_stride = 1,
	.writeable_reg = mtd_otp_nvmem_writeable_reg,
	.name = "mtd-otp",
};

static struct nvmem_config mtd_otp_nvmem_config = {
	.read_only = true,
	.owner = THIS_MODULE,
};

struct mtd_nvmem *mtd_otp_nvmem_register(struct mtd_info *info)
{
	struct mtd_nvmem *nvmem;
	struct device *dev = &info->dev;

	nvmem = kzalloc(sizeof(*nvmem), GFP_KERNEL);
	if (!nvmem)
		return ERR_PTR(-ENOMEM);

	mtd_otp_regmap_config.max_register = info->size;

	nvmem->regmap = regmap_init(dev, &mtd_otp_bus, info,
				  &mtd_otp_regmap_config);
	if (IS_ERR(nvmem->regmap)) {
		dev_err(dev, "regmap init failed");
		goto out_free;
	}

	mtd_otp_nvmem_config.dev = dev;
	mtd_otp_nvmem_config.name = info->name;

	nvmem->dev = nvmem_register(&mtd_otp_nvmem_config);

	if (!nvmem->dev) {
		dev_err(dev, "failed to register nvmem");
		goto out_regmap;
	}

	return nvmem;

out_regmap:
	regmap_exit(nvmem->regmap);

out_free:
	kfree(nvmem);
	return NULL;
}
EXPORT_SYMBOL_GPL(mtd_otp_nvmem_register);

void mtd_otp_nvmem_remove(struct mtd_nvmem *nvmem)
{
	nvmem_unregister(nvmem->dev);
	regmap_exit(nvmem->regmap);
	kfree(nvmem);
}
EXPORT_SYMBOL_GPL(mtd_otp_nvmem_remove);
