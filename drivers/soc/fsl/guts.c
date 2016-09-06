/*
 * Freescale QorIQ Platforms GUTS Driver
 *
 * Copyright (C) 2016 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/io.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/of_fdt.h>
#include <linux/sys_soc.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/glob.h>
#include <linux/fsl/guts.h>
#include <linux/fsl/svr.h>

struct guts {
	struct ccsr_guts __iomem *regs;
	bool little_endian;
};

static struct guts *guts;
static struct soc_device_attribute *soc_dev_attr;
static struct soc_device *soc_dev;


/* SoC attribute definition for QorIQ platform */
static const struct soc_device_attribute qoriq_soc[] = {
#ifdef CONFIG_PPC
	/*
	 * Power Architecture-based SoCs T Series
	 */

	/* SoC: T1024/T1014/T1023/T1013 Rev: 1.0 */
	{ .soc_id	= "svr:0x85400010,name:T1024,die:T1024",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x85480010,name:T1024E,die:T1024",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x85440010,name:T1014,die:T1024",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x854C0010,name:T1014E,die:T1024",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x85410010,name:T1023,die:T1024",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x85490010,name:T1023E,die:T1024",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x85450010,name:T1013,die:T1024",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x854D0010,name:T1013E,die:T1024",
	  .revision	= "1.0",
	},
	/* SoC: T1040/T1020/T1042/T1022 Rev: 1.0/1.1 */
	{ .soc_id	= "svr:0x85200010,name:T1040,die:T1040",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x85280010,name:T1040E,die:T1040",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x85210010,name:T1020,die:T1040",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x85290010,name:T1020E,die:T1040",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x85200210,name:T1042,die:T1040",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x85280210,name:T1042E,die:T1040",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x85210210,name:T1022,die:T1040",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x85290210,name:T1022E,die:T1040",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x85200011,name:T1040,die:T1040",
	  .revision	= "1.1",
	},
	{ .soc_id	= "svr:0x85280011,name:T1040E,die:T1040",
	  .revision	= "1.1",
	},
	{ .soc_id	= "svr:0x85210011,name:T1020,die:T1040",
	  .revision	= "1.1",
	},
	{ .soc_id	= "svr:0x85290011,name:T1020E,die:T1040",
	  .revision	= "1.1",
	},
	{ .soc_id	= "svr:0x85200211,name:T1042,die:T1040",
	  .revision	= "1.1",
	},
	{ .soc_id	= "svr:0x85280211,name:T1042E,die:T1040",
	  .revision	= "1.1",
	},
	{ .soc_id	= "svr:0x85210211,name:T1022,die:T1040",
	  .revision	= "1.1",
	},
	{ .soc_id	= "svr:0x85290211,name:T1022E,die:T1040",
	  .revision	= "1.1",
	},
	/* SoC: T2080/T2081 Rev: 1.0/1.1 */
	{ .soc_id	= "svr:0x85300010,name:T2080,die:T2080",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x85380010,name:T2080E,die:T2080",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x85310010,name:T2081,die:T2080",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x85390010,name:T2081E,die:T2080",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x85300011,name:T2080,die:T2080",
	  .revision	= "1.1",
	},
	{ .soc_id	= "svr:0x85380011,name:T2080E,die:T2080",
	  .revision	= "1.1",
	},
	{ .soc_id	= "svr:0x85310011,name:T2081,die:T2080",
	  .revision	= "1.1",
	},
	{ .soc_id	= "svr:0x85390011,name:T2081E,die:T2080",
	  .revision	= "1.1",
	},
	/* SoC: T4240/T4160/T4080 Rev: 1.0/2.0 */
	{ .soc_id	= "svr:0x82400010,name:T4240,die:T4240",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x82480010,name:T4240E,die:T4240",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x82410010,name:T4160,die:T4240",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x82490010,name:T4160E,die:T4240",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x82400020,name:T4240,die:T4240",
	  .revision	= "2.0",
	},
	{ .soc_id	= "svr:0x82480020,name:T4240E,die:T4240",
	  .revision	= "2.0",
	},
	{ .soc_id	= "svr:0x82410020,name:T4160,die:T4240",
	  .revision	= "2.0",
	},
	{ .soc_id	= "svr:0x82490020,name:T4160E,die:T4240",
	  .revision	= "2.0",
	},
	{ .soc_id	= "svr:0x82410220,name:T4080,die:T4240",
	  .revision	= "2.0",
	},
	{ .soc_id	= "svr:0x82490220,name:T4080E,die:T4240",
	  .revision	= "2.0",
	},
#endif /* CONFIG_PPC */
#if defined(CONFIG_ARCH_MXC) || defined(CONFIG_ARCH_LAYERSCAPE)
	/*
	 * ARM-based SoCs LS Series
	 */

	/* SoC: LS1021A/LS1020A/LS1022A Rev: 1.0/2.0 */
	{ .soc_id	= "svr:0x87001110,name:LS1021A,die:LS1021A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87081110,name:LS1021AE,die:LS1021A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87001010,name:LS1020A,die:LS1021A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87081010,name:LS1020AE,die:LS1021A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87001210,name:LS1022A,die:LS1021A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87081210,name:LS1022AE,die:LS1021A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87001120,name:LS1021A,die:LS1021A",
	  .revision	= "2.0",
	},
	{ .soc_id	= "svr:0x87081120,name:LS1021AE,die:LS1021A",
	  .revision	= "2.0",
	},
	{ .soc_id	= "svr:0x87001020,name:LS1020A,die:LS1021A",
	  .revision	= "2.0",
	},
	{ .soc_id	= "svr:0x87081020,name:LS1020AE,die:LS1021A",
	  .revision	= "2.0",
	},
	{ .soc_id	= "svr:0x87001220,name:LS1022A,die:LS1021A",
	  .revision	= "2.0",
	},
	{ .soc_id	= "svr:0x87081220,name:LS1022AE,die:LS1021A",
	  .revision	= "2.0",
	},
	/* SoC: LS1046A/LS1026A Rev: 1.0 */
	{ .soc_id	= "svr:0x87070110,name:LS1046A,die:LS1046A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87070010,name:LS1046AE,die:LS1046A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87070910,name:LS1026A,die:LS1046A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87070810,name:LS1026AE,die:LS1046A",
	  .revision	= "1.0",
	},
	/* SoC: LS1043A/LS1023A Rev: 1.0 Package: 21*21 */
	{ .soc_id	= "svr:0x87920110,name:LS1043A,die:LS1043A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87920010,name:LS1043AE,die:LS1043A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87920910,name:LS1023A,die:LS1043A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87920810,name:LS1023AE,die:LS1043A",
	  .revision	= "1.0",
	},
	/* SoC: LS1043A/LS1023A Rev: 1.0 Package: 23*23 */
	{ .soc_id	= "svr:0x87920310,name:LS1043A,die:LS1043A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87920210,name:LS1043AE,die:LS1043A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87920B10,name:LS1023A,die:LS1043A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87920A10,name:LS1023AE,die:LS1043A",
	  .revision	= "1.0",
	},
	/* SoC: LS1012A Rev: 1.0 */
	{ .soc_id	= "svr:0x87040110,name:LS1012A,die:LS1012A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87040010,name:LS1012AE,die:LS1012A",
	  .revision	= "1.0",
	},
	/* SoC: LS2088A/LS2048A/LS2084A/LS2044A Rev: 1.0 */
	{ .soc_id	= "svr:0x87090110,name:LS2088A,die:LS2088A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87090010,name:LS2088AE,die:LS2088A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87092110,name:LS2048A,die:LS2088A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87092010,name:LS2048AE,die:LS2088A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87091110,name:LS2084A,die:LS2088A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87091010,name:LS2084AE,die:LS2088A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87093110,name:LS2044A,die:LS2088A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87093010,name:LS2044AE,die:LS2088A",
	  .revision	= "1.0",
	},
	/* SoC: LS2080A/LS2040A Rev: 1.0 */
	{ .soc_id	= "svr:0x87011010,name:LS2080AE,die:LS2080A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87013010,name:LS2040AE,die:LS2080A",
	  .revision	= "1.0",
	},
	/* SoC: LS2085A Rev: 1.0 */
	{ .soc_id	= "svr:0x87010110,name:LS2085A,die:LS2085A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87010010,name:LS2085AE,die:LS2085A",
	  .revision	= "1.0",
	},
	/* SoC: LS1088A/LS1048A/LS1084A/LS1044A Rev: 1.0 */
	{ .soc_id	= "svr:0x87030110,name:LS1088A,die:LS1088A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87030010,name:LS1088AE,die:LS1088A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87032110,name:LS1048A,die:LS1088A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87032010,name:LS1048AE,die:LS1088A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87030310,name:LS1084A,die:LS1088A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87030210,name:LS1084AE,die:LS1088A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87032310,name:LS1044A,die:LS1088A",
	  .revision	= "1.0",
	},
	{ .soc_id	= "svr:0x87032210,name:LS1044AE,die:LS1088A",
	  .revision	= "1.0",
	},
#endif /* CONFIG_ARCH_MXC || CONFIG_ARCH_LAYERSCAPE */
	{ },
};

static const struct soc_device_attribute *fsl_soc_device_match(
	unsigned int svr, const struct soc_device_attribute *matches)
{
	char svr_match[50];
	int n;

	n = sprintf(svr_match, "*%08x*", svr);

	do {
		if (!matches->soc_id)
			return NULL;
		if (glob_match(svr_match, matches->soc_id))
			break;
	} while (matches++);

	return matches;
}

unsigned int fsl_guts_get_svr(void)
{
	unsigned int svr = 0;

	if (!guts || !guts->regs)
		return svr;

	if (guts->little_endian)
		svr = ioread32(&guts->regs->svr);
	else
		svr = ioread32be(&guts->regs->svr);

	return svr;
}
EXPORT_SYMBOL(fsl_guts_get_svr);

static int fsl_guts_probe(struct platform_device *pdev)
{
	struct device_node *np;
	const struct soc_device_attribute *fsl_soc;
	const char *machine;
	unsigned int svr;
	int ret = 0;

	np = pdev->dev.of_node;

	/* Initialize guts */
	guts = kzalloc(sizeof(*guts), GFP_KERNEL);
	if (!guts)
		return -ENOMEM;

	guts->little_endian = of_property_read_bool(np, "little-endian");

	guts->regs = of_iomap(np, 0);
	if (!guts->regs) {
		ret = -ENOMEM;
		goto out_free;
	}

	/* Register soc device */
	soc_dev_attr = kzalloc(sizeof(*soc_dev_attr), GFP_KERNEL);
	if (!soc_dev_attr) {
		ret = -ENOMEM;
		goto out_unmap;
	}

	machine = of_flat_dt_get_machine_name();
	if (machine)
		soc_dev_attr->machine = kasprintf(GFP_KERNEL, "%s", machine);

	soc_dev_attr->family = kasprintf(GFP_KERNEL, "QorIQ");

	svr = fsl_guts_get_svr();
	fsl_soc = fsl_soc_device_match(svr, qoriq_soc);
	if (fsl_soc) {
		soc_dev_attr->soc_id = kasprintf(GFP_KERNEL, "%s",
						 fsl_soc->soc_id);
		soc_dev_attr->revision = kasprintf(GFP_KERNEL, "%s",
						   fsl_soc->revision);
	} else {
		soc_dev_attr->soc_id = kasprintf(GFP_KERNEL, "0x%08x", svr);
		soc_dev_attr->revision = kasprintf(GFP_KERNEL, "%d.%d",
						   SVR_MAJ(svr), SVR_MIN(svr));
	}

	soc_dev = soc_device_register(soc_dev_attr);
	if (IS_ERR(soc_dev)) {
		ret = -ENODEV;
		goto out;
	} else {
		pr_info("Detected: %s\n", soc_dev_attr->machine);
		pr_info("Detected SoC family: %s\n", soc_dev_attr->family);
		pr_info("Detected SoC ID: %s, revision: %s\n",
			soc_dev_attr->soc_id, soc_dev_attr->revision);
	}
	return 0;
out:
	kfree(soc_dev_attr->machine);
	kfree(soc_dev_attr->family);
	kfree(soc_dev_attr->soc_id);
	kfree(soc_dev_attr->revision);
	kfree(soc_dev_attr);
out_unmap:
	iounmap(guts->regs);
out_free:
	kfree(guts);
	return ret;
}

static int fsl_guts_remove(struct platform_device *dev)
{
	kfree(soc_dev_attr->machine);
	kfree(soc_dev_attr->family);
	kfree(soc_dev_attr->soc_id);
	kfree(soc_dev_attr->revision);
	kfree(soc_dev_attr);
	soc_device_unregister(soc_dev);
	iounmap(guts->regs);
	kfree(guts);
	return 0;
}

/*
 * Table for matching compatible strings, for device tree
 * guts node, for Freescale QorIQ SOCs.
 */
static const struct of_device_id fsl_guts_of_match[] = {
	{ .compatible = "fsl,qoriq-device-config-1.0", },
	{ .compatible = "fsl,qoriq-device-config-2.0", },
	{ .compatible = "fsl,p1010-guts", },
	{ .compatible = "fsl,p1020-guts", },
	{ .compatible = "fsl,p1021-guts", },
	{ .compatible = "fsl,p1022-guts", },
	{ .compatible = "fsl,p1023-guts", },
	{ .compatible = "fsl,p2020-guts", },
	{ .compatible = "fsl,bsc9131-guts", },
	{ .compatible = "fsl,bsc9132-guts", },
	{ .compatible = "fsl,mpc8536-guts", },
	{ .compatible = "fsl,mpc8544-guts", },
	{ .compatible = "fsl,mpc8548-guts", },
	{ .compatible = "fsl,mpc8568-guts", },
	{ .compatible = "fsl,mpc8569-guts", },
	{ .compatible = "fsl,mpc8572-guts", },
	{ .compatible = "fsl,ls1021a-dcfg", },
	{ .compatible = "fsl,ls1043a-dcfg", },
	{ .compatible = "fsl,ls2080a-dcfg", },
	{}
};

static struct platform_driver fsl_guts_driver = {
	.driver = {
		.name = "fsl-guts",
		.of_match_table = fsl_guts_of_match,
	},
	.probe = fsl_guts_probe,
	.remove = fsl_guts_remove,
};

module_platform_driver(fsl_guts_driver);
