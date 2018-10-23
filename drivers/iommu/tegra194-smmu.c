/*
 * IOMMU API for Tegra194 Dual ARM SMMU implementation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Copyright (C) 2018 Nvidia Corporation
 *
 * Author: Krishna Reddy <vdumpa@nvidia.com>
 */

#define pr_fmt(fmt) "tegra194-smmu: " fmt

#include "arm-smmu-common.h"

/* Tegra194 has three SMMU instances.
 * Two of the SMMU instances are used by specific set of devices to
 * access IOVA addresses in interleaved fashion.
 * The 3rd SMMU instance is used alone by specific set of devices.
 * This driver only support Dual SMMU configuration which interleaves
 * IOVA accesses across two SMMU's.
 * For the 3rd SMMU instance, Default ARM SMMU driver is used.
 */
#define NUM_SMMU_INSTANCES 2

struct tegra194_smmu {
	void __iomem			*bases[NUM_SMMU_INSTANCES];
	struct arm_smmu_device		*smmu;
};

static struct tegra194_smmu t194_smmu;

static inline void writel_one(u32 val, volatile void __iomem *virt_addr)
{
	writel(val, virt_addr);
}

static inline void writel_relaxed_one(u32 val,
			volatile void __iomem *virt_addr)
{
	writel_relaxed(val, virt_addr);
}

#define WRITEL_FN(fn, call, type) \
static inline void fn(type val, volatile void __iomem *virt_addr) \
{ \
	int i; \
	u32 offset = abs(virt_addr - t194_smmu.bases[0]); \
	for (i = 0; i < NUM_SMMU_INSTANCES; i++) \
		call(val, t194_smmu.bases[i] + offset); \
}

/* Override writel* macros to program all the smmu instances
 * transparently through arm-smmu-common.c code.
 */
WRITEL_FN(writel_relaxed_all, writel_relaxed, u32);
WRITEL_FN(writeq_relaxed_all, writeq_relaxed, u64);
WRITEL_FN(writel_all, writel, u32);

#undef writel_relaxed
#undef writeq_relaxed
#undef writel
#define writel_relaxed writel_relaxed_all
#define writeq_relaxed writeq_relaxed_all
#define writel writel_all

#include "arm-smmu-common.c"

#define TO_INSTANCE(addr, inst) \
	(addr - t194_smmu.bases[0] + t194_smmu.bases[inst])

static void arm_smmu_tlb_sync_global(struct arm_smmu_device *smmu)
{
	int i;
	void __iomem *base;
	unsigned long flags;

	spin_lock_irqsave(&smmu->global_sync_lock, flags);
	for (i = 0; i < NUM_SMMU_INSTANCES; i++) {
		base = t194_smmu.bases[i];
		__arm_smmu_tlb_sync(smmu, base + ARM_SMMU_GR0_sTLBGSYNC,
				    base + ARM_SMMU_GR0_sTLBGSTATUS);
	}
	spin_unlock_irqrestore(&smmu->global_sync_lock, flags);
}

static void arm_smmu_tlb_sync_context(void *cookie)
{
	int i;
	struct arm_smmu_domain *smmu_domain = cookie;
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	void __iomem *base;
	unsigned long flags;

	spin_lock_irqsave(&smmu_domain->cb_lock, flags);
	for (i = 0; i < NUM_SMMU_INSTANCES; i++) {
		base = ARM_SMMU_CB(smmu, smmu_domain->cfg.cbndx);
		base = TO_INSTANCE(base, i);
		__arm_smmu_tlb_sync(smmu, base + ARM_SMMU_CB_TLBSYNC,
				    base + ARM_SMMU_CB_TLBSTATUS);
	}
	spin_unlock_irqrestore(&smmu_domain->cb_lock, flags);
}

static irqreturn_t arm_smmu_context_fault(int irq, void *dev)
{
	int i;
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(dev);
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	void __iomem *cb_base;
	irqreturn_t irq_state;

	for (i = 0; i < NUM_SMMU_INSTANCES; i++) {
		cb_base = ARM_SMMU_CB(smmu, cfg->cbndx);
		cb_base = TO_INSTANCE(cb_base, i);

		irq_state = arm_smmu_context_fault_common(smmu, cfg, cb_base);

		if (irq_state == IRQ_HANDLED)
			break;
	}

	return irq_state;
}

static irqreturn_t arm_smmu_global_fault(int irq, void *dev)
{
	int i;
	struct arm_smmu_device *smmu = dev;
	irqreturn_t irq_state;

	for (i = 0; i < NUM_SMMU_INSTANCES; i++) {
		void __iomem *gr0_base = t194_smmu.bases[i];

		irq_state = arm_smmu_global_fault_common(smmu, gr0_base);

		if (irq_state == IRQ_HANDLED)
			break;
	}

	return irq_state;
}

ARM_SMMU_MATCH_DATA(arm_mmu500, ARM_SMMU_V2, ARM_MMU500);

static const struct of_device_id t194_smmu_of_match[] = {
	{ .compatible = "tegra194,arm,mmu-500", .data = &arm_mmu500 },
	{ },
};
MODULE_DEVICE_TABLE(of, t194_smmu_of_match);

static int t194_smmu_device_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct device *dev = &pdev->dev;
	int i, err;

	if (t194_smmu.smmu) {
		pr_err("One instance of Tegra194 SMMU platform device is allowed\n");
		return -ENODEV;
	}

	for (i = 1; i < NUM_SMMU_INSTANCES; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res)
			return -ENODEV;
		t194_smmu.bases[i] = devm_ioremap_resource(dev, res);
		if (IS_ERR(t194_smmu.bases[i]))
			return PTR_ERR(t194_smmu.bases[i]);
	}

	err = arm_smmu_device_probe_common(pdev, &t194_smmu.bases[0]);
	if (err)
		return err;

	t194_smmu.smmu = platform_get_drvdata(pdev);
	return 0;
}

static struct platform_driver arm_smmu_driver = {
	.driver	= {
		.name		= "tegra194-arm-smmu",
		.of_match_table	= of_match_ptr(t194_smmu_of_match),
		.pm		= &arm_smmu_pm_ops,
	},
	.probe	= t194_smmu_device_probe,
	.remove	= arm_smmu_device_remove,
	.shutdown = arm_smmu_device_shutdown,
};
module_platform_driver(arm_smmu_driver);

MODULE_DESCRIPTION("IOMMU API for Tegra194 SMMU implementation");
MODULE_AUTHOR("Krishna Reddy <vdumpa@nvidia.com>");
MODULE_LICENSE("GPL v2");
