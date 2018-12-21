// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 2017-2018, Intel Corporation.
 *  Copyright (C) 2012 Freescale Semiconductor, Inc.
 *  Copyright (C) 2012 Linaro Ltd.
 *
 *  Based on syscon driver.
 */

#include <linux/arm-smccc.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/mfd/altera-sysmgr.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/slab.h>

/**
 * struct altr_sysmgr - Altera SOCFPGA System Manager
 * @regmap: the regmap used for System Manager accesses.
 * @base  : the base address for the System Manager
 */
struct altr_sysmgr {
	struct regmap   *regmap;
	resource_size_t *base;
};

/**
 * Only 1 instance of System Manager is needed but many
 * consumers will want to access it with the matching
 * functions below.
 */
static struct altr_sysmgr *p_sysmgr;

/**
 * s10_protected_reg_write
 * Write to a protected SMC register.
 * @base: Base address of System Manager
 * @reg:  Address offset of register
 * @val:  Value to write
 * Return: INTEL_SIP_SMC_STATUS_OK (0) on success
 *	   INTEL_SIP_SMC_REG_ERROR on error
 *	   INTEL_SIP_SMC_RETURN_UNKNOWN_FUNCTION if not supported
 */
static int s10_protected_reg_write(void *base,
				   unsigned int reg, unsigned int val)
{
	struct arm_smccc_res result;
	unsigned long sysmgr_base = (unsigned long)base;

	arm_smccc_smc(INTEL_SIP_SMC_REG_WRITE, sysmgr_base + reg,
		      val, 0, 0, 0, 0, 0, &result);

	return (int)result.a0;
}

/**
 * s10_protected_reg_read
 * Read the status of a protected SMC register
 * @base: Base address of System Manager.
 * @reg:  Address of register
 * @val:  Value read.
 * Return: INTEL_SIP_SMC_STATUS_OK (0) on success
 *	   INTEL_SIP_SMC_REG_ERROR on error
 *	   INTEL_SIP_SMC_RETURN_UNKNOWN_FUNCTION if not supported
 */
static int s10_protected_reg_read(void *base,
				  unsigned int reg, unsigned int *val)
{
	struct arm_smccc_res result;
	unsigned long sysmgr_base = (unsigned long)base;

	arm_smccc_smc(INTEL_SIP_SMC_REG_READ, sysmgr_base + reg,
		      0, 0, 0, 0, 0, 0, &result);

	*val = (unsigned int)result.a1;

	return (int)result.a0;
}

static struct regmap_config altr_sysmgr_regmap_cfg = {
	.name = "altr_sysmgr",
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.fast_io = true,
	.use_single_read = true,
	.use_single_write = true,
};

/**
 * socfpga_is_s10
 * Determine if running on Stratix10 platform.
 * Return: True if running Stratix10, otherwise false.
 */
static int socfpga_is_s10(struct device_node *np)
{
	return of_device_is_compatible(np, "altr,sys-mgr-s10");
}

/**
 * of_sysmgr_register
 * Create and register the Altera System Manager regmap.
 * ARM32 is a mmio regmap while ARM64 needs a physical address
 * for the SMC call.
 * Return: Pointer to new sysmgr on success.
 *         Pointer error on failure.
 */
static struct altr_sysmgr *of_sysmgr_register(struct device_node *np)
{
	struct altr_sysmgr *sysmgr;
	struct regmap *regmap;
	int ret;
	struct regmap_config sysmgr_config = altr_sysmgr_regmap_cfg;
	struct resource res;

	if (!of_device_is_compatible(np, "altr,sys-mgr") &&
	    !of_device_is_compatible(np, "altr,sys-mgr-s10"))
		return ERR_PTR(-EINVAL);

	sysmgr = kzalloc(sizeof(*sysmgr), GFP_KERNEL);
	if (!sysmgr)
		return ERR_PTR(-ENOMEM);

	if (of_address_to_resource(np, 0, &res)) {
		ret = -ENOMEM;
		goto err_map;
	}

	sysmgr_config.max_register = resource_size(&res) -
				     sysmgr_config.reg_stride;

	if (socfpga_is_s10(np)) {
		/* Need physical address for SMCC call */
		sysmgr->base = (resource_size_t *)res.start;
		sysmgr_config.reg_read = s10_protected_reg_read;
		sysmgr_config.reg_write = s10_protected_reg_write;

		regmap = regmap_init(NULL, NULL, sysmgr->base, &sysmgr_config);
	} else {
		sysmgr->base = ioremap(res.start, resource_size(&res));

		if (!sysmgr->base) {
			ret = -ENOMEM;
			goto err_map;
		}

		regmap = regmap_init_mmio(NULL, sysmgr->base, &sysmgr_config);
		if (IS_ERR(regmap))
			iounmap(sysmgr->base);
	}
	if (IS_ERR(regmap)) {
		pr_err("regmap init failed\n");
		ret = PTR_ERR(regmap);
		goto err_map;
	}

	sysmgr->regmap = regmap;

	p_sysmgr = sysmgr;

	return sysmgr;

err_map:
	kfree(sysmgr);
	return ERR_PTR(ret);
}

struct regmap *altr_sysmgr_node_to_regmap(struct device_node *np)
{
	struct altr_sysmgr *sysmgr = NULL;

	if (!p_sysmgr)
		sysmgr = of_sysmgr_register(np);
	else
		sysmgr = p_sysmgr;

	if (IS_ERR(sysmgr))
		return ERR_CAST(sysmgr);

	return sysmgr->regmap;
}
EXPORT_SYMBOL_GPL(altr_sysmgr_node_to_regmap);

struct regmap *altr_sysmgr_regmap_lookup_by_phandle(struct device_node *np,
						    const char *property)
{
	struct device_node *sysmgr_np;
	struct regmap *regmap;

	if (property)
		sysmgr_np = of_parse_phandle(np, property, 0);
	else
		sysmgr_np = np;

	if (!sysmgr_np)
		return ERR_PTR(-ENODEV);

	regmap = altr_sysmgr_node_to_regmap(sysmgr_np);
	of_node_put(sysmgr_np);

	return regmap;
}
EXPORT_SYMBOL_GPL(altr_sysmgr_regmap_lookup_by_phandle);

static int sysmgr_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct altr_sysmgr *sysmgr;
	struct resource *res;

	/* Skip Initialization if already created */
	if (p_sysmgr)
		goto finish;

	sysmgr = of_sysmgr_register(pdev->dev.of_node);
	if (IS_ERR(sysmgr)) {
		dev_err(dev, "regmap init failed\n");
		return -ENODEV;
	}

finish:
	platform_set_drvdata(pdev, p_sysmgr);

	dev_dbg(dev, "regmap %pR registered\n", res);

	return 0;
}

static const struct of_device_id altr_sysmgr_of_match[] = {
	{ .compatible = "altr,sys-mgr" },
	{ .compatible = "altr,sys-mgr-s10" },
	{},
};
MODULE_DEVICE_TABLE(of, altr_sysmgr_of_match);

static struct platform_driver altr_sysmgr_driver = {
	.probe =  sysmgr_probe,
	.driver = {
		.name = "altr,system_manager",
		.of_match_table = altr_sysmgr_of_match,
	},
};

static int __init altr_sysmgr_init(void)
{
	return platform_driver_register(&altr_sysmgr_driver);
}
core_initcall(altr_sysmgr_init);

static void __exit altr_sysmgr_exit(void)
{
	platform_driver_unregister(&altr_sysmgr_driver);
}
module_exit(altr_sysmgr_exit);

MODULE_AUTHOR("Thor Thayer <>");
MODULE_DESCRIPTION("SOCFPGA System Manager driver");
MODULE_LICENSE("GPL v2");
