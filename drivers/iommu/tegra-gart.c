/*
 * IOMMU API for GART in Tegra20
 *
 * Copyright (c) 2010-2012, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>

#include <soc/tegra/mc.h>

/* bitmap of the page sizes currently supported */
#define GART_IOMMU_PGSIZES	(SZ_4K)

#define GART_REG_BASE		0x24
#define GART_CONFIG		(0x24 - GART_REG_BASE)
#define GART_ENTRY_ADDR		(0x28 - GART_REG_BASE)
#define GART_ENTRY_DATA		(0x2c - GART_REG_BASE)

#define GART_ENTRY_PHYS_ADDR_VALID	BIT(31)

#define GART_PAGE_SHIFT		12
#define GART_PAGE_SIZE		(1 << GART_PAGE_SHIFT)
#define GART_PAGE_MASK		GENMASK(30, GART_PAGE_SHIFT)

struct gart_device {
	void __iomem		*regs;
	u32			*savedata;
	unsigned long		iovmm_base;	/* offset to vmm_area start */
	unsigned long		iovmm_end;	/* offset to vmm_area end */
	spinlock_t		pte_lock;	/* for pagetable */
	spinlock_t		dom_lock;	/* for active domain */
	unsigned int		active_devices;	/* number of active devices */
	struct iommu_domain	*active_domain;	/* current active domain */
	struct iommu_device	iommu;		/* IOMMU Core handle */
	struct device		*dev;
};

static struct gart_device *gart_handle; /* unique for a system */

static bool gart_debug;

/*
 * Any interaction between any block on PPSB and a block on APB or AHB
 * must have these read-back to ensure the APB/AHB bus transaction is
 * complete before initiating activity on the PPSB block.
 */
#define FLUSH_GART_REGS(gart)	readl_relaxed((gart)->regs + GART_CONFIG)

#define for_each_gart_pte(gart, iova)					\
	for (iova = gart->iovmm_base;					\
	     iova < gart->iovmm_end;					\
	     iova += GART_PAGE_SIZE)

static inline void gart_set_pte(struct gart_device *gart,
				unsigned long iova, u32 pte)
{
	writel_relaxed(iova, gart->regs + GART_ENTRY_ADDR);
	writel_relaxed(pte, gart->regs + GART_ENTRY_DATA);

	dev_dbg(gart->dev, "GART: %s %08lx:%08lx\n",
		pte ? "map" : "unmap", iova, pte & GART_PAGE_MASK);
}

static inline unsigned long gart_read_pte(struct gart_device *gart,
					  unsigned long iova)
{
	unsigned long pte;

	writel_relaxed(iova, gart->regs + GART_ENTRY_ADDR);
	pte = readl_relaxed(gart->regs + GART_ENTRY_DATA);

	return pte;
}

static void do_gart_setup(struct gart_device *gart, const u32 *data)
{
	unsigned long iova;

	for_each_gart_pte(gart, iova)
		gart_set_pte(gart, iova, data ? *(data++) : 0);

	writel_relaxed(1, gart->regs + GART_CONFIG);
	FLUSH_GART_REGS(gart);
}

static inline bool gart_iova_range_invalid(struct gart_device *gart,
					   unsigned long iova, size_t bytes)
{
	return unlikely(iova < gart->iovmm_base ||
			iova + bytes > gart->iovmm_end);
}

static inline bool gart_pte_valid(struct gart_device *gart, unsigned long iova)
{
	return !!(gart_read_pte(gart, iova) & GART_ENTRY_PHYS_ADDR_VALID);
}

static int gart_iommu_attach_dev(struct iommu_domain *domain,
				 struct device *dev)
{
	struct gart_device *gart = gart_handle;
	int ret = 0;

	spin_lock(&gart->dom_lock);

	if (gart->active_domain && gart->active_domain != domain) {
		ret = -EBUSY;
	} else if (dev->archdata.iommu != domain) {
		dev->archdata.iommu = domain;
		gart->active_domain = domain;
		gart->active_devices++;
	}

	spin_unlock(&gart->dom_lock);

	return ret;
}

static void gart_iommu_detach_dev(struct iommu_domain *domain,
				  struct device *dev)
{
	struct gart_device *gart = gart_handle;

	spin_lock(&gart->dom_lock);

	if (dev->archdata.iommu == domain) {
		dev->archdata.iommu = NULL;

		if (--gart->active_devices == 0)
			gart->active_domain = NULL;
	}

	spin_unlock(&gart->dom_lock);
}

static struct iommu_domain *gart_iommu_domain_alloc(unsigned type)
{
	struct iommu_domain *domain;

	if (type != IOMMU_DOMAIN_UNMANAGED)
		return NULL;

	domain = kzalloc(sizeof(*domain), GFP_KERNEL);
	if (domain) {
		domain->geometry.aperture_start = gart_handle->iovmm_base;
		domain->geometry.aperture_end = gart_handle->iovmm_end - 1;
		domain->geometry.force_aperture = true;
	}

	return domain;
}

static void gart_iommu_domain_free(struct iommu_domain *domain)
{
	kfree(domain);
}

static int __gart_iommu_map(struct gart_device *gart, unsigned long iova,
			    phys_addr_t pa)
{
	if (unlikely(gart_debug) && gart_pte_valid(gart, iova)) {
		dev_WARN(gart->dev, "GART: Page entry is in-use\n");
		return -EBUSY;
	}

	gart_set_pte(gart, iova, GART_ENTRY_PHYS_ADDR_VALID | pa);

	return 0;
}

static int gart_iommu_map(struct iommu_domain *domain, unsigned long iova,
			  phys_addr_t pa, size_t bytes, int prot)
{
	struct gart_device *gart = gart_handle;
	unsigned long flags;
	int ret;

	if (gart_iova_range_invalid(gart, iova, bytes))
		return -EINVAL;

	spin_lock_irqsave(&gart->pte_lock, flags);
	ret = __gart_iommu_map(gart, iova, pa);
	spin_unlock_irqrestore(&gart->pte_lock, flags);

	return ret;
}

static void __gart_iommu_unmap(struct gart_device *gart, unsigned long iova)
{
	if (unlikely(gart_debug) && !gart_pte_valid(gart, iova)) {
		dev_WARN(gart->dev, "GART: Page entry is invalid\n");
		return;
	}

	gart_set_pte(gart, iova, 0);
}

static size_t gart_iommu_unmap(struct iommu_domain *domain, unsigned long iova,
			       size_t bytes)
{
	struct gart_device *gart = gart_handle;
	unsigned long flags;

	if (gart_iova_range_invalid(gart, iova, bytes))
		return 0;

	spin_lock_irqsave(&gart->pte_lock, flags);
	__gart_iommu_unmap(gart, iova);
	spin_unlock_irqrestore(&gart->pte_lock, flags);

	return bytes;
}

static phys_addr_t gart_iommu_iova_to_phys(struct iommu_domain *domain,
					   dma_addr_t iova)
{
	struct gart_device *gart = gart_handle;
	unsigned long flags;
	unsigned long pte;

	if (gart_iova_range_invalid(gart, iova, SZ_4K))
		return -EINVAL;

	spin_lock_irqsave(&gart->pte_lock, flags);
	pte = gart_read_pte(gart, iova);
	spin_unlock_irqrestore(&gart->pte_lock, flags);

	return pte & GART_PAGE_MASK;
}

static bool gart_iommu_capable(enum iommu_cap cap)
{
	return false;
}

static int gart_iommu_add_device(struct device *dev)
{
	struct iommu_group *group;

	if (!dev->iommu_fwspec)
		return -ENODEV;

	group = iommu_group_get_for_dev(dev);
	if (IS_ERR(group))
		return PTR_ERR(group);

	iommu_group_put(group);

	iommu_device_link(&gart_handle->iommu, dev);

	return 0;
}

static void gart_iommu_remove_device(struct device *dev)
{
	iommu_group_remove_device(dev);
	iommu_device_unlink(&gart_handle->iommu, dev);
}

static int gart_iommu_of_xlate(struct device *dev,
			       struct of_phandle_args *args)
{
	return 0;
}

static void gart_iommu_sync(struct iommu_domain *domain)
{
	struct gart_device *gart = gart_handle;

	FLUSH_GART_REGS(gart);
}

static const struct iommu_ops gart_iommu_ops = {
	.capable	= gart_iommu_capable,
	.domain_alloc	= gart_iommu_domain_alloc,
	.domain_free	= gart_iommu_domain_free,
	.attach_dev	= gart_iommu_attach_dev,
	.detach_dev	= gart_iommu_detach_dev,
	.add_device	= gart_iommu_add_device,
	.remove_device	= gart_iommu_remove_device,
	.device_group	= generic_device_group,
	.map		= gart_iommu_map,
	.map_sg		= default_iommu_map_sg,
	.unmap		= gart_iommu_unmap,
	.iova_to_phys	= gart_iommu_iova_to_phys,
	.pgsize_bitmap	= GART_IOMMU_PGSIZES,
	.of_xlate	= gart_iommu_of_xlate,
	.iotlb_sync_map	= gart_iommu_sync,
	.iotlb_sync	= gart_iommu_sync,
};

int tegra_gart_suspend(struct gart_device *gart)
{
	u32 *data = gart->savedata;
	unsigned long iova;

	for_each_gart_pte(gart, iova)
		*(data++) = gart_read_pte(gart, iova);

	return 0;
}

int tegra_gart_resume(struct gart_device *gart)
{
	do_gart_setup(gart, gart->savedata);

	return 0;
}

struct gart_device *tegra_gart_probe(struct device *dev,
				     const struct tegra_smmu_soc *soc,
				     struct tegra_mc *mc)
{
	struct gart_device *gart;
	struct resource *res;
	int ret;

	BUILD_BUG_ON(PAGE_SHIFT != GART_PAGE_SHIFT);

	/* Tegra30+ has an SMMU and no GART */
	if (soc)
		return NULL;

	/* the GART memory aperture is required */
	res = platform_get_resource(to_platform_device(dev), IORESOURCE_MEM, 1);
	if (!res) {
		dev_err(dev, "GART: Memory aperture resource unavailable\n");
		return ERR_PTR(-ENXIO);
	}

	gart = kzalloc(sizeof(*gart), GFP_KERNEL);
	if (!gart)
		return ERR_PTR(-ENOMEM);

	gart_handle = gart;

	gart->dev = dev;
	gart->regs = mc->regs + GART_REG_BASE;
	gart->iovmm_base = res->start;
	gart->iovmm_end = res->start + resource_size(res);
	spin_lock_init(&gart->pte_lock);
	spin_lock_init(&gart->dom_lock);

	do_gart_setup(gart, NULL);

	ret = iommu_device_sysfs_add(&gart->iommu, dev, NULL, "gart");
	if (ret)
		goto free_gart;

	iommu_device_set_ops(&gart->iommu, &gart_iommu_ops);
	iommu_device_set_fwnode(&gart->iommu, dev->fwnode);

	ret = iommu_device_register(&gart->iommu);
	if (ret)
		goto remove_sysfs;

	gart->savedata = vmalloc(resource_size(res) >> GART_PAGE_SHIFT);
	if (!gart->savedata) {
		ret = -ENOMEM;
		goto unregister_iommu;
	}

	return gart;

unregister_iommu:
	iommu_device_unregister(&gart->iommu);
remove_sysfs:
	iommu_device_sysfs_remove(&gart->iommu);
free_gart:
	kfree(gart);

	return ERR_PTR(ret);
}

module_param(gart_debug, bool, 0644);

MODULE_PARM_DESC(gart_debug, "Enable GART debugging");
MODULE_DESCRIPTION("IOMMU API for GART in Tegra20");
MODULE_AUTHOR("Hiroshi DOYU <hdoyu@nvidia.com>");
MODULE_LICENSE("GPL v2");
