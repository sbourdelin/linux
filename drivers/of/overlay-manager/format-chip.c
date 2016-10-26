/*
 * Copyright (C) 2016 - Antoine Tenart <antoine.tenart@free-electrons.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/device.h>
#include <linux/overlay-manager.h>
#include <linux/slab.h>

#define CAPE_CHIP_MAGIC		0x43484950
#define CAPE_CHIP_VERSION	1
#define CAPE_CHIP_CANDIDATES	2

static int cape_chip_parse(struct device *dev, void *data, char ***candidates,
			   unsigned *n)
{
	struct chip_header *header = (struct chip_header *)data;
	char **tmp;
	int err;

	if (dip_convert(header->magic) != CAPE_CHIP_MAGIC)
		return -EINVAL;

	if (dip_convert(header->version) > CAPE_CHIP_VERSION)
		return -EINVAL;

	tmp = devm_kzalloc(dev, CAPE_CHIP_CANDIDATES * sizeof(char *), GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	tmp[0] = devm_kasprintf(dev, GFP_KERNEL, "%x-%x-%x",
				dip_convert(header->vendor_id),
				dip_convert(header->product_id),
				dip_convert(header->product_version));
	if (!tmp[0]) {
		err = -ENOMEM;
		goto err_free_list;
	}

	tmp[1] = devm_kasprintf(dev, GFP_KERNEL, "%x-%x",
				dip_convert(header->vendor_id),
				dip_convert(header->product_id));
	if (!tmp[1]) {
		err = -ENOMEM;
		goto err_free_0;
	}

	*candidates = tmp;
	*n = CAPE_CHIP_CANDIDATES;

	return 0;

err_free_0:
	devm_kfree(dev, tmp[0]);
err_free_list:
	devm_kfree(dev, tmp);
	return err;
}

static struct overlay_mgr_format format_chip = {
	.name	= "Nextthing C.H.I.P. dip header format",
	.parse	= &cape_chip_parse,
};

static int __init cape_chip_init(void)
{
	return overlay_mgr_register_format(&format_chip);
}
device_initcall(cape_chip_init);
