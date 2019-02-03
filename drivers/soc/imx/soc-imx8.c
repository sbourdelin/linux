// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019 NXP.
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/sys_soc.h>
#include <linux/platform_device.h>
#include <linux/of.h>

#define ANADIG_DIGPROG		0x6c

struct imx8_soc_data {
	char *name;
	u32 (*soc_revision)(void);
};

static u32 __init imx_init_revision_from_anatop(void)
{
	struct device_node *np;
	void __iomem *anatop_base;
	u32 digprog;

	np = of_find_compatible_node(NULL, NULL, "fsl,imx8mq-anatop");
	anatop_base = of_iomap(np, 0);
	WARN_ON(!anatop_base);
	digprog = readl_relaxed(anatop_base + ANADIG_DIGPROG);
	iounmap(anatop_base);

	/*
	 * Bit[7:4] is the base layer revision,
	 * Bit[3:0] is the metal layer revision
	 * e.g. 0x10 stands for Tapeout 1.0
	 */
	return digprog & 0xff;
}

u32 imx8mq_soc_revision(void)
{
	return imx_init_revision_from_anatop();
}

struct imx8_soc_data imx8mq_soc_data = {
	.name = "i.MX8MQ",
	.soc_revision = imx8mq_soc_revision,
};

static const struct of_device_id imx8_soc_match[] = {
	{ .compatible = "fsl,imx8mq", .data = &imx8mq_soc_data, },
	{ }
};

static int __init imx8_soc_init(void)
{
	struct soc_device_attribute *soc_dev_attr;
	struct soc_device *soc_dev;
	struct device_node *root;
	const struct of_device_id *id;
	u32 soc_rev = 0;
	const struct imx8_soc_data *data;
	int ret;

	soc_dev_attr = kzalloc(sizeof(*soc_dev_attr), GFP_KERNEL);
	if (!soc_dev_attr)
		return -ENODEV;

	soc_dev_attr->family = "Freescale i.MX";

	root = of_find_node_by_path("/");
	ret = of_property_read_string(root, "model", &soc_dev_attr->machine);
	if (ret)
		goto free_soc;

	id = of_match_node(imx8_soc_match, root);
	if (!id)
		goto free_soc;

	of_node_put(root);

	data = id->data;
	if (data) {
		soc_dev_attr->soc_id = data->name;
		if (data->soc_revision)
			soc_rev = data->soc_revision();
	}

	soc_dev_attr->revision = kasprintf(GFP_KERNEL, "%d.%d",
					   (soc_rev >> 4) & 0xf,
					    soc_rev & 0xf);
	if (!soc_dev_attr->revision)
		goto free_soc;

	soc_dev = soc_device_register(soc_dev_attr);
	if (IS_ERR(soc_dev))
		goto free_rev;

	return 0;

free_rev:
	kfree(soc_dev_attr->revision);
free_soc:
	kfree(soc_dev_attr);
	return -ENODEV;
}
device_initcall(imx8_soc_init);
