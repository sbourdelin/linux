/*
 * Hisilicon Hi6220 IOMMU driver
 *
 * Copyright (c) 2015 Hisilicon Limited.
 *
 * Author: Chen Feng <puck.chen@hisilicon.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) "IOMMU: " fmt

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/sizes.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/pm_runtime.h>
#include <linux/iova.h>
#include <linux/iopoll.h>

#define SMMU_CTRL_OFFSET             (0x0000)
#define SMMU_ENABLE_OFFSET           (0x0004)
#define SMMU_PTBR_OFFSET             (0x0008)
#define SMMU_START_OFFSET            (0x000C)
#define SMMU_END_OFFSET              (0x0010)
#define SMMU_INTMASK_OFFSET          (0x0014)
#define SMMU_RINTSTS_OFFSET          (0x0018)
#define SMMU_MINTSTS_OFFSET          (0x001C)
#define SMMU_INTCLR_OFFSET           (0x0020)
#define SMMU_STATUS_OFFSET           (0x0024)
#define SMMU_AXIID_OFFSET            (0x0028)
#define SMMU_CNTCTRL_OFFSET          (0x002C)
#define SMMU_TRANSCNT_OFFSET         (0x0030)
#define SMMU_L0TLBHITCNT_OFFSET      (0x0034)
#define SMMU_L1TLBHITCNT_OFFSET      (0x0038)
#define SMMU_WRAPCNT_OFFSET          (0x003C)
#define SMMU_SEC_START_OFFSET        (0x0040)
#define SMMU_SEC_END_OFFSET          (0x0044)
#define SMMU_VERSION_OFFSET          (0x0048)
#define SMMU_IPTSRC_OFFSET           (0x004C)
#define SMMU_IPTPA_OFFSET            (0x0050)
#define SMMU_TRBA_OFFSET             (0x0054)
#define SMMU_BYS_START_OFFSET        (0x0058)
#define SMMU_BYS_END_OFFSET          (0x005C)
#define SMMU_RAM_OFFSET              (0x1000)

#define SMMU_CTRL_INVALID            (BIT(10))
#define SMMU_SR_REGS_NUM             (15)
#define SMMU_REGS_SGMT_END           (0x60)
#define PAGE_ENTRY_VALID             (0x1)
#define IOPAGE_SHIFT                 (12)
#define IOVA_PFN(addr)               ((addr) >> IOPAGE_SHIFT)
#define IOVA_PAGE_SZ                 (SZ_4K)

/**
 * The iova address from 0 ~ 2G
 */
#define IOVA_START                   (0x0)
#define IOVA_END                     (0x80000000)

struct hi6220_smmu {
	unsigned int irq;
	void __iomem *reg_base;
	struct clk *smmu_peri_clk;
	struct clk *smmu_clk;
	struct clk *media_sc_clk;
	spinlock_t spinlock; /*spinlock for tlb invalid*/
	dma_addr_t pgtable_phy;
	void *pgtable_virt;
	u32  pgtable_size;
	u32  *sr_data;
};

struct hi6220_domain {
	struct hi6220_smmu *smmu_dev;
	struct iommu_domain io_domain;
};

static struct hi6220_smmu *smmu_dev_handle;
static struct iova_domain iova_allocator;

static struct hi6220_domain *to_hi6220_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct hi6220_domain, io_domain);
}

static inline void __smmu_writel(struct hi6220_smmu *smmu_dev, u32 value,
				 unsigned long offset)
{
	writel(value, smmu_dev->reg_base + offset);
}

static inline u32 __smmu_readl(struct hi6220_smmu *smmu_dev,
			       unsigned long offset)
{
	return readl(smmu_dev->reg_base + offset);
}

/**
 * store and load the regs
 */
static void __store_regs(struct hi6220_smmu *smmu_dev)
{
	int i;
	u32 *data = smmu_dev->sr_data;

	for (i = 0; i < SMMU_SR_REGS_NUM; i++)
		data[i] = __smmu_readl(smmu_dev, i * 4);
}

static void __load_regs(struct hi6220_smmu *smmu_dev)
{
	int i;
	u32 *data = smmu_dev->sr_data;

	for (i = 0; i < SMMU_SR_REGS_NUM; i++)
		__smmu_writel(smmu_dev, data[i], i * 4);
}

static inline void __set_smmu_pte(unsigned int *pte,
				  dma_addr_t phys_addr)
{
	if ((*pte & PAGE_ENTRY_VALID))
		pr_err("set pte[%p]->%x already set!\n", pte, *pte);

	*pte = (unsigned int)(phys_addr | PAGE_ENTRY_VALID);
}

static inline void __clear_smmu_pte(unsigned int *pte)
{
	if (!(*pte & PAGE_ENTRY_VALID))
		pr_err("clear pte[%p] %x err!\n", pte, *pte);

	*pte = 0;
}

static inline int __invalid_smmu_tlb(struct hi6220_domain *m_domain,
				     unsigned long iova, size_t size)
{
	unsigned long flags;
	unsigned int smmu_ctrl = 0;
	unsigned int start_pfn = 0;
	unsigned int end_pfn   = 0;
	unsigned int tmp;
	int ret;
	void __iomem *reg_base;
	struct hi6220_smmu *smmu_dev = m_domain->smmu_dev;
	dma_addr_t smmu_pgtbl_phy = m_domain->smmu_dev->pgtable_phy;

	reg_base = smmu_dev->reg_base;
	start_pfn = IOVA_PFN(iova);
	end_pfn   = IOVA_PFN(iova + size);

	spin_lock_irqsave(&smmu_dev->spinlock, flags);
	__smmu_writel(smmu_dev, smmu_pgtbl_phy + start_pfn * sizeof(int),
		      SMMU_START_OFFSET);
	__smmu_writel(smmu_dev, smmu_pgtbl_phy + end_pfn * sizeof(int),
		      SMMU_END_OFFSET);

	smmu_ctrl = __smmu_readl(smmu_dev, SMMU_CTRL_OFFSET);
	smmu_ctrl = smmu_ctrl | SMMU_CTRL_INVALID;
	__smmu_writel(smmu_dev, smmu_ctrl, SMMU_CTRL_OFFSET);
	spin_unlock_irqrestore(&smmu_dev->spinlock, flags);

	ret = readl_poll_timeout_atomic(reg_base + SMMU_CNTCTRL_OFFSET, tmp,
					!(tmp & SMMU_CTRL_INVALID), 5, 50);
	if (ret) {
		pr_err("invalid smmu tlb error!");
		return ret;
	}
	return 0;
}

static int __smmu_enable(struct hi6220_smmu *smmu_dev)
{
	/**
	 * enable clock first
	 */
	if (clk_prepare_enable(smmu_dev->media_sc_clk)) {
		pr_err("clk_prepare_enable media_sc_clk is falied\n");
		return -ENODEV;
	}
	if (clk_prepare_enable(smmu_dev->smmu_peri_clk)) {
		pr_err("clk_prepare_enable smmu_peri_clk is falied\n");
		return -ENODEV;
	}
	if (clk_prepare_enable(smmu_dev->smmu_clk)) {
		pr_err("clk_prepare_enable smmu_clk is falied\n");
		return -ENODEV;
	}

	/**
	 * load the store register when resume
	 */
	if (smmu_dev->sr_data)
		__load_regs(smmu_dev);

	/*set axi id*/
	__smmu_writel(smmu_dev, 0xC70000FF, SMMU_AXIID_OFFSET);
	/*iommu page size 4K */
	__smmu_writel(smmu_dev, 0x000001A6, SMMU_CTRL_OFFSET);
	/*clear interrupt pa address*/
	__smmu_writel(smmu_dev, 0x00000000, SMMU_IPTPA_OFFSET);
	/*clear interrupt*/
	__smmu_writel(smmu_dev, 0xFF, SMMU_INTCLR_OFFSET);

	/*set page table phy_addr for ptable and preload*/
	__smmu_writel(smmu_dev, smmu_dev->pgtable_phy, SMMU_PTBR_OFFSET);
	__smmu_writel(smmu_dev, smmu_dev->pgtable_phy, SMMU_START_OFFSET);
	/*set the page table memory size*/
	__smmu_writel(smmu_dev, smmu_dev->pgtable_size, SMMU_END_OFFSET);
	__smmu_writel(smmu_dev, 0x3, SMMU_ENABLE_OFFSET);

	return 0;
}

/**
 * interrupt happen when operator error
 */
static irqreturn_t hi6220_smmu_isr(int irq, void *data)
{
	int          i;
	int          index;
	unsigned int irq_stat;
	struct hi6220_smmu *smmu_dev = (struct hi6220_smmu *)data;
	unsigned int pgt = 0;
	unsigned int pc_pgt = 0;

	irq_stat = __smmu_readl(smmu_dev, SMMU_MINTSTS_OFFSET);

	/**
	 * clear smmu interrupt
	 */
	__smmu_writel(smmu_dev, 0xff, SMMU_INTCLR_OFFSET);
	pgt = __smmu_readl(smmu_dev, SMMU_PTBR_OFFSET);
	pc_pgt = __smmu_readl(smmu_dev, SMMU_IPTSRC_OFFSET);

	/**
	 * dump key register of smmu
	 */
	pr_err("\n irq status= %08x\n"
			" SMMU_CTRL_OFFSET = %08x\n"
			" SMMU_ENABLE_OFFSET = %08x\n"
			" SMMU_PTBR_OFFSET = %08x\n"
			" SMMU_START_OFFSET = %08x\n"
			" SMMU_END_OFFSET = %08x\n"
			" SMMU_IPTSRC_OFFSET = %08x\n",
			irq_stat,
			__smmu_readl(smmu_dev, SMMU_CTRL_OFFSET),
			__smmu_readl(smmu_dev, SMMU_ENABLE_OFFSET),
			__smmu_readl(smmu_dev, SMMU_PTBR_OFFSET),
			__smmu_readl(smmu_dev, SMMU_START_OFFSET),
			__smmu_readl(smmu_dev, SMMU_END_OFFSET),
			__smmu_readl(smmu_dev, SMMU_IPTSRC_OFFSET));
	/**
	 * test bit0 to bit5
	 */
	if (irq_stat & 0x1)
		pr_err("page_size L1 TLB_size ddr_size configure error\n");
	if (irq_stat & BIT(1)) {
		index = (pc_pgt - pgt) / 4;
		pr_err("ptable entry error:IOMMU_VA=0x%x\n", index * SZ_4K);
	}
	if (irq_stat & BIT(2))
		pr_err("AXI master0 receive error response\n");
	if (irq_stat & BIT(3))
		pr_err("AXI master1 receive error response\n");
	if (irq_stat & BIT(4))
		pr_err("AXI master0 access timeout\n");
	if (irq_stat & BIT(5))
		pr_err("AXI master1 access timeout\n");

	/**
	 * dump all smmu register for error check
	 */
	for (i = 0; i < SMMU_REGS_SGMT_END; i += 4)
		pr_err("[%08x] ", __smmu_readl(smmu_dev, i));

	return IRQ_HANDLED;
}

static struct iommu_domain *hi6220_domain_alloc(unsigned type)
{
	struct hi6220_domain *m_domain;
	struct hi6220_smmu *smmu_dev;

	if (type != IOMMU_DOMAIN_UNMANAGED)
		return NULL;

	smmu_dev = smmu_dev_handle;
	if (!smmu_dev)
		return NULL;

	m_domain = kzalloc(sizeof(*m_domain), GFP_KERNEL);
	if (!m_domain)
		return NULL;

	m_domain->smmu_dev = smmu_dev_handle;
	m_domain->io_domain.geometry.aperture_start = IOVA_START;
	m_domain->io_domain.geometry.aperture_end = IOVA_END;
	m_domain->io_domain.geometry.force_aperture = true;

	return &m_domain->io_domain;
}

static void hi6220_domain_free(struct iommu_domain *domain)
{
	struct hi6220_domain *hi6220_domain = to_hi6220_domain(domain);

	kfree(hi6220_domain);
}

static int hi6220_smmu_attach_dev(struct iommu_domain *domain,
				  struct device *dev)
{
	dev->archdata.iommu = &iova_allocator;

	return 0;
}

static void hi6220_smmu_detach_dev(struct iommu_domain *domain,
				   struct device *dev)
{
	dev->archdata.iommu = NULL;
}

static int hi6220_smmu_map(struct iommu_domain *domain,
			   unsigned long iova, phys_addr_t pa,
			   size_t size, int smmu_prot)
{
	struct hi6220_domain *m_domain = to_hi6220_domain(domain);
	struct hi6220_smmu *smmu_dev = m_domain->smmu_dev;
	unsigned int *page_table = (unsigned int *)smmu_dev->pgtable_virt;

	__set_smmu_pte(page_table + IOVA_PFN(iova), pa);

	__invalid_smmu_tlb(m_domain, iova, size);

	return 0;
}

static size_t hi6220_smmu_unmap(struct iommu_domain *domain, unsigned long iova,
				size_t size)
{
	struct hi6220_domain *m_domain = to_hi6220_domain(domain);
	struct hi6220_smmu *smmu_dev = m_domain->smmu_dev;
	int *page_table = (unsigned int *)smmu_dev->pgtable_virt;

	__clear_smmu_pte(page_table + IOVA_PFN(iova));

	__invalid_smmu_tlb(m_domain, iova, size);

	return IOVA_PAGE_SZ;
}

static const struct iommu_ops hi6220_smmu_ops = {
	.domain_alloc = hi6220_domain_alloc,
	.domain_free = hi6220_domain_free,
	.attach_dev = hi6220_smmu_attach_dev,
	.detach_dev = hi6220_smmu_detach_dev,
	.map = hi6220_smmu_map,
	.unmap = hi6220_smmu_unmap,
	.map_sg = default_iommu_map_sg,
	.pgsize_bitmap = IOVA_PAGE_SZ,
};

static int hi6220_smmu_probe(struct platform_device *pdev)
{
	int ret;
	int irq;
	struct hi6220_smmu *smmu_dev  = NULL;
	struct resource *res = NULL;

	smmu_dev = devm_kzalloc(&pdev->dev, sizeof(*smmu_dev), GFP_KERNEL);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	smmu_dev->reg_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(smmu_dev->reg_base))
		return PTR_ERR(smmu_dev->reg_base);

	smmu_dev->pgtable_size = (IOVA_END - IOVA_START) / SZ_4K * sizeof(int);
	smmu_dev->media_sc_clk = devm_clk_get(&pdev->dev, "media-sc");
	smmu_dev->smmu_peri_clk = devm_clk_get(&pdev->dev, "smmu-peri");
	smmu_dev->smmu_clk  = devm_clk_get(&pdev->dev, "smmu");
	if (IS_ERR(smmu_dev->media_sc_clk) || IS_ERR(smmu_dev->smmu_peri_clk) ||
	    IS_ERR(smmu_dev->media_sc_clk)) {
		pr_err("clk is not ready!\n");
		return -EINVAL;
	}

	irq = platform_get_irq(pdev, 0);
	ret = devm_request_irq(&pdev->dev, irq, hi6220_smmu_isr, 0,
			       dev_name(&pdev->dev), smmu_dev);
	if (ret) {
		pr_err("Unabled to register handler of irq %d\n", irq);
		return ret;
	}

	spin_lock_init(&smmu_dev->spinlock);

	__smmu_enable(smmu_dev);

	iova_cache_get();
	init_iova_domain(&iova_allocator, IOVA_PAGE_SZ,
			 IOVA_PFN(IOVA_START), IOVA_PFN(IOVA_END));

	smmu_dev->sr_data = vmalloc(sizeof(u32) * SMMU_SR_REGS_NUM);
	if (!smmu_dev->sr_data) {
		iova_cache_put();
		return -ENOMEM;
	}

	dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	smmu_dev->pgtable_virt = dma_zalloc_coherent(&pdev->dev,
						     smmu_dev->pgtable_size,
						     &smmu_dev->pgtable_phy,
						     GFP_KERNEL);

	if (!smmu_dev->pgtable_virt) {
		pr_err("alloc pagetable mem failed\n");
		vfree(smmu_dev->sr_data);
		iova_cache_put();
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, smmu_dev);
	bus_set_iommu(&platform_bus_type, &hi6220_smmu_ops);
	smmu_dev_handle = smmu_dev;

	return 0;
}

static int hi6220_smmu_suspend(struct platform_device *pdev,
			       pm_message_t state)
{
	struct hi6220_smmu *smmu_dev = dev_get_drvdata(&pdev->dev);

	__store_regs(smmu_dev);

	if (smmu_dev->smmu_clk)
		clk_disable_unprepare(smmu_dev->smmu_clk);
	if (smmu_dev->media_sc_clk)
		clk_disable_unprepare(smmu_dev->media_sc_clk);
	if (smmu_dev->smmu_peri_clk)
		clk_disable_unprepare(smmu_dev->smmu_peri_clk);

	return 0;
}

static int hi6220_smmu_resume(struct platform_device *pdev)
{
	struct hi6220_smmu *smmu_dev = dev_get_drvdata(&pdev->dev);

	__smmu_enable(smmu_dev);
	return 0;
}

static const struct of_device_id of_smmu_match_tbl[] = {
	{
		.compatible = "hisilicon,hi6220-smmu",
	},
	{ }
};

static struct platform_driver hi6220_smmu_driver = {
	.driver  = {
		.name = "smmu-hi6220",
		.of_match_table = of_smmu_match_tbl,
	},
	.probe  =  hi6220_smmu_probe,
	.suspend = hi6220_smmu_suspend,
	.resume  = hi6220_smmu_resume,
};

static int __init hi6220_smmu_init(void)
{
	int ret;

	ret = platform_driver_register(&hi6220_smmu_driver);
	return ret;
}

subsys_initcall(hi6220_smmu_init);
